#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <optional>

// Core data models shared across the platform-access layer and HardwareManager.

// A single fan curve control point.
struct FanCurvePoint {
    int tempC;     // 0..105 inclusive (Req 6.4)
    int percent;   // 0..100 inclusive (Req 6.5)
};

// A FanCurve is an ordered list of 2..8 points with strictly ascending tempC
// (Req 6.2-6.4).
using FanCurve = QVector<FanCurvePoint>;

// Boreas's canonical set of power profiles (Req 4.1). GPU modes are deliberately
// NOT modelled as an enum: they are dynamic QString names reported at runtime
// (Req 5.1-5.3), so no fixed enum exists for them.
enum class PowerProfile { Quiet, Balanced, Performance };

// Platform-string mapping table. platform_profile_choices naming varies by
// platform (e.g. "quiet"/"balanced"/"performance" vs "low-power"/"cool"), so the
// canonical PowerProfile enum is translated to/from whatever the platform
// reports through this table. Each entry lists the canonical profile, its
// display name, and the platform strings it maps to (first is the preferred
// value to write; the rest are additional accepted aliases when matching a
// platform-reported string).
struct PowerProfileMapping {
    PowerProfile profile;
    const char *displayName;
    std::array<const char *, 3> platformStrings;  // nullptr-terminated within the array
};

inline const std::array<PowerProfileMapping, 3> &powerProfileMappings() {
    static const std::array<PowerProfileMapping, 3> table = {{
        {PowerProfile::Quiet,       "Quiet",       {"quiet",       "low-power", nullptr}},
        {PowerProfile::Balanced,    "Balanced",    {"balanced",    "cool",      nullptr}},
        {PowerProfile::Performance, "Performance", {"performance", "perform",   nullptr}},
    }};
    return table;
}

// Canonical display name for a PowerProfile (e.g. "Quiet").
inline QString powerProfileName(PowerProfile profile) {
    for (const auto &m : powerProfileMappings()) {
        if (m.profile == profile)
            return QString::fromLatin1(m.displayName);
    }
    return QString();
}

// Preferred platform string to write for a PowerProfile (e.g. "quiet").
inline QString powerProfilePlatformString(PowerProfile profile) {
    for (const auto &m : powerProfileMappings()) {
        if (m.profile == profile)
            return QString::fromLatin1(m.platformStrings[0]);
    }
    return QString();
}

// Map a canonical display name back to a PowerProfile (case-insensitive).
inline std::optional<PowerProfile> powerProfileFromName(const QString &name) {
    for (const auto &m : powerProfileMappings()) {
        if (name.compare(QString::fromLatin1(m.displayName), Qt::CaseInsensitive) == 0)
            return m.profile;
    }
    return std::nullopt;
}

// Map a platform-reported string (any accepted alias) to a PowerProfile
// (case-insensitive).
inline std::optional<PowerProfile> powerProfileFromPlatformString(const QString &platformString) {
    for (const auto &m : powerProfileMappings()) {
        for (const char *alias : m.platformStrings) {
            if (alias == nullptr)
                break;
            if (platformString.compare(QString::fromLatin1(alias), Qt::CaseInsensitive) == 0)
                return m.profile;
        }
    }
    return std::nullopt;
}

// Per-profile persisted settings unit (Req 11, 12). These are the operator's
// *desired* settings and are kept distinct from live hardware values (Req 11.6):
// live values come from telemetry properties, persisted values from
// SettingsStore.
struct ProfileSettings {
    // fanCurve is the CPU fan curve (pwm1). gpuFanCurve is the GPU fan curve
    // (pwm2). They are edited and applied independently (CPU/GPU toggle in the
    // editor). Older persisted files only carry `fanCurve`; when gpuFanCurve is
    // empty on load it falls back to the CPU curve / built-in default so nothing
    // breaks (Req 11.1, 12).
    FanCurve fanCurve;      // CPU fan (pwm1)
    FanCurve gpuFanCurve;   // GPU fan (pwm2)
    int tempThresholdC;     // 0..105, warning/soft limit (Req 11.2, 12)
    int pl1Watts;           // sustained (Req 11.3, 12)
    int pl2Watts;           // boost (Req 11.3, 12)

    // Per-profile RyzenAdj SMU limits (watts / degrees C). -1 means "unset /
    // leave unchanged". Persisted so a profile's advanced power tuning is
    // remembered across restarts and re-applied on startup. Mirrors
    // RyzenAdjSettings but lives inside the per-profile block.
    int ryzenStapmW = -1;
    int ryzenFastW = -1;
    int ryzenSlowW = -1;
    int ryzenApuSlowW = -1;
    int ryzenTctlC = -1;
};

// Advanced AMD power-tuning settings applied via the optional RyzenAdj backend.
// These are the SMU (System Management Unit) limits RyzenAdj exposes and which
// the in-kernel ppt_* sysfs nodes do NOT: STAPM (sustained), fast/slow PPT,
// APU-slow, and the Tctl thermal limit. Each field is optional: a value < 0
// means "leave this limit unchanged" so the operator can tune one knob without
// disturbing the others. Power limits are stored in WATTS here and converted to
// RyzenAdj's milliwatt units at the CLI boundary; tctlTempC is in degrees C.
//
// IMPORTANT: SMU limits are volatile — the firmware resets them periodically
// and after suspend/resume, so a re-apply loop is required to keep them in
// effect (this is inherent to RyzenAdj, not a Boreas limitation).
struct RyzenAdjSettings {
    int stapmLimitW = -1;    // --stapm-limit (sustained)
    int fastLimitW = -1;     // --fast-limit  (boost/fast PPT)
    int slowLimitW = -1;     // --slow-limit  (slow PPT)
    int apuSlowLimitW = -1;  // --apu-slow-limit
    int tctlTempC = -1;      // --tctl-temp   (thermal limit, degrees C)

    // True when at least one field is set (>= 0), i.e. there is something to
    // apply / re-apply.
    bool hasAny() const {
        return stapmLimitW >= 0 || fastLimitW >= 0 || slowLimitW >= 0
            || apuSlowLimitW >= 0 || tctlTempC >= 0;
    }
};

// A single status-log entry. HardwareManager keeps a ring buffer capped at 10
// entries (Req 13.4/13.5) exposed via recentLog.
struct LogEntry {
    QString timestamp;   // ISO-8601 local time
    QString action;      // e.g. "Apply PL1/PL2 (Performance)"
    bool success;
    QString message;     // detail incl. read-back discrepancy if any
};

// -----------------------------------------------------------------------------
// Capability model (Req 3)
// -----------------------------------------------------------------------------
//
// Each control is capability-detected at runtime. HardwareManager keeps one
// Capability record per ControlId and exposes them via the `capabilities`
// property so QML can bind enabled/visible + reason text and produce the
// Unavailable_State for unsupported controls (Req 3.2, 3.3, 3.7).
enum class ControlId {
    PowerProfile,
    GpuMode,
    FanCurve,
    TemperatureRead,       // aggregate (CPU and GPU probed separately below)
    TemperatureThreshold,
    PowerLimits,
    BatteryChargeLimit,    // battery end-charge threshold (global, not per-profile)
    RyzenAdj               // optional advanced AMD SMU power tuning (ryzenadj CLI)
};

struct Capability {
    bool supported = false;   // false => Unavailable_State in QML (Req 3.2)
    QString reason;           // shown verbatim when unsupported (Req 3.3)
};

// Canonical string keys used for the `capabilities` QVariantMap so QML binds to
// stable identifiers. CPU/GPU temperature reads are surfaced separately so a
// GPU sensor can be Unavailable while CPU works (see design Capability model).
inline QString controlIdKey(ControlId id) {
    switch (id) {
    case ControlId::PowerProfile:         return QStringLiteral("PowerProfile");
    case ControlId::GpuMode:              return QStringLiteral("GpuMode");
    case ControlId::FanCurve:             return QStringLiteral("FanCurve");
    case ControlId::TemperatureRead:      return QStringLiteral("TemperatureRead");
    case ControlId::TemperatureThreshold: return QStringLiteral("TemperatureThreshold");
    case ControlId::PowerLimits:          return QStringLiteral("PowerLimits");
    case ControlId::BatteryChargeLimit:   return QStringLiteral("BatteryChargeLimit");
    case ControlId::RyzenAdj:             return QStringLiteral("RyzenAdj");
    }
    return QString();
}
