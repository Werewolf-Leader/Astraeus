#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <optional>

#include "DataModels.h"

struct WriteOutcome {
    enum class Status { Ok, Timeout, IoError, Unsupported, PermissionDenied };
    Status status;
    QString message;
};

// Abstracts sysfs reads, writes, and hwmon resolution.
class ISysfsAccess {
public:
    virtual ~ISysfsAccess() = default;
    virtual bool exists(const QString &path) const = 0;
    virtual bool isWritable(const QString &path) const = 0;
    virtual std::optional<QString> read(const QString &path) const = 0;
    // Write with a timeout to survive blocking-write driver bugs.
    virtual WriteOutcome write(const QString &path, const QString &value,
                               int timeoutMs) = 0;

    // Resolve a hwmon tempN_input path by device name + label (avoids unstable
    // hwmonN indices which the kernel assigns in driver-probe order).
    virtual std::optional<QString> resolveHwmonInput(
        const QStringList &namesInPriority,
        const QStringList &labelsInPriority) const = 0;

    // Resolve a hwmon device directory by its reported `name` attribute.
    // Returns the directory path (e.g. "/sys/class/hwmon/hwmon6"), not a file.
    virtual std::optional<QString> resolveHwmonDir(
        const QStringList &namesInPriority) const = 0;
};

// Abstracts D-Bus calls to supergfxctl and asusd.
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

// Abstracts the optional RyzenAdj backend. Power limits cross this boundary
// in watts; the implementation converts to ryzenadj's milliwatt CLI units.
class IRyzenAdjAccess {
public:
    virtual ~IRyzenAdjAccess() = default;
    virtual bool available() const = 0;
    virtual WriteOutcome apply(const RyzenAdjSettings &settings) = 0;
    virtual std::optional<RyzenAdjSettings> readInfo() const = 0;
};

// Abstracts JSON persistence.
class ISettingsStore {
public:
    virtual ~ISettingsStore() = default;
    virtual std::optional<ProfileSettings> load(const QString &profile) const = 0;
    virtual bool save(const QString &profile, const ProfileSettings &s) = 0;

    // Global battery end-charge threshold (not per-profile).
    // Default no-ops keep simple test doubles working without overriding.
    virtual std::optional<int> loadChargeLimit() const { return std::nullopt; }
    virtual bool saveChargeLimit(int /*percent*/) { return false; }
};
