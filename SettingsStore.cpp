#include "SettingsStore.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>

namespace {

constexpr int kSchemaVersion = 1;

// Serialize a single ProfileSettings block to the schema-defined JSON object.
QJsonObject profileToJson(const ProfileSettings &s)
{
    auto curveToJson = [](const FanCurve &curve) {
        QJsonArray arr;
        for (const FanCurvePoint &p : curve) {
            QJsonObject point;
            point.insert(QStringLiteral("tempC"), p.tempC);
            point.insert(QStringLiteral("percent"), p.percent);
            arr.append(point);
        }
        return arr;
    };

    QJsonObject obj;
    obj.insert(QStringLiteral("fanCurve"), curveToJson(s.fanCurve));
    // GPU fan curve (pwm2). New field; older files simply omit it and load()
    // falls back to the CPU curve.
    obj.insert(QStringLiteral("gpuFanCurve"), curveToJson(s.gpuFanCurve));
    obj.insert(QStringLiteral("tempThresholdC"), s.tempThresholdC);
    obj.insert(QStringLiteral("pl1Watts"), s.pl1Watts);
    obj.insert(QStringLiteral("pl2Watts"), s.pl2Watts);
    // Per-profile RyzenAdj SMU limits (optional; older files omit them).
    obj.insert(QStringLiteral("ryzenStapmW"), s.ryzenStapmW);
    obj.insert(QStringLiteral("ryzenFastW"), s.ryzenFastW);
    obj.insert(QStringLiteral("ryzenSlowW"), s.ryzenSlowW);
    obj.insert(QStringLiteral("ryzenApuSlowW"), s.ryzenApuSlowW);
    obj.insert(QStringLiteral("ryzenTctlC"), s.ryzenTctlC);
    return obj;
}

// Parse a single profile JSON object into ProfileSettings. Returns nullopt on
// any structural problem so load() can report a missing/unreadable block.
std::optional<ProfileSettings> profileFromJson(const QJsonValue &value)
{
    if (!value.isObject())
        return std::nullopt;

    const QJsonObject obj = value.toObject();

    if (!obj.contains(QStringLiteral("fanCurve"))
        || !obj.value(QStringLiteral("fanCurve")).isArray())
        return std::nullopt;
    if (!obj.value(QStringLiteral("tempThresholdC")).isDouble())
        return std::nullopt;
    if (!obj.value(QStringLiteral("pl1Watts")).isDouble())
        return std::nullopt;
    if (!obj.value(QStringLiteral("pl2Watts")).isDouble())
        return std::nullopt;

    ProfileSettings s;
    // Parse a JSON array of {tempC, percent} into a FanCurve. Returns false on
    // any structural problem.
    auto parseCurve = [](const QJsonArray &arr, FanCurve &out) -> bool {
        for (const QJsonValue &pv : arr) {
            if (!pv.isObject())
                return false;
            const QJsonObject po = pv.toObject();
            if (!po.value(QStringLiteral("tempC")).isDouble()
                || !po.value(QStringLiteral("percent")).isDouble())
                return false;
            FanCurvePoint point;
            point.tempC = po.value(QStringLiteral("tempC")).toInt();
            point.percent = po.value(QStringLiteral("percent")).toInt();
            out.append(point);
        }
        return true;
    };

    const QJsonArray fanCurve = obj.value(QStringLiteral("fanCurve")).toArray();
    if (!parseCurve(fanCurve, s.fanCurve))
        return std::nullopt;

    // GPU curve is optional (added later). If present and valid, use it; if
    // absent or malformed, fall back to the CPU curve so the GPU fan still gets
    // a sane curve rather than an empty one.
    if (obj.contains(QStringLiteral("gpuFanCurve"))
        && obj.value(QStringLiteral("gpuFanCurve")).isArray()) {
        FanCurve gpu;
        if (parseCurve(obj.value(QStringLiteral("gpuFanCurve")).toArray(), gpu)
            && !gpu.isEmpty())
            s.gpuFanCurve = gpu;
    }
    if (s.gpuFanCurve.isEmpty())
        s.gpuFanCurve = s.fanCurve;

    s.tempThresholdC = obj.value(QStringLiteral("tempThresholdC")).toInt();
    s.pl1Watts = obj.value(QStringLiteral("pl1Watts")).toInt();
    s.pl2Watts = obj.value(QStringLiteral("pl2Watts")).toInt();

    // Per-profile RyzenAdj limits are optional; default to -1 (unset) when
    // absent so older files load cleanly.
    auto intOr = [&obj](const QString &key, int fallback) {
        return obj.value(key).isDouble() ? obj.value(key).toInt() : fallback;
    };
    s.ryzenStapmW = intOr(QStringLiteral("ryzenStapmW"), -1);
    s.ryzenFastW = intOr(QStringLiteral("ryzenFastW"), -1);
    s.ryzenSlowW = intOr(QStringLiteral("ryzenSlowW"), -1);
    s.ryzenApuSlowW = intOr(QStringLiteral("ryzenApuSlowW"), -1);
    s.ryzenTctlC = intOr(QStringLiteral("ryzenTctlC"), -1);
    return s;
}

} // namespace

SettingsStore::SettingsStore()
    : m_filePath(defaultFilePath())
{
}

SettingsStore::SettingsStore(const QString &filePath)
    : m_filePath(filePath)
{
}

QString SettingsStore::defaultFilePath()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return base + QStringLiteral("/boreas/boreas.json");
}

std::optional<ProfileSettings> SettingsStore::load(const QString &profile) const
{
    QFile file(m_filePath);
    if (!file.exists())
        return std::nullopt;
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;

    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject root = doc.object();
    const QJsonValue profilesValue = root.value(QStringLiteral("profiles"));
    if (!profilesValue.isObject())
        return std::nullopt;

    const QJsonObject profiles = profilesValue.toObject();
    if (!profiles.contains(profile))
        return std::nullopt;

    return profileFromJson(profiles.value(profile));
}

bool SettingsStore::save(const QString &profile, const ProfileSettings &s)
{
    // Ensure the containing "boreas/" directory exists.
    const QFileInfo info(m_filePath);
    const QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return false;
    }

    // Read-modify-write: start from the existing document (if any valid one
    // exists) so other profiles' blocks are preserved (Req 11.6).
    QJsonObject root;
    QJsonObject profiles;

    QFile inFile(m_filePath);
    if (inFile.exists() && inFile.open(QIODevice::ReadOnly)) {
        const QByteArray raw = inFile.readAll();
        inFile.close();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            root = doc.object();
            const QJsonValue existing = root.value(QStringLiteral("profiles"));
            if (existing.isObject())
                profiles = existing.toObject();
        }
    }

    profiles.insert(profile, profileToJson(s));
    root.insert(QStringLiteral("version"), kSchemaVersion);
    root.insert(QStringLiteral("profiles"), profiles);

    const QJsonDocument outDoc(root);

    QFile outFile(m_filePath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    const QByteArray bytes = outDoc.toJson(QJsonDocument::Indented);
    const qint64 written = outFile.write(bytes);
    outFile.close();
    return written == bytes.size();
}

std::optional<int> SettingsStore::loadChargeLimit() const
{
    QFile file(m_filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject root = doc.object();
    const QJsonValue v = root.value(QStringLiteral("batteryChargeLimit"));
    if (!v.isDouble())
        return std::nullopt;
    return v.toInt();
}

bool SettingsStore::saveChargeLimit(int percent)
{
    const QFileInfo info(m_filePath);
    const QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return false;

    // Read-modify-write the root so profile blocks are preserved.
    QJsonObject root;
    QFile inFile(m_filePath);
    if (inFile.exists() && inFile.open(QIODevice::ReadOnly)) {
        const QByteArray raw = inFile.readAll();
        inFile.close();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject())
            root = doc.object();
    }

    root.insert(QStringLiteral("version"), kSchemaVersion);
    root.insert(QStringLiteral("batteryChargeLimit"), percent);

    QFile outFile(m_filePath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    const qint64 written = outFile.write(bytes);
    outFile.close();
    return written == bytes.size();
}
