#include "DBusAccess.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QVariant>

namespace {

// --- supergfxctl (GPU mode) D-Bus constants -----------------------------
//
// supergfxctl exposes a Daemon object on the system bus. The exact member
// names have shifted across releases; these are the commonly-used ones. If a
// call fails against a given daemon version we degrade gracefully rather than
// asserting a particular shape.
constexpr const char *kSupergfxService = "org.supergfxctl.Daemon";
constexpr const char *kSupergfxPath = "/org/supergfxctl/Gfx";
constexpr const char *kSupergfxInterface = "org.supergfxctl.Daemon";
constexpr const char *kSupergfxMethodSupported = "Supported";
constexpr const char *kSupergfxMethodGetMode = "Mode";
constexpr const char *kSupergfxMethodSetMode = "SetMode";

// --- asusd (fan curves) D-Bus constants ---------------------------------
//
// asusd (asus-linux.org) exposes fan-curve control under org.asuslinux.Daemon.
// The FanCurves interface is per-profile; exact method signatures vary by
// version, so setFanCurve reports Unsupported on any failure.
constexpr const char *kAsusdService = "org.asuslinux.Daemon";
constexpr const char *kAsusdFanCurvesPath = "/org/asuslinux/FanCurves";
constexpr const char *kAsusdFanCurvesInterface = "org.asuslinux.FanCurves";
constexpr const char *kAsusdMethodSetFanCurve = "SetFanCurve";

// supergfxctl reports/accepts modes as integer enum indices. This table maps
// those indices to the human-readable names used throughout Boreas. Kept as a
// clearly-commented constant since the exact wire type can be an int enum.
QString gpuModeName(int index)
{
    switch (index) {
    case 0: return QStringLiteral("Hybrid");
    case 1: return QStringLiteral("Integrated");
    case 2: return QStringLiteral("NvidiaNoModeset");
    case 3: return QStringLiteral("Vfio");
    case 4: return QStringLiteral("AsusEgpu");
    case 5: return QStringLiteral("AsusMuxDgpu");
    default: return QString();
    }
}

int gpuModeIndex(const QString &name)
{
    for (int i = 0; i <= 5; ++i) {
        if (gpuModeName(i).compare(name, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

} // namespace

bool DBusAccess::serviceAvailable(const QString &service) const
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        return false;
    }
    QDBusConnectionInterface *iface = bus.interface();
    if (!iface) {
        return false;
    }
    // isServiceRegistered covers already-running services; activatable
    // services that are not yet started are also queried as a fallback.
    if (iface->isServiceRegistered(service).value()) {
        return true;
    }
    const QStringList activatable = iface->activatableServiceNames().value();
    return activatable.contains(service);
}

std::optional<QStringList> DBusAccess::supportedGpuModes() const
{
    if (!serviceAvailable(QLatin1String(kSupergfxService))) {
        return std::nullopt;
    }

    QDBusInterface iface(QLatin1String(kSupergfxService),
                         QLatin1String(kSupergfxPath),
                         QLatin1String(kSupergfxInterface),
                         QDBusConnection::systemBus());
    if (!iface.isValid()) {
        return std::nullopt;
    }

    QDBusReply<QList<uint>> reply =
        iface.call(QLatin1String(kSupergfxMethodSupported));
    if (!reply.isValid()) {
        return std::nullopt;
    }

    QStringList modes;
    for (uint idx : reply.value()) {
        const QString name = gpuModeName(static_cast<int>(idx));
        if (!name.isEmpty()) {
            modes.append(name);
        }
    }
    if (modes.isEmpty()) {
        return std::nullopt;
    }
    return modes;
}

std::optional<QString> DBusAccess::activeGpuMode() const
{
    if (!serviceAvailable(QLatin1String(kSupergfxService))) {
        return std::nullopt;
    }

    QDBusInterface iface(QLatin1String(kSupergfxService),
                         QLatin1String(kSupergfxPath),
                         QLatin1String(kSupergfxInterface),
                         QDBusConnection::systemBus());
    if (!iface.isValid()) {
        return std::nullopt;
    }

    QDBusReply<uint> reply = iface.call(QLatin1String(kSupergfxMethodGetMode));
    if (!reply.isValid()) {
        return std::nullopt;
    }

    const QString name = gpuModeName(static_cast<int>(reply.value()));
    if (name.isEmpty()) {
        return std::nullopt;
    }
    return name;
}

WriteOutcome DBusAccess::setGpuMode(const QString &mode)
{
    if (!serviceAvailable(QLatin1String(kSupergfxService))) {
        return WriteOutcome{
            WriteOutcome::Status::Unsupported,
            QStringLiteral("supergfxctl D-Bus service not available")};
    }

    const int index = gpuModeIndex(mode);
    if (index < 0) {
        return WriteOutcome{
            WriteOutcome::Status::Unsupported,
            QStringLiteral("Unknown GPU mode: %1").arg(mode)};
    }

    QDBusInterface iface(QLatin1String(kSupergfxService),
                         QLatin1String(kSupergfxPath),
                         QLatin1String(kSupergfxInterface),
                         QDBusConnection::systemBus());
    if (!iface.isValid()) {
        return WriteOutcome{
            WriteOutcome::Status::Unsupported,
            QStringLiteral("supergfxctl Daemon interface not reachable")};
    }

    QDBusReply<void> reply = iface.call(QLatin1String(kSupergfxMethodSetMode),
                                        static_cast<uint>(index));
    if (!reply.isValid()) {
        return WriteOutcome{
            WriteOutcome::Status::IoError,
            QStringLiteral("Failed to set GPU mode via supergfxctl: %1")
                .arg(reply.error().message())};
    }

    return WriteOutcome{WriteOutcome::Status::Ok, QString()};
}

bool DBusAccess::fanCurvesSupported() const
{
    // asusd must be present for fan-curve control. Presence of the daemon is
    // the primary signal; deeper probing is avoided so we never block or crash.
    return serviceAvailable(QLatin1String(kAsusdService));
}

WriteOutcome DBusAccess::setFanCurve(const QString &profile,
                                     const QVariantList &points)
{
    if (!serviceAvailable(QLatin1String(kAsusdService))) {
        return WriteOutcome{
            WriteOutcome::Status::Unsupported,
            QStringLiteral("Fan curve control not supported by current driver")};
    }

    QDBusInterface iface(QLatin1String(kAsusdService),
                         QLatin1String(kAsusdFanCurvesPath),
                         QLatin1String(kAsusdFanCurvesInterface),
                         QDBusConnection::systemBus());
    if (!iface.isValid()) {
        return WriteOutcome{
            WriteOutcome::Status::Unsupported,
            QStringLiteral("asusd FanCurves interface not reachable")};
    }

    // The exact SetFanCurve signature is asusd-version dependent. We pass the
    // profile name and the point list through; on any mismatch/failure we
    // return Unsupported rather than crashing.
    QDBusReply<void> reply = iface.call(QLatin1String(kAsusdMethodSetFanCurve),
                                        profile, QVariant(points));
    if (!reply.isValid()) {
        return WriteOutcome{
            WriteOutcome::Status::Unsupported,
            QStringLiteral("Failed to set fan curve via asusd: %1")
                .arg(reply.error().message())};
    }

    return WriteOutcome{WriteOutcome::Status::Ok, QString()};
}
