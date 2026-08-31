#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <optional>

struct FanCurvePoint {
    int tempC;    // 0..105
    int percent;  // 0..100
};

using FanCurve = QVector<FanCurvePoint>;

enum class PowerProfile { Quiet, Balanced, Performance };

struct PowerProfileMapping {
    PowerProfile profile;
    const char *displayName;
    std::array<const char *, 3> platformStrings;
};

inline const std::array<PowerProfileMapping, 3> &powerProfileMappings() {
    static const std::array<PowerProfileMapping, 3> table = {{
        {PowerProfile::Quiet,       "Quiet",       {"quiet",       "low-power", nullptr}},
        {PowerProfile::Balanced,    "Balanced",    {"balanced",    "cool",      nullptr}},
        {PowerProfile::Performance, "Performance", {"performance", "perform",   nullptr}},
    }};
    return table;
}

inline QString powerProfileName(PowerProfile profile) {
    for (const auto &m : powerProfileMappings()) {
        if (m.profile == profile)
            return QString::fromLatin1(m.displayName);
    }
    return QString();
}

inline QString powerProfilePlatformString(PowerProfile profile) {
    for (const auto &m : powerProfileMappings()) {
        if (m.profile == profile)
            return QString::fromLatin1(m.platformStrings[0]);
    }
    return QString();
}

inline std::optional<PowerProfile> powerProfileFromName(const QString &name) {
    for (const auto &m : powerProfileMappings()) {
        if (name.compare(QString::fromLatin1(m.displayName), Qt::CaseInsensitive) == 0)
            return m.profile;
    }
    return std::nullopt;
}

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

// Per-profile persisted settings. Kept distinct from live hardware values —
// live values come from telemetry properties, persisted values from SettingsStore.
struct ProfileSettings {
    FanCurve fanCurve;       // CPU fan (pwm1)
    FanCurve gpuFanCurve;    // GPU fan (pwm2)
    int tempThresholdC;
    int pl1Watts;
    int pl2Watts;

    // Per-profile RyzenAdj SMU limits in watts / degrees C. -1 = unset.
    int ryzenStapmW = -1;
    int ryzenFastW = -1;
    int ryzenSlowW = -1;
    int ryzenApuSlowW = -1;
    int ryzenTctlC = -1;
};

// AMD SMU power limits applied via the optional RyzenAdj backend.
// Values < 0 mean "leave unchanged". Power limits are in watts; tctlTempC in degrees C.
// SMU limits are volatile — firmware resets them on resume, requiring a re-apply loop.
struct RyzenAdjSettings {
    int stapmLimitW = -1;
    int fastLimitW = -1;
    int slowLimitW = -1;
    int apuSlowLimitW = -1;
    int tctlTempC = -1;

    bool hasAny() const {
        return stapmLimitW >= 0 || fastLimitW >= 0 || slowLimitW >= 0
            || apuSlowLimitW >= 0 || tctlTempC >= 0;
    }
};

struct LogEntry {
    QString timestamp;
    QString action;
    bool success;
    QString message;
};

enum class ControlId {
    PowerProfile,
    GpuMode,
    FanCurve,
    TemperatureRead,
    TemperatureThreshold,
    PowerLimits,
    BatteryChargeLimit,
    RyzenAdj
};

struct Capability {
    bool supported = false;
    QString reason;
};

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
