#include "RyzenAdjAccess.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

namespace {
// Common absolute locations RyzenAdj installs to, tried after PATH lookup.
const QStringList kCommonBinaryPaths = {
    QStringLiteral("/usr/bin/ryzenadj"),
    QStringLiteral("/usr/local/bin/ryzenadj"),
    QStringLiteral("/opt/ryzenadj/ryzenadj"),
};

// Convert watts to RyzenAdj's milliwatt CLI unit.
int wattsToMilliwatts(int watts) { return watts * 1000; }
} // namespace

RyzenAdjAccess::RyzenAdjAccess()
    : m_binaryPath(resolveBinary())
{
}

RyzenAdjAccess::RyzenAdjAccess(const QString &binaryPath)
    : m_binaryPath(binaryPath)
{
}

QString RyzenAdjAccess::resolveBinary()
{
    // PATH lookup first (respects a user/system install on PATH).
    const QString fromPath =
        QStandardPaths::findExecutable(QStringLiteral("ryzenadj"));
    if (!fromPath.isEmpty())
        return fromPath;

    // Fall back to well-known absolute locations.
    for (const QString &candidate : kCommonBinaryPaths) {
        QFileInfo info(candidate);
        if (info.exists() && info.isExecutable())
            return candidate;
    }
    return QString();
}

bool RyzenAdjAccess::available() const
{
    if (m_binaryPath.isEmpty())
        return false;
    QFileInfo info(m_binaryPath);
    return info.exists() && info.isExecutable();
}

bool RyzenAdjAccess::run(const QStringList &args, int timeoutMs, int &exitCode,
                         QString &stdOut, QString &stdErr) const
{
    if (m_binaryPath.isEmpty())
        return false;

    QProcess proc;
    proc.setProgram(m_binaryPath);
    proc.setArguments(args);
    proc.start();

    if (!proc.waitForStarted(timeoutMs))
        return false;
    if (!proc.waitForFinished(timeoutMs)) {
        // Timed out: kill the child so it does not linger, and report failure.
        proc.kill();
        proc.waitForFinished(500);
        return false;
    }

    exitCode = proc.exitCode();
    stdOut = QString::fromUtf8(proc.readAllStandardOutput());
    stdErr = QString::fromUtf8(proc.readAllStandardError());
    return proc.exitStatus() == QProcess::NormalExit;
}

WriteOutcome RyzenAdjAccess::apply(const RyzenAdjSettings &settings)
{
    if (!available()) {
        return WriteOutcome{WriteOutcome::Status::Unsupported,
                            QStringLiteral("ryzenadj is not installed")};
    }
    if (!settings.hasAny()) {
        // Nothing to do; treat as a successful no-op so callers do not have to
        // special-case an empty request.
        return WriteOutcome{WriteOutcome::Status::Ok, QString()};
    }

    // Build the argument list. Only set fields (>= 0) are passed so the operator
    // can tune one knob without disturbing the others. Power limits convert to
    // milliwatts; tctl-temp is degrees C.
    QStringList args;
    if (settings.stapmLimitW >= 0)
        args << QStringLiteral("--stapm-limit=%1")
                    .arg(wattsToMilliwatts(settings.stapmLimitW));
    if (settings.fastLimitW >= 0)
        args << QStringLiteral("--fast-limit=%1")
                    .arg(wattsToMilliwatts(settings.fastLimitW));
    if (settings.slowLimitW >= 0)
        args << QStringLiteral("--slow-limit=%1")
                    .arg(wattsToMilliwatts(settings.slowLimitW));
    if (settings.apuSlowLimitW >= 0)
        args << QStringLiteral("--apu-slow-limit=%1")
                    .arg(wattsToMilliwatts(settings.apuSlowLimitW));
    if (settings.tctlTempC >= 0)
        args << QStringLiteral("--tctl-temp=%1").arg(settings.tctlTempC);

    int exitCode = -1;
    QString out;
    QString err;
    if (!run(args, kApplyTimeoutMs, exitCode, out, err)) {
        return WriteOutcome{WriteOutcome::Status::Timeout,
                            QStringLiteral("ryzenadj did not complete in time")};
    }

    // IMPORTANT: ryzenadj returns exit code 0 even when it fails to initialise
    // (it prints the error to stdout/stderr and exits 0). So we cannot trust the
    // exit code alone — we must inspect the combined output for known failure
    // markers. This is the difference between Boreas honestly reporting a
    // failure vs. falsely claiming success.
    const QString combined = (out + QLatin1Char('\n') + err);
    const QString lowered = combined.toLower();

    // Secure Boot / kernel lockdown blocks RyzenAdj's /dev/mem + PCI access.
    // This is the most common real-world blocker on stock ASUS installs.
    if (lowered.contains(QStringLiteral("secure boot"))
        || lowered.contains(QStringLiteral("pci bus is not writeable"))
        || lowered.contains(QStringLiteral("not writeable"))) {
        return WriteOutcome{
            WriteOutcome::Status::PermissionDenied,
            QStringLiteral("RyzenAdj is blocked by Secure Boot / kernel lockdown. "
                           "Disable Secure Boot in BIOS, or install the ryzen_smu "
                           "kernel module, to enable advanced power tuning.")};
    }

    // General init failure (SMU object unavailable, driver not ready, etc.).
    if (lowered.contains(QStringLiteral("unable to init"))
        || lowered.contains(QStringLiteral("unable to get"))
        || lowered.contains(QStringLiteral("failed"))
        || lowered.contains(QStringLiteral("error"))) {
        const QString detail = combined.trimmed().isEmpty()
            ? QStringLiteral("ryzenadj could not initialise the SMU")
            : combined.trimmed();
        // Root is still required; if the message hints at permissions, say so.
        if (lowered.contains(QStringLiteral("permission"))
            || lowered.contains(QStringLiteral("/dev/mem"))
            || lowered.contains(QStringLiteral("root"))) {
            return WriteOutcome{WriteOutcome::Status::PermissionDenied,
                                QStringLiteral("ryzenadj requires root and SMU "
                                               "access: %1").arg(detail)};
        }
        return WriteOutcome{WriteOutcome::Status::IoError, detail};
    }

    if (exitCode != 0) {
        const QString detail = combined.trimmed().isEmpty()
            ? QStringLiteral("ryzenadj exited with code %1").arg(exitCode)
            : combined.trimmed();
        return WriteOutcome{WriteOutcome::Status::IoError, detail};
    }

    return WriteOutcome{WriteOutcome::Status::Ok, QString()};
}

std::optional<RyzenAdjSettings> RyzenAdjAccess::readInfo() const
{
    if (!available())
        return std::nullopt;

    int exitCode = -1;
    QString out;
    QString err;
    if (!run(QStringList{QStringLiteral("--info")}, kInfoTimeoutMs, exitCode,
             out, err)) {
        return std::nullopt;
    }
    if (exitCode != 0 || out.trimmed().isEmpty())
        return std::nullopt;

    // ryzenadj exits 0 even when SMU init fails (Secure Boot / lockdown), in
    // which case the output is just an error and has no metric table. Treat any
    // known init-failure marker as "no info available" so callers do not display
    // bogus values.
    const QString lowered = (out + QLatin1Char('\n') + err).toLower();
    if (lowered.contains(QStringLiteral("unable to init"))
        || lowered.contains(QStringLiteral("secure boot"))
        || lowered.contains(QStringLiteral("not writeable"))
        || lowered.contains(QStringLiteral("unable to get"))) {
        return std::nullopt;
    }

    // `ryzenadj -i` prints a table of "NAME | VALUE | ..." rows. Parse the
    // limit rows we care about. Values are in watts for power limits and
    // degrees C for the thermal limit (RyzenAdj prints the human-readable W,
    // not the raw mW, in --info). We match leniently and round to the nearest
    // integer.
    RyzenAdjSettings s;

    auto parseRow = [&out](const QString &key) -> std::optional<double> {
        // Match a line containing the key, then the first floating-point number
        // after a '|' separator. RyzenAdj uses keys like "STAPM LIMIT",
        // "PPT LIMIT FAST", "PPT LIMIT SLOW", "StapmTimeConst", "THM LIMIT CORE".
        const QRegularExpression re(
            QStringLiteral("%1\\s*\\|\\s*([0-9]+\\.?[0-9]*)").arg(key),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(out);
        if (m.hasMatch()) {
            bool ok = false;
            const double v = m.captured(1).toDouble(&ok);
            if (ok)
                return v;
        }
        return std::nullopt;
    };

    if (const auto v = parseRow(QStringLiteral("STAPM LIMIT")))
        s.stapmLimitW = static_cast<int>(*v + 0.5);
    if (const auto v = parseRow(QStringLiteral("PPT LIMIT FAST")))
        s.fastLimitW = static_cast<int>(*v + 0.5);
    if (const auto v = parseRow(QStringLiteral("PPT LIMIT SLOW")))
        s.slowLimitW = static_cast<int>(*v + 0.5);
    if (const auto v = parseRow(QStringLiteral("PPT LIMIT APU")))
        s.apuSlowLimitW = static_cast<int>(*v + 0.5);
    if (const auto v = parseRow(QStringLiteral("THM LIMIT CORE")))
        s.tctlTempC = static_cast<int>(*v + 0.5);

    return s;
}
