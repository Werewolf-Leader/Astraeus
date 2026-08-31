#pragma once

#include <QString>

#include <optional>

#include "DataModels.h"
#include "PlatformAccess.h"

// Real ISettingsStore implementation persisting a single versioned JSON
// document under QStandardPaths::AppConfigLocation at "/boreas/boreas.json",
// keyed per profile. save() performs a read-modify-write merge so writing one
// profile leaves other profiles' persisted blocks unchanged (Req 11.6).
//
// load() returns std::nullopt on a missing file, unreadable/corrupt JSON, or a
// missing profile block, and never throws (Req 11.5).
class SettingsStore : public ISettingsStore {
public:
    SettingsStore();
    // Testing / customization hook: use an explicit config file path instead of
    // the AppConfigLocation-derived default.
    explicit SettingsStore(const QString &filePath);

    std::optional<ProfileSettings> load(const QString &profile) const override;
    bool save(const QString &profile, const ProfileSettings &s) override;

    std::optional<int> loadChargeLimit() const override;
    bool saveChargeLimit(int percent) override;

    QString filePath() const { return m_filePath; }

private:
    static QString defaultFilePath();

    QString m_filePath;
};
