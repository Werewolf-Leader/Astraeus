#pragma once

#include "PlatformAccess.h"

// Concrete IDBusAccess backed by the real system D-Bus.
//
// GPU-mode operations talk to supergfxctl (org.supergfxctl); fan-curve
// operations talk to asusd (org.asuslinux.Daemon). Because both daemons are
// optional and their exact interface shapes vary across versions, every call
// probes availability first and degrades gracefully: reads return
// std::nullopt and writes return a WriteOutcome (Unsupported/IoError) rather
// than crashing (Req 3.4 — fail visibly, never fatally).
class DBusAccess : public IDBusAccess {
public:
    DBusAccess() = default;
    ~DBusAccess() override = default;

    bool serviceAvailable(const QString &service) const override;
    std::optional<QStringList> supportedGpuModes() const override;
    std::optional<QString> activeGpuMode() const override;
    WriteOutcome setGpuMode(const QString &mode) override;
    bool fanCurvesSupported() const override;
    WriteOutcome setFanCurve(const QString &profile,
                             const QVariantList &points) override;
};
