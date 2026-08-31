#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <optional>

#include "DataModels.h"

// Common result for fallible writes: distinguishes success, timeout, io-error,
// unsupported, and permission-denied outcomes.
struct WriteOutcome {
    enum class Status { Ok, Timeout, IoError, Unsupported, PermissionDenied };
    Status status;
    QString message;
};

// Abstracts /sys reads and writes plus existence/permission probing.
class ISysfsAccess {
public:
    virtual ~ISysfsAccess() = default;
    virtual bool exists(const QString &path) const = 0;
    virtual bool isWritable(const QString &path) const = 0;
    virtual std::optional<QString> read(const QString &path) const = 0;
    // Write with a timeout to survive the known PL1/PL2 blocking-write bug.
    virtual WriteOutcome write(const QString &path, const QString &value,
                               int timeoutMs) = 0;

    // Resolve a hwmon tempN_input path dynamically by device name + label,
    // instead of relying on unstable hwmonN indices (which the kernel assigns
    // in driver-probe order and are NOT stable across boots).
    //
    // Scans /sys/class/hwmon/hwmon*, reading each hwmon's `name` attribute. For
    // the first hwmon whose name matches an entry in namesInPriority (tried in
    // order), it finds the tempN_input whose sibling tempN_label matches one of
    // labelsInPriority (case-insensitive). If the matching hwmon exposes no
    // matching label, it falls back to that hwmon's temp1_input. Returns the
    // absolute path to the chosen tempN_input, or std::nullopt if no name in
    // namesInPriority resolves to a usable input.
    virtual std::optional<QString> resolveHwmonInput(
        const QStringList &namesInPriority,
        const QStringList &labelsInPriority) const = 0;

    // Resolve the directory of a hwmon device by its reported `name` attribute,
    // trying namesInPriority in order. Scans /sys/class/hwmon/hwmon*, reading
    // each `name` file, and returns the absolute path of the first hwmon
    // directory whose name matches (e.g. "asus_custom_fan_curve" ->
    // "/sys/class/hwmon/hwmon6"). Unlike resolveHwmonInput this returns the
    // device DIRECTORY (not a tempN_input file) so callers can address arbitrary
    // sibling attributes such as pwm*_auto_point*_{temp,pwm}. Returns
    // std::nullopt when no name resolves. hwmonN indices are NOT stable across
    // boots, so this name-based resolution is the supported way to locate it.
    virtual std::optional<QString> resolveHwmonDir(
        const QStringList &namesInPriority) const = 0;
};

// Abstracts the D-Bus calls to supergfxctl and asusd.
class IDBusAccess {
public:
    virtual ~IDBusAccess() = default;
    virtual bool serviceAvailable(const QString &service) const = 0;
    virtual std::optional<QStringList> supportedGpuModes() const = 0;
    virtual std::optional<QString> activeGpuMode() const = 0;
    virtual WriteOutcome setGpuMode(const QString &mode) = 0;
    virtual bool fanCurvesSupported() const = 0;
    virtual WriteOutcome setFanCurve(const QString &profile,
                                     const QVariantList &points) = 0;
};

// Abstracts the optional RyzenAdj backend (AMD SMU power tuning). The concrete
// implementation shells out to the `ryzenadj` CLI; a fake is used in tests. All
// methods are failure-tolerant and never throw. Power limits cross this boundary
// in WATTS (RyzenAdjSettings); the implementation converts to RyzenAdj's
// milliwatt CLI units internally.
class IRyzenAdjAccess {
public:
    virtual ~IRyzenAdjAccess() = default;

    // True when the RyzenAdj backend is usable (e.g. the `ryzenadj` binary is
    // present). When false the RyzenAdj control is reported unavailable.
    virtual bool available() const = 0;

    // Apply the given SMU limits. Only fields >= 0 are passed through; a
    // settings value of -1 leaves that limit unchanged. Returns a WriteOutcome
    // reflecting the CLI exit status (Ok / PermissionDenied / Timeout /
    // Unsupported / IoError with a message).
    virtual WriteOutcome apply(const RyzenAdjSettings &settings) = 0;

    // Read the SMU's current limits back (parsed from `ryzenadj -i`), or
    // std::nullopt when the info read fails. Values are returned in WATTS /
    // degrees C to match RyzenAdjSettings.
    virtual std::optional<RyzenAdjSettings> readInfo() const = 0;
};

// Abstracts persistence (JSON file or QSettings) so round-trip is unit-testable.
class ISettingsStore {
public:
    virtual ~ISettingsStore() = default;
    virtual std::optional<ProfileSettings> load(const QString &profile) const = 0;
    virtual bool save(const QString &profile, const ProfileSettings &s) = 0;

    // Global (not per-profile) persisted battery end-charge threshold percent.
    // Returns std::nullopt when never saved. There is one battery, so this is a
    // single global value rather than per-profile. Default no-op implementations
    // keep simple test doubles working without overriding.
    virtual std::optional<int> loadChargeLimit() const { return std::nullopt; }
    virtual bool saveChargeLimit(int /*percent*/) { return false; }
};
