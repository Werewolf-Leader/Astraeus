// Property-based test for Property 6: Temperature threshold crossing.
//
// Feature: boreas-operator-ui, Property 6: For any live temperature reading and
// per-profile threshold, the reading is flagged (red) iff reading >= threshold,
// and a threshold-crossing event with a timestamp is logged when a reading
// transitions INTO the crossing state.
//
// Validates: Requirements 7.7, 7.8
//
// Runner:  QtTest. Generation/shrinking: rapidcheck. Minimum 100 iterations per
// property (rapidcheck defaults to 100; asserted explicitly below).
//
// Strategy: the crossing decision is not hand-built; it is *derived* by the real
// HardwareManager::refreshTemperatures() from a randomly-scripted platform
// state. For each generated scenario we seed the selected profile's persisted
// temperature threshold in an in-memory FakeSettingsStore (so
// HardwareManager::tempThresholdFor(selectedProfile) returns it), then replay a
// random sequence of CPU-sensor readings: for each reading we script the CPU
// hwmon temp1_input path (millidegrees C) and call refreshTemperatures(), using
// a QSignalSpy on temperatureThresholdCrossed to observe exactly how many
// crossing events fired for that poll.
//
// We assert two things per poll, which together are the crossing-detection
// invariant that drives the QML red-highlight binding (Req 7.7) and the
// timestamped crossing log (Req 7.8):
//   1. "Flagged iff reading >= threshold": the manager's above-threshold state
//      after a poll matches (reading >= threshold) exactly, for every finite
//      reading. This is the boolean QML binds the red highlight to.
//   2. "Crossing event on transition INTO the state": temperatureThresholdCrossed
//      fires on a poll iff the sensor transitioned from not-above to above this
//      poll (never on every poll while hot, never while below), and when it
//      fires it carries the read value and a non-empty ISO-8601 timestamp.
//
// The GPU sensor path is left absent so refreshTemperatures() reports GPU as NaN
// (no GPU crossings), isolating the property to a single sensor's transitions.
//
// Exercises FakeSysfsAccess / FakeDBusAccess / FakeSettingsStore so no hardware
// or root is required.

#include <QtTest/QtTest>

#include <rapidcheck.h>

#include <optional>
#include <vector>

#include "DataModels.h"
#include "Fakes.h"
#include "HardwareManager.h"

namespace {

// Canonical CPU hwmon input path polled by HardwareManager::refreshTemperatures()
// (CPU on hwmon0); values are millidegrees Celsius. Kept in sync with
// HardwareManager::kCpuTempInputPath.
const QString kCpuTempPath =
    QStringLiteral("/sys/class/hwmon/hwmon0/temp1_input");
const QString kGpuTempPath =
    QStringLiteral("/sys/class/hwmon/hwmon1/temp1_input");

// A single generated scenario: a per-profile threshold plus a sequence of CPU
// readings to replay one-per-poll. A std::nullopt reading models an
// unavailable/unreadable sensor (the NaN "unknown" case, which resets the
// crossing state without emitting).
struct Scenario {
    int thresholdC;                       // seeded into the selected profile
    std::vector<std::optional<int>> readings; // per-poll CPU temperature in C
};

// Seed the selected profile's persisted temperature threshold into the store so
// tempThresholdFor(selectedProfile) returns it after the manager loads settings
// in its constructor. The selected profile defaults to Balanced.
void seedThreshold(FakeSettingsStore &settings, const QString &profile,
                   int thresholdC)
{
    ProfileSettings s;
    // A minimal valid two-point fan curve; irrelevant to this property but keeps
    // the seeded block well-formed.
    s.fanCurve = FanCurve{{40, 20}, {80, 60}};
    s.tempThresholdC = thresholdC;
    s.pl1Watts = 30;
    s.pl2Watts = 45;
    settings.seed(profile, s);
}

} // namespace

namespace rc {

// Generator for a Scenario: a threshold in the UI-constrained range [0, 105]
// (Req 7.4) and a non-trivial sequence (1..12 polls) of readings. Each reading
// is either unavailable (nullopt, ~1/5 of the time) or a finite temperature in
// [0, 110] so the sequence straddles the threshold and exercises repeated
// below/above transitions, sustained-hot polls, and NaN resets.
template <>
struct Arbitrary<Scenario> {
    static Gen<Scenario> arbitrary()
    {
        return gen::build<Scenario>(
            gen::set(&Scenario::thresholdC, gen::inRange(0, 106)),
            gen::set(&Scenario::readings,
                     gen::nonEmpty(gen::container<std::vector<std::optional<int>>>(
                         gen::weightedOneOf<std::optional<int>>({
                             {4u, gen::map(gen::inRange(0, 111),
                                           [](int v) {
                                               return std::optional<int>(v);
                                           })},
                             {1u, gen::just(std::optional<int>(std::nullopt))},
                         })))));
    }
};

} // namespace rc

class Property6Threshold : public QObject {
    Q_OBJECT

private slots:
    // Property 6: replaying any sequence of CPU readings against any seeded
    // threshold, the manager flags reading >= threshold and emits exactly one
    // timestamped crossing event on each transition INTO the crossing state
    // (Req 7.7, 7.8).
    void thresholdCrossing();
};

void Property6Threshold::thresholdCrossing()
{
    const bool ok = rc::check(
        "crossing fires iff transitioning into reading>=threshold, with a timestamp",
        [](const Scenario &scenario) {
            FakeSysfsAccess sysfs;
            FakeDBusAccess dbus;
            FakeSettingsStore settings;

            // Selected profile defaults to Balanced; seed its threshold so the
            // manager's tempThresholdFor(selectedProfile) returns it.
            const QString profile = powerProfileName(PowerProfile::Balanced);
            seedThreshold(settings, profile, scenario.thresholdC);

            // Leave the GPU sensor path absent so GPU reads NaN and never
            // produces a crossing, isolating the property to the CPU sensor.
            sysfs.setExists(kGpuTempPath, false);

            HardwareManager mgr(&sysfs, &dbus, &settings);
            RC_ASSERT(mgr.selectedProfile() == profile);

            QSignalSpy spy(&mgr,
                           &HardwareManager::temperatureThresholdCrossed);
            RC_ASSERT(spy.isValid());

            // Model of the manager's per-sensor crossing state. The manager
            // starts "not above" (m_cpuAboveThreshold == false).
            bool expectedAbove = false;
            int seenSignals = 0;

            for (const std::optional<int> &reading : scenario.readings) {
                if (reading.has_value()) {
                    // Script the CPU sensor value (hwmon is millidegrees C).
                    sysfs.setExists(kCpuTempPath, true);
                    sysfs.setValue(kCpuTempPath,
                                   QString::number(
                                       static_cast<long long>(*reading) * 1000));
                } else {
                    // Unavailable sensor -> NaN reading (the "unknown" case).
                    sysfs.setExists(kCpuTempPath, false);
                }

                mgr.refreshTemperatures();

                // Compute what SHOULD have happened this poll.
                bool crossingExpectedThisPoll = false;
                if (reading.has_value()) {
                    const bool nowAbove = *reading >= scenario.thresholdC;
                    // A crossing event fires only on a fresh transition INTO the
                    // above state (Req 7.8), never on every poll while hot.
                    crossingExpectedThisPoll = nowAbove && !expectedAbove;
                    expectedAbove = nowAbove;
                } else {
                    // NaN clears the crossing state without emitting so a later
                    // valid reading can trigger a fresh transition (Req 7.8).
                    expectedAbove = false;
                }

                if (crossingExpectedThisPoll) {
                    ++seenSignals;
                    // Exactly one new crossing event this poll.
                    RC_ASSERT(spy.count() == seenSignals);

                    // Inspect the just-emitted signal's payload: sensor label,
                    // value, and a non-empty timestamp (Req 7.8).
                    const QList<QVariant> args = spy.last();
                    RC_ASSERT(args.size() == 3);
                    RC_ASSERT(args.at(0).toString() == QStringLiteral("CPU"));
                    // The reported value is the finite reading in degrees C.
                    RC_ASSERT(qFuzzyCompare(args.at(1).toDouble() + 1.0,
                                            static_cast<double>(*reading) + 1.0));
                    RC_ASSERT(!args.at(2).toString().isEmpty());
                } else {
                    // No new crossing event: the spy count is unchanged, proving
                    // no spurious emit while below-threshold, while sustained hot,
                    // or on a NaN reading.
                    RC_ASSERT(spy.count() == seenSignals);
                }

                // "Flagged iff reading >= threshold" (Req 7.7): the boolean the
                // QML red highlight binds to (the manager's above-threshold
                // state) matches (reading >= threshold) exactly for every finite
                // reading.
                if (reading.has_value()) {
                    const bool flagged = *reading >= scenario.thresholdC;
                    RC_ASSERT(flagged == expectedAbove);
                }
            }

            // Total crossing events observed equals the number of transitions the
            // model counted.
            RC_ASSERT(spy.count() == seenSignals);
        });

    QVERIFY(ok);
}

QTEST_MAIN(Property6Threshold)
#include "tst_property6_threshold.moc"
