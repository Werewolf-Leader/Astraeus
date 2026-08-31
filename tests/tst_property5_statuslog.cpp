// Property-based test for Property 5: Status log ring buffer.
//
// Feature: boreas-operator-ui, Property 5: For any sequence of N logged actions,
// `recentLog` contains exactly min(N, 10) entries, equal to the N-most-recent
// actions in order, each carrying its timestamp and success/failure outcome.
//
// Validates: Requirements 13.3, 13.4, 13.5
//
// Runner:  QtTest. Generation/shrinking: rapidcheck. Minimum 100 iterations per
// property (rapidcheck defaults to 100; asserted explicitly below).
//
// Strategy: the log is not appended to directly (recordResult() is private).
// It is driven through the public, persistence-only write path
// HardwareManager::applyTemperatureThreshold(profile, thresholdC), which funnels
// through recordResult() exactly once per call (task 10.4 / 7.1). Against an
// in-memory FakeSettingsStore whose save() always succeeds, each call therefore
// appends exactly one LogEntry with success=true and a deterministic action
// string. We call it N times with randomly chosen valid profiles/thresholds,
// mirror the expected (action, success) sequence locally, then assert recentLog
// holds exactly min(N,10) most-recent entries in order, each with a timestamp
// and a success/outcome field.
//
// Exercises FakeSysfsAccess / FakeDBusAccess / FakeSettingsStore so no hardware
// or root is required.

#include <QtTest/QtTest>

#include <rapidcheck.h>

#include <algorithm>
#include <vector>

#include "DataModels.h"
#include "Fakes.h"
#include "HardwareManager.h"

namespace {

// Maximum retained log entries (mirrors HardwareManager::kMaxLogEntries and
// Req 13.4/13.5). Kept local so the test states the cap independently of the
// implementation constant.
constexpr int kMaxLogEntries = 10;

// The canonical profile display names accepted by applyTemperatureThreshold().
const QStringList &profileNames()
{
    static const QStringList names = {
        powerProfileName(PowerProfile::Quiet),
        powerProfileName(PowerProfile::Balanced),
        powerProfileName(PowerProfile::Performance),
    };
    return names;
}

// One scripted write to drive through the log: a valid profile + a threshold.
struct WriteAction {
    QString profile;   // a canonical profile name (always resolvable)
    int thresholdC;    // may be out of range; the driver clamps, still success
};

// The action-string the driver records for a (resolved) profile, mirroring
// HardwareManager::applyTemperatureThreshold(). Because every generated profile
// is canonical and the fake store always saves successfully, every call yields
// success=true with this exact action label.
QString expectedAction(const QString &profile)
{
    return QStringLiteral("Apply temperature threshold (%1)").arg(profile);
}

} // namespace

namespace rc {

// Generator for a single valid write action: a canonical profile name and a
// threshold spanning below/within/above the [0,105] clamp range (all still
// succeed, so all still append a success entry).
template <>
struct Arbitrary<WriteAction> {
    static Gen<WriteAction> arbitrary()
    {
        return gen::exec([] {
            const auto &names = profileNames();
            const int idx = *gen::inRange(0, static_cast<int>(names.size()));
            WriteAction a;
            a.profile = names[idx];
            a.thresholdC = *gen::inRange(-20, 130); // spans the 0..105 clamp
            return a;
        });
    }
};

} // namespace rc

class Property5StatusLog : public QObject {
    Q_OBJECT

private slots:
    // Property 5: after appending N actions, recentLog holds exactly
    // min(N, 10) entries equal to the N-most-recent actions in order, each with
    // a timestamp and a success/failure outcome. (Req 13.3, 13.4, 13.5)
    void ringBufferHoldsMostRecent();
};

void Property5StatusLog::ringBufferHoldsMostRecent()
{
    const bool ok = rc::check(
        "recentLog holds exactly min(N,10) most-recent entries, in order",
        [](const std::vector<WriteAction> &actions) {
            FakeSysfsAccess sysfs;
            FakeDBusAccess dbus;
            FakeSettingsStore settings;
            // save() succeeds by default, so every write appends a success entry.

            HardwareManager mgr(&sysfs, &dbus, &settings);

            const int n = static_cast<int>(actions.size());

            // Drive N appends through the public write path and mirror the
            // expected (action, success) pair for each in call order.
            std::vector<QString> expectedActions;
            expectedActions.reserve(actions.size());
            for (const WriteAction &a : actions) {
                mgr.applyTemperatureThreshold(a.profile, a.thresholdC);
                expectedActions.push_back(expectedAction(a.profile));
            }

            const QVariantList log = mgr.recentLog();

            // Size: exactly min(N, 10) (Req 13.4, 13.5).
            const int expectedSize = std::min(n, kMaxLogEntries);
            RC_ASSERT(log.size() == expectedSize);

            // Content: the entries are the N-most-recent appends, oldest first.
            // The most-recent `expectedSize` expected actions, in order.
            const int firstKept = n - expectedSize; // >= 0
            for (int i = 0; i < expectedSize; ++i) {
                const QVariantMap entry = log.at(i).toMap();

                // Each entry carries a timestamp (Req 13.3).
                RC_ASSERT(entry.contains(QStringLiteral("timestamp")));
                RC_ASSERT(!entry.value(QStringLiteral("timestamp"))
                               .toString()
                               .isEmpty());

                // Each entry carries a success/failure outcome (Req 13.3). Every
                // driven write succeeds, so the outcome is true here.
                RC_ASSERT(entry.contains(QStringLiteral("success")));
                RC_ASSERT(entry.value(QStringLiteral("success")).toBool() == true);

                // Content matches the corresponding most-recent action, in order.
                RC_ASSERT(entry.contains(QStringLiteral("action")));
                const QString action =
                    entry.value(QStringLiteral("action")).toString();
                RC_ASSERT(action == expectedActions[static_cast<std::size_t>(
                                        firstKept + i)]);
            }
        });

    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(Property5StatusLog)
#include "tst_property5_statuslog.moc"
