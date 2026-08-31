#pragma once

// Test-only in-memory fakes for the platform-access interfaces.
//
// This header is intentionally header-only and is NOT part of the production
// build target (qt_add_executable(Boreas ...)). It is consumed only by the
// test targets introduced in later tasks. Keeping it header-only keeps the
// test doubles trivially reusable across test translation units.
//
// Each fake implements one of the interfaces from PlatformAccess.h with an
// in-memory backing store plus setter "scripting" helpers so a test can seed
// state and force specific outcomes (existence, writability, WriteOutcome,
// supported modes, availability, etc.). Where useful, calls are recorded so
// later property tests (tasks 10.x) can assert on the exact arguments the
// mediator passed down.

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <optional>

#include "PlatformAccess.h"
#include "DataModels.h"

// -----------------------------------------------------------------------------
// FakeSysfsAccess
// -----------------------------------------------------------------------------
//
// In-memory path -> value map with per-path existence and writability flags,
// plus a scriptable WriteOutcome so a test can force the result of the next (or
// every) write. write() records the calls it receives.
class FakeSysfsAccess : public ISysfsAccess {
public:
    // A single recorded write() invocation.
    struct WriteCall {
        QString path;
        QString value;
        int timeoutMs;
    };

    FakeSysfsAccess() = default;
    ~FakeSysfsAccess() override = default;

    // ---- ISysfsAccess ----

    bool exists(const QString &path) const override
    {
        // A path exists if it was explicitly marked existing, or if it has a
        // seeded value and was not explicitly marked non-existent.
        if (m_existence.contains(path))
            return m_existence.value(path);
        return m_values.contains(path);
    }

    bool isWritable(const QString &path) const override
    {
        return m_writable.value(path, false);
    }

    std::optional<QString> read(const QString &path) const override
    {
        if (!exists(path))
            return std::nullopt;
        if (m_values.contains(path))
            return m_values.value(path);
        return std::nullopt;
    }

    WriteOutcome write(const QString &path, const QString &value,
                       int timeoutMs) override
    {
        m_writeCalls.append(WriteCall{path, value, timeoutMs});
        m_lastWrite = WriteCall{path, value, timeoutMs};

        // Determine the outcome to report. A per-path override takes precedence
        // over the global scripted outcome.
        WriteOutcome outcome = m_scriptedOutcome;
        if (m_scriptedOutcomeForPath.contains(path))
            outcome = m_scriptedOutcomeForPath.value(path);

        // On a successful outcome, reflect the write into the in-memory map so
        // a subsequent read()/read-back observes the accepted value. When a
        // read-back override is scripted for this path, the driver is modelled
        // as accepting a value distinct from the requested one (e.g. clamping),
        // so the override is what a subsequent read() observes rather than the
        // requested value. This lets a test drive the read-back-consistency
        // property with a perturbed read-back (Property 3).
        if (outcome.status == WriteOutcome::Status::Ok) {
            if (m_readBackOverride.contains(path))
                m_values.insert(path, m_readBackOverride.value(path));
            else
                m_values.insert(path, value);
        }

        return outcome;
    }

    std::optional<QString> resolveHwmonInput(
        const QStringList &namesInPriority,
        const QStringList &labelsInPriority) const override
    {
        // 1) Explicit scripted overrides win: if a test set a resolved path for
        //    any name in the priority list, return it (in priority order). This
        //    lets a test drive the new dynamic-resolution behavior directly.
        for (const QString &name : namesInPriority) {
            const auto it = m_resolvedHwmonInput.constFind(name.trimmed().toLower());
            if (it != m_resolvedHwmonInput.constEnd() && exists(it.value()))
                return it.value();
        }

        // 2) Seeded hwmon entries (addHwmon): scan them by name + label,
        //    mirroring the real SysfsAccess scan so name/label-driven tests work
        //    without touching the on-disk /sys layout.
        for (const QString &wantName : namesInPriority) {
            const QString key = wantName.trimmed().toLower();
            for (const HwmonEntry &entry : m_hwmonEntries) {
                if (entry.name.trimmed().toLower() != key)
                    continue;
                // Prefer a label match in label-priority order.
                for (const QString &wantLabel : labelsInPriority) {
                    for (auto lit = entry.inputByLabel.constBegin();
                         lit != entry.inputByLabel.constEnd(); ++lit) {
                        if (lit.key().compare(wantLabel, Qt::CaseInsensitive) == 0
                            && exists(lit.value()))
                            return lit.value();
                    }
                }
                // Fall back to the entry's default input.
                if (!entry.defaultInput.isEmpty() && exists(entry.defaultInput))
                    return entry.defaultInput;
            }
        }

        // 3) Backward-compatible fallback: map the well-known CPU/GPU sensor
        //    name families to the legacy canonical hwmon paths existing tests
        //    seed, returning one only when that path was seeded as existing.
        //    This keeps pre-existing tests (which seed hwmon0/hwmon1 temp inputs
        //    without name files) passing unchanged.
        for (const QString &name : namesInPriority) {
            const QString key = name.trimmed().toLower();
            const auto it = legacyHwmonFallback().constFind(key);
            if (it != legacyHwmonFallback().constEnd() && exists(it.value()))
                return it.value();
        }

        return std::nullopt;
    }

    std::optional<QString> resolveHwmonDir(
        const QStringList &namesInPriority) const override
    {
        for (const QString &name : namesInPriority) {
            const auto it = m_hwmonDirByName.constFind(name.trimmed().toLower());
            if (it != m_hwmonDirByName.constEnd())
                return it.value();
        }
        return std::nullopt;
    }

    // ---- Seeding / scripting helpers ----

    // Seed (or overwrite) the value at a path. Marks the path existing unless
    // an explicit existence flag was already set.
    void setValue(const QString &path, const QString &value)
    {
        m_values.insert(path, value);
    }

    // Script the hwmon directory returned by resolveHwmonDir() for a given
    // device name (matched case-insensitively).
    void setHwmonDir(const QString &name, const QString &dir)
    {
        m_hwmonDirByName.insert(name.trimmed().toLower(), dir);
    }

    // Script the resolved hwmon tempN_input path returned for a given hwmon
    // device name (matched case-insensitively against the names passed to
    // resolveHwmonInput). Highest-priority resolution mechanism.
    void setResolvedHwmonInput(const QString &name, const QString &path)
    {
        m_resolvedHwmonInput.insert(name.trimmed().toLower(), path);
    }

    // Seed a full hwmon entry so resolveHwmonInput() can resolve it by
    // name + label the way the real scan does. `inputByLabel` maps a
    // tempN_label value to its tempN_input path; `defaultInput` is used when no
    // label matches (typically the temp1_input path). Marks the involved paths
    // existing so a subsequent read()/resolve observes them.
    void addHwmon(const QString &name,
                  const QMap<QString, QString> &inputByLabel,
                  const QString &defaultInput)
    {
        HwmonEntry entry;
        entry.name = name;
        entry.inputByLabel = inputByLabel;
        entry.defaultInput = defaultInput;
        for (auto it = inputByLabel.constBegin(); it != inputByLabel.constEnd();
             ++it)
            setExists(it.value(), true);
        if (!defaultInput.isEmpty())
            setExists(defaultInput, true);
        m_hwmonEntries.append(entry);
    }

    // Force whether a path is reported as existing.
    void setExists(const QString &path, bool exists)
    {
        m_existence.insert(path, exists);
    }

    // Force whether a path is reported as writable.
    void setWritable(const QString &path, bool writable)
    {
        m_writable.insert(path, writable);
    }

    // Script the WriteOutcome returned for every write (unless a per-path
    // override is set). Defaults to Ok.
    void setWriteOutcome(WriteOutcome outcome)
    {
        m_scriptedOutcome = outcome;
    }

    // Convenience overload: script only the status with an empty message.
    void setWriteStatus(WriteOutcome::Status status, const QString &message = QString())
    {
        m_scriptedOutcome = WriteOutcome{status, message};
    }

    // Script a WriteOutcome that applies only to writes targeting a specific
    // path (overrides the global scripted outcome for that path).
    void setWriteOutcomeForPath(const QString &path, WriteOutcome outcome)
    {
        m_scriptedOutcomeForPath.insert(path, outcome);
    }

    // Script the value a path reports on read-back after a *successful* write,
    // independent of the value that was written. This models a driver that
    // accepts a value different from the requested one (clamping, rounding, or
    // rejecting silently), so a subsequent read()/read-back returns the override
    // rather than the requested value. With no override set, a successful write
    // reflects the requested value verbatim (equal read-back). Used by Property
    // 3 (read-back consistency after write).
    void setReadBackOverride(const QString &path, const QString &readBackValue)
    {
        m_readBackOverride.insert(path, readBackValue);
    }

    // Remove any read-back override for a path (subsequent successful writes
    // reflect the requested value again).
    void clearReadBackOverride(const QString &path)
    {
        m_readBackOverride.remove(path);
    }

    // ---- Inspection helpers (for tests) ----

    const QVector<WriteCall> &writeCalls() const { return m_writeCalls; }
    std::optional<WriteCall> lastWrite() const { return m_lastWrite; }
    void clearWriteCalls()
    {
        m_writeCalls.clear();
        m_lastWrite.reset();
    }

private:
    // A seeded hwmon device for name/label-driven resolution.
    struct HwmonEntry {
        QString name;                          // hwmon `name` value
        QMap<QString, QString> inputByLabel;   // tempN_label -> tempN_input path
        QString defaultInput;                  // used when no label matches
    };

    // Legacy name-family -> canonical hwmon input path fallback, so tests that
    // predate dynamic resolution (seeding hwmon0/hwmon1 temp inputs without name
    // files) keep resolving as before. Additive; consulted only after explicit
    // overrides and seeded entries fail.
    static const QMap<QString, QString> &legacyHwmonFallback()
    {
        static const QMap<QString, QString> map = {
            {QStringLiteral("k10temp"),
             QStringLiteral("/sys/class/hwmon/hwmon0/temp1_input")},
            {QStringLiteral("coretemp"),
             QStringLiteral("/sys/class/hwmon/hwmon0/temp1_input")},
            {QStringLiteral("amdgpu"),
             QStringLiteral("/sys/class/hwmon/hwmon1/temp1_input")},
            {QStringLiteral("nvidia"),
             QStringLiteral("/sys/class/hwmon/hwmon1/temp1_input")},
        };
        return map;
    }

    QMap<QString, QString> m_values;
    QMap<QString, bool> m_existence;
    QMap<QString, bool> m_writable;

    // Scripted resolved hwmon input path per device name (lowercased key).
    QMap<QString, QString> m_resolvedHwmonInput;
    // Scripted hwmon directory per device name (lowercased key) for
    // resolveHwmonDir().
    QMap<QString, QString> m_hwmonDirByName;
    // Seeded hwmon devices for name/label-driven resolution.
    QVector<HwmonEntry> m_hwmonEntries;

    WriteOutcome m_scriptedOutcome{WriteOutcome::Status::Ok, QString()};
    QMap<QString, WriteOutcome> m_scriptedOutcomeForPath;

    // Per-path read-back value a successful write reflects, when set, instead of
    // the requested value (models a perturbed driver read-back).
    QMap<QString, QString> m_readBackOverride;

    QVector<WriteCall> m_writeCalls;
    std::optional<WriteCall> m_lastWrite;
};

// -----------------------------------------------------------------------------
// FakeDBusAccess
// -----------------------------------------------------------------------------
//
// Scripted supported-mode list, active mode, service availability,
// fanCurvesSupported flag, and scriptable WriteOutcome for setGpuMode /
// setFanCurve. Records the last setGpuMode / setFanCurve arguments so later
// property tests can assert on them.
class FakeDBusAccess : public IDBusAccess {
public:
    struct FanCurveCall {
        QString profile;
        QVariantList points;
    };

    FakeDBusAccess() = default;
    ~FakeDBusAccess() override = default;

    // ---- IDBusAccess ----

    bool serviceAvailable(const QString &service) const override
    {
        return m_serviceAvailable.value(service, false);
    }

    std::optional<QStringList> supportedGpuModes() const override
    {
        return m_supportedGpuModes;
    }

    std::optional<QString> activeGpuMode() const override
    {
        return m_activeGpuMode;
    }

    WriteOutcome setGpuMode(const QString &mode) override
    {
        m_setGpuModeCalls.append(mode);
        m_lastSetGpuMode = mode;
        WriteOutcome outcome = m_setGpuModeOutcome;
        // Reflect a successful mode change into the active mode.
        if (outcome.status == WriteOutcome::Status::Ok)
            m_activeGpuMode = mode;
        return outcome;
    }

    bool fanCurvesSupported() const override
    {
        return m_fanCurvesSupported;
    }

    WriteOutcome setFanCurve(const QString &profile,
                             const QVariantList &points) override
    {
        m_setFanCurveCalls.append(FanCurveCall{profile, points});
        m_lastSetFanCurve = FanCurveCall{profile, points};
        return m_setFanCurveOutcome;
    }

    // ---- Seeding / scripting helpers ----

    void setServiceAvailable(const QString &service, bool available)
    {
        m_serviceAvailable.insert(service, available);
    }

    // Set the scripted supported GPU mode list (std::nullopt models a failed
    // query, e.g. an unavailable daemon).
    void setSupportedGpuModes(std::optional<QStringList> modes)
    {
        m_supportedGpuModes = std::move(modes);
    }

    void setActiveGpuMode(std::optional<QString> mode)
    {
        m_activeGpuMode = std::move(mode);
    }

    void setGpuModeOutcome(WriteOutcome outcome)
    {
        m_setGpuModeOutcome = outcome;
    }

    void setFanCurvesSupported(bool supported)
    {
        m_fanCurvesSupported = supported;
    }

    void setFanCurveOutcome(WriteOutcome outcome)
    {
        m_setFanCurveOutcome = outcome;
    }

    // ---- Inspection helpers (for tests) ----

    const QStringList &setGpuModeCalls() const { return m_setGpuModeCalls; }
    std::optional<QString> lastSetGpuMode() const { return m_lastSetGpuMode; }

    const QVector<FanCurveCall> &setFanCurveCalls() const { return m_setFanCurveCalls; }
    std::optional<FanCurveCall> lastSetFanCurve() const { return m_lastSetFanCurve; }

    void clearCalls()
    {
        m_setGpuModeCalls.clear();
        m_lastSetGpuMode.reset();
        m_setFanCurveCalls.clear();
        m_lastSetFanCurve.reset();
    }

private:
    QMap<QString, bool> m_serviceAvailable;
    std::optional<QStringList> m_supportedGpuModes;
    std::optional<QString> m_activeGpuMode;
    bool m_fanCurvesSupported = false;

    WriteOutcome m_setGpuModeOutcome{WriteOutcome::Status::Ok, QString()};
    WriteOutcome m_setFanCurveOutcome{WriteOutcome::Status::Ok, QString()};

    QStringList m_setGpuModeCalls;
    std::optional<QString> m_lastSetGpuMode;

    QVector<FanCurveCall> m_setFanCurveCalls;
    std::optional<FanCurveCall> m_lastSetFanCurve;
};

// -----------------------------------------------------------------------------
// FakeSettingsStore
// -----------------------------------------------------------------------------
//
// In-memory QMap<QString, ProfileSettings> backing load/save with correct
// per-profile isolation: saving one profile never disturbs another profile's
// stored settings, and loading a missing profile returns std::nullopt.
class FakeSettingsStore : public ISettingsStore {
public:
    FakeSettingsStore() = default;
    ~FakeSettingsStore() override = default;

    // ---- ISettingsStore ----

    std::optional<ProfileSettings> load(const QString &profile) const override
    {
        if (m_forceLoadFailure)
            return std::nullopt;
        if (!m_store.contains(profile))
            return std::nullopt;
        return m_store.value(profile);
    }

    bool save(const QString &profile, const ProfileSettings &s) override
    {
        // Record every save() invocation so tests can assert on the exact
        // number of persistence writes reaching the store (a save is counted
        // even when it is scripted to fail, because the write path was still
        // taken). Additive: does not change existing load/save behaviour.
        ++m_saveCallCount;
        m_saveCalls.append(profile);

        if (m_forceSaveFailure)
            return false;
        // Isolation: only the target profile's entry is touched.
        m_store.insert(profile, s);
        return true;
    }

    // ---- Seeding / scripting helpers ----

    // Seed a profile's settings directly (bypassing save()).
    void seed(const QString &profile, const ProfileSettings &s)
    {
        m_store.insert(profile, s);
    }

    // Force load() to always fail (models a missing / unreadable block).
    void setForceLoadFailure(bool fail) { m_forceLoadFailure = fail; }

    // Force save() to always fail.
    void setForceSaveFailure(bool fail) { m_forceSaveFailure = fail; }

    // ---- Inspection helpers (for tests) ----

    bool contains(const QString &profile) const { return m_store.contains(profile); }
    const QMap<QString, ProfileSettings> &store() const { return m_store; }

    // Number of times save() was invoked (regardless of success). Lets tests
    // assert that a read-only / edit-only call sequence performs zero
    // persistence writes.
    int saveCallCount() const { return m_saveCallCount; }
    // Profile names passed to each save() call, in order.
    const QStringList &saveCalls() const { return m_saveCalls; }
    void clearSaveCalls()
    {
        m_saveCallCount = 0;
        m_saveCalls.clear();
    }

private:
    QMap<QString, ProfileSettings> m_store;
    bool m_forceLoadFailure = false;
    bool m_forceSaveFailure = false;
    int m_saveCallCount = 0;
    QStringList m_saveCalls;
};

// -----------------------------------------------------------------------------
// FakeRyzenAdjAccess
// -----------------------------------------------------------------------------
//
// In-memory fake for the optional RyzenAdj backend. A test can script
// availability, the WriteOutcome returned by apply(), and the RyzenAdjSettings
// returned by readInfo(). apply() records every call so tests can assert on the
// exact settings passed down and the number of (re-)apply invocations.
class FakeRyzenAdjAccess : public IRyzenAdjAccess {
public:
    FakeRyzenAdjAccess() = default;
    ~FakeRyzenAdjAccess() override = default;

    // ---- IRyzenAdjAccess ----

    bool available() const override { return m_available; }

    WriteOutcome apply(const RyzenAdjSettings &settings) override
    {
        m_applyCalls.append(settings);
        m_lastApply = settings;
        if (m_applyOutcome.status == WriteOutcome::Status::Ok) {
            // Reflect a successful apply into the info read-back so a subsequent
            // readInfo() observes the applied values (only fields that were set).
            if (settings.stapmLimitW >= 0) m_info.stapmLimitW = settings.stapmLimitW;
            if (settings.fastLimitW >= 0) m_info.fastLimitW = settings.fastLimitW;
            if (settings.slowLimitW >= 0) m_info.slowLimitW = settings.slowLimitW;
            if (settings.apuSlowLimitW >= 0) m_info.apuSlowLimitW = settings.apuSlowLimitW;
            if (settings.tctlTempC >= 0) m_info.tctlTempC = settings.tctlTempC;
        }
        return m_applyOutcome;
    }

    std::optional<RyzenAdjSettings> readInfo() const override
    {
        if (!m_infoAvailable)
            return std::nullopt;
        return m_info;
    }

    // ---- Seeding / scripting helpers ----

    void setAvailable(bool available) { m_available = available; }
    void setApplyOutcome(WriteOutcome outcome) { m_applyOutcome = outcome; }
    void setInfo(const RyzenAdjSettings &info) { m_info = info; }
    void setInfoAvailable(bool available) { m_infoAvailable = available; }

    // ---- Inspection helpers (for tests) ----

    const QVector<RyzenAdjSettings> &applyCalls() const { return m_applyCalls; }
    std::optional<RyzenAdjSettings> lastApply() const { return m_lastApply; }
    int applyCallCount() const { return m_applyCalls.size(); }
    void clearApplyCalls()
    {
        m_applyCalls.clear();
        m_lastApply.reset();
    }

private:
    bool m_available = false;
    bool m_infoAvailable = true;
    WriteOutcome m_applyOutcome{WriteOutcome::Status::Ok, QString()};
    RyzenAdjSettings m_info;
    QVector<RyzenAdjSettings> m_applyCalls;
    std::optional<RyzenAdjSettings> m_lastApply;
};
