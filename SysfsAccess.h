#pragma once

#include "PlatformAccess.h"

// Concrete ISysfsAccess backed by the real /sys filesystem.
//
// The write path uses a bounded-wait mechanism (a worker thread joined with a
// timeout) so that a blocking sysfs write cannot hang the caller indefinitely.
// This exists specifically to survive the known PL1/PL2 blocking-write caveat
// where some kernels block the write for ~60s before returning an I/O error.
class SysfsAccess : public ISysfsAccess {
public:
    SysfsAccess() = default;
    ~SysfsAccess() override = default;

    bool exists(const QString &path) const override;
    bool isWritable(const QString &path) const override;
    std::optional<QString> read(const QString &path) const override;
    // Runs the actual write on a worker thread and waits at most timeoutMs.
    // On timeout returns WriteOutcome::Status::Timeout with a descriptive
    // message; the detached write may still complete/fail in the background.
    WriteOutcome write(const QString &path, const QString &value,
                       int timeoutMs) override;

    // Resolve a hwmon tempN_input by device name + label, scanning
    // /sys/class/hwmon. See ISysfsAccess::resolveHwmonInput for the contract.
    std::optional<QString> resolveHwmonInput(
        const QStringList &namesInPriority,
        const QStringList &labelsInPriority) const override;

    // Resolve a hwmon device directory by its reported `name`. See
    // ISysfsAccess::resolveHwmonDir for the contract.
    std::optional<QString> resolveHwmonDir(
        const QStringList &namesInPriority) const override;
};
