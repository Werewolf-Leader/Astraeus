#include "HardwareManager.h"

#include <QDateTime>
#include <QDebug>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <limits>

#include "SysfsAccess.h"
#include "DBusAccess.h"
#include "SettingsStore.h"
#include "RyzenAdjAccess.h"

HardwareManager::HardwareManager(QObject *parent)
    : QObject(parent)
    , m_sysfs(new SysfsAccess())
    , m_dbus(new DBusAccess())
    , m_settings(new SettingsStore())
    , m_ryzenAdj(new RyzenAdjAccess())
    , m_ownsAccessors(true)
    , m_selectedProfile(powerProfileName(PowerProfile::Balanced))
{
    qDebug() << "[Boreas] HardwareManager created";
    loadPersistedSettings();
    detectCapabilities();
    reapplyPersistedOnStartup();
}

HardwareManager::HardwareManager(ISysfsAccess *sysfs, IDBusAccess *dbus,
                                 ISettingsStore *settings, QObject *parent,
                                 IRyzenAdjAccess *ryzenAdj)
    : QObject(parent)
    , m_sysfs(sysfs)
    , m_dbus(dbus)
    , m_settings(settings)
    , m_ryzenAdj(ryzenAdj)
    , m_ownsAccessors(false)
    , m_selectedProfile(powerProfileName(PowerProfile::Balanced))
{
    // Injected accessors are owned by the caller; this instance never deletes
    // them (see ownership note in the header). m_ryzenAdj may be nullptr.
    loadPersistedSettings();
    detectCapabilities();
    reapplyPersistedOnStartup();
}

HardwareManager::~HardwareManager()
{
    if (m_ownsAccessors) {
        delete m_sysfs;
        delete m_dbus;
        delete m_settings;
        delete m_ryzenAdj;
    }
}

void HardwareManager::refreshSensors()
{
    qDebug() << "[Boreas] C++ received Refresh button click";
    // Retained for backward compatibility; re-expressed to drive a temperature
    // poll so existing bindings keep working (task 6.2, design "Existing member
    // retained").
    refreshTemperatures();
}

// ---- selectedProfile property (Req 12) ----

QString HardwareManager::selectedProfile() const
{
    return m_selectedProfile;
}

void HardwareManager::setSelectedProfile(const QString &profile)
{
    // Only accept canonical profile names; ignore anything unrecognized so the
    // scope always maps to a real per-profile settings block.
    if (!powerProfileFromName(profile).has_value())
        return;

    const QString canonical =
        powerProfileName(powerProfileFromName(profile).value());
    if (canonical == m_selectedProfile)
        return;

    m_selectedProfile = canonical;
    // Surface the newly selected profile's persisted RyzenAdj limits in the UI
    // (the fan-curve/threshold/battery bindings already re-read on this signal).
    loadRyzenAdjForProfile(canonical);
    emit selectedProfileChanged();
}

// ---- Per-profile persisted-settings accessors (Req 12) ----

QVariantList HardwareManager::fanCurveFor(const QString &profile,
                                          const QString &fan) const
{
    // "gpu" selects the GPU (pwm2) curve; anything else (default "cpu") selects
    // the CPU (pwm1) curve.
    const bool gpu = (fan.compare(QStringLiteral("gpu"), Qt::CaseInsensitive) == 0);

    const auto it = m_persistedSettings.constFind(profile);
    if (it != m_persistedSettings.constEnd())
        return fanCurveToVariantList(gpu ? it->gpuFanCurve : it->fanCurve);

    // Unknown / unloaded profile: fall back to that profile's defaults if the
    // name is a canonical one, otherwise return an empty list.
    if (const auto p = powerProfileFromName(profile)) {
        const ProfileSettings def = defaultSettingsFor(*p);
        return fanCurveToVariantList(gpu ? def.gpuFanCurve : def.fanCurve);
    }
    return QVariantList();
}

int HardwareManager::tempThresholdFor(const QString &profile) const
{
    const auto it = m_persistedSettings.constFind(profile);
    if (it != m_persistedSettings.constEnd())
        return it->tempThresholdC;

    if (const auto p = powerProfileFromName(profile))
        return defaultSettingsFor(*p).tempThresholdC;
    return 0;
}

QVariantMap HardwareManager::powerLimitsFor(const QString &profile) const
{
    QVariantMap result;

    const auto it = m_persistedSettings.constFind(profile);
    if (it != m_persistedSettings.constEnd()) {
        result.insert(QStringLiteral("pl1"), it->pl1Watts);
        result.insert(QStringLiteral("pl2"), it->pl2Watts);
        return result;
    }

    if (const auto p = powerProfileFromName(profile)) {
        const ProfileSettings def = defaultSettingsFor(*p);
        result.insert(QStringLiteral("pl1"), def.pl1Watts);
        result.insert(QStringLiteral("pl2"), def.pl2Watts);
    } else {
        result.insert(QStringLiteral("pl1"), 0);
        result.insert(QStringLiteral("pl2"), 0);
    }
    return result;
}

// ---- Persistence loading (Req 11.4, 11.5, 11.7) ----

void HardwareManager::loadPersistedSettings()
{
    m_persistedSettings.clear();

    QStringList fallbackProfiles;

    for (const auto &mapping : powerProfileMappings()) {
        const QString name = powerProfileName(mapping.profile);

        std::optional<ProfileSettings> loaded;
        if (m_settings)
            loaded = m_settings->load(name);

        if (loaded.has_value()) {
            m_persistedSettings.insert(name, loaded.value());
        } else {
            // Missing or unreadable block: apply defaults and note the fallback
            // so it can be reported without terminating (Req 11.5).
            m_persistedSettings.insert(name, defaultSettingsFor(mapping.profile));
            fallbackProfiles.append(name);
        }
    }

    QString message;
    if (fallbackProfiles.isEmpty()) {
        message = QStringLiteral("Persisted settings loaded for all profiles.");
    } else {
        message = QStringLiteral(
                      "Applied default settings for %1 profile(s) with missing or "
                      "unreadable stored settings: %2.")
                      .arg(fallbackProfiles.size())
                      .arg(fallbackProfiles.join(QStringLiteral(", ")));
    }

    emit persistedSettingsLoaded(message);
}

// ---- Built-in defaults ----

ProfileSettings HardwareManager::defaultSettingsFor(PowerProfile profile)
{
    ProfileSettings s;
    // A minimal valid two-point fan curve (2..8 points, strictly ascending
    // tempC, tempC in [0,105], percent in [0,100]) per profile.
    switch (profile) {
    case PowerProfile::Quiet:
        s.fanCurve = FanCurve{{40, 20}, {80, 60}};
        s.tempThresholdC = 85;
        s.pl1Watts = 15;
        s.pl2Watts = 25;
        break;
    case PowerProfile::Balanced:
        s.fanCurve = FanCurve{{40, 30}, {80, 75}};
        s.tempThresholdC = 90;
        s.pl1Watts = 28;
        s.pl2Watts = 45;
        break;
    case PowerProfile::Performance:
        s.fanCurve = FanCurve{{40, 40}, {80, 90}};
        s.tempThresholdC = 95;
        s.pl1Watts = 45;
        s.pl2Watts = 65;
        break;
    }
    // The GPU fan defaults mirror the CPU curve unless the operator diverges
    // them; keeping them equal by default preserves prior single-curve behavior.
    s.gpuFanCurve = s.fanCurve;
    return s;
}

QVariantList HardwareManager::fanCurveToVariantList(const FanCurve &curve)
{
    QVariantList list;
    for (const FanCurvePoint &p : curve) {
        QVariantMap point;
        point.insert(QStringLiteral("tempC"), p.tempC);
        point.insert(QStringLiteral("percent"), p.percent);
        list.append(point);
    }
    return list;
}

// ============================================================================
// Capability-model region (task 5.1)
// ============================================================================
//
// detectCapabilities() probes every control at runtime via the injected
// platform-access interfaces and records a Capability{ supported, reason } per
// control. A missing sysfs path, unwritable attribute, or unavailable D-Bus
// service maps to supported=false with a human-readable reason; nothing here
// throws or terminates the app (Req 3.1, 3.2, 3.3, 3.4). Because QML binds a
// control's enabled/visible state to capabilities[control].supported, an
// unsupported control never surfaces an enabled write action (Req 3.7).

namespace {
// Backing sysfs paths per the Hardware Dependency Map. Kept local so the probes
// have a single source of truth.
const QString kPlatformProfilePath =
    QStringLiteral("/sys/firmware/acpi/platform_profile");
const QString kPlatformProfileChoicesPath =
    QStringLiteral("/sys/firmware/acpi/platform_profile_choices");

// PL1/PL2 primary locations (newer kernels relocate these under asus-armoury).
const QString kPl1PathAsusNbWmi =
    QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl1_spl");
const QString kPl2PathAsusNbWmi =
    QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl2_sppt");
const QString kPl1PathAsusArmoury =
    QStringLiteral("/sys/devices/platform/asus-armoury/ppt_pl1_spl");
const QString kPl2PathAsusArmoury =
    QStringLiteral("/sys/devices/platform/asus-armoury/ppt_pl2_sppt");

// asusd fan-curve bounds discovery is not sysfs-backed here; PL bound discovery
// looks for optional *_min / *_max siblings the driver may expose.
const char *kPowerLimitMinSuffix = "_min";
const char *kPowerLimitMaxSuffix = "_max";

// D-Bus service names probed for GPU mode (supergfxctl) and profile fallback
// / fan curves (asusd).
const QString kSupergfxctlService = QStringLiteral("org.supergfxctl.Daemon");
const QString kAsusdService = QStringLiteral("org.asuslinux.Daemon");

// Kernel-native fan-curve control (asus-wmi "asus_custom_fan_curve" hwmon).
// This is the direct-sysfs backend used when the asusd D-Bus daemon is absent:
// the device exposes pwm{1,2}_auto_point{1..8}_{temp,pwm} attributes, where
// pwm1 is the CPU fan and pwm2 the GPU fan. hwmonN indices are unstable, so the
// device is resolved by its reported `name`, never by a fixed index.
const QString kFanCurveHwmonName = QStringLiteral("asus_custom_fan_curve");
// The "asus" hwmon exposes live fan RPM as fan1_input (CPU) / fan2_input (GPU).
const QString kFanRpmHwmonName = QStringLiteral("asus");
// The driver exposes a FIXED number of auto-points per fan.
constexpr int kFanCurveHwmonPoints = 8;
// Fan-curve PWM values are on a 0..255 scale (percent is converted to this).
constexpr int kFanCurvePwmMax = 255;

// Battery charge-limit control. The end-charge threshold (percent) is a single
// global sysfs node exposed by the ACPI battery driver; capacity/status are the
// standard power_supply attributes. Not per-profile.
const QString kBatterySupplyDir =
    QStringLiteral("/sys/class/power_supply/BAT0");
const QString kBatteryChargeLimitPath =
    QStringLiteral("/sys/class/power_supply/BAT0/charge_control_end_threshold");
const QString kBatteryCapacityPath =
    QStringLiteral("/sys/class/power_supply/BAT0/capacity");
const QString kBatteryStatusPath =
    QStringLiteral("/sys/class/power_supply/BAT0/status");
} // namespace

QVariantMap HardwareManager::capabilities() const
{
    QVariantMap map;
    for (const auto &entry : m_capabilities) {
        QVariantMap cap;
        cap.insert(QStringLiteral("supported"), entry.second.supported);
        cap.insert(QStringLiteral("reason"), entry.second.reason);
        map.insert(controlIdKey(entry.first), cap);
    }
    return map;
}

bool HardwareManager::tempThresholdIsHardwareLimit() const
{
    return m_tempThresholdIsHardwareLimit;
}

QVariantMap HardwareManager::powerLimitBounds() const
{
    QVariantMap bounds;
    bounds.insert(QStringLiteral("min"), m_powerLimitMinWatts);
    bounds.insert(QStringLiteral("max"), m_powerLimitMaxWatts);
    bounds.insert(QStringLiteral("estimated"), m_powerLimitBoundsEstimated);
    return bounds;
}

void HardwareManager::detectCapabilities()
{
    m_capabilities[ControlId::PowerProfile]         = detectPowerProfile();
    m_capabilities[ControlId::GpuMode]              = detectGpuMode();
    m_capabilities[ControlId::FanCurve]             = detectFanCurve();
    m_capabilities[ControlId::TemperatureRead]      = detectTemperatureRead();
    m_capabilities[ControlId::TemperatureThreshold] = detectTemperatureThreshold();
    m_capabilities[ControlId::PowerLimits]          = detectPowerLimits();
    m_capabilities[ControlId::BatteryChargeLimit]   = detectBatteryChargeLimit();
    m_capabilities[ControlId::RyzenAdj]             = detectRyzenAdj();

    emit capabilitiesChanged();
}

// ---- Individual control probes ----

Capability HardwareManager::detectPowerProfile() const
{
    // Primary: the ACPI platform_profile attribute. Fallback: the asusd D-Bus
    // service (Req 3, Hardware Dependency Map).
    if (m_sysfs && m_sysfs->exists(kPlatformProfilePath))
        return Capability{true, QString()};

    if (m_dbus && m_dbus->serviceAvailable(kAsusdService))
        return Capability{true, QString()};

    return Capability{false,
                      QStringLiteral("Power profile control not available "
                                     "(platform_profile missing and asusd unavailable)")};
}

Capability HardwareManager::detectGpuMode() const
{
    // supergfxctl D-Bus service must be available AND report a usable set of
    // supported modes (Req 5.1, 5.8). supergfxctl is being phased out, so this
    // must degrade cleanly.
    const bool serviceUp = m_dbus && m_dbus->serviceAvailable(kSupergfxctlService);
    if (!serviceUp) {
        return Capability{false,
                          QStringLiteral("supergfxctl D-Bus service not available")};
    }

    const std::optional<QStringList> modes =
        m_dbus ? m_dbus->supportedGpuModes() : std::nullopt;
    if (!modes.has_value() || modes->isEmpty()) {
        return Capability{false,
                          QStringLiteral("supergfxctl reported no supported GPU modes")};
    }

    return Capability{true, QString()};
}

Capability HardwareManager::detectFanCurve() const
{
    // Primary: asusd fan-curve support over D-Bus (Req 6.11).
    if (m_dbus && m_dbus->fanCurvesSupported())
        return Capability{true, QString()};

    // Fallback: the kernel-native asus_custom_fan_curve hwmon driver, which lets
    // us write the curve directly to sysfs without the asusd daemon. This is the
    // common case on ASUS laptops running a stock kernel.
    if (sysfsFanCurveAvailable())
        return Capability{true, QString()};

    // The absence message is the EXACT string required by Requirement 6.11.
    return Capability{false,
                      QStringLiteral("Fan curve control not supported by current driver")};
}

Capability HardwareManager::detectTemperatureRead() const
{
    // CPU and GPU sensors are probed separately so a GPU sensor can be missing
    // while CPU works. The aggregate TemperatureRead capability is supported if
    // at least the CPU sensor is readable; the reason notes any missing sensor.
    //
    // Sensors live under hwmon with UNSTABLE numbering (assigned in driver-probe
    // order, not stable across boots), so we resolve them dynamically by device
    // name + label via the same resolver the read path uses. A control shows
    // supported iff its sensor resolves.
    const bool cpuOk = cpuTempInputPath().has_value();
    const bool gpuOk = gpuTempInputPath().has_value();

    if (!cpuOk && !gpuOk) {
        return Capability{false,
                          QStringLiteral("No usable hwmon temperature sensor found")};
    }
    if (cpuOk && !gpuOk) {
        return Capability{true,
                          QStringLiteral("GPU temperature sensor unavailable")};
    }
    if (!cpuOk && gpuOk) {
        return Capability{true,
                          QStringLiteral("CPU temperature sensor unavailable")};
    }
    return Capability{true, QString()};
}

Capability HardwareManager::detectTemperatureThreshold()
{
    // The temperature threshold is ALWAYS available as a Boreas-side soft limit
    // (Req 7.6). The tempThresholdIsHardwareLimit flag reflects whether a real
    // driver throttle target exists (Req 7.5). No such target is guaranteed on
    // typical ASUS hardware (design Risk callout), so this defaults to a soft
    // limit unless a hardware throttle attribute is present.
    const QString throttlePath =
        QStringLiteral("/sys/devices/platform/asus-nb-wmi/throttle_thermal_policy");
    m_tempThresholdIsHardwareLimit =
        m_sysfs && m_sysfs->exists(throttlePath);

    return Capability{true, QString()};
}

Capability HardwareManager::detectPowerLimits()
{
    // PL1/PL2 require the attribute to exist AND be writable (Req 8.1). The path
    // may live under asus-nb-wmi (older) or asus-armoury (newer).
    auto resolvePl = [this](const QString &primary, const QString &alt)
        -> std::optional<QString> {
        if (m_sysfs && m_sysfs->exists(primary))
            return primary;
        if (m_sysfs && m_sysfs->exists(alt))
            return alt;
        return std::nullopt;
    };

    const std::optional<QString> pl1 =
        resolvePl(kPl1PathAsusNbWmi, kPl1PathAsusArmoury);
    const std::optional<QString> pl2 =
        resolvePl(kPl2PathAsusNbWmi, kPl2PathAsusArmoury);

    if (!pl1.has_value() || !pl2.has_value()) {
        // Reset to the conservative estimate so QML never surfaces stale bounds.
        m_powerLimitMinWatts = 1;
        m_powerLimitMaxWatts = 200;
        m_powerLimitBoundsEstimated = true;
        return Capability{false,
                          QStringLiteral("PL1/PL2 not exposed by current driver")};
    }

    const bool writable =
        m_sysfs && m_sysfs->isWritable(*pl1) && m_sysfs->isWritable(*pl2);
    if (!writable) {
        m_powerLimitMinWatts = 1;
        m_powerLimitMaxWatts = 200;
        m_powerLimitBoundsEstimated = true;
        return Capability{false,
                          QStringLiteral("PL1/PL2 not writable by current driver")};
    }

    // Attempt to discover real bounds from optional sibling *_min / *_max
    // attributes; otherwise fall back to the conservative estimate 1..200 W and
    // flag it estimated (Req 8.4, 8.5).
    const QString pl1Min = *pl1 + QString::fromLatin1(kPowerLimitMinSuffix);
    const QString pl1Max = *pl1 + QString::fromLatin1(kPowerLimitMaxSuffix);

    bool discovered = false;
    if (m_sysfs && m_sysfs->exists(pl1Min) && m_sysfs->exists(pl1Max)) {
        const std::optional<QString> minStr = m_sysfs->read(pl1Min);
        const std::optional<QString> maxStr = m_sysfs->read(pl1Max);
        if (minStr && maxStr) {
            bool minOk = false;
            bool maxOk = false;
            const int minVal = minStr->trimmed().toInt(&minOk);
            const int maxVal = maxStr->trimmed().toInt(&maxOk);
            if (minOk && maxOk && minVal < maxVal) {
                m_powerLimitMinWatts = minVal;
                m_powerLimitMaxWatts = maxVal;
                m_powerLimitBoundsEstimated = false;
                discovered = true;
            }
        }
    }

    if (!discovered) {
        m_powerLimitMinWatts = 1;
        m_powerLimitMaxWatts = 200;
        m_powerLimitBoundsEstimated = true;
    }

    return Capability{true, QString()};
}

Capability HardwareManager::detectBatteryChargeLimit()
{
    // The end-charge threshold node must exist AND be writable to offer the
    // control. capacity/status are read opportunistically for the readout even
    // when the limit is not writable. Refresh the cached values as a side effect
    // so the properties reflect the live state right after detection.
    refreshBattery();

    if (!m_sysfs || !m_sysfs->exists(kBatteryChargeLimitPath)) {
        return Capability{false,
                          QStringLiteral("Battery charge limit not exposed by "
                                         "current driver")};
    }
    if (!m_sysfs->isWritable(kBatteryChargeLimitPath)) {
        return Capability{false,
                          QStringLiteral("Battery charge limit not writable "
                                         "(root privileges required)")};
    }
    return Capability{true, QString()};
}

Capability HardwareManager::detectRyzenAdj()
{
    // Optional/experimental backend. Available only when the ryzenadj binary is
    // present; otherwise reported unavailable with an actionable reason. Reading
    // the current values as a side effect primes the UI display.
    if (!m_ryzenAdj || !m_ryzenAdj->available()) {
        return Capability{false,
                          QStringLiteral("RyzenAdj not installed (advanced AMD "
                                         "power tuning unavailable)")};
    }
    refreshRyzenAdj();
    return Capability{true, QString()};
}

// ============================================================================
// Status log ring buffer & apply-result helper (task 7.1)
// ============================================================================
//
// recordResult() is the single choke point every write path funnels through so
// that each Write_Operation produces exactly one applyResult(...) signal and
// exactly one LogEntry (Req 2.5, 13.3). The log is a ring buffer capped at
// kMaxLogEntries (10): when a new entry overflows the cap the oldest entry is
// dropped, so recentLog always exposes the 10 most-recent actions, oldest first
// (Req 13.4, 13.5).

QVariantList HardwareManager::recentLog() const
{
    QVariantList list;
    for (const LogEntry &e : m_recentLog) {
        QVariantMap entry;
        entry.insert(QStringLiteral("timestamp"), e.timestamp);
        entry.insert(QStringLiteral("action"), e.action);
        entry.insert(QStringLiteral("success"), e.success);
        entry.insert(QStringLiteral("message"), e.message);
        list.append(entry);
    }
    return list;
}

void HardwareManager::recordResult(const QString &action, bool success,
                                   const QString &message)
{
    // Report the outcome to QML first (Req 2.5). Every write path relies on this
    // helper emitting applyResult exactly once per call.
    emit applyResult(success, message);

    // Append exactly one entry stamped with the current ISO-8601 local time
    // (Req 13.3).
    LogEntry entry;
    entry.timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    entry.action = action;
    entry.success = success;
    entry.message = message;
    m_recentLog.append(entry);

    // Trim to the 10 most recent, dropping the oldest overflow (Req 13.4, 13.5).
    while (m_recentLog.size() > kMaxLogEntries)
        m_recentLog.removeFirst();

    emit recentLogChanged();
}

// ============================================================================
// Battery charge-limit controller
// ============================================================================
//
// The end-charge threshold is a single global sysfs node
// (/sys/class/power_supply/BAT0/charge_control_end_threshold), not a per-profile
// value, so it is NOT part of ProfileSettings and is not persisted by Boreas;
// the kernel retains the last written value. capacity/status are read-only
// telemetry. Every write funnels through recordResult() exactly once (Req 2.5,
// 13.3) and reports the KERNEL read-back value, never the merely requested one.

int HardwareManager::chargeLimit() const
{
    return m_chargeLimit;
}

int HardwareManager::batteryCapacity() const
{
    return m_batteryCapacity;
}

QString HardwareManager::batteryStatus() const
{
    return m_batteryStatus;
}

void HardwareManager::refreshBattery()
{
    int newLimit = -1;
    int newCapacity = -1;
    QString newStatus;

    if (m_sysfs && m_sysfs->exists(kBatteryChargeLimitPath)) {
        if (const std::optional<QString> raw = m_sysfs->read(kBatteryChargeLimitPath)) {
            bool ok = false;
            const int v = raw->trimmed().toInt(&ok);
            if (ok)
                newLimit = v;
        }
    }
    if (m_sysfs && m_sysfs->exists(kBatteryCapacityPath)) {
        if (const std::optional<QString> raw = m_sysfs->read(kBatteryCapacityPath)) {
            bool ok = false;
            const int v = raw->trimmed().toInt(&ok);
            if (ok)
                newCapacity = v;
        }
    }
    if (m_sysfs && m_sysfs->exists(kBatteryStatusPath)) {
        if (const std::optional<QString> raw = m_sysfs->read(kBatteryStatusPath))
            newStatus = raw->trimmed();
    }

    const bool changed = (newLimit != m_chargeLimit)
        || (newCapacity != m_batteryCapacity)
        || (newStatus != m_batteryStatus);

    m_chargeLimit = newLimit;
    m_batteryCapacity = newCapacity;
    m_batteryStatus = newStatus;

    if (changed)
        emit batteryStateChanged();
}

void HardwareManager::applyChargeLimit(int limitPercent)
{
    const QString action = QStringLiteral("Apply battery charge limit");

    // Refuse when the control is unavailable/unwritable; report without writing.
    if (!m_sysfs || !m_sysfs->exists(kBatteryChargeLimitPath)) {
        recordResult(action, false,
                     QStringLiteral("Battery charge limit not exposed by current "
                                    "driver; not applied."));
        return;
    }
    if (!m_sysfs->isWritable(kBatteryChargeLimitPath)) {
        recordResult(action, false,
                     QStringLiteral("Battery charge limit is not writable (root "
                                    "privileges required); not applied."));
        return;
    }

    // Clamp to the safe [kMinChargeLimit, 100] range. A clamped value is noted
    // in the result so the operator sees what was actually written.
    int clamped = limitPercent;
    if (clamped < kMinChargeLimit)
        clamped = kMinChargeLimit;
    else if (clamped > 100)
        clamped = 100;

    const WriteOutcome outcome =
        m_sysfs->write(kBatteryChargeLimitPath, QString::number(clamped),
                       kChargeLimitWriteTimeoutMs);
    if (outcome.status != WriteOutcome::Status::Ok) {
        QString detail;
        switch (outcome.status) {
        case WriteOutcome::Status::PermissionDenied:
            detail = QStringLiteral("permission denied (root privileges required)");
            break;
        case WriteOutcome::Status::Timeout:
            detail = QStringLiteral("the write timed out");
            break;
        case WriteOutcome::Status::Unsupported:
            detail = QStringLiteral("not supported by current driver");
            break;
        case WriteOutcome::Status::IoError:
        default:
            detail = outcome.message.isEmpty()
                ? QStringLiteral("driver reported an I/O error")
                : outcome.message;
            break;
        }
        recordResult(action, false,
                     QStringLiteral("Failed to set battery charge limit: %1.")
                         .arg(detail));
        return;
    }

    // Read back so the reported/displayed limit is the kernel's value, never the
    // merely requested one; refreshBattery() updates m_chargeLimit + emits the
    // property change.
    refreshBattery();
    const int readBack = m_chargeLimit;

    if (readBack < 0) {
        recordResult(action, false,
                     QStringLiteral("Battery charge limit written but read-back "
                                    "failed; could not confirm the applied value."));
        return;
    }

    QString message;
    if (readBack != limitPercent) {
        message = QStringLiteral("Battery charge limit set to %1%% (requested "
                                 "%2%%, constrained to %3-100%%).")
                      .arg(readBack)
                      .arg(limitPercent)
                      .arg(kMinChargeLimit);
    } else {
        message = QStringLiteral("Battery charge limit set to %1%%.")
                      .arg(readBack);
    }

    // Persist the confirmed limit globally so it is remembered across restarts
    // and re-applied on startup (the kernel also retains it, but persisting lets
    // Boreas reassert it after a firmware/boot reset).
    if (m_settings)
        m_settings->saveChargeLimit(readBack);

    recordResult(action, true, message);
}

// ============================================================================
// Advanced power tuning (RyzenAdj) — optional/experimental
// ============================================================================
//
// RyzenAdj tunes volatile AMD SMU limits (STAPM / fast / slow / APU-slow PPT and
// the Tctl thermal limit) that the in-kernel ppt_* sysfs nodes do not expose.
// Because the SMU resets these limits periodically and after suspend/resume, an
// optional re-apply timer re-sends the last-applied set on an interval so the
// tuning stays in effect. The write path funnels through recordResult() exactly
// once; the timer-driven re-apply is silent (no log spam).

QVariantMap HardwareManager::ryzenAdjValues() const
{
    QVariantMap m;
    m.insert(QStringLiteral("stapmW"), m_ryzenAdjValues.stapmLimitW);
    m.insert(QStringLiteral("fastW"), m_ryzenAdjValues.fastLimitW);
    m.insert(QStringLiteral("slowW"), m_ryzenAdjValues.slowLimitW);
    m.insert(QStringLiteral("apuSlowW"), m_ryzenAdjValues.apuSlowLimitW);
    m.insert(QStringLiteral("tctlTempC"), m_ryzenAdjValues.tctlTempC);
    return m;
}

bool HardwareManager::ryzenAdjAutoReapply() const
{
    return m_ryzenAdjReapplyTimer && m_ryzenAdjReapplyTimer->isActive();
}

void HardwareManager::setRyzenAdjAutoReapply(bool enabled)
{
    if (enabled) {
        if (!m_ryzenAdj || !m_ryzenAdj->available())
            return;  // nothing to re-apply against
        if (!m_ryzenAdjReapplyTimer) {
            m_ryzenAdjReapplyTimer = new QTimer(this);
            m_ryzenAdjReapplyTimer->setInterval(kRyzenAdjReapplyIntervalMs);
            connect(m_ryzenAdjReapplyTimer, &QTimer::timeout, this,
                    &HardwareManager::reapplyRyzenAdj);
        }
        if (!m_ryzenAdjReapplyTimer->isActive())
            m_ryzenAdjReapplyTimer->start();
    } else if (m_ryzenAdjReapplyTimer) {
        m_ryzenAdjReapplyTimer->stop();
    }
    emit ryzenAdjChanged();
}

void HardwareManager::refreshRyzenAdj()
{
    if (!m_ryzenAdj || !m_ryzenAdj->available())
        return;
    const std::optional<RyzenAdjSettings> info = m_ryzenAdj->readInfo();
    if (!info.has_value())
        return;
    m_ryzenAdjValues = *info;
    emit ryzenAdjChanged();
}

void HardwareManager::reapplyRyzenAdj()
{
    // Silent re-apply of the last-applied set (SMU limits are volatile). No log
    // entry so the timer does not flood the status log; a failure is ignored
    // here because the next tick will retry and the operator sees live values
    // via refreshRyzenAdj().
    if (!m_ryzenAdj || !m_ryzenAdj->available())
        return;
    if (!m_ryzenAdjLastApplied.hasAny())
        return;
    m_ryzenAdj->apply(m_ryzenAdjLastApplied);
}

void HardwareManager::loadRyzenAdjForProfile(const QString &profile)
{
    const auto canonical = powerProfileFromName(profile);
    if (!canonical.has_value())
        return;
    const QString name = powerProfileName(*canonical);

    ProfileSettings settings;
    const auto it = m_persistedSettings.constFind(name);
    settings = (it != m_persistedSettings.constEnd())
        ? it.value() : defaultSettingsFor(*canonical);

    RyzenAdjSettings r;
    r.stapmLimitW = settings.ryzenStapmW;
    r.fastLimitW = settings.ryzenFastW;
    r.slowLimitW = settings.ryzenSlowW;
    r.apuSlowLimitW = settings.ryzenApuSlowW;
    r.tctlTempC = settings.ryzenTctlC;

    m_ryzenAdjLastApplied = r;
    // Reflect the stored (desired) values in the UI even before a hardware
    // read-back; a subsequent refreshRyzenAdj() may overwrite with live values.
    m_ryzenAdjValues = r;
    emit ryzenAdjChanged();
}

void HardwareManager::reapplyPersistedOnStartup()
{
    // 1) RyzenAdj: load the selected profile's saved limits and, if the backend
    //    is available and there is something to set, silently re-apply them and
    //    start the periodic re-apply loop (SMU limits are volatile).
    loadRyzenAdjForProfile(m_selectedProfile);
    if (m_ryzenAdj && m_ryzenAdj->available() && m_ryzenAdjLastApplied.hasAny()) {
        m_ryzenAdj->apply(m_ryzenAdjLastApplied);
        refreshRyzenAdj();
        setRyzenAdjAutoReapply(true);
    }

    // 2) Battery: re-apply the persisted global charge limit if the control is
    //    writable. Best-effort and silent; the kernel usually retains it, but
    //    reasserting covers a firmware/boot reset.
    if (m_settings && m_sysfs
        && m_sysfs->exists(kBatteryChargeLimitPath)
        && m_sysfs->isWritable(kBatteryChargeLimitPath)) {
        const std::optional<int> saved = m_settings->loadChargeLimit();
        if (saved.has_value()) {
            int v = *saved;
            if (v < kMinChargeLimit) v = kMinChargeLimit;
            else if (v > 100) v = 100;
            m_sysfs->write(kBatteryChargeLimitPath, QString::number(v),
                           kChargeLimitWriteTimeoutMs);
        }
    }
    refreshBattery();
}

void HardwareManager::applyRyzenAdj(int stapmW, int fastW, int slowW,
                                    int apuSlowW, int tctlTempC)
{
    const QString action = QStringLiteral("Apply advanced power (RyzenAdj)");

    if (!m_ryzenAdj || !m_ryzenAdj->available()) {
        recordResult(action, false,
                     QStringLiteral("RyzenAdj is not installed; advanced power "
                                    "tuning not applied."));
        return;
    }

    RyzenAdjSettings req;
    req.stapmLimitW = stapmW;
    req.fastLimitW = fastW;
    req.slowLimitW = slowW;
    req.apuSlowLimitW = apuSlowW;
    req.tctlTempC = tctlTempC;

    if (!req.hasAny()) {
        recordResult(action, false,
                     QStringLiteral("No advanced power values set; nothing to "
                                    "apply."));
        return;
    }

    const WriteOutcome outcome = m_ryzenAdj->apply(req);
    if (outcome.status != WriteOutcome::Status::Ok) {
        QString detail;
        switch (outcome.status) {
        case WriteOutcome::Status::PermissionDenied:
            detail = outcome.message.isEmpty()
                ? QStringLiteral("root privileges required")
                : outcome.message;
            break;
        case WriteOutcome::Status::Timeout:
            detail = QStringLiteral("ryzenadj timed out");
            break;
        case WriteOutcome::Status::Unsupported:
            detail = outcome.message.isEmpty()
                ? QStringLiteral("not supported")
                : outcome.message;
            break;
        case WriteOutcome::Status::IoError:
        default:
            detail = outcome.message.isEmpty()
                ? QStringLiteral("ryzenadj reported an error")
                : outcome.message;
            break;
        }
        recordResult(action, false,
                     QStringLiteral("Failed to apply advanced power limits: %1.")
                         .arg(detail));
        return;
    }

    // Remember the applied set for the re-apply loop, then read back so the
    // displayed values reflect what the SMU actually holds (RyzenAdj may clamp).
    m_ryzenAdjLastApplied = req;
    refreshRyzenAdj();

    // Persist the applied limits into the CURRENTLY SELECTED profile so they are
    // remembered across restarts and can be auto-reapplied on startup.
    if (const auto canonical = powerProfileFromName(m_selectedProfile)) {
        const QString name = powerProfileName(*canonical);
        ProfileSettings settings;
        const auto it = m_persistedSettings.find(name);
        settings = (it != m_persistedSettings.end())
            ? it.value() : defaultSettingsFor(*canonical);
        settings.ryzenStapmW = req.stapmLimitW;
        settings.ryzenFastW = req.fastLimitW;
        settings.ryzenSlowW = req.slowLimitW;
        settings.ryzenApuSlowW = req.apuSlowLimitW;
        settings.ryzenTctlC = req.tctlTempC;
        m_persistedSettings.insert(name, settings);
        if (m_settings)
            m_settings->save(name, settings);
    }

    recordResult(action, true,
                 QStringLiteral("Advanced power limits applied and saved via "
                                "RyzenAdj."));
}

// ============================================================================
// Telemetry: active profile & GPU mode (task 6.1)
// ============================================================================
//
// These are read-only telemetry paths (no writes). refreshActiveProfile() reads
// the ACPI platform_profile attribute via ISysfsAccess; refreshGpuMode() reads
// the supported set + active mode via IDBusAccess (supergfxctl). Every failure
// is surfaced through readFailed(controlId, message) and never terminates the
// app (Req 3.4, 3.5); on failure the last-known value is retained so QML never
// shows a spurious change (Req 4.2, 5.4-5.8).

QString HardwareManager::activePowerProfile() const
{
    return m_activePowerProfile;
}

QString HardwareManager::activeGpuMode() const
{
    return m_activeGpuMode;
}

QStringList HardwareManager::supportedGpuModes() const
{
    return m_supportedGpuModes;
}

void HardwareManager::refreshActiveProfile()
{
    // Missing accessor or sysfs path/service -> report without terminating
    // (Req 3.4, 3.5). The canonical control id matches ControlId::PowerProfile.
    const QString controlId = controlIdKey(ControlId::PowerProfile);

    if (!m_sysfs || !m_sysfs->exists(kPlatformProfilePath)) {
        emit readFailed(controlId,
                        QStringLiteral("Active power profile read failed: "
                                       "platform_profile attribute not available"));
        return;
    }

    const std::optional<QString> raw = m_sysfs->read(kPlatformProfilePath);
    if (!raw.has_value()) {
        emit readFailed(controlId,
                        QStringLiteral("Active power profile read failed: "
                                       "could not read platform_profile"));
        return;
    }

    // Translate whatever platform string the attribute reports (any accepted
    // alias) into Boreas's canonical profile name (Req 4.2). An unrecognized
    // value is a read failure rather than a silent mismatch.
    const std::optional<PowerProfile> profile =
        powerProfileFromPlatformString(raw->trimmed());
    if (!profile.has_value()) {
        emit readFailed(controlId,
                        QStringLiteral("Active power profile read failed: "
                                       "unrecognized platform_profile value \"%1\"")
                            .arg(raw->trimmed()));
        return;
    }

    const QString canonical = powerProfileName(*profile);
    if (canonical != m_activePowerProfile) {
        m_activePowerProfile = canonical;
        emit activePowerProfileChanged();
    }
}

void HardwareManager::refreshGpuMode()
{
    // GPU mode telemetry is backed by the supergfxctl D-Bus service. If the
    // service is unavailable or either read fails, surface readFailed and retain
    // the last-known values (Req 5.1, 5.4, 5.6, 5.7, 5.8, 3.4, 3.5).
    const QString controlId = controlIdKey(ControlId::GpuMode);

    if (!m_dbus || !m_dbus->serviceAvailable(kSupergfxctlService)) {
        emit readFailed(controlId,
                        QStringLiteral("GPU mode read failed: "
                                       "supergfxctl D-Bus service not available"));
        return;
    }

    // Supported set first so the UI only ever offers confirmed modes
    // (Req 5.1-5.3).
    const std::optional<QStringList> modes = m_dbus->supportedGpuModes();
    if (!modes.has_value() || modes->isEmpty()) {
        emit readFailed(controlId,
                        QStringLiteral("GPU mode read failed: "
                                       "supergfxctl reported no supported GPU modes"));
        return;
    }

    if (*modes != m_supportedGpuModes) {
        m_supportedGpuModes = *modes;
        emit supportedGpuModesChanged();
    }

    // Active mode (Req 5.4, 5.5).
    const std::optional<QString> active = m_dbus->activeGpuMode();
    if (!active.has_value()) {
        emit readFailed(controlId,
                        QStringLiteral("GPU mode read failed: "
                                       "could not read active GPU mode"));
        return;
    }

    if (*active != m_activeGpuMode) {
        m_activeGpuMode = *active;
        emit activeGpuModeChanged();
    }
}

// ============================================================================
// Telemetry: temperature polling (task 6.2)
// ============================================================================
//
// refreshTemperatures() performs one read-only poll of the CPU/GPU hwmon
// temperature inputs (values are millidegrees Celsius) and converts them to
// degrees Celsius (Req 7.1, 7.2). A missing path, unreadable attribute, or
// unparseable value yields the NaN sentinel for that sensor (rendered as "—" in
// QML rather than a stale value) and emits readFailed(controlId, message) so a
// banner can surface within ~2 s (Req 3.4, 3.5). setTemperaturePollingActive()
// drives a QTimer at an interval in [1000, 2000] ms while the Limits tab is
// visible (Req 7.3). Threshold-crossing detection emits
// temperatureThresholdCrossed() only on the transition INTO the crossing state
// against the selected profile's threshold (Req 7.8).

// hwmon device name / label priority lists used to resolve the CPU/GPU sensors
// dynamically (see the bug note in HardwareManager.h). CPU is typically
// "k10temp" (AMD, label "Tctl") or "coretemp" (Intel, label "Package id 0");
// GPU is "amdgpu" (label "edge") or "nvidia" (label "junction"/"edge"). Because
// hwmon indices are unstable across boots, we NEVER hardcode hwmonN here.
const QStringList HardwareManager::kCpuHwmonNames =
    {QStringLiteral("k10temp"), QStringLiteral("coretemp")};
const QStringList HardwareManager::kCpuHwmonLabels =
    {QStringLiteral("Tctl"), QStringLiteral("Package id 0"),
     QStringLiteral("Tccd1")};
const QStringList HardwareManager::kGpuHwmonNames =
    {QStringLiteral("amdgpu"), QStringLiteral("nvidia")};
const QStringList HardwareManager::kGpuHwmonLabels =
    {QStringLiteral("edge"), QStringLiteral("junction")};

std::optional<QString> HardwareManager::cpuTempInputPath() const
{
    // Return the cached path only while it still exists; a hwmon renumber (e.g.
    // across a suspend/resume driver reload) invalidates the cache and forces a
    // fresh resolution (design: "resolve once, re-resolve if the cached path
    // stops existing").
    if (!m_cpuTempInputPath.isEmpty() && m_sysfs
        && m_sysfs->exists(m_cpuTempInputPath))
        return m_cpuTempInputPath;

    m_cpuTempInputPath.clear();
    if (!m_sysfs)
        return std::nullopt;

    const std::optional<QString> resolved =
        m_sysfs->resolveHwmonInput(kCpuHwmonNames, kCpuHwmonLabels);
    if (resolved.has_value())
        m_cpuTempInputPath = *resolved;
    qDebug() << "[Boreas][TEMP-DEBUG] resolved CPU temp input:"
             << (resolved.has_value() ? *resolved : QStringLiteral("<none>"));
    return resolved;
}

std::optional<QString> HardwareManager::gpuTempInputPath() const
{
    if (!m_gpuTempInputPath.isEmpty() && m_sysfs
        && m_sysfs->exists(m_gpuTempInputPath))
        return m_gpuTempInputPath;

    m_gpuTempInputPath.clear();
    if (!m_sysfs)
        return std::nullopt;

    const std::optional<QString> resolved =
        m_sysfs->resolveHwmonInput(kGpuHwmonNames, kGpuHwmonLabels);
    if (resolved.has_value())
        m_gpuTempInputPath = *resolved;
    qDebug() << "[Boreas][TEMP-DEBUG] resolved GPU temp input:"
             << (resolved.has_value() ? *resolved : QStringLiteral("<none>"));
    return resolved;
}

double HardwareManager::cpuTemperature() const
{
    return m_cpuTemperature;
}

double HardwareManager::gpuTemperature() const
{
    return m_gpuTemperature;
}

int HardwareManager::cpuFanRpm() const
{
    return m_cpuFanRpm;
}

int HardwareManager::gpuFanRpm() const
{
    return m_gpuFanRpm;
}

QVariantMap HardwareManager::fanRpmRange() const
{
    QVariantMap m;
    m.insert(QStringLiteral("cpuMin"), m_cpuFanRpmMin);
    m.insert(QStringLiteral("cpuMax"), m_cpuFanRpmMax);
    m.insert(QStringLiteral("gpuMin"), m_gpuFanRpmMin);
    m.insert(QStringLiteral("gpuMax"), m_gpuFanRpmMax);
    return m;
}

std::optional<QString> HardwareManager::resolveFanRpmHwmonDir() const
{
    if (!m_fanHwmonDir.isEmpty() && m_sysfs
        && m_sysfs->exists(m_fanHwmonDir + QStringLiteral("/fan1_input")))
        return m_fanHwmonDir;

    m_fanHwmonDir.clear();
    if (!m_sysfs)
        return std::nullopt;
    const std::optional<QString> dir =
        m_sysfs->resolveHwmonDir(QStringList{kFanRpmHwmonName});
    if (dir.has_value()
        && m_sysfs->exists(*dir + QStringLiteral("/fan1_input")))
        m_fanHwmonDir = *dir;
    return m_fanHwmonDir.isEmpty() ? std::nullopt
                                   : std::optional<QString>(m_fanHwmonDir);
}

int HardwareManager::readFanRpm(int fanIndex) const
{
    const std::optional<QString> dir = resolveFanRpmHwmonDir();
    if (!dir.has_value() || !m_sysfs)
        return -1;
    const QString path =
        *dir + QStringLiteral("/fan%1_input").arg(fanIndex);
    if (!m_sysfs->exists(path))
        return -1;
    const std::optional<QString> raw = m_sysfs->read(path);
    if (!raw.has_value())
        return -1;
    bool ok = false;
    const int rpm = raw->trimmed().toInt(&ok);
    // Treat 0 as a failed/transient EC read — a running laptop fan never truly
    // stops, so 0 is always a stale register snapshot rather than a real speed.
    if (!ok || rpm <= 0)
        return -1;
    return rpm;
}

void HardwareManager::refreshFanRpm()
{
    const int cpu = readFanRpm(1);
    const int gpu = readFanRpm(2);
    if (cpu != m_cpuFanRpm || gpu != m_gpuFanRpm) {
        m_cpuFanRpm = cpu;
        m_gpuFanRpm = gpu;
        emit fanRpmChanged();
    }
}

double HardwareManager::readTemperatureC(const QString &path,
                                         const QString &sensorLabel)
{
    // Missing path or no sysfs accessor -> unavailable sentinel + read failure.
    if (!m_sysfs || !m_sysfs->exists(path)) {
        emit readFailed(controlIdKey(ControlId::TemperatureRead),
                        QStringLiteral("%1 temperature sensor unavailable")
                            .arg(sensorLabel));
        return std::numeric_limits<double>::quiet_NaN();
    }

    const std::optional<QString> raw = m_sysfs->read(path);
    if (!raw.has_value()) {
        emit readFailed(controlIdKey(ControlId::TemperatureRead),
                        QStringLiteral("Failed to read %1 temperature")
                            .arg(sensorLabel));
        return std::numeric_limits<double>::quiet_NaN();
    }

    bool ok = false;
    const long long milliC = raw->trimmed().toLongLong(&ok);
    if (!ok) {
        emit readFailed(controlIdKey(ControlId::TemperatureRead),
                        QStringLiteral("Unparseable %1 temperature reading")
                            .arg(sensorLabel));
        return std::numeric_limits<double>::quiet_NaN();
    }

    // hwmon tempN_input is expressed in millidegrees Celsius.
    return static_cast<double>(milliC) / 1000.0;
}

double HardwareManager::readResolvedTemperatureC(
    const std::optional<QString> &resolvedPath, const QString &sensorLabel)
{
    // A sensor that failed to resolve is treated exactly like a missing path:
    // emit the "sensor unavailable" read failure and return the NaN sentinel
    // (Req 3.4, 3.5, 7.1, 7.2).
    if (!resolvedPath.has_value()) {
        emit readFailed(controlIdKey(ControlId::TemperatureRead),
                        QStringLiteral("%1 temperature sensor unavailable")
                            .arg(sensorLabel));
        return std::numeric_limits<double>::quiet_NaN();
    }
    return readTemperatureC(*resolvedPath, sensorLabel);
}

void HardwareManager::evaluateThresholdCrossing(const QString &sensor,
                                                double valueC, int thresholdC,
                                                bool &prevAbove)
{
    // A NaN reading is "unknown": clear the crossing state without emitting so a
    // later valid reading can trigger a fresh transition (Req 7.8).
    if (std::isnan(valueC)) {
        prevAbove = false;
        return;
    }

    const bool nowAbove = valueC >= static_cast<double>(thresholdC);
    if (nowAbove && !prevAbove) {
        // Fresh transition into the crossing state: emit once with a timestamp
        // (Req 7.8). The status-log entry itself is plumbed via task 7.1.
        emit temperatureThresholdCrossed(
            sensor, valueC, QDateTime::currentDateTime().toString(Qt::ISODate));
    }
    prevAbove = nowAbove;
}

void HardwareManager::refreshTemperatures()
{
    // Resolve the sensor paths dynamically each poll (cached; re-resolved only
    // if a cached path vanished). A sensor that does not resolve is reported as
    // unavailable and yields the NaN sentinel, mirroring a missing-path read.
    const double cpu = readResolvedTemperatureC(cpuTempInputPath(),
                                                QStringLiteral("CPU"));
    const double gpu = readResolvedTemperatureC(gpuTempInputPath(),
                                                QStringLiteral("GPU"));

    // Only notify QML when a value actually changes. NaN != NaN, so a stayed-NaN
    // reading does not spuriously re-notify.
    if (!(cpu == m_cpuTemperature) && !(std::isnan(cpu) && std::isnan(m_cpuTemperature))) {
        m_cpuTemperature = cpu;
        emit cpuTemperatureChanged();
    }
    if (!(gpu == m_gpuTemperature) && !(std::isnan(gpu) && std::isnan(m_gpuTemperature))) {
        m_gpuTemperature = gpu;
        emit gpuTemperatureChanged();
    }

    // Threshold-crossing detection against the selected profile's persisted
    // threshold (Req 7.8).
    const int thresholdC = tempThresholdFor(m_selectedProfile);
    evaluateThresholdCrossing(QStringLiteral("CPU"), m_cpuTemperature, thresholdC,
                              m_cpuAboveThreshold);
    evaluateThresholdCrossing(QStringLiteral("GPU"), m_gpuTemperature, thresholdC,
                              m_gpuAboveThreshold);

    // Live fan RPM is polled alongside temperatures so the editor readout stays
    // current while the tab is visible.
    refreshFanRpm();
}

void HardwareManager::setTemperaturePollingActive(bool active)
{
    if (active) {
        if (!m_tempPollTimer) {
            m_tempPollTimer = new QTimer(this);
            // Interval kept within the required [1000, 2000] ms range (Req 7.3).
            m_tempPollTimer->setInterval(kTempPollIntervalMs);
            connect(m_tempPollTimer, &QTimer::timeout, this,
                    &HardwareManager::refreshTemperatures);
        }
        if (!m_tempPollTimer->isActive())
            m_tempPollTimer->start();
        // Refresh immediately so the readout is not blank for up to one interval.
        refreshTemperatures();
    } else if (m_tempPollTimer) {
        m_tempPollTimer->stop();
    }
}

// ============================================================================
// Write path: temperature threshold (task 10.4)
// ============================================================================
//
// applyTemperatureThreshold() persists a per-profile temperature warning
// threshold. Unlike the other write paths this is soft-limit metadata only
// (Req 7.6, design Risk callout): it issues NO hardware write. The flow is:
//   1. Resolve the profile name to a canonical profile; an unknown name is a
//      failure reported via recordResult() without persisting.
//   2. Clamp the requested threshold to the allowed range [0, 105] (Req 7.4).
//   3. Update the in-memory per-profile settings (kept distinct from live
//      hardware values, Req 11.6) so subsequent reads via tempThresholdFor()
//      reflect the change, and threshold-crossing detection uses the new value.
//   4. Persist through ISettingsStore::save scoped to that profile (Req 11.2,
//      12.4).
//   5. Report the outcome via recordResult() so exactly one applyResult(...) is
//      emitted and exactly one LogEntry is appended (Req 2.5, 13.3).

void HardwareManager::applyTemperatureThreshold(const QString &profile,
                                                int thresholdC)
{
    // Resolve to a canonical profile name so persistence is always keyed
    // consistently with loadPersistedSettings() / the per-profile accessors.
    const std::optional<PowerProfile> canonicalProfile =
        powerProfileFromName(profile);
    if (!canonicalProfile.has_value()) {
        recordResult(QStringLiteral("Apply temperature threshold (%1)").arg(profile),
                     false,
                     QStringLiteral("Unknown power profile \"%1\"; threshold not "
                                    "persisted.")
                         .arg(profile));
        return;
    }

    const QString name = powerProfileName(*canonicalProfile);
    const QString action =
        QStringLiteral("Apply temperature threshold (%1)").arg(name);

    // Constrain to the allowed range [0, 105] (Req 7.4). A clamped value is
    // noted in the result message so the operator sees what was actually
    // persisted.
    int clamped = thresholdC;
    if (clamped < 0)
        clamped = 0;
    else if (clamped > 105)
        clamped = 105;

    // Update the in-memory per-profile settings first so reads reflect the
    // change even if this profile had only defaults so far. Missing blocks fall
    // back to that profile's defaults before applying the new threshold.
    ProfileSettings settings;
    const auto it = m_persistedSettings.find(name);
    if (it != m_persistedSettings.end())
        settings = it.value();
    else
        settings = defaultSettingsFor(*canonicalProfile);

    settings.tempThresholdC = clamped;
    m_persistedSettings.insert(name, settings);

    // Persist the updated block (soft-limit metadata only; no hardware write).
    const bool saved = m_settings && m_settings->save(name, settings);

    if (!saved) {
        recordResult(action, false,
                     QStringLiteral("Failed to persist temperature threshold "
                                    "(%1 \u00B0C) for %2.")
                         .arg(clamped)
                         .arg(name));
        return;
    }

    QString message;
    if (clamped != thresholdC) {
        message = QStringLiteral("Temperature threshold set to %1 \u00B0C for %2 "
                                 "(requested %3 \u00B0C, constrained to 0-105).")
                      .arg(clamped)
                      .arg(name)
                      .arg(thresholdC);
    } else {
        message = QStringLiteral("Temperature threshold set to %1 \u00B0C for %2.")
                      .arg(clamped)
                      .arg(name);
    }

    recordResult(action, true, message);
}

// ==== end write path: temperature threshold ================================

// ============================================================================
// Write path: fan curve (task 9.1)
// ============================================================================
//
// validateFanCurve() is the SINGLE SOURCE OF TRUTH for fan-curve validity. It
// is a pure query with no side effects and issues no writes (Req 6.7): both the
// QML Apply-button gating and applyFanCurve() below consult it, so the "valid"
// predicate is defined in exactly one place. A curve is valid iff (Req 6.3-6.6):
//   * it has between 2 and 8 points inclusive, AND
//   * temperatures are strictly ascending across the ordered points, AND
//   * every temperature is in [0, 105] degrees Celsius, AND
//   * every fan percent is in [0, 100].
//
// applyFanCurve() is a privileged write path reachable ONLY from the QML
// confirmation accept handler (Req 6.8-6.10, 9); it performs no confirmation of
// its own. It re-validates through validateFanCurve() so an invalid curve never
// reaches the driver, writes via IDBusAccess::setFanCurve, reports the outcome
// through recordResult() (exactly one applyResult + one LogEntry, Req 2.5,
// 13.3), and persists the curve for the profile on success (Req 11.1). Every
// return path funnels through recordResult() exactly once.

QVariantMap HardwareManager::validateFanCurve(const QVariantList &points) const
{
    auto invalid = [](const QString &reason) {
        QVariantMap m;
        m.insert(QStringLiteral("valid"), false);
        m.insert(QStringLiteral("reason"), reason);
        return m;
    };

    // Point-count bounds (Req 6.3).
    const int count = points.size();
    if (count < 2 || count > 8) {
        return invalid(
            QStringLiteral("Fan curve must have between 2 and 8 points (has %1).")
                .arg(count));
    }

    int previousTempC = 0;
    for (int i = 0; i < count; ++i) {
        const QVariantMap point = points.at(i).toMap();

        bool tempOk = false;
        bool percentOk = false;
        const int tempC = point.value(QStringLiteral("tempC")).toInt(&tempOk);
        const int percent =
            point.value(QStringLiteral("percent")).toInt(&percentOk);

        if (!tempOk || !percentOk) {
            return invalid(
                QStringLiteral("Fan curve point %1 is missing a numeric tempC/"
                               "percent value.")
                    .arg(i + 1));
        }

        // Temperature range (Req 6.4).
        if (tempC < 0 || tempC > 105) {
            return invalid(
                QStringLiteral("Fan curve temperature %1 \u00B0C at point %2 is "
                               "out of range 0-105 \u00B0C.")
                    .arg(tempC)
                    .arg(i + 1));
        }

        // Percent range (Req 6.5).
        if (percent < 0 || percent > 100) {
            return invalid(
                QStringLiteral("Fan curve percent %1%% at point %2 is out of "
                               "range 0-100%%.")
                    .arg(percent)
                    .arg(i + 1));
        }

        // Strictly ascending temperatures across the ordered points (Req 6.4).
        if (i > 0 && tempC <= previousTempC) {
            return invalid(
                QStringLiteral("Fan curve temperatures must strictly ascend: "
                               "point %1 (%2 \u00B0C) is not greater than the "
                               "previous point (%3 \u00B0C).")
                    .arg(i + 1)
                    .arg(tempC)
                    .arg(previousTempC));
        }
        previousTempC = tempC;
    }

    QVariantMap result;
    result.insert(QStringLiteral("valid"), true);
    result.insert(QStringLiteral("reason"), QString());
    return result;
}

FanCurve HardwareManager::fanCurveFromVariantList(const QVariantList &points)
{
    FanCurve curve;
    curve.reserve(points.size());
    for (const QVariant &v : points) {
        const QVariantMap point = v.toMap();
        FanCurvePoint p;
        p.tempC = point.value(QStringLiteral("tempC")).toInt();
        p.percent = point.value(QStringLiteral("percent")).toInt();
        curve.append(p);
    }
    return curve;
}

std::optional<QString> HardwareManager::resolveFanCurveHwmonDir() const
{
    if (!m_sysfs)
        return std::nullopt;
    return m_sysfs->resolveHwmonDir(QStringList{kFanCurveHwmonName});
}

bool HardwareManager::sysfsFanCurveAvailable() const
{
    const std::optional<QString> dir = resolveFanCurveHwmonDir();
    if (!dir.has_value())
        return false;
    // Require at least the first CPU-fan auto-point to exist so we do not claim
    // support for a device that exposes an unexpected attribute layout.
    return m_sysfs
        && m_sysfs->exists(*dir + QStringLiteral("/pwm1_auto_point1_pwm"))
        && m_sysfs->exists(*dir + QStringLiteral("/pwm1_auto_point1_temp"));
}

WriteOutcome HardwareManager::writeFanCurveToSysfs(const QString &hwmonDir,
                                                   int fanIndex,
                                                   const QVariantList &points)
{
    // The driver has a fixed 8 auto-points per fan. Pad a shorter curve by
    // repeating the last point (temp held, percent held) so unused points do
    // not leave a low fan speed at high temperature, and clamp to 8 points. The
    // curve has already passed validateFanCurve() (ascending temps, ranges), so
    // this only reshapes it to the hardware's fixed table.
    const FanCurve curve = fanCurveFromVariantList(points);
    if (curve.isEmpty())
        return WriteOutcome{WriteOutcome::Status::IoError,
                            QStringLiteral("empty fan curve")};

    const QString prefix =
        hwmonDir + QStringLiteral("/pwm%1_auto_point").arg(fanIndex);
    const QString enablePath =
        hwmonDir + QStringLiteral("/pwm%1_enable").arg(fanIndex);

    int lastTempC = 0;   // tracks the previous written temp to keep temps ascending

    for (int i = 0; i < kFanCurveHwmonPoints; ++i) {
        int tempC;
        int pct;
        if (i < curve.size()) {
            tempC = curve[i].tempC;
            pct = curve[i].percent;
        } else {
            // Padding slot: hold the last real point's fan percent, but keep the
            // temperature STRICTLY ASCENDING (temp+1 per slot, capped at 105).
            // The driver can reject or misread a table with duplicate/
            // non-ascending temps, which previously left the tail (e.g.
            // 100,100) ineffective.
            pct = curve.last().percent;
            tempC = lastTempC + 1;
        }

        // Clamp defensively even though validateFanCurve() guarantees ranges.
        if (pct < 0) pct = 0;
        else if (pct > 100) pct = 100;
        if (tempC < 0) tempC = 0;
        else if (tempC > 105) tempC = 105;
        // Guarantee strict ascent across the full 8-slot table.
        if (i > 0 && tempC <= lastTempC)
            tempC = (lastTempC < 105) ? lastTempC + 1 : 105;
        lastTempC = tempC;

        // Convert percent (0..100) to the driver's PWM scale (0..255), rounding
        // to nearest (hwmon pwm convention; asus_custom_fan_curve uses 0..255).
        const int pwm = static_cast<int>(
            std::lround(static_cast<double>(pct) * kFanCurvePwmMax / 100.0));

        const QString tempPath =
            prefix + QStringLiteral("%1_temp").arg(i + 1);
        const QString pwmPath =
            prefix + QStringLiteral("%1_pwm").arg(i + 1);

        // Write temperature first, then PWM, for each point. Any non-Ok outcome
        // aborts and is returned so the single recordResult() in applyFanCurve()
        // reports a specific failure.
        const WriteOutcome tOut =
            m_sysfs->write(tempPath, QString::number(tempC),
                           kFanCurveWriteTimeoutMs);
        if (tOut.status != WriteOutcome::Status::Ok)
            return tOut;

        const WriteOutcome pOut =
            m_sysfs->write(pwmPath, QString::number(pwm),
                           kFanCurveWriteTimeoutMs);
        if (pOut.status != WriteOutcome::Status::Ok)
            return pOut;
    }

    // Activate the custom curve. On the asus_custom_fan_curve driver, writing
    // pwmN_enable = 1 (manual/custom) is what makes the EC actually follow the
    // auto-points we just wrote. pwmN_enable = 2 leaves the EC on its BIOS/auto
    // default and IGNORES the custom points (verified on ROG Zephyrus G14: at
    // enable=2 the fan stayed pinned regardless of the written curve; at
    // enable=1 it ramped along the curve). This is best-effort: if the enable
    // node is absent or not writable we do not fail the whole apply, since the
    // point writes themselves already succeeded.
    if (m_sysfs && m_sysfs->exists(enablePath) && m_sysfs->isWritable(enablePath)) {
        m_sysfs->write(enablePath, QStringLiteral("1"), kFanCurveWriteTimeoutMs);
    }

    return WriteOutcome{WriteOutcome::Status::Ok, QString()};
}

void HardwareManager::applyFanCurve(const QString &profile,
                                    const QVariantList &points,
                                    const QString &fan)
{
    // Channel select: "gpu" -> pwm2 / gpuFanCurve; anything else -> CPU (pwm1).
    const bool gpu = (fan.compare(QStringLiteral("gpu"), Qt::CaseInsensitive) == 0);
    const int fanIndex = gpu ? 2 : 1;
    const QString fanLabel = gpu ? QStringLiteral("GPU") : QStringLiteral("CPU");

    // Resolve to a canonical profile name so persistence is keyed consistently
    // with loadPersistedSettings() / the per-profile accessors. An unknown name
    // is a failure reported without any write.
    const std::optional<PowerProfile> canonicalProfile =
        powerProfileFromName(profile);
    if (!canonicalProfile.has_value()) {
        recordResult(QStringLiteral("Apply %1 fan curve (%2)").arg(fanLabel, profile),
                     false,
                     QStringLiteral("Unknown power profile \"%1\"; fan curve not "
                                    "applied.")
                         .arg(profile));
        return;
    }

    const QString name = powerProfileName(*canonicalProfile);
    const QString action =
        QStringLiteral("Apply %1 fan curve (%2)").arg(fanLabel, name);

    // Re-validate through the single source of truth so an invalid curve never
    // reaches the driver, even if QML's gating is bypassed (Req 6.3-6.6).
    const QVariantMap validation = validateFanCurve(points);
    if (!validation.value(QStringLiteral("valid")).toBool()) {
        recordResult(action, false,
                     QStringLiteral("Fan curve rejected: %1")
                         .arg(validation.value(QStringLiteral("reason")).toString()));
        return;
    }

    // Select a backend. Prefer the asusd D-Bus daemon when present (it applies
    // per-profile curves natively); otherwise fall back to the kernel-native
    // asus_custom_fan_curve hwmon and write the curve directly to sysfs. When
    // neither exists the driver does not expose fan-curve control (Req 6.11);
    // use the exact required string so QML/log surfaces are consistent.
    const bool dbusBackend = m_dbus && m_dbus->fanCurvesSupported();
    const std::optional<QString> hwmonDir =
        dbusBackend ? std::nullopt : resolveFanCurveHwmonDir();
    const bool sysfsBackend = !dbusBackend && hwmonDir.has_value()
        && sysfsFanCurveAvailable();

    if (!dbusBackend && !sysfsBackend) {
        recordResult(action, false,
                     QStringLiteral("Fan curve control not supported by current "
                                    "driver"));
        return;
    }

    // Map a failing WriteOutcome to a specific operator-facing message.
    auto fanFailureMessage = [](const WriteOutcome &outcome) -> QString {
        switch (outcome.status) {
        case WriteOutcome::Status::PermissionDenied:
            return QStringLiteral("permission denied (root privileges required)");
        case WriteOutcome::Status::Timeout:
            return QStringLiteral("the write timed out");
        case WriteOutcome::Status::Unsupported:
            return QStringLiteral("not supported by current driver");
        case WriteOutcome::Status::IoError:
        default: {
            QString detail = outcome.message;
            if (detail.isEmpty())
                detail = QStringLiteral("driver reported a write failure");
            return detail;
        }
        }
    };

    if (dbusBackend) {
        // Write via the D-Bus (asusd) fan-curve interface (Req 6.10). asusd's
        // per-profile SetFanCurve applies the whole profile; the CPU/GPU split
        // is a direct-sysfs concept, so over D-Bus both channels follow the same
        // submitted curve. The persisted per-fan field is still updated below so
        // the editor round-trips correctly.
        const WriteOutcome outcome = m_dbus->setFanCurve(name, points);
        if (outcome.status != WriteOutcome::Status::Ok) {
            recordResult(action, false,
                         QStringLiteral("Failed to apply %1 fan curve for %2: %3.")
                             .arg(fanLabel, name, fanFailureMessage(outcome)));
            return;
        }
    } else {
        // Direct-sysfs backend: write ONLY the selected fan's channel (CPU=pwm1,
        // GPU=pwm2) so the two fans can carry independent curves. If the GPU
        // channel does not exist on this unit, report it rather than silently
        // succeeding.
        const QString firstPoint =
            *hwmonDir + QStringLiteral("/pwm%1_auto_point1_pwm").arg(fanIndex);
        if (!m_sysfs || !m_sysfs->exists(firstPoint)) {
            recordResult(action, false,
                         QStringLiteral("%1 fan channel (pwm%2) is not exposed by "
                                        "the current driver; not applied.")
                             .arg(fanLabel).arg(fanIndex));
            return;
        }
        const WriteOutcome out = writeFanCurveToSysfs(*hwmonDir, fanIndex, points);
        if (out.status != WriteOutcome::Status::Ok) {
            recordResult(action, false,
                         QStringLiteral("Failed to apply %1 fan curve for %2: %3.")
                             .arg(fanLabel, name, fanFailureMessage(out)));
            return;
        }
    }

    // Persist on success, keeping other per-profile settings intact (Req 11.1).
    // Only the selected fan's curve field is updated.
    ProfileSettings settings;
    const auto it = m_persistedSettings.find(name);
    if (it != m_persistedSettings.end())
        settings = it.value();
    else
        settings = defaultSettingsFor(*canonicalProfile);

    if (gpu)
        settings.gpuFanCurve = fanCurveFromVariantList(points);
    else
        settings.fanCurve = fanCurveFromVariantList(points);
    m_persistedSettings.insert(name, settings);

    const bool saved = m_settings && m_settings->save(name, settings);
    if (!saved) {
        // The hardware write succeeded but persistence failed; report the
        // partial success so the operator knows the curve is live but not saved.
        recordResult(action, false,
                     QStringLiteral("%1 fan curve applied to %2 but could not be "
                                    "persisted.")
                         .arg(fanLabel, name));
        return;
    }

    recordResult(action, true,
                 QStringLiteral("%1 fan curve applied and saved for %2 (%3 points).")
                     .arg(fanLabel, name)
                     .arg(points.size()));
}

// ============================================================================
// Fan calibration: measure a fan's RPM range by sweeping its curve
// ============================================================================
//
// There is no direct-duty pwm node on this hardware, so we calibrate by
// temporarily forcing the fan's auto-point curve to a flat 0% and then a flat
// 100% (with pwmN_enable=1 so the EC follows it), letting the fan settle, and
// reading fanN_input at each extreme. The profile's saved curve is restored
// afterwards so calibration leaves no lasting change. Every return path funnels
// through recordResult() exactly once.

void HardwareManager::calibrateFan(const QString &fan)
{
    const bool gpu = (fan.compare(QStringLiteral("gpu"), Qt::CaseInsensitive) == 0);
    const int fanIndex = gpu ? 2 : 1;
    const QString fanLabel = gpu ? QStringLiteral("GPU") : QStringLiteral("CPU");
    const QString action = QStringLiteral("Calibrate %1 fan").arg(fanLabel);

    // Requires the direct-sysfs backend (asus_custom_fan_curve). The D-Bus
    // (asusd) path does not expose a raw sweep, so calibration is sysfs-only.
    const std::optional<QString> hwmonDir = resolveFanCurveHwmonDir();
    if (!hwmonDir.has_value() || !sysfsFanCurveAvailable()) {
        recordResult(action, false,
                     QStringLiteral("Calibration needs the direct sysfs fan-curve "
                                    "driver, which is not available."));
        return;
    }

    const QString firstPwm =
        *hwmonDir + QStringLiteral("/pwm%1_auto_point1_pwm").arg(fanIndex);
    if (!m_sysfs || !m_sysfs->exists(firstPwm)) {
        recordResult(action, false,
                     QStringLiteral("%1 fan channel (pwm%2) is not exposed; cannot "
                                    "calibrate.")
                         .arg(fanLabel).arg(fanIndex));
        return;
    }
    if (!m_sysfs->isWritable(firstPwm)) {
        recordResult(action, false,
                     QStringLiteral("Calibration requires root privileges "
                                    "(fan control is not writable)."));
        return;
    }

    // Build a flat curve at a given percent across all 8 ascending-temp points.
    auto flatCurve = [](int percent) -> QVariantList {
        QVariantList pts;
        for (int i = 0; i < 8; ++i) {
            QVariantMap p;
            p.insert(QStringLiteral("tempC"), 20 + i * 5); // ascending, in-range
            p.insert(QStringLiteral("percent"), percent);
            pts.append(p);
        }
        return pts;
    };

    // Settle time and sampling: give the fan time to spin up/down, then take the
    // extreme (min at 0%, max at 100%) of a few samples.
    const int settleMs = 2500;
    const int sampleCount = 4;
    const int sampleGapMs = 250;

    // Phase 1: force 0% and read the low RPM.
    WriteOutcome w = writeFanCurveToSysfs(*hwmonDir, fanIndex, flatCurve(0));
    if (w.status != WriteOutcome::Status::Ok) {
        recordResult(action, false,
                     QStringLiteral("Failed to drive %1 fan for calibration: %2.")
                         .arg(fanLabel)
                         .arg(w.message.isEmpty()
                                  ? QStringLiteral("write error")
                                  : w.message));
        return;
    }
    QThread::msleep(settleMs);
    int minRpm = -1;
    for (int i = 0; i < sampleCount; ++i) {
        const int r = readFanRpm(fanIndex);
        if (r >= 0 && (minRpm < 0 || r < minRpm))
            minRpm = r;
        QThread::msleep(sampleGapMs);
    }

    // Phase 2: force 100% and read the high RPM.
    w = writeFanCurveToSysfs(*hwmonDir, fanIndex, flatCurve(100));
    int maxRpm = -1;
    if (w.status == WriteOutcome::Status::Ok) {
        QThread::msleep(settleMs);
        for (int i = 0; i < sampleCount; ++i) {
            const int r = readFanRpm(fanIndex);
            if (r > maxRpm)
                maxRpm = r;
            QThread::msleep(sampleGapMs);
        }
    }

    // Restore the profile's saved curve for this fan so calibration is
    // non-destructive. Uses the persisted curve (falls back to defaults).
    const QVariantList saved =
        fanCurveFor(m_selectedProfile, gpu ? QStringLiteral("gpu")
                                           : QStringLiteral("cpu"));
    if (!saved.isEmpty())
        writeFanCurveToSysfs(*hwmonDir, fanIndex, saved);

    // Refresh the live reading after restore.
    refreshFanRpm();

    if (minRpm < 0 && maxRpm < 0) {
        recordResult(action, false,
                     QStringLiteral("Calibration could not read %1 fan RPM.")
                         .arg(fanLabel));
        return;
    }

    // Record the measured range.
    if (gpu) {
        m_gpuFanRpmMin = minRpm;
        m_gpuFanRpmMax = maxRpm;
    } else {
        m_cpuFanRpmMin = minRpm;
        m_cpuFanRpmMax = maxRpm;
    }
    emit fanRpmRangeChanged();

    recordResult(action, true,
                 QStringLiteral("%1 fan calibrated: %2\u2013%3 RPM.")
                     .arg(fanLabel)
                     .arg(minRpm < 0 ? QStringLiteral("?") : QString::number(minRpm))
                     .arg(maxRpm < 0 ? QStringLiteral("?") : QString::number(maxRpm)));
}

// ==== end write path: fan curve ============================================

// ============================================================================
// Write path: GPU mode (task 10.2)
// ============================================================================
//
// applyGpuMode() is the single privileged write path for switching GPU mode. It
// is reachable ONLY from the QML confirmation dialog's accept handler: the
// disruptive-action confirmation is enforced in QML, so C++ never writes while a
// confirmation is pending or after a cancel (Req 5.9, 5.10, 5.13, 9.1-9.3). This
// method therefore performs no confirmation of its own.
//
// Flow:
//   1. Refuse a mode that is not in the runtime-detected supported set so the
//      driver is never asked to switch to an unconfirmed mode (Req 5.2, 5.3).
//      This is reported via recordResult() without any write.
//   2. Write via IDBusAccess::setGpuMode (Req 5.11, 9.4).
//   3. On a write failure surface it through recordResult() and retain the
//      last-known activeGpuMode; the QML selector is bound to activeGpuMode, so
//      it reverts to the previously active mode (Req 5.12).
//   4. On write success perform a read-back of the active GPU mode and update
//      activeGpuMode ONLY after the read-back succeeds (Req 10.1, 10.2). A
//      read-back that fails or differs from the requested mode is reported in
//      the applyResult message (Req 10.3) while still adopting the driver-
//      reported value as the source of truth.
//
// Every return path funnels through recordResult() exactly once so each write
// invocation emits exactly one applyResult(...) and appends exactly one LogEntry
// (Req 2.5, 13.3).

void HardwareManager::applyGpuMode(const QString &modeName)
{
    const QString action = QStringLiteral("Apply GPU mode (%1)").arg(modeName);

    // Only offer/apply modes the platform confirmed at runtime (Req 5.2, 5.3).
    // The supported set is populated by refreshGpuMode(); guarding here means a
    // bypass of QML's subset gating still cannot reach the driver with an
    // unconfirmed mode.
    if (!m_supportedGpuModes.contains(modeName)) {
        recordResult(action, false,
                     QStringLiteral("GPU mode \"%1\" is not in the set of "
                                    "supported modes; not applied.")
                         .arg(modeName));
        return;
    }

    // Refuse when the supergfxctl D-Bus service is unavailable (Req 5.8). No
    // accessor / no service means there is no write path to take.
    if (!m_dbus || !m_dbus->serviceAvailable(kSupergfxctlService)) {
        recordResult(action, false,
                     QStringLiteral("GPU mode switch failed: supergfxctl D-Bus "
                                    "service not available"));
        return;
    }

    // Perform the write (Req 5.11, 9.4).
    const WriteOutcome outcome = m_dbus->setGpuMode(modeName);
    if (outcome.status != WriteOutcome::Status::Ok) {
        QString detail = outcome.message;
        if (detail.isEmpty())
            detail = QStringLiteral("driver reported a write failure");
        // On failure the previously active mode is retained (Req 5.12): we do
        // NOT touch m_activeGpuMode, so the activeGpuMode-bound selector reverts.
        recordResult(action, false,
                     QStringLiteral("Failed to switch GPU mode to %1: %2.")
                         .arg(modeName)
                         .arg(detail));
        return;
    }

    // Write succeeded: read back the active GPU mode and only then adopt it as
    // the new source of truth (Req 10.1, 10.2). activeGpuMode is updated ONLY
    // after a successful read-back.
    const std::optional<QString> readBack = m_dbus->activeGpuMode();
    if (!readBack.has_value()) {
        // The switch was accepted but we could not confirm it. Retain the
        // last-known activeGpuMode (do not update it on an unsuccessful read-
        // back) and report the missing confirmation (Req 10.3).
        recordResult(action, true,
                     QStringLiteral("GPU mode switch to %1 accepted, but the "
                                    "active mode could not be read back to "
                                    "confirm it. A logout or reboot may be "
                                    "required.")
                         .arg(modeName));
        return;
    }

    // Adopt the driver-reported value as the source of truth and notify QML
    // (Req 10.2). The selector, bound to activeGpuMode, now reflects reality.
    if (*readBack != m_activeGpuMode) {
        m_activeGpuMode = *readBack;
        emit activeGpuModeChanged();
    }

    if (*readBack != modeName) {
        // Read-back differs from the requested mode: display the driver value
        // and report the discrepancy (Req 10.3). GPU mode changes can require a
        // logout/reboot, so this is an expected outcome for some switches.
        recordResult(action, true,
                     QStringLiteral("GPU mode switch to %1 requested; driver "
                                    "reports active mode \"%2\". The change may "
                                    "require a logout or reboot to take full "
                                    "effect.")
                         .arg(modeName)
                         .arg(*readBack));
        return;
    }

    recordResult(action, true,
                 QStringLiteral("GPU mode switched to %1 (confirmed by read-"
                                "back).")
                     .arg(modeName));
}

// ==== end write path: GPU mode =============================================

// ============================================================================
// Write path: power limits (task 10.3)
// ============================================================================
//
// applyPowerLimits() writes PL1/PL2 (watts) for a profile through
// ISysfsAccess::write(path, value, timeoutMs), then reads the values back from
// the driver and reports any discrepancy. The design draws a hard line: the UI
// always shows the driver's read-back value, never the merely requested value
// (Req 8.10, 10.2), and a discrepancy is reported through the applyResult
// message iff the read-back differs from the request (Req 8.10, 10.3).
//
// The PL1/PL2 sysfs write is known to block for ~60 s on some kernels before
// returning an I/O error, so the write uses a bounded timeout
// (kPowerLimitWriteTimeoutMs) and every PL write is treated as fallible. On a
// Timeout the exact design message is reported and a read-back is still
// performed so the operator sees the value the hardware currently holds.
//
// Every return path funnels through recordResult() exactly once so a single
// applyResult(...) is emitted and a single LogEntry is appended (Req 2.5, 13.3).

std::optional<QString> HardwareManager::resolvePl1Path() const
{
    if (m_sysfs && m_sysfs->exists(kPl1PathAsusNbWmi))
        return kPl1PathAsusNbWmi;
    if (m_sysfs && m_sysfs->exists(kPl1PathAsusArmoury))
        return kPl1PathAsusArmoury;
    return std::nullopt;
}

std::optional<QString> HardwareManager::resolvePl2Path() const
{
    if (m_sysfs && m_sysfs->exists(kPl2PathAsusNbWmi))
        return kPl2PathAsusNbWmi;
    if (m_sysfs && m_sysfs->exists(kPl2PathAsusArmoury))
        return kPl2PathAsusArmoury;
    return std::nullopt;
}

std::optional<int> HardwareManager::readBackPowerLimit(const QString &path) const
{
    if (!m_sysfs || !m_sysfs->exists(path))
        return std::nullopt;

    const std::optional<QString> raw = m_sysfs->read(path);
    if (!raw.has_value())
        return std::nullopt;

    bool ok = false;
    const int value = raw->trimmed().toInt(&ok);
    if (!ok)
        return std::nullopt;
    return value;
}

void HardwareManager::applyPowerLimits(const QString &profile, int pl1Watts,
                                       int pl2Watts)
{
    // Resolve to a canonical profile name so persistence is keyed consistently
    // with loadPersistedSettings() / the per-profile accessors. An unknown name
    // is a failure reported without any write (Req 8.7).
    const std::optional<PowerProfile> canonicalProfile =
        powerProfileFromName(profile);
    if (!canonicalProfile.has_value()) {
        recordResult(QStringLiteral("Apply PL1/PL2 (%1)").arg(profile), false,
                     QStringLiteral("Unknown power profile \"%1\"; power limits "
                                    "not applied.")
                         .arg(profile));
        return;
    }

    const QString name = powerProfileName(*canonicalProfile);
    const QString action = QStringLiteral("Apply PL1/PL2 (%1)").arg(name);

    // Resolve the PL1/PL2 sysfs paths. A missing attribute means the driver does
    // not expose power limiting; report as unsupported without writing (Req 8.9).
    const std::optional<QString> pl1Path = resolvePl1Path();
    const std::optional<QString> pl2Path = resolvePl2Path();
    if (!pl1Path.has_value() || !pl2Path.has_value()) {
        recordResult(action, false,
                     QStringLiteral("Power limits not exposed by current driver; "
                                    "PL1/PL2 not applied."));
        return;
    }

    // Helper mapping a failing WriteOutcome to a specific operator-facing
    // message. Timeout uses the exact design string for the PL1/PL2 blocking
    // write caveat.
    auto failureMessage = [](const QString &label, const WriteOutcome &outcome)
        -> QString {
        switch (outcome.status) {
        case WriteOutcome::Status::Timeout:
            return QStringLiteral("Power limit write timed out (known driver "
                                  "issue on some kernels)");
        case WriteOutcome::Status::PermissionDenied:
            return QStringLiteral("Permission denied writing %1 (root privileges "
                                  "required).")
                .arg(label);
        case WriteOutcome::Status::Unsupported:
            return QStringLiteral("%1 write not supported by current driver.")
                .arg(label);
        case WriteOutcome::Status::IoError:
        default: {
            QString detail = outcome.message;
            if (detail.isEmpty())
                detail = QStringLiteral("driver reported an I/O error");
            return QStringLiteral("Failed to write %1: %2.").arg(label).arg(detail);
        }
        }
    };

    // Write PL1 then PL2 with a bounded timeout (Req 8.7). Each write is
    // fallible; on any non-Ok outcome report the specific failure. On a Timeout
    // we still perform a read-back so the operator sees the hardware's actual
    // value (design PL1/PL2 caveat).
    const WriteOutcome pl1Outcome =
        m_sysfs->write(*pl1Path, QString::number(pl1Watts),
                       kPowerLimitWriteTimeoutMs);
    if (pl1Outcome.status != WriteOutcome::Status::Ok) {
        const QString baseMsg = failureMessage(QStringLiteral("PL1"), pl1Outcome);
        // Read back the current PL1/PL2 so the message reflects the live values.
        const std::optional<int> pl1Now = readBackPowerLimit(*pl1Path);
        const std::optional<int> pl2Now = readBackPowerLimit(*pl2Path);
        QString msg = baseMsg;
        if (pl1Now.has_value() || pl2Now.has_value()) {
            msg += QStringLiteral(" Current values: PL1 %1 W, PL2 %2 W.")
                       .arg(pl1Now.has_value()
                                ? QString::number(*pl1Now)
                                : QStringLiteral("unknown"))
                       .arg(pl2Now.has_value()
                                ? QString::number(*pl2Now)
                                : QStringLiteral("unknown"));
        }
        recordResult(action, false, msg);
        return;
    }

    const WriteOutcome pl2Outcome =
        m_sysfs->write(*pl2Path, QString::number(pl2Watts),
                       kPowerLimitWriteTimeoutMs);
    if (pl2Outcome.status != WriteOutcome::Status::Ok) {
        const QString baseMsg = failureMessage(QStringLiteral("PL2"), pl2Outcome);
        const std::optional<int> pl1Now = readBackPowerLimit(*pl1Path);
        const std::optional<int> pl2Now = readBackPowerLimit(*pl2Path);
        QString msg = baseMsg;
        if (pl1Now.has_value() || pl2Now.has_value()) {
            msg += QStringLiteral(" Current values: PL1 %1 W, PL2 %2 W.")
                       .arg(pl1Now.has_value()
                                ? QString::number(*pl1Now)
                                : QStringLiteral("unknown"))
                       .arg(pl2Now.has_value()
                                ? QString::number(*pl2Now)
                                : QStringLiteral("unknown"));
        }
        recordResult(action, false, msg);
        return;
    }

    // Both writes succeeded: read back the applied values from the driver
    // (Req 8.8, 10.1). The displayed/persisted values are the read-back values,
    // never the requested ones (Req 8.10, 10.2).
    const std::optional<int> pl1ReadBack = readBackPowerLimit(*pl1Path);
    const std::optional<int> pl2ReadBack = readBackPowerLimit(*pl2Path);

    if (!pl1ReadBack.has_value() || !pl2ReadBack.has_value()) {
        // Write reported success but the read-back could not be completed. Treat
        // the read-back failure as a discrepancy the operator must see (Req 8.10,
        // 10.3); persist the requested values so they are not lost, but make the
        // failure explicit.
        ProfileSettings settings;
        const auto it = m_persistedSettings.find(name);
        if (it != m_persistedSettings.end())
            settings = it.value();
        else
            settings = defaultSettingsFor(*canonicalProfile);
        settings.pl1Watts = pl1ReadBack.value_or(pl1Watts);
        settings.pl2Watts = pl2ReadBack.value_or(pl2Watts);
        m_persistedSettings.insert(name, settings);
        if (m_settings)
            m_settings->save(name, settings);

        recordResult(action, false,
                     QStringLiteral("Power limits written for %1 but read-back "
                                    "failed; could not confirm the applied "
                                    "values.")
                         .arg(name));
        return;
    }

    // Persist the confirmed (read-back) values on success (Req 11.3), keeping
    // other per-profile settings intact.
    ProfileSettings settings;
    const auto it = m_persistedSettings.find(name);
    if (it != m_persistedSettings.end())
        settings = it.value();
    else
        settings = defaultSettingsFor(*canonicalProfile);
    settings.pl1Watts = *pl1ReadBack;
    settings.pl2Watts = *pl2ReadBack;
    m_persistedSettings.insert(name, settings);

    const bool saved = m_settings && m_settings->save(name, settings);

    // Report the outcome. If the read-back differs from what was requested,
    // state the discrepancy; the reported values are always the driver's
    // read-back (Req 8.10, 10.3).
    const bool discrepancy =
        (*pl1ReadBack != pl1Watts) || (*pl2ReadBack != pl2Watts);

    if (!saved) {
        // Hardware write + read-back succeeded but persistence failed; report
        // the partial success so the operator knows the values are live but not
        // saved.
        QString msg =
            QStringLiteral("Power limits applied for %1 (PL1 %2 W, PL2 %3 W) but "
                           "could not be persisted.")
                .arg(name)
                .arg(*pl1ReadBack)
                .arg(*pl2ReadBack);
        recordResult(action, false, msg);
        return;
    }

    QString message;
    if (discrepancy) {
        message = QStringLiteral("Power limits applied for %1. Driver reported "
                                 "PL1 %2 W, PL2 %3 W (requested PL1 %4 W, PL2 %5 W).")
                      .arg(name)
                      .arg(*pl1ReadBack)
                      .arg(*pl2ReadBack)
                      .arg(pl1Watts)
                      .arg(pl2Watts);
    } else {
        message = QStringLiteral("Power limits applied and saved for %1 "
                                 "(PL1 %2 W, PL2 %3 W).")
                      .arg(name)
                      .arg(*pl1ReadBack)
                      .arg(*pl2ReadBack);
    }

    // A read-back discrepancy is still a successful write: the values were
    // applied, they simply differ from the request (Req 8.10, 10.3). The message
    // carries the discrepancy so the operator sees exactly what the driver holds.
    recordResult(action, true, message);
}

// ==== end write path: power limits =========================================

// ============================================================================
// Write path: power profile (task 10.1)
// ============================================================================
//
// setPowerProfile() is the single write path for the active power profile. It
// is reachable from the QML Power_Profile selector (Req 4.4). Unlike the
// per-profile settings write paths, it persists nothing: the power profile is
// live platform state, not saved per-profile metadata.
//
// Flow:
//   1. Resolve the requested name to a canonical PowerProfile; an unknown name
//      is a failure reported via recordResult() without any write.
//   2. Resolve the platform_profile sysfs attribute; a missing/unwritable path
//      is a failure reported via recordResult() without writing (Req 4.6).
//   3. Write the mapped platform string (PowerProfile -> platform string via the
//      DataModels.h mapping table) through ISysfsAccess::write with a bounded
//      timeout. Any non-Ok WriteOutcome is reported with a specific message
//      (Req 4.6).
//   4. On write success, read back the active profile via refreshActiveProfile()
//      (Req 4.5, 10.1). The reported value is the driver read-back, never the
//      merely requested value (Req 10.2); when the read-back differs from the
//      request the applyResult message states the discrepancy (Req 10.3).
//
// Every return path funnels through recordResult() exactly once so a single
// applyResult(...) is emitted and a single LogEntry is appended (Req 2.5, 4.5,
// 13.3).

void HardwareManager::setPowerProfile(const QString &profileName)
{
    // Resolve to a canonical profile so the platform string written is always
    // the preferred value for a known profile. An unknown name is a failure
    // reported without any write.
    const std::optional<PowerProfile> canonicalProfile =
        powerProfileFromName(profileName);
    if (!canonicalProfile.has_value()) {
        recordResult(QStringLiteral("Apply power profile (%1)").arg(profileName),
                     false,
                     QStringLiteral("Unknown power profile \"%1\"; profile not "
                                    "applied.")
                         .arg(profileName));
        return;
    }

    const QString name = powerProfileName(*canonicalProfile);
    const QString action = QStringLiteral("Apply power profile (%1)").arg(name);

    // Resolve the platform_profile attribute. A missing or unwritable path means
    // the platform does not expose profile control here; report without writing
    // (Req 4.6).
    if (!m_sysfs || !m_sysfs->exists(kPlatformProfilePath)) {
        recordResult(action, false,
                     QStringLiteral("Power profile control not available "
                                    "(platform_profile attribute missing); %1 not "
                                    "applied.")
                         .arg(name));
        return;
    }
    if (!m_sysfs->isWritable(kPlatformProfilePath)) {
        recordResult(action, false,
                     QStringLiteral("Power profile control not writable "
                                    "(root privileges required); %1 not applied.")
                         .arg(name));
        return;
    }

    // Write the mapped platform string with a bounded timeout so a blocking
    // write cannot hang the UI (Req 4.4, 4.6).
    const QString platformString =
        powerProfilePlatformString(*canonicalProfile);
    const WriteOutcome outcome =
        m_sysfs->write(kPlatformProfilePath, platformString,
                       kPowerLimitWriteTimeoutMs);
    if (outcome.status != WriteOutcome::Status::Ok) {
        QString detail;
        switch (outcome.status) {
        case WriteOutcome::Status::Timeout:
            detail = QStringLiteral("the write timed out");
            break;
        case WriteOutcome::Status::PermissionDenied:
            detail = QStringLiteral("permission denied (root privileges "
                                    "required)");
            break;
        case WriteOutcome::Status::Unsupported:
            detail = QStringLiteral("not supported by the current driver");
            break;
        case WriteOutcome::Status::IoError:
        default:
            detail = outcome.message.isEmpty()
                         ? QStringLiteral("the driver reported an I/O error")
                         : outcome.message;
            break;
        }
        recordResult(action, false,
                     QStringLiteral("Failed to apply power profile %1: %2.")
                         .arg(name)
                         .arg(detail));
        return;
    }

    // Write succeeded: read back the active profile from the driver so the UI
    // reflects what the hardware accepted, never the merely requested value
    // (Req 4.5, 10.1, 10.2). refreshActiveProfile() updates m_activePowerProfile
    // and emits activePowerProfileChanged() on a change.
    refreshActiveProfile();
    const QString readBack = m_activePowerProfile;

    if (readBack.isEmpty()) {
        // The write was accepted but the active profile could not be confirmed
        // by a read-back. Report the missing confirmation (Req 10.3).
        recordResult(action, true,
                     QStringLiteral("Power profile change to %1 accepted, but the "
                                    "active profile could not be read back to "
                                    "confirm it.")
                         .arg(name));
        return;
    }

    if (readBack.compare(name, Qt::CaseInsensitive) != 0) {
        // Read-back differs from the requested profile: display the driver value
        // and report the discrepancy (Req 10.2, 10.3).
        recordResult(action, true,
                     QStringLiteral("Power profile change to %1 requested; driver "
                                    "reports active profile \"%2\".")
                         .arg(name)
                         .arg(readBack));
        return;
    }

    recordResult(action, true,
                 QStringLiteral("Power profile set to %1 (confirmed by read-"
                                "back).")
                     .arg(name));
}

// ==== end write path: power profile ========================================
