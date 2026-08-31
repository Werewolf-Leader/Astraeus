#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <limits>
#include <optional>

#include "DataModels.h"
// PlatformAccess.h defines WriteOutcome (returned by value from the private
// sysfs write helpers below) plus the ISysfsAccess/IDBusAccess/ISettingsStore
// interfaces. It is a lightweight, dependency-free header.
#include "PlatformAccess.h"

class QTimer;

class HardwareManager : public QObject
{
    Q_OBJECT

    // Profile currently selected in the UI; scopes per-profile settings on both
    // tabs (Req 12). Persisted per-profile values are looked up against this.
    Q_PROPERTY(QString selectedProfile READ selectedProfile WRITE setSelectedProfile
                   NOTIFY selectedProfileChanged)

    // ==== Capability-model region (task 5.1) ================================
    // Capability report per control as a QVariantMap keyed by control id ->
    //   { supported: bool, reason: QString }. QML binds enabled/visible +
    //   reason text to this (Req 3.2, 3.3, 3.7).
    Q_PROPERTY(QVariantMap capabilities READ capabilities NOTIFY capabilitiesChanged)
    // True when the driver exposes a real hardware throttle target; controls the
    // threshold label (Req 7.5/7.6).
    Q_PROPERTY(bool tempThresholdIsHardwareLimit READ tempThresholdIsHardwareLimit
                   NOTIFY capabilitiesChanged)
    // Discovered PL1/PL2 bounds; QVariantMap { min:int, max:int, estimated:bool }
    // (Req 8.4/8.5).
    Q_PROPERTY(QVariantMap powerLimitBounds READ powerLimitBounds NOTIFY capabilitiesChanged)
    // ==== end capability-model region ======================================

    // ==== Telemetry: temperature polling (task 6.2) ========================
    // Live CPU temperature in degrees Celsius; NaN sentinel when unavailable
    // so QML renders "—" rather than a stale value (Req 7.1).
    Q_PROPERTY(double cpuTemperature READ cpuTemperature NOTIFY cpuTemperatureChanged)
    // Live GPU temperature in degrees Celsius; NaN sentinel when unavailable
    // (Req 7.2).
    Q_PROPERTY(double gpuTemperature READ gpuTemperature NOTIFY gpuTemperatureChanged)

    // Live fan speeds in RPM (from the asus hwmon fanN_input); -1 when the
    // sensor is unavailable. Refreshed on the same poll as temperatures.
    Q_PROPERTY(int cpuFanRpm READ cpuFanRpm NOTIFY fanRpmChanged)
    Q_PROPERTY(int gpuFanRpm READ gpuFanRpm NOTIFY fanRpmChanged)
    // ==== end telemetry: temperature polling ===============================

    // ==== Telemetry: active profile & GPU mode (task 6.1) ==================
    // Active power profile reported by the platform ("Quiet"/"Balanced"/
    // "Performance"); empty when unknown/unreadable (Req 4.2).
    Q_PROPERTY(QString activePowerProfile READ activePowerProfile
                   NOTIFY activePowerProfileChanged)
    // Currently active GPU mode name reported at runtime; empty when
    // unknown/unreadable (Req 5.4, 5.5).
    Q_PROPERTY(QString activeGpuMode READ activeGpuMode NOTIFY activeGpuModeChanged)
    // Runtime-detected list of supported GPU mode names; only these are offered
    // by the UI (Req 5.1-5.3).
    Q_PROPERTY(QStringList supportedGpuModes READ supportedGpuModes
                   NOTIFY supportedGpuModesChanged)
    // ==== end telemetry: active profile & GPU mode =========================

    // ==== Status log ring buffer & apply-result helper (task 7.1) ==========
    // Last 10 log entries as a QVariantList of {timestamp, action, success,
    // message}, newest last. Backed by a ring buffer capped at 10 (Req 13.3-13.5).
    Q_PROPERTY(QVariantList recentLog READ recentLog NOTIFY recentLogChanged)
    // ==== end status log region ============================================

    // ==== Battery charge-limit controller ==================================
    // Battery end-charge threshold as a percent (0..100). This is a single
    // global control (not per-profile): the value the battery stops charging
    // at. -1 means unknown/unavailable. Persisting is delegated to the kernel,
    // which retains the sysfs value.
    Q_PROPERTY(int chargeLimit READ chargeLimit NOTIFY batteryStateChanged)
    // Live battery charge percent (0..100); -1 when unavailable.
    Q_PROPERTY(int batteryCapacity READ batteryCapacity NOTIFY batteryStateChanged)
    // Live battery status string ("Charging", "Discharging", "Full",
    // "Not charging", ...); empty when unavailable.
    Q_PROPERTY(QString batteryStatus READ batteryStatus NOTIFY batteryStateChanged)
    // ==== end battery charge-limit controller ==============================

    // ==== Advanced power tuning (RyzenAdj) — optional/experimental =========
    // Current RyzenAdj SMU limits as a QVariantMap
    //   { stapmW, fastW, slowW, apuSlowW, tctlTempC } in watts / degrees C.
    // A field is -1 when unknown. Populated from `ryzenadj --info`.
    Q_PROPERTY(QVariantMap ryzenAdjValues READ ryzenAdjValues NOTIFY ryzenAdjChanged)
    // Whether the periodic re-apply loop is active (SMU limits are volatile and
    // reset by firmware / after resume, so re-applying keeps them in effect).
    Q_PROPERTY(bool ryzenAdjAutoReapply READ ryzenAdjAutoReapply
                   WRITE setRyzenAdjAutoReapply NOTIFY ryzenAdjChanged)
    // ==== end advanced power tuning ========================================

public:
    // Production constructor: allocates and OWNS the real SysfsAccess /
    // DBusAccess / SettingsStore implementations. This keeps existing callers
    // (e.g. `HardwareManager mgr;` in main.cpp) working unchanged.
    explicit HardwareManager(QObject *parent = nullptr);

    // Injecting constructor (for tests): accepts platform-access interfaces the
    // manager DOES NOT own. Ownership model:
    //   - The default constructor allocates the real implementations and owns
    //     them (deletes them in the destructor).
    //   - This injecting constructor takes raw pointers to test doubles it does
    //     NOT own; the caller (the test) retains ownership and lifetime control.
    // The m_ownsAccessors flag records which case applies so the destructor
    // only deletes what it created.
    //
    // The RyzenAdj accessor is optional (defaults to nullptr) so existing
    // three-arg test call sites keep compiling; a nullptr simply means the
    // RyzenAdj control is reported unavailable.
    HardwareManager(ISysfsAccess *sysfs, IDBusAccess *dbus,
                    ISettingsStore *settings, QObject *parent = nullptr,
                    IRyzenAdjAccess *ryzenAdj = nullptr);

    ~HardwareManager() override;

    // ---- selectedProfile property (Req 12) ----

    // The profile currently scoping per-profile settings on both tabs.
    QString selectedProfile() const;
    // Change the selected profile; unknown names are ignored. Emits
    // selectedProfileChanged() only on an actual change.
    void setSelectedProfile(const QString &profile);

    // ---- Per-profile persisted-settings accessors (Req 12) ----
    //
    // These return the operator's *desired* (persisted) settings for a profile,
    // kept distinct from live hardware values (Req 11.6). An unknown profile
    // name falls back to that profile's built-in defaults.

    // Persisted fan curve for a profile as a QVariantList of {tempC, percent}
    // (Req 6.2, 12.3). `fan` selects the channel: "cpu" (pwm1, default) or
    // "gpu" (pwm2). The default keeps existing callers working.
    Q_INVOKABLE QVariantList fanCurveFor(const QString &profile,
                                         const QString &fan = QStringLiteral("cpu")) const;

    // Persisted temperature threshold (degrees C) for a profile (Req 12.2).
    Q_INVOKABLE int tempThresholdFor(const QString &profile) const;

    // Persisted {pl1, pl2} in watts for a profile (Req 12.2).
    Q_INVOKABLE QVariantMap powerLimitsFor(const QString &profile) const;

    // ==== Capability-model region (task 5.1) ================================

    // Capability report per control keyed by control id -> { supported, reason }.
    QVariantMap capabilities() const;

    // True when a real hardware throttle target backs the temperature threshold
    // (Req 7.5/7.6); false => the threshold is a Boreas-side soft limit.
    bool tempThresholdIsHardwareLimit() const;

    // Discovered PL1/PL2 bounds { min:int, max:int, estimated:bool } (Req 8.4/8.5).
    QVariantMap powerLimitBounds() const;

    // Probe every control at runtime and (re)populate the capability model
    // (Req 3.1). Safe to call repeatedly (startup + manual refresh). Never
    // terminates on a missing dependency (Req 3.4); missing paths/services map
    // to Capability{ supported=false, reason=... }. Emits capabilitiesChanged().
    Q_INVOKABLE void detectCapabilities();

    // ==== end capability-model region ======================================

    // ==== Battery charge-limit controller ==================================

    // Current battery end-charge threshold percent (0..100); -1 when unknown.
    int chargeLimit() const;
    // Live battery charge percent (0..100); -1 when unavailable.
    int batteryCapacity() const;
    // Live battery status string; empty when unavailable.
    QString batteryStatus() const;

    // Re-read the battery charge limit, capacity, and status from sysfs and
    // emit batteryStateChanged() on any change. Read-only; never terminates on a
    // missing attribute (Req 3.4, 3.5).
    Q_INVOKABLE void refreshBattery();

    // Write a new battery end-charge threshold (percent). The value is clamped
    // to a safe [20, 100] range (charge limits below 20% are rejected by most
    // firmware and offer no benefit). Writes via ISysfsAccess::write, reads the
    // value back so the reported/displayed limit is always the kernel's
    // read-back (never the merely requested value), and reports the outcome
    // through recordResult() so exactly one applyResult(...) is emitted. An
    // unavailable control is reported as a failure without any write.
    Q_INVOKABLE void applyChargeLimit(int limitPercent);

    // ==== end battery charge-limit controller ==============================

    // ==== Advanced power tuning (RyzenAdj) — optional/experimental =========

    // Current SMU limits { stapmW, fastW, slowW, apuSlowW, tctlTempC }; a field
    // is -1 when unknown.
    QVariantMap ryzenAdjValues() const;

    // Whether the periodic re-apply loop is running.
    bool ryzenAdjAutoReapply() const;
    void setRyzenAdjAutoReapply(bool enabled);

    // Re-read the SMU limits via `ryzenadj --info` and emit ryzenAdjChanged().
    Q_INVOKABLE void refreshRyzenAdj();

    // Apply advanced SMU limits. Any argument < 0 leaves that limit unchanged.
    // Power limits are in WATTS; tctlTempC in degrees C. Writes via
    // IRyzenAdjAccess::apply, reads the values back, remembers them as the
    // "last applied" set for the re-apply loop, and reports the outcome through
    // recordResult() (exactly one applyResult). An unavailable backend is
    // reported as a failure without any write.
    Q_INVOKABLE void applyRyzenAdj(int stapmW, int fastW, int slowW,
                                   int apuSlowW, int tctlTempC);

    // ==== end advanced power tuning ========================================

    // ==== Telemetry: active profile & GPU mode (task 6.1) ==================

    // Active power profile canonical display name; empty when unknown (Req 4.2).
    QString activePowerProfile() const;

    // Currently active GPU mode name; empty when unknown (Req 5.4, 5.5).
    QString activeGpuMode() const;

    // Runtime-detected supported GPU mode names (Req 5.1-5.3).
    QStringList supportedGpuModes() const;

    // Re-read the active power profile from the platform_profile sysfs
    // attribute. On failure emits readFailed("PowerProfile", message) and
    // leaves the last-known value untouched; never terminates (Req 4.2, 3.4,
    // 3.5).
    Q_INVOKABLE void refreshActiveProfile();

    // Re-read the supported set and currently active GPU mode via the
    // supergfxctl D-Bus service. On failure emits readFailed("GpuMode",
    // message); never terminates (Req 5.1, 5.4, 5.6, 5.7, 5.8, 3.4, 3.5).
    Q_INVOKABLE void refreshGpuMode();

    // ==== end telemetry: active profile & GPU mode =========================

    // ==== Status log ring buffer & apply-result helper (task 7.1) ==========

    // The most recent log entries (capped at 10, oldest first) as a
    // QVariantList of { timestamp, action, success, message } for QML to bind
    // to the Status_Log_Panel (Req 13.3-13.5).
    QVariantList recentLog() const;

    // ==== end status log region ============================================

    // ==== Telemetry: temperature polling (task 6.2) ========================

    // Live CPU temperature in degrees Celsius; NaN when the sensor is
    // unavailable or a read failed (Req 7.1).
    double cpuTemperature() const;

    // Live GPU temperature in degrees Celsius; NaN when unavailable (Req 7.2).
    double gpuTemperature() const;

    // Live fan speeds in RPM (asus hwmon fanN_input); -1 when unavailable.
    int cpuFanRpm() const;
    int gpuFanRpm() const;

    // Calibrated min/max RPM per fan as a QVariantMap
    //   { cpuMin, cpuMax, gpuMin, gpuMax } (-1 when not yet calibrated).
    QVariantMap fanRpmRange() const;

    // Measure a fan's RPM range by briefly sweeping its curve to 0% then 100%
    // and reading fanN_input, then restoring the profile's saved curve. `fan`
    // is "cpu" or "gpu". Requires the direct-sysfs fan-curve backend + root.
    // Reports the outcome (and measured min/max) via recordResult(). This is a
    // blocking sweep of a few seconds; the QML side warns the operator first.
    Q_INVOKABLE void calibrateFan(const QString &fan);

    // Read one poll of CPU/GPU temperatures from hwmon (millidegrees C ->
    // degrees C). A missing/unreadable sensor sets that reading to NaN and
    // emits readFailed(); it never throws or terminates (Req 7.1-7.3, 3.4,
    // 3.5). Also runs threshold-crossing detection against the selected
    // profile's threshold (Req 7.8).
    Q_INVOKABLE void refreshTemperatures();

    // Start/stop the temperature poll timer as the Limits tab shows/hides. The
    // timer fires refreshTemperatures() on an interval in [1000, 2000] ms
    // (Req 7.3). Activating triggers an immediate refresh so the readout is not
    // blank for up to one interval.
    Q_INVOKABLE void setTemperaturePollingActive(bool active);

    // ==== end telemetry: temperature polling ===============================

    // ==== Write path: fan curve (task 9.1) =================================

    // Pure validation helper and single source of truth for fan-curve validity,
    // callable from QML to gate the "Apply Fan Curve" action. Returns a
    // QVariantMap { valid:bool, reason:QString }. A curve is valid iff it has
    // 2..8 points inclusive AND tempC is strictly ascending across the ordered
    // points AND every tempC is in [0,105] AND every percent is in [0,100]
    // (Req 6.3, 6.4, 6.5, 6.6). Has NO side effects and never writes (Req 6.7).
    // Accepts the QVariantList of {tempC, percent} shape QML emits.
    Q_INVOKABLE QVariantMap validateFanCurve(const QVariantList &points) const;

    // Apply the fan curve for a profile. MUST only be reached from the QML
    // confirmation accept handler (Req 6.8-6.10, 9); this method performs no
    // confirmation of its own. Re-validates via validateFanCurve() as the single
    // source of truth and refuses invalid input without writing. On a valid
    // curve it writes via IDBusAccess::setFanCurve, emits exactly one applyResult
    // via recordResult(), and, on success, persists the curve for the profile
    // (Req 6.10, 6.11, 11.1). Every path ends in exactly one recordResult()
    // call. points: QVariantList of {tempC:int, percent:int}.
    // `fan` selects the channel: "cpu" (pwm1, default) or "gpu" (pwm2). Only the
    // selected fan's pwm channel is written and only that fan's persisted curve
    // is updated. The default keeps existing callers/tests (2-arg) working.
    Q_INVOKABLE void applyFanCurve(const QString &profile,
                                   const QVariantList &points,
                                   const QString &fan = QStringLiteral("cpu"));

    // ==== end write path: fan curve ========================================

    // ==== Write path: GPU mode (task 10.2) =================================

    // Apply a GPU mode by name. This is the ONLY write path for GPU mode and is
    // reachable exclusively from the QML confirmation dialog's accept handler
    // (Req 5.9, 5.10, 9.1-9.3); it performs no confirmation of its own. Refuses
    // a mode absent from the runtime-detected supported set without writing
    // (Req 5.2, 5.3). On a supported mode it writes via IDBusAccess::setGpuMode
    // and emits exactly one applyResult via recordResult() (Req 5.11, 9.4). On
    // success it performs a read-back of the active GPU mode and updates
    // activeGpuMode only after that read-back succeeds (Req 10.1, 10.2); on
    // failure the last-known activeGpuMode is retained so the selector, bound to
    // activeGpuMode, reverts to the previously active mode (Req 5.12, 5.13).
    // Every path ends in exactly one recordResult() call.
    Q_INVOKABLE void applyGpuMode(const QString &modeName);

    // ==== end write path: GPU mode =========================================

    // ==== Write path: temperature threshold (task 10.4) ===================

    // Persist a temperature warning threshold (degrees C) for a profile
    // (Req 11.2, 12.4). This is soft-limit metadata only: it performs NO
    // hardware write. The value is constrained to [0, 105] (Req 7.4), stored in
    // the in-memory per-profile settings, written through ISettingsStore::save,
    // and reported via recordResult() so exactly one applyResult(...) is emitted
    // and exactly one LogEntry appended (Req 2.5, 13.3). An unknown profile name
    // is reported as a failure without persisting.
    Q_INVOKABLE void applyTemperatureThreshold(const QString &profile, int thresholdC);

    // ==== end write path: temperature threshold ============================

    // ==== Write path: power limits (task 10.3) =============================

    // Write PL1/PL2 (watts) for a profile, then read back the applied values
    // from the driver and report any discrepancy (Req 8.6-8.10, 10.1-10.3).
    // MUST only be reached from the QML "Apply Power Limits" action (Req 8.6,
    // 8.7); this method performs no confirmation of its own. Flow:
    //   1. Resolve the profile name to a canonical profile; an unknown name is a
    //      failure reported via recordResult() without any write.
    //   2. Resolve the PL1/PL2 sysfs paths (asus-nb-wmi primary, asus-armoury
    //      relocation); a missing path is an Unsupported failure (Req 8.9).
    //   3. Write each value via ISysfsAccess::write(path, value, timeoutMs) with
    //      a bounded timeout to survive the PL1/PL2 blocking-write caveat. Any
    //      Timeout/IoError/PermissionDenied/Unsupported outcome is reported via
    //      recordResult() with a specific message (Req 8.9); a Timeout uses the
    //      exact "Power limit write timed out (known driver issue on some
    //      kernels)" message and still performs a read-back (design caveat).
    //   4. On a successful write, read back PL1/PL2 from the driver (Req 8.8,
    //      10.1). The reported/persisted value is always the driver read-back,
    //      never the merely requested value (Req 8.10, 10.2). When read-back
    //      differs from the request, the applyResult message states the
    //      discrepancy (Req 8.10, 10.3).
    //   5. Persist the confirmed values for the profile on success (Req 11.3),
    //      keeping other per-profile settings intact.
    // Every return path funnels through recordResult() exactly once.
    Q_INVOKABLE void applyPowerLimits(const QString &profile, int pl1Watts,
                                      int pl2Watts);

    // ==== end write path: power limits =====================================

    // ==== Write path: power profile (task 10.1) ============================

    // Apply a power profile by name. This is the ONLY write path for the active
    // power profile and is reachable from the QML Power_Profile selector
    // (Req 4.4). Flow:
    //   1. Resolve the profile name to a canonical PowerProfile; an unknown name
    //      is a failure reported via recordResult() without any write.
    //   2. Resolve the platform_profile sysfs path; a missing/unwritable
    //      attribute is a failure reported via recordResult() without writing.
    //   3. Write the mapped platform string (PowerProfile -> platform string via
    //      the DataModels.h mapping table) through ISysfsAccess::write with a
    //      bounded timeout. Any non-Ok WriteOutcome is reported via
    //      recordResult() with a specific message (Req 4.6).
    //   4. On a successful write, read back the active profile via
    //      refreshActiveProfile() (Req 4.5, 10.1). The reported value is the
    //      driver read-back, never the merely requested value (Req 10.2); when
    //      the read-back differs from the request the applyResult message states
    //      the discrepancy (Req 10.3).
    // Persists nothing: the power profile is live platform state, not per-profile
    // persisted settings. Every return path funnels through recordResult()
    // exactly once so exactly one applyResult(...) is emitted (Req 4.5).
    Q_INVOKABLE void setPowerProfile(const QString &profileName);

    // ==== end write path: power profile ====================================

public slots:
    void refreshSensors();

signals:
    // Selected profile changed (Req 12).
    void selectedProfileChanged();

    // Persisted settings finished loading; carries a per-profile failure summary
    // if any (Req 11.4/11.5).
    void persistedSettingsLoaded(const QString &message);

    // Capability model changed (after detect / refresh) (Req 3).
    void capabilitiesChanged();

    // ==== Telemetry: active profile & GPU mode (task 6.1) ==================

    // Active power profile changed after a refresh (Req 4.2).
    void activePowerProfileChanged();

    // Active GPU mode changed after a refresh (Req 5.4-5.7).
    void activeGpuModeChanged();

    // Supported GPU mode set changed after a refresh (Req 5.1-5.3).
    void supportedGpuModesChanged();

    // A read operation failed; carries a control id + human-readable message
    // for a <=2s UI banner so the failing control/operation is identified
    // without terminating (Req 3.4, 3.5). Shared by all read-only telemetry
    // paths.
    void readFailed(const QString &controlId, const QString &message);

    // ==== end telemetry: active profile & GPU mode =========================

    // ==== Status log ring buffer & apply-result helper (task 7.1) ==========

    // Outcome of any Write_Operation, emitted by every write path via
    // recordResult() (Req 2.5).
    void applyResult(bool success, const QString &message);

    // Log list updated; also exposed via the recentLog property (Req 13.3-13.5).
    void recentLogChanged();

    // ==== end status log region ============================================

    // Battery charge limit / capacity / status changed after a refresh or a
    // successful applyChargeLimit().
    void batteryStateChanged();

    // RyzenAdj values or auto-reapply state changed.
    void ryzenAdjChanged();

    // ==== Telemetry: temperature polling (task 6.2) ========================

    // Live CPU/GPU temperature change notifications (Req 7.1, 7.2).
    void cpuTemperatureChanged();
    void gpuTemperatureChanged();

    // Live fan RPM changed, and calibrated RPM range changed.
    void fanRpmChanged();
    void fanRpmRangeChanged();

    // NOTE: readFailed() is declared once in the task 6.1 telemetry region and
    // reused here for temperature read failures (Req 3.5).

    // A live temperature transitioned into the crossing state (met or exceeded
    // the selected profile's threshold); carries the sensor name, value in
    // degrees C, and an ISO-8601 local timestamp (Req 7.7, 7.8).
    void temperatureThresholdCrossed(const QString &sensor, double valueC,
                                     const QString &timestamp);

    // ==== end telemetry: temperature polling ===============================

private:
    // Load persisted settings for every canonical profile from the settings
    // store, applying built-in defaults for any profile whose block is missing
    // or unreadable, and emit persistedSettingsLoaded(message) with a summary of
    // any fallbacks (Req 11.4, 11.5, 11.7). Never terminates on failure.
    void loadPersistedSettings();

    // Built-in default settings for a canonical profile (used when no persisted
    // block exists or it is unreadable).
    static ProfileSettings defaultSettingsFor(PowerProfile profile);

    // Convert a stored FanCurve into the QVariantList of {tempC, percent} shape
    // QML consumes.
    static QVariantList fanCurveToVariantList(const FanCurve &curve);

    // ==== Write path: fan curve (task 9.1) =================================
    // Convert the QVariantList of {tempC, percent} shape QML passes into a
    // FanCurve for persistence. Assumes the list has already passed
    // validateFanCurve(); missing/unparseable fields coerce to 0.
    static FanCurve fanCurveFromVariantList(const QVariantList &points);

    // Resolve the asus_custom_fan_curve hwmon directory (kernel-native
    // fan-curve control), or std::nullopt when the driver is not present. The
    // directory exposes pwm{1,2}_auto_point{1..8}_{temp,pwm} attributes.
    std::optional<QString> resolveFanCurveHwmonDir() const;

    // True when the kernel asus_custom_fan_curve driver is present (the direct
    // sysfs fan-curve backend is usable). Consulted by detectFanCurve() as a
    // fallback when asusd is not available, and by applyFanCurve().
    bool sysfsFanCurveAvailable() const;

    // Write a validated fan curve directly to the asus_custom_fan_curve hwmon
    // attributes for a channel (fanIndex: 1 => CPU pwm1, 2 => GPU pwm2). The
    // curve is padded/truncated to the driver's fixed 8 points, temperatures are
    // written in degrees C, and percents are converted to the driver's 0..255
    // PWM scale. Returns the first non-Ok WriteOutcome encountered, or Ok when
    // every point write succeeded. Assumes the curve already passed
    // validateFanCurve().
    WriteOutcome writeFanCurveToSysfs(const QString &hwmonDir, int fanIndex,
                                      const QVariantList &points);

    // Bounded timeout (ms) for a single fan-curve point write. Kept short: these
    // EC attributes do not exhibit the PL1/PL2 blocking-write pathology.
    static constexpr int kFanCurveWriteTimeoutMs = 2000;
    // ==== end write path: fan curve ========================================

    // ==== Write path: power limits (task 10.3) =============================
    // Resolve the sysfs path for a PL attribute, preferring the asus-nb-wmi
    // location and falling back to the asus-armoury relocation. Returns
    // std::nullopt when neither path exists (Req 8.9). Mirrors the resolution
    // used by detectPowerLimits().
    std::optional<QString> resolvePl1Path() const;
    std::optional<QString> resolvePl2Path() const;

    // Bounded timeout (ms) applied to PL1/PL2 sysfs writes so a blocking write
    // does not hang the UI; on timeout the write is treated as fallible and a
    // read-back is still performed (design PL1/PL2 blocking-write caveat).
    static constexpr int kPowerLimitWriteTimeoutMs = 3000;

    // Read back a single PL attribute as an integer watt value, or std::nullopt
    // when the path is missing/unreadable/unparseable (Req 8.8).
    std::optional<int> readBackPowerLimit(const QString &path) const;
    // ==== end write path: power limits =====================================

    ISysfsAccess *m_sysfs = nullptr;
    IDBusAccess *m_dbus = nullptr;
    ISettingsStore *m_settings = nullptr;
    // Optional advanced power-tuning backend (ryzenadj CLI). May be nullptr when
    // not injected/allocated, in which case the RyzenAdj control is unavailable.
    IRyzenAdjAccess *m_ryzenAdj = nullptr;

    // True only when this instance allocated the accessors itself (default
    // ctor) and is therefore responsible for deleting them.
    bool m_ownsAccessors = false;

    // The UI-selected profile scoping per-profile settings (Req 12). Defaults to
    // the canonical Balanced profile.
    QString m_selectedProfile;

    // Persisted per-profile settings, keyed by canonical display name
    // ("Quiet"/"Balanced"/"Performance"). Kept distinct from live hardware
    // values (Req 11.6).
    QMap<QString, ProfileSettings> m_persistedSettings;

    // ==== Capability-model region (task 5.1) ================================

    // Per-control capability records populated by detectCapabilities(). Ordered
    // so `capabilities()` emits a stable map. Keyed by ControlId.
    std::map<ControlId, Capability> m_capabilities;

    // Whether a real driver throttle target backs the temperature threshold
    // (Req 7.5/7.6). Defaults to a Boreas-side soft limit.
    bool m_tempThresholdIsHardwareLimit = false;

    // Discovered PL1/PL2 bounds surfaced via powerLimitBounds() (Req 8.4/8.5).
    // When the driver does not expose discoverable bounds these fall back to the
    // conservative estimate 1..200 W flagged estimated=true (Req 8.5).
    int m_powerLimitMinWatts = 1;
    int m_powerLimitMaxWatts = 200;
    bool m_powerLimitBoundsEstimated = true;

    // ==== Battery charge-limit controller ==================================
    // Cached battery state, refreshed by refreshBattery()/detectBatteryChargeLimit()
    // and after a successful applyChargeLimit(). -1 / empty means unavailable.
    int m_chargeLimit = -1;
    int m_batteryCapacity = -1;
    QString m_batteryStatus;
    // Minimum end-charge threshold we allow writing. Firmware typically rejects
    // very low limits and they offer no longevity benefit.
    static constexpr int kMinChargeLimit = 20;
    // Bounded timeout (ms) for the charge-limit write.
    static constexpr int kChargeLimitWriteTimeoutMs = 2000;
    // ==== end battery charge-limit controller ==============================

    // ==== Advanced power tuning (RyzenAdj) — optional/experimental =========
    // Last SMU limits read back from `ryzenadj --info`; fields -1 when unknown.
    RyzenAdjSettings m_ryzenAdjValues;
    // The last settings the operator applied, re-applied periodically because
    // the SMU resets volatile limits. Empty (hasAny()==false) until first apply.
    RyzenAdjSettings m_ryzenAdjLastApplied;
    // Periodic re-apply timer + interval. Off by default; enabled via
    // setRyzenAdjAutoReapply(true). Parented to this.
    QTimer *m_ryzenAdjReapplyTimer = nullptr;
    static constexpr int kRyzenAdjReapplyIntervalMs = 10000;
    // Silently re-apply the last-applied settings (no log entry) for the timer.
    void reapplyRyzenAdj();

    // Load a profile's persisted RyzenAdj limits into m_ryzenAdjLastApplied /
    // m_ryzenAdjValues (so the UI shows them). Does not write hardware.
    void loadRyzenAdjForProfile(const QString &profile);

    // Called once at construction (after settings load + capability detect) to
    // re-apply persisted state so it survives restarts: the selected profile's
    // RyzenAdj limits and the global battery charge limit. Best-effort and
    // silent (no log spam); safe when the backends are unavailable.
    void reapplyPersistedOnStartup();
    // ==== end advanced power tuning ========================================

    // Individual per-control detection probes. Each returns a Capability and,
    // where relevant, updates the associated bound/flag members. All are
    // failure-tolerant (Req 3.4).
    Capability detectPowerProfile() const;
    Capability detectGpuMode() const;
    Capability detectFanCurve() const;
    Capability detectTemperatureRead() const;   // CPU/GPU probed separately
    Capability detectTemperatureThreshold();     // sets m_tempThresholdIsHardwareLimit
    Capability detectPowerLimits();               // sets bound/estimated members
    Capability detectBatteryChargeLimit();        // sets battery cache members
    Capability detectRyzenAdj();                  // optional advanced backend

    // ==== end capability-model region ======================================

    // ==== Telemetry: active profile & GPU mode (task 6.1) ==================

    // Last successfully read active power profile (canonical display name);
    // empty until the first successful read (Req 4.2).
    QString m_activePowerProfile;

    // Last successfully read active GPU mode name; empty until the first
    // successful read (Req 5.4, 5.5).
    QString m_activeGpuMode;

    // Last successfully read supported GPU mode list (Req 5.1-5.3).
    QStringList m_supportedGpuModes;

    // ==== end telemetry: active profile & GPU mode =========================

    // ==== Status log ring buffer & apply-result helper (task 7.1) ==========

    // Maximum number of retained log entries (Req 13.4/13.5).
    static constexpr int kMaxLogEntries = 10;

    // Ring buffer of the most recent log entries, oldest first, capped at
    // kMaxLogEntries. The single write-path helper recordResult() appends here
    // and trims the oldest overflow (Req 13.3-13.5).
    QVector<LogEntry> m_recentLog;

    // Single helper every write path calls to report an outcome: emits exactly
    // one applyResult(success, message), appends exactly one LogEntry (stamped
    // with the current ISO-8601 local time), trims to the last kMaxLogEntries,
    // and emits recentLogChanged() (Req 2.5, 13.3-13.5).
    void recordResult(const QString &action, bool success, const QString &message);

    // ==== end status log region ============================================

    // ==== Telemetry: temperature polling (task 6.2) ========================

    // Live readings in degrees Celsius. NaN means "unavailable"; QML renders a
    // dash instead of a stale value (Req 7.1, 7.2). Default to NaN until the
    // first successful poll.
    double m_cpuTemperature = std::numeric_limits<double>::quiet_NaN();
    double m_gpuTemperature = std::numeric_limits<double>::quiet_NaN();

    // Live fan RPM (asus hwmon fanN_input); -1 when unavailable. Cached path to
    // the asus fan hwmon dir (resolved by name, like the temperature sensors).
    int m_cpuFanRpm = -1;
    int m_gpuFanRpm = -1;
    mutable QString m_fanHwmonDir;   // dir of the "asus" hwmon exposing fanN_input
    // Calibrated RPM range per fan; -1 until calibrated.
    int m_cpuFanRpmMin = -1;
    int m_cpuFanRpmMax = -1;
    int m_gpuFanRpmMin = -1;
    int m_gpuFanRpmMax = -1;
    // Resolve (and cache) the asus fan hwmon dir exposing fanN_input.
    std::optional<QString> resolveFanRpmHwmonDir() const;
    // Read fanN_input RPM for a fan index (1=cpu, 2=gpu); -1 on failure.
    int readFanRpm(int fanIndex) const;
    // Refresh cpu/gpu fan RPM and emit fanRpmChanged() on change.
    void refreshFanRpm();

    // Poll timer driving refreshTemperatures() while the Limits tab is visible;
    // interval kept in [1000, 2000] ms (Req 7.3). Parented to this so it is
    // destroyed with the manager.
    QTimer *m_tempPollTimer = nullptr;

    // Poll interval in milliseconds, held within the required [1000, 2000] range.
    static constexpr int kTempPollIntervalMs = 1500;

    // hwmon sensors are resolved dynamically by device `name` + `tempN_label`
    // rather than by hardcoded hwmonN index: the kernel assigns hwmon indices in
    // driver-probe order and they are NOT stable across boots, so hardcoding
    // them pointed CPU/GPU reads at the wrong (or input-less) device. Values are
    // millidegrees Celsius.
    //
    // Name/label priority lists passed to ISysfsAccess::resolveHwmonInput().
    static const QStringList kCpuHwmonNames;    // e.g. {"k10temp","coretemp"}
    static const QStringList kCpuHwmonLabels;   // e.g. {"Tctl","Package id 0",...}
    static const QStringList kGpuHwmonNames;    // e.g. {"amdgpu","nvidia"}
    static const QStringList kGpuHwmonLabels;   // e.g. {"edge","junction"}

    // Lazily-resolved + cached hwmon tempN_input paths. Empty until first
    // resolution. Re-resolved if the cached path stops existing.
    mutable QString m_cpuTempInputPath;
    mutable QString m_gpuTempInputPath;

    // Resolve (and cache) the CPU/GPU hwmon tempN_input paths. Returns the
    // cached path when still valid, otherwise re-resolves via
    // ISysfsAccess::resolveHwmonInput(). Returns std::nullopt when no sensor
    // resolves.
    std::optional<QString> cpuTempInputPath() const;
    std::optional<QString> gpuTempInputPath() const;

    // Previous crossing state per sensor so temperatureThresholdCrossed() fires
    // only on the transition INTO the crossing state, not every poll while hot
    // (Req 7.8). false = below threshold / unknown.
    bool m_cpuAboveThreshold = false;
    bool m_gpuAboveThreshold = false;

    // Read one hwmon millidegree input and convert to degrees C, or NaN if the
    // path is missing/unreadable/unparseable. On failure emits readFailed() with
    // the given sensor label (Req 3.4, 3.5).
    double readTemperatureC(const QString &path, const QString &sensorLabel);

    // Read a dynamically-resolved sensor: a std::nullopt path (nothing
    // resolved) is treated as an unavailable sensor (readFailed + NaN),
    // otherwise delegates to readTemperatureC(). (Req 3.4, 3.5, 7.1, 7.2).
    double readResolvedTemperatureC(const std::optional<QString> &resolvedPath,
                                    const QString &sensorLabel);

    // Update the crossing state for one sensor and emit
    // temperatureThresholdCrossed() on a fresh transition into crossing (Req 7.8).
    void evaluateThresholdCrossing(const QString &sensor, double valueC,
                                   int thresholdC, bool &prevAbove);

    // ==== end telemetry: temperature polling ===============================
};
