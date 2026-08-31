#include "SysfsAccess.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QFileInfo>
#include <QIODevice>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

bool SysfsAccess::exists(const QString &path) const
{
    return QFileInfo::exists(path);
}

bool SysfsAccess::isWritable(const QString &path) const
{
    QFileInfo info(path);
    // A sysfs attribute must exist and carry the writable permission bit.
    return info.exists() && info.isWritable();
}

std::optional<QString> SysfsAccess::read(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }
    const QByteArray data = f.readAll();
    if (f.error() != QFileDevice::NoError) {
        return std::nullopt;
    }
    // sysfs values are typically a single line; trim trailing newline/space.
    return QString::fromUtf8(data).trimmed();
}

WriteOutcome SysfsAccess::write(const QString &path, const QString &value,
                                int timeoutMs)
{
    // Fast pre-checks that don't risk blocking. These let us return precise
    // IoError/PermissionDenied messages without spawning a worker.
    QFileInfo info(path);
    if (!info.exists()) {
        return WriteOutcome{WriteOutcome::Status::IoError,
                            QStringLiteral("Path does not exist: %1").arg(path)};
    }
    if (!info.isWritable()) {
        return WriteOutcome{
            WriteOutcome::Status::PermissionDenied,
            QStringLiteral("Path is not writable (insufficient permissions): %1")
                .arg(path)};
    }

    // The actual write may block on some kernels (PL1/PL2 caveat), so it runs
    // on a worker thread. We wait at most timeoutMs for it to complete.
    //
    // The promise/future are held via shared_ptr so that if we time out and
    // abandon the wait, the still-running worker keeps valid state to write
    // into and does not touch freed stack memory.
    auto resultPromise = std::make_shared<std::promise<WriteOutcome>>();
    std::future<WriteOutcome> resultFuture = resultPromise->get_future();

    const QByteArray payload = value.toUtf8();

    std::thread worker([path, payload, resultPromise]() {
        WriteOutcome outcome{WriteOutcome::Status::Ok, QString()};
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            const QFileDevice::FileError err = f.error();
            if (err == QFileDevice::PermissionsError) {
                outcome = WriteOutcome{
                    WriteOutcome::Status::PermissionDenied,
                    QStringLiteral("Permission denied opening %1: %2")
                        .arg(path, f.errorString())};
            } else {
                outcome = WriteOutcome{
                    WriteOutcome::Status::IoError,
                    QStringLiteral("Failed to open %1 for writing: %2")
                        .arg(path, f.errorString())};
            }
            resultPromise->set_value(outcome);
            return;
        }

        const qint64 written = f.write(payload);
        // Flush explicitly; this is where a stuck driver typically blocks.
        const bool flushed = f.flush();
        if (written != payload.size() || !flushed
            || f.error() != QFileDevice::NoError) {
            const QFileDevice::FileError err = f.error();
            if (err == QFileDevice::PermissionsError) {
                outcome = WriteOutcome{
                    WriteOutcome::Status::PermissionDenied,
                    QStringLiteral("Permission denied writing %1: %2")
                        .arg(path, f.errorString())};
            } else {
                outcome = WriteOutcome{
                    WriteOutcome::Status::IoError,
                    QStringLiteral("I/O error writing %1: %2")
                        .arg(path, f.errorString())};
            }
        }
        f.close();
        resultPromise->set_value(outcome);
    });

    const std::chrono::milliseconds waitFor(timeoutMs < 0 ? 0 : timeoutMs);
    const std::future_status status = resultFuture.wait_for(waitFor);

    if (status == std::future_status::ready) {
        // Worker finished within the budget; join and return its outcome.
        worker.join();
        return resultFuture.get();
    }

    // Timed out. Detach so the still-blocked worker can wind down on its own
    // without us waiting on it. The shared promise/future state stays alive.
    worker.detach();
    return WriteOutcome{
        WriteOutcome::Status::Timeout,
        QStringLiteral("Power limit write timed out (known driver issue on some "
                       "kernels)")};
}

std::optional<QString> SysfsAccess::resolveHwmonInput(
    const QStringList &namesInPriority,
    const QStringList &labelsInPriority) const
{
    // hwmon indices (hwmon0, hwmon1, ...) are assigned in driver-probe order and
    // are NOT stable across boots, so we resolve by the driver-reported `name`
    // attribute plus the per-input `tempN_label`. This is the fix for the bug
    // where hardcoded indices pointed CPU/GPU temperature reads at the wrong (or
    // input-less) hwmon device.
    const QString hwmonRoot = QStringLiteral("/sys/class/hwmon");
    QDir rootDir(hwmonRoot);
    if (!rootDir.exists())
        return std::nullopt;

    // Collect the hwmon* directories keyed by their reported name so we can try
    // names in the caller's priority order regardless of on-disk ordering.
    QMap<QString, QStringList> dirsByName; // lowercased name -> hwmon dir paths
    const QStringList entries =
        rootDir.entryList(QStringList{QStringLiteral("hwmon*")},
                          QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        const QString hwmonDir = hwmonRoot + QLatin1Char('/') + entry;
        const std::optional<QString> name = read(hwmonDir + QStringLiteral("/name"));
        if (!name.has_value())
            continue;
        dirsByName[name->trimmed().toLower()].append(hwmonDir);
    }

    // Locate a tempN_input under a hwmon dir whose sibling tempN_label matches
    // one of labelsInPriority (case-insensitive). Falls back to temp1_input when
    // no label matches. Returns nullopt if the dir exposes no temp input at all.
    auto resolveInDir = [this, &labelsInPriority](
                            const QString &hwmonDir) -> std::optional<QString> {
        QDir dir(hwmonDir);
        const QStringList inputs =
            dir.entryList(QStringList{QStringLiteral("temp*_input")},
                          QDir::Files | QDir::System, QDir::Name);
        if (inputs.isEmpty())
            return std::nullopt;

        // Prefer an input whose label matches, honouring label priority order.
        for (const QString &wantLabel : labelsInPriority) {
            for (const QString &inputName : inputs) {
                // temp<N>_input -> temp<N>_label
                QString labelName = inputName;
                labelName.replace(QStringLiteral("_input"),
                                  QStringLiteral("_label"));
                const std::optional<QString> label =
                    read(hwmonDir + QLatin1Char('/') + labelName);
                if (label.has_value()
                    && label->trimmed().compare(wantLabel, Qt::CaseInsensitive)
                           == 0) {
                    return hwmonDir + QLatin1Char('/') + inputName;
                }
            }
        }

        // No matching label: fall back to temp1_input when present, else the
        // first available temp*_input.
        const QString temp1 = hwmonDir + QStringLiteral("/temp1_input");
        if (inputs.contains(QStringLiteral("temp1_input")))
            return temp1;
        return hwmonDir + QLatin1Char('/') + inputs.first();
    };

    for (const QString &wantName : namesInPriority) {
        const auto it = dirsByName.constFind(wantName.trimmed().toLower());
        if (it == dirsByName.constEnd())
            continue;
        for (const QString &hwmonDir : it.value()) {
            if (const std::optional<QString> resolved = resolveInDir(hwmonDir))
                return resolved;
        }
    }

    return std::nullopt;
}

std::optional<QString> SysfsAccess::resolveHwmonDir(
    const QStringList &namesInPriority) const
{
    const QString hwmonRoot = QStringLiteral("/sys/class/hwmon");
    QDir rootDir(hwmonRoot);
    if (!rootDir.exists())
        return std::nullopt;

    // Map each hwmon* directory to its reported `name`, then try the caller's
    // names in priority order regardless of on-disk ordering.
    QMap<QString, QStringList> dirsByName; // lowercased name -> hwmon dir paths
    const QStringList entries =
        rootDir.entryList(QStringList{QStringLiteral("hwmon*")},
                          QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        const QString hwmonDir = hwmonRoot + QLatin1Char('/') + entry;
        const std::optional<QString> name = read(hwmonDir + QStringLiteral("/name"));
        if (!name.has_value())
            continue;
        dirsByName[name->trimmed().toLower()].append(hwmonDir);
    }

    for (const QString &wantName : namesInPriority) {
        const auto it = dirsByName.constFind(wantName.trimmed().toLower());
        if (it != dirsByName.constEnd() && !it.value().isEmpty())
            return it.value().first();
    }

    return std::nullopt;
}
