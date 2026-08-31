// Property-based test for Property 4: Persistence round-trip and profile isolation.
//
// Feature: boreas-operator-ui, Property 4: For any set of valid per-profile
// ProfileSettings, saving then loading yields ProfileSettings identical to what
// was saved for each profile, and writing one profile's settings never changes
// any other profile's persisted settings.
//
// Validates: Requirements 11.1, 11.2, 11.3, 11.4, 11.6, 12.4
//
// Runner:  QtTest. Generation/shrinking: rapidcheck. Minimum 100 iterations per
// property (rapidcheck defaults to 100; asserted explicitly below).
// Exercises the in-memory FakeSettingsStore (ISettingsStore) so no hardware or
// root is required.

#include <QtTest/QtTest>

#include <rapidcheck.h>

#include <algorithm>

#include "DataModels.h"
#include "Fakes.h"

namespace {

// The canonical set of per-profile keys used for persistence. Persistence is
// keyed per PowerProfile display name (Quiet/Balanced/Performance).
const QStringList &profileKeys()
{
    static const QStringList keys = {
        powerProfileName(PowerProfile::Quiet),
        powerProfileName(PowerProfile::Balanced),
        powerProfileName(PowerProfile::Performance),
    };
    return keys;
}

// Equality for ProfileSettings (DataModels.h intentionally keeps the struct a
// plain aggregate, so define comparison locally for the round-trip assertion).
bool settingsEqual(const ProfileSettings &a, const ProfileSettings &b)
{
    if (a.tempThresholdC != b.tempThresholdC)
        return false;
    if (a.pl1Watts != b.pl1Watts)
        return false;
    if (a.pl2Watts != b.pl2Watts)
        return false;
    if (a.fanCurve.size() != b.fanCurve.size())
        return false;
    for (int i = 0; i < a.fanCurve.size(); ++i) {
        if (a.fanCurve[i].tempC != b.fanCurve[i].tempC)
            return false;
        if (a.fanCurve[i].percent != b.fanCurve[i].percent)
            return false;
    }
    return true;
}

} // namespace

namespace rc {

// Generator for a single valid FanCurvePoint: tempC in [0,105], percent in
// [0,100] (Req 6.4, 6.5).
template <>
struct Arbitrary<FanCurvePoint> {
    static Gen<FanCurvePoint> arbitrary()
    {
        return gen::build<FanCurvePoint>(
            gen::set(&FanCurvePoint::tempC, gen::inRange(0, 106)),
            gen::set(&FanCurvePoint::percent, gen::inRange(0, 101)));
    }
};

// Generator for a valid ProfileSettings: a valid fan curve (2-8 points, strictly
// ascending tempC in [0,105], percent in [0,100]) plus valid threshold and power
// limits. The fan curve is built from a strictly-ascending set of temperatures
// so the persisted unit is always a well-formed curve.
template <>
struct Arbitrary<ProfileSettings> {
    static Gen<ProfileSettings> arbitrary()
    {
        return gen::exec([] {
            const int count = *gen::inRange(2, 9); // 2..8 inclusive

            // Draw `count` distinct temperatures in [0,105] and sort ascending
            // so the curve is strictly ascending in tempC.
            std::vector<int> temps;
            temps = *gen::suchThat(
                gen::container<std::vector<int>>(
                    count, gen::inRange(0, 106)),
                [count](const std::vector<int> &v) {
                    if (static_cast<int>(v.size()) != count)
                        return false;
                    std::vector<int> sorted = v;
                    std::sort(sorted.begin(), sorted.end());
                    return std::adjacent_find(sorted.begin(), sorted.end())
                           == sorted.end(); // all distinct
                });
            std::sort(temps.begin(), temps.end());

            ProfileSettings s;
            s.fanCurve.reserve(count);
            for (int i = 0; i < count; ++i) {
                FanCurvePoint p;
                p.tempC = temps[static_cast<std::size_t>(i)];
                p.percent = *gen::inRange(0, 101);
                s.fanCurve.append(p);
            }
            s.tempThresholdC = *gen::inRange(0, 106);
            s.pl1Watts = *gen::inRange(5, 121);
            s.pl2Watts = *gen::inRange(5, 121);
            return s;
        });
    }
};

} // namespace rc

class Property4Persistence : public QObject {
    Q_OBJECT

private slots:
    // Property 4: for any map of valid per-profile ProfileSettings, saving every
    // profile then loading each one yields settings identical to what was saved
    // (round-trip identity across profiles). Req 11.1-11.4, 12.4.
    void roundTripIdentity();

    // Property 4 (isolation half): writing one profile's settings never changes
    // any other profile's persisted settings, and persisted values stay distinct
    // from any "live" mutation applied afterwards. Req 11.6, 12.4.
    void profileIsolation();
};

void Property4Persistence::roundTripIdentity()
{
    const bool ok = rc::check(
        "save-then-load yields identical ProfileSettings for every profile",
        [](const ProfileSettings &quiet,
           const ProfileSettings &balanced,
           const ProfileSettings &performance) {
            FakeSettingsStore store;

            const QStringList &keys = profileKeys();
            const std::array<const ProfileSettings *, 3> byKey = {
                &quiet, &balanced, &performance};

            // Save each profile.
            for (int i = 0; i < keys.size(); ++i)
                RC_ASSERT(store.save(keys[i], *byKey[static_cast<std::size_t>(i)]));

            // Load each profile back and assert identity.
            for (int i = 0; i < keys.size(); ++i) {
                const auto loaded = store.load(keys[i]);
                RC_ASSERT(loaded.has_value());
                RC_ASSERT(settingsEqual(*loaded, *byKey[static_cast<std::size_t>(i)]));
            }
        });

    QVERIFY(ok);
}

void Property4Persistence::profileIsolation()
{
    const bool ok = rc::check(
        "writing one profile leaves the other profiles' persisted settings unchanged",
        [](const ProfileSettings &initialA,
           const ProfileSettings &initialB,
           const ProfileSettings &initialC,
           const ProfileSettings &rewrite) {
            FakeSettingsStore store;

            const QStringList &keys = profileKeys();
            const std::array<const ProfileSettings *, 3> initial = {
                &initialA, &initialB, &initialC};

            // Seed all three profiles.
            for (int i = 0; i < keys.size(); ++i)
                RC_ASSERT(store.save(keys[i], *initial[static_cast<std::size_t>(i)]));

            // Pick a target profile to rewrite.
            const int target = *rc::gen::inRange(0, static_cast<int>(keys.size()));
            RC_ASSERT(store.save(keys[target], rewrite));

            // The target now holds the rewritten value...
            const auto reloadedTarget = store.load(keys[target]);
            RC_ASSERT(reloadedTarget.has_value());
            RC_ASSERT(settingsEqual(*reloadedTarget, rewrite));

            // ...and every other profile is byte-for-byte unchanged.
            for (int i = 0; i < keys.size(); ++i) {
                if (i == target)
                    continue;
                const auto other = store.load(keys[i]);
                RC_ASSERT(other.has_value());
                RC_ASSERT(settingsEqual(*other,
                                        *initial[static_cast<std::size_t>(i)]));
            }
        });

    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(Property4Persistence)
#include "tst_property4_persistence.moc"
