#pragma once

#include "PlatformAccess.h"

#include <QString>

// Concrete IRyzenAdjAccess backed by the `ryzenadj` command-line tool
// (https://github.com/FlyGoat/RyzenAdj). RyzenAdj tunes AMD SMU power limits
// (STAPM / fast / slow / APU-slow PPT and the Tctl thermal limit) that the
// in-kernel ppt_* sysfs nodes do not expose.
//
// Design choices:
//   - CLI, not libryzenadj: shelling out means Boreas builds and runs on
//     machines without RyzenAdj installed; the feature is simply reported
//     unavailable (Req 3.x capability model). No build-time dependency.
//   - Every invocation is bounded by a timeout via QProcess so a hung SMU call
//     cannot freeze the UI, mirroring the SysfsAccess bounded-write philosophy.
//   - Failure-tolerant: a missing binary / non-zero exit / parse failure maps to
//     available()==false or a WriteOutcome, never a crash.
//
// RyzenAdj power values are in MILLIWATTS on the CLI; this class converts
// to/from watts at the boundary so the rest of Boreas works in watts.
class RyzenAdjAccess : public IRyzenAdjAccess {
public:
    RyzenAdjAccess();
    explicit RyzenAdjAccess(const QString &binaryPath);
    ~RyzenAdjAccess() override = default;

    bool available() const override;
    WriteOutcome apply(const RyzenAdjSettings &settings) override;
    std::optional<RyzenAdjSettings> readInfo() const override;

    // Resolved absolute path to the ryzenadj binary (empty when not found).
    QString binaryPath() const { return m_binaryPath; }

private:
    // Locate the ryzenadj binary in PATH and common install locations. Returns
    // an empty string when not found.
    static QString resolveBinary();

    // Run the binary with args, bounded by timeoutMs. Fills exitCode/stdout/
    // stderr. Returns false when the process could not be started or timed out.
    bool run(const QStringList &args, int timeoutMs, int &exitCode,
             QString &stdOut, QString &stdErr) const;

    // Bounded timeouts for apply (write) and info (read).
    static constexpr int kApplyTimeoutMs = 4000;
    static constexpr int kInfoTimeoutMs = 3000;

    QString m_binaryPath;
};
