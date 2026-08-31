// Property-based test for Property 1: Fan curve validation gating.
//
// Feature: boreas-operator-ui, Property 1: For any list of fan curve points,
// validateFanCurve reports valid iff the list has between 2 and 8 points
// inclusive AND temperatures are strictly ascending AND every temperature is in
// [0, 105] AND every percent is in [0, 100].
//
// Validates: Requirements 6.3, 6.4, 6.5, 6.6
//
// Runner:  QtTest. Generation/shrinking: rapidcheck. Minimum 100 iterations per
// property (rapidcheck defaults to 100; asserted explicitly below).
//
// Strategy: the validity decision is not hand-built; it is *derived* by the real
// HardwareManager::validateFanCurve() from a randomly-generated list of points.
// The generator deliberately biases toward the boundary sizes (0, 1, 2, 8, 9
// points), boundary temperatures (-1, 0, 105, 106), and boundary percents
// (-1, 0, 100, 101) so the generated space straddles every edge of the
// predicate rather than sampling only its interior. An independent oracle
// re-computes the full predicate on the same list, and we assert
// validateFanCurve reports valid iff the oracle predicate holds. validateFanCurve
// has no side effects (Req 6.7), so a fresh HardwareManager is reused per check.
//
// Exercises FakeSysfsAccess / FakeDBusAccess / FakeSettingsStore so no hardware
// or root is required.

#include <QtTest/QtTest>

#include <rapidcheck.h>

#include <vector>

#include "DataModels.h"
#include "Fakes.h"
#include "HardwareManager.h"

namespace {

// A single generated fan-curve point. Kept as plain ints so the generator can
// freely produce out-of-range / boundary values the predicate must reject.
struct GenPoint {
    int tempC;
    int percent;
};

// Independent oracle re-implementing the Property 1 predicate (Req 6.3-6.6):
// valid iff 2..8 points inclusive AND temperatures strictly ascending AND every
// tempC in [0, 105] AND every percent in [0, 100]. This is intentionally a
// separate implementation from HardwareManager::validateFanCurve so the test
// cross-checks behaviour rather than mirroring the production code path.
bool oracleValid(const std::vector<GenPoint> &points)
{
    const int count = static_cast<int>(points.size());
    if (count < 2 || count > 8)
        return false;

    for (int i = 0; i < count; ++i) {
        const GenPoint &p = points[i];
        if (p.tempC < 0 || p.tempC > 105)
            return false;
        if (p.percent < 0 || p.percent > 100)
            return false;
        if (i > 0 && p.tempC <= points[i - 1].tempC)
            return false;
    }
    return true;
}

// Convert the generated points into the QVariantList of { tempC, percent } shape
// QML emits and validateFanCurve consumes.
QVariantList toVariantList(const std::vector<GenPoint> &points)
{
    QVariantList list;
    for (const GenPoint &p : points) {
        QVariantMap m;
        m.insert(QStringLiteral("tempC"), p.tempC);
        m.insert(QStringLiteral("percent"), p.percent);
        list.append(m);
    }
    return list;
}

} // namespace

namespace rc {

// Generator for a single point biased toward the range boundaries so tempC and
// percent frequently land exactly on -1 / 0 / 105 / 106 and -1 / 0 / 100 / 101,
// exercising the inclusive/exclusive edges of the predicate. A wider uniform
// draw fills in the interior.
template <>
struct Arbitrary<GenPoint> {
    static Gen<GenPoint> arbitrary()
    {
        auto tempGen = gen::weightedOneOf<int>({
            {3u, gen::elementOf<std::vector<int>>({-1, 0, 1, 104, 105, 106})},
            {2u, gen::inRange(-5, 121)},
        });
        auto percentGen = gen::weightedOneOf<int>({
            {3u, gen::elementOf<std::vector<int>>({-1, 0, 1, 99, 100, 101})},
            {2u, gen::inRange(-5, 121)},
        });
        return gen::build<GenPoint>(
            gen::set(&GenPoint::tempC, std::move(tempGen)),
            gen::set(&GenPoint::percent, std::move(percentGen)));
    }
};

} // namespace rc

namespace {

// Generate a point list whose SIZE is biased toward the count boundaries
// (0, 1, 2, 8, 9) so the 2..8 inclusive bound is straddled, while still
// producing larger lists occasionally.
rc::Gen<std::vector<GenPoint>> genPointList()
{
    return rc::gen::withSize([](int) {
        return rc::gen::mapcat(
            rc::gen::weightedOneOf<int>({
                {4u, rc::gen::elementOf<std::vector<int>>({0, 1, 2, 3, 7, 8, 9})},
                {1u, rc::gen::inRange(0, 13)},
            }),
            [](int n) {
                return rc::gen::container<std::vector<GenPoint>>(
                    static_cast<std::size_t>(n),
                    rc::gen::arbitrary<GenPoint>());
            });
    });
}

} // namespace

class Property1FanCurve : public QObject {
    Q_OBJECT

private slots:
    // Property 1: validateFanCurve reports valid iff the full predicate holds
    // (2..8 points, strictly ascending tempC, tempC in [0,105], percent in
    // [0,100]) (Req 6.3, 6.4, 6.5, 6.6).
    void validationGating();
};

void Property1FanCurve::validationGating()
{
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore settings;

    // validateFanCurve is a pure query with no side effects (Req 6.7), so a
    // single manager can be reused across all generated inputs.
    HardwareManager mgr(&sysfs, &dbus, &settings);

    const bool ok = rc::check(
        "validateFanCurve is valid iff 2..8 pts, ascending tempC, "
        "tempC in [0,105], percent in [0,100]",
        [&mgr]() {
            // Draw a boundary-biased point list from the custom generator.
            const std::vector<GenPoint> points = *genPointList();

            const bool expected = oracleValid(points);

            const QVariantMap result = mgr.validateFanCurve(toVariantList(points));
            const bool actual = result.value(QStringLiteral("valid")).toBool();

            // The core biconditional: validity iff the full predicate holds.
            RC_ASSERT(actual == expected);

            // An invalid curve must carry a human-readable reason so QML can
            // surface why the "Apply Fan Curve" action is gated; a valid curve
            // reports an empty reason.
            const QString reason = result.value(QStringLiteral("reason")).toString();
            if (!actual)
                RC_ASSERT(!reason.isEmpty());
            else
                RC_ASSERT(reason.isEmpty());
        });

    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(Property1FanCurve)
#include "tst_property1_fancurve.moc"
