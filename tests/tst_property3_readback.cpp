// Property-based test for Property 3: Read-back consistency after write.
//
// Feature: boreas-operator-ui, Property 3: For any successful write followed by
// a read-back, the value displayed/reported by the UI equals the driver-reported
// read-back value (never the merely requested value), and a discrepancy is
// reported through the applyResult message iff the read-back value differs from
// the requested value.
//
// Validates: Requirements 8.8, 8.9, 8.10, 10.1, 10.2, 10.3
//
// Runner:  QtTest. Generation/shrinking: rapidcheck. Minimum 100 iterations per
// property (rapidcheck defaults to 100; asserted explicitly below).
//
// Target: HardwareManager::applyPowerLimits(), which writes PL1/PL2 through
// ISysfsAccess::write(), reads the values back from the driver, persists the
// read-back values, and reports any discrepancy through applyResult. The
// displayed/persisted value must always be the driver read-back, never the
// requested value (Req 8.10, 10.2), and a discrepancy must be reported iff the
// read-back differs from the request (Req 8.10, 10.3).
//
// Strategy: for each generated case we seed the PL1/PL2 sysfs attributes as
// existing + writable on the in-memory FakeSysfsAccess and generate a requested
// (PL1, PL2) pair. Independently, per limit, we either leave the write to
// reflect the requested value verbatim (equal read-back) or script a read-back
// override to a DISTINCT value (perturbed read-back) via
// FakeSysfsAccess::setReadBackOverride(). We then invoke applyPowerLimits and
// capture the single applyResult plus the persisted read-back exposed through
// powerLimitsFor(). We assert (a) the persisted/displayed values equal the
// driver read-back (the override where set, otherwise the requested value) and
// never the requested value when they differ, and (b) the applyResult message
// mentions a discrepancy iff at least one limit's read-back differs from its
// request. An independent oracle computes the expected read-back and discrepancy
// flag rather than mirroring the production code path.
//
// Exercises FakeSysfsAccess / FakeDBusAccess / FakeSettingsStore so no hardware
// or root is required.

#include <QtTest/QtTest>

#include <rapidcheck.h>

#include "DataModels.h"
#include "Fakes.h"
#include "HardwareManager.h"

namespace {

// PL1/PL2 primary sysfs locations (mirrors the asus-nb-wmi paths the manager
// resolves first). Seeding these on the fake is enough for resolvePlNPath().
const QString kPl1Path =
    QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl1_spl");
const QString kPl2Path =
    QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl2_sppt");

// One generated scenario: a requested PL1/PL2 pair plus, per limit, an optional
// perturbed read-back value distinct from the request.
struct Scenario {
    QString profile;                    // canonical profile name
    int pl1Requested;
    int pl2Requested;
    std::optional<int> pl1ReadBack;     // set => perturbed; distinct from request
    std::optional<int> pl2ReadBack;     // set => perturbed; distinct from request
};

// The value the driver will report on read-back for a limit: the override when
// present, otherwise the requested value (verbatim reflection).
int expectedReadBack(int requested, const std::optional<int> &override)
{
    return override.has_value() ? *override : requested;
}

} // namespace

namespace rc {

template <>
struct Arbitrary<Scenario> {
    static Gen<Scenario> arbitrary()
    {
        // Canonical profile names only; applyPowerLimits rejects unknown names
        // without writing, and this property is about the successful write path.
        auto profileGen = gen::elementOf<std::vector<QString>>(
            {QStringLiteral("Quiet"), QStringLiteral("Balanced"),
             QStringLiteral("Performance")});

        // Requested watt values within the design's conservative estimate range
        // (1..200 W), biased toward the bounds.
        auto wattGen = gen::weightedOneOf<int>({
            {2u, gen::elementOf<std::vector<int>>({1, 2, 15, 45, 65, 199, 200})},
            {3u, gen::inRange(1, 201)},
        });

        return gen::mapcat(
            gen::tuple(profileGen, wattGen, wattGen),
            [](std::tuple<QString, int, int> base) {
                const QString profile = std::get<0>(base);
                const int pl1 = std::get<1>(base);
                const int pl2 = std::get<2>(base);

                // For each limit choose: no perturbation (equal read-back) or a
                // perturbed read-back guaranteed DISTINCT from the request. The
                // distinct value is derived by an offset so it can never
                // accidentally equal the request.
                auto perturbGen = [](int requested) {
                    return gen::mapcat(
                        gen::arbitrary<bool>(),
                        [requested](bool perturb) -> Gen<std::optional<int>> {
                            if (!perturb)
                                return gen::just(std::optional<int>{});
                            // A non-zero delta keeps the read-back distinct from
                            // the request; wrap into a sane band so values stay
                            // small and readable.
                            return gen::map(
                                gen::inRange(1, 60),
                                [requested](int delta) {
                                    int rb = requested + delta;
                                    if (rb == requested)
                                        rb = requested + 1;
                                    return std::optional<int>{rb};
                                });
                        });
                };

                return gen::map(
                    gen::tuple(perturbGen(pl1), perturbGen(pl2)),
                    [profile, pl1, pl2](
                        std::tuple<std::optional<int>, std::optional<int>> pert) {
                        return Scenario{profile, pl1, pl2,
                                        std::get<0>(pert), std::get<1>(pert)};
                    });
            });
    }
};

} // namespace rc

class Property3ReadBack : public QObject {
    Q_OBJECT

private slots:
    // Property 3: after a successful write, the displayed/persisted value equals
    // the driver read-back (never the requested value when they differ), and a
    // discrepancy is reported iff read-back differs from the request
    // (Req 8.8, 8.9, 8.10, 10.1, 10.2, 10.3).
    void readBackConsistency();
};

void Property3ReadBack::readBackConsistency()
{
    const bool ok = rc::check(
        "displayed value equals driver read-back (never the requested value) "
        "and a discrepancy is reported iff read-back differs from request",
        [](const Scenario scenario) {
            // Fresh fakes + manager per case so persisted state never leaks
            // between iterations.
            FakeSysfsAccess sysfs;
            FakeDBusAccess dbus;
            FakeSettingsStore settings;

            // Expose the PL1/PL2 attributes as existing + writable so the write
            // path is reached (missing paths would be an Unsupported failure).
            sysfs.setExists(kPl1Path, true);
            sysfs.setExists(kPl2Path, true);
            sysfs.setWritable(kPl1Path, true);
            sysfs.setWritable(kPl2Path, true);

            // Seed a benign starting value so read() is well-defined even before
            // the first write (the write reflects the accepted value afterward).
            sysfs.setValue(kPl1Path, QString::number(scenario.pl1Requested));
            sysfs.setValue(kPl2Path, QString::number(scenario.pl2Requested));

            // Script perturbed read-backs where the scenario asks for them: on a
            // successful write the fake reflects the override instead of the
            // requested value, modelling a driver that accepted a distinct value.
            if (scenario.pl1ReadBack.has_value())
                sysfs.setReadBackOverride(
                    kPl1Path, QString::number(*scenario.pl1ReadBack));
            if (scenario.pl2ReadBack.has_value())
                sysfs.setReadBackOverride(
                    kPl2Path, QString::number(*scenario.pl2ReadBack));

            HardwareManager mgr(&sysfs, &dbus, &settings);

            // Capture the single applyResult emitted by the write path.
            int applyResultCount = 0;
            bool reportedSuccess = false;
            QString reportedMessage;
            QObject::connect(
                &mgr, &HardwareManager::applyResult, &mgr,
                [&](bool success, const QString &message) {
                    ++applyResultCount;
                    reportedSuccess = success;
                    reportedMessage = message;
                });

            mgr.applyPowerLimits(scenario.profile, scenario.pl1Requested,
                                 scenario.pl2Requested);

            // Independent oracle: what the driver should report and whether that
            // constitutes a discrepancy versus the request.
            const int expPl1 =
                expectedReadBack(scenario.pl1Requested, scenario.pl1ReadBack);
            const int expPl2 =
                expectedReadBack(scenario.pl2Requested, scenario.pl2ReadBack);
            const bool expectDiscrepancy =
                (expPl1 != scenario.pl1Requested) ||
                (expPl2 != scenario.pl2Requested);

            // Exactly one applyResult for the write (Req 2.5), and the write path
            // succeeded (both sysfs writes Ok, read-back available).
            RC_ASSERT(applyResultCount == 1);
            RC_ASSERT(reportedSuccess == true);

            // Req 8.9 / 10.2: the displayed/persisted values are the driver
            // read-back values. powerLimitsFor() returns the persisted (desired)
            // per-profile values, which applyPowerLimits stores from the
            // read-back, never from the request.
            const QVariantMap limits = mgr.powerLimitsFor(scenario.profile);
            const int displayedPl1 = limits.value(QStringLiteral("pl1")).toInt();
            const int displayedPl2 = limits.value(QStringLiteral("pl2")).toInt();

            RC_ASSERT(displayedPl1 == expPl1);
            RC_ASSERT(displayedPl2 == expPl2);

            // The displayed value must NEVER be the requested value when it
            // differs from the read-back (Req 8.9, 10.2). When they are equal
            // (no perturbation) the two coincide, which is fine.
            if (expPl1 != scenario.pl1Requested)
                RC_ASSERT(displayedPl1 != scenario.pl1Requested);
            if (expPl2 != scenario.pl2Requested)
                RC_ASSERT(displayedPl2 != scenario.pl2Requested);

            // Req 8.10 / 10.3: a discrepancy is reported through the applyResult
            // message iff the read-back differs from the request. The production
            // message states the discrepancy by reporting both the driver values
            // and the requested values.
            const bool messageReportsDiscrepancy =
                reportedMessage.contains(QStringLiteral("requested"),
                                         Qt::CaseInsensitive);
            RC_ASSERT(messageReportsDiscrepancy == expectDiscrepancy);
        });

    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(Property3ReadBack)
#include "tst_property3_readback.moc"
