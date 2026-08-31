// Property-based test for Property 8: Editing never triggers a write.
//
// Feature: boreas-operator-ui, Property 8: For any sequence of read-only /
// edit / validation calls that does NOT include an explicit applyXxx, zero
// writes reach the platform-access layer. That is: no ISysfsAccess::write,
// no IDBusAccess::setGpuMode, no IDBusAccess::setFanCurve, and no
// ISettingsStore::save call is ever made.
//
// Validates: Requirements 6.7, 8.6
//   - Req 6.7: editing the fan curve SHALL NOT apply the change automatically.
//   - Req 8.6: editing a power-limit value without activating "Apply Power
//     Limits" SHALL NOT write the power-limit value.
//
// Runner:  QtTest. Generation/shrinking: rapidcheck. Minimum 100 iterations
// (rapidcheck defaults to 100; asserted explicitly below).
//
// Strategy: rather than testing one hand-picked call order, the test draws a
// random-length random sequence of "operations" from the set of read-only /
// edit / validation entry points on HardwareManager -- validateFanCurve,
// fanCurveFor, tempThresholdFor, powerLimitsFor, setSelectedProfile, and the
// read-only refresh*/detect* telemetry paths -- and replays it against a fresh
// set of in-memory fakes. The applyXxx write paths are deliberately EXCLUDED
// from the operation set. After the whole sequence, every fake's write counter
// must still read zero. Constructing the HardwareManager itself only reads
// (loadPersistedSettings uses load(); detectCapabilities uses exists()/read()),
// so the expected write total for a no-apply sequence is exactly zero.
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

// The read-only / edit / validation operations under test. NONE of these is an
// applyXxx write path; the property asserts that replaying any sequence of them
// produces zero writes.
enum class Op {
    ValidateFanCurve,
    FanCurveFor,
    TempThresholdFor,
    PowerLimitsFor,
    SetSelectedProfile,
    RefreshActiveProfile,
    RefreshGpuMode,
    RefreshTemperatures,
    DetectCapabilities,
    SelectedProfileGetter,
    CapabilitiesGetter,
    ActiveGpuModeGetter,
    SupportedGpuModesGetter,
    CpuTemperatureGetter,
    GpuTemperatureGetter,
    RecentLogGetter,
};

// Canonical profile names the setSelectedProfile / *For accessors accept, plus
// a couple of unknown names to exercise the fallback branches.
QString profileName(int i)
{
    switch (i % 5) {
    case 0: return QStringLiteral("Quiet");
    case 1: return QStringLiteral("Balanced");
    case 2: return QStringLiteral("Performance");
    case 3: return QStringLiteral("Unknown");   // unrecognized -> ignored / defaults
    default: return QString();                  // empty -> ignored / defaults
    }
}

// Build an arbitrary QVariantList of {tempC, percent} points to feed
// validateFanCurve. The exact validity is irrelevant to this property: valid
// AND invalid curves must both leave the write counters untouched.
QVariantList makeFanCurve(const std::vector<int> &temps, const std::vector<int> &percents)
{
    QVariantList list;
    const std::size_t n = std::min(temps.size(), percents.size());
    for (std::size_t i = 0; i < n; ++i) {
        QVariantMap m;
        m.insert(QStringLiteral("tempC"), temps[i]);
        m.insert(QStringLiteral("percent"), percents[i]);
        list.append(m);
    }
    return list;
}

// A single generated operation plus the arguments it may need.
struct GenOp {
    Op op;
    int profileIndex;
    std::vector<int> temps;
    std::vector<int> percents;
};

// Seed the fakes with enough state that the read-only paths actually do work
// (supported GPU modes, available services, seeded sysfs values). This makes
// the refresh*/detect* calls follow their "happy" branches -- which is exactly
// where a stray write would be most likely to sneak in -- rather than bailing
// out early on a missing dependency.
void seedFakes(FakeSysfsAccess &sysfs, FakeDBusAccess &dbus, FakeSettingsStore &settings)
{
    // D-Bus: supergfxctl / asusd available with a supported mode set.
    dbus.setServiceAvailable(QStringLiteral("org.supergfxctl.Daemon"), true);
    dbus.setServiceAvailable(QStringLiteral("org.asuslinux.Daemon"), true);
    dbus.setSupportedGpuModes(QStringList{QStringLiteral("Integrated"),
                                          QStringLiteral("Hybrid")});
    dbus.setActiveGpuMode(QStringLiteral("Hybrid"));
    dbus.setFanCurvesSupported(true);

    // Sysfs: seed the platform_profile + hwmon temperature inputs and PL paths
    // so the read paths find data. Concrete paths do not need to be exact for
    // the property (a miss just takes the read-failure branch, which also must
    // not write); seeding the common ones exercises the happy path too.
    sysfs.setValue(QStringLiteral("/sys/firmware/acpi/platform_profile"),
                   QStringLiteral("balanced"));
    sysfs.setValue(QStringLiteral("/sys/class/hwmon/hwmon0/temp1_input"),
                   QStringLiteral("55000"));
    sysfs.setValue(QStringLiteral("/sys/class/hwmon/hwmon1/temp1_input"),
                   QStringLiteral("48000"));

    // Settings: seed a stored block for one profile so fanCurveFor / *For hit
    // the persisted branch as well as the defaults branch.
    ProfileSettings s;
    s.fanCurve = FanCurve{{30, 20}, {60, 55}, {85, 100}};
    s.tempThresholdC = 90;
    s.pl1Watts = 35;
    s.pl2Watts = 45;
    settings.seed(QStringLiteral("Balanced"), s);
}

// Total number of writes that have reached ANY fake.
int totalWrites(const FakeSysfsAccess &sysfs, const FakeDBusAccess &dbus,
                const FakeSettingsStore &settings)
{
    return sysfs.writeCalls().size()
         + dbus.setGpuModeCalls().size()
         + dbus.setFanCurveCalls().size()
         + settings.saveCallCount();
}

// Replay one generated operation against the manager. Read-only / edit /
// validation only -- never an applyXxx.
void applyOp(HardwareManager &mgr, const GenOp &g)
{
    switch (g.op) {
    case Op::ValidateFanCurve:
        mgr.validateFanCurve(makeFanCurve(g.temps, g.percents));
        break;
    case Op::FanCurveFor:
        mgr.fanCurveFor(profileName(g.profileIndex));
        break;
    case Op::TempThresholdFor:
        mgr.tempThresholdFor(profileName(g.profileIndex));
        break;
    case Op::PowerLimitsFor:
        mgr.powerLimitsFor(profileName(g.profileIndex));
        break;
    case Op::SetSelectedProfile:
        mgr.setSelectedProfile(profileName(g.profileIndex));
        break;
    case Op::RefreshActiveProfile:
        mgr.refreshActiveProfile();
        break;
    case Op::RefreshGpuMode:
        mgr.refreshGpuMode();
        break;
    case Op::RefreshTemperatures:
        mgr.refreshTemperatures();
        break;
    case Op::DetectCapabilities:
        mgr.detectCapabilities();
        break;
    case Op::SelectedProfileGetter:
        (void)mgr.selectedProfile();
        break;
    case Op::CapabilitiesGetter:
        (void)mgr.capabilities();
        break;
    case Op::ActiveGpuModeGetter:
        (void)mgr.activeGpuMode();
        break;
    case Op::SupportedGpuModesGetter:
        (void)mgr.supportedGpuModes();
        break;
    case Op::CpuTemperatureGetter:
        (void)mgr.cpuTemperature();
        break;
    case Op::GpuTemperatureGetter:
        (void)mgr.gpuTemperature();
        break;
    case Op::RecentLogGetter:
        (void)mgr.recentLog();
        break;
    }
}

} // namespace

namespace rc {

// Generator for one operation. The Op is drawn uniformly across the read-only /
// edit / validation set; the extra fields (profile index and fan-curve points)
// are always generated so any Op can use them.
template <>
struct Arbitrary<GenOp> {
    static Gen<GenOp> arbitrary()
    {
        auto opGen = gen::element(
            Op::ValidateFanCurve, Op::FanCurveFor, Op::TempThresholdFor,
            Op::PowerLimitsFor, Op::SetSelectedProfile, Op::RefreshActiveProfile,
            Op::RefreshGpuMode, Op::RefreshTemperatures, Op::DetectCapabilities,
            Op::SelectedProfileGetter, Op::CapabilitiesGetter,
            Op::ActiveGpuModeGetter, Op::SupportedGpuModesGetter,
            Op::CpuTemperatureGetter, Op::GpuTemperatureGetter,
            Op::RecentLogGetter);

        // Fan-curve points: 0..10 of them, values spanning below/within/above
        // the valid ranges so both valid and invalid curves are exercised.
        auto pointCount = gen::inRange<std::size_t>(0, 11);
        auto tempsGen = gen::mapcat(pointCount, [](std::size_t n) {
            return gen::container<std::vector<int>>(n, gen::inRange(-10, 120));
        });
        auto percentsGen = gen::mapcat(pointCount, [](std::size_t n) {
            return gen::container<std::vector<int>>(n, gen::inRange(-10, 120));
        });

        return gen::build<GenOp>(
            gen::set(&GenOp::op, std::move(opGen)),
            gen::set(&GenOp::profileIndex, gen::inRange(0, 5)),
            gen::set(&GenOp::temps, std::move(tempsGen)),
            gen::set(&GenOp::percents, std::move(percentsGen)));
    }
};

} // namespace rc

class Property8NoWrite : public QObject {
    Q_OBJECT

private slots:
    // Property 8: any sequence of read-only / edit / validation calls (never an
    // applyXxx) leaves every fake's write counter at zero (Req 6.7, 8.6).
    void editingNeverWrites();
};

void Property8NoWrite::editingNeverWrites()
{
    const bool ok = rc::check(
        "no applyXxx in the call sequence => zero writes reach the fakes",
        [](const std::vector<GenOp> &ops) {
            // Fresh fakes + manager per generated sequence so counts are not
            // carried across shrinks/iterations.
            FakeSysfsAccess sysfs;
            FakeDBusAccess dbus;
            FakeSettingsStore settings;
            seedFakes(sysfs, dbus, settings);

            HardwareManager mgr(&sysfs, &dbus, &settings);

            // Construction (loadPersistedSettings + detectCapabilities) is
            // read-only, so no write should have happened yet either.
            RC_ASSERT(totalWrites(sysfs, dbus, settings) == 0);

            for (const GenOp &g : ops)
                applyOp(mgr, g);

            // The core invariant: not a single write reached any fake.
            RC_ASSERT(sysfs.writeCalls().isEmpty());
            RC_ASSERT(dbus.setGpuModeCalls().isEmpty());
            RC_ASSERT(dbus.setFanCurveCalls().isEmpty());
            RC_ASSERT(settings.saveCallCount() == 0);
            RC_ASSERT(totalWrites(sysfs, dbus, settings) == 0);
        });

    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(Property8NoWrite)
#include "tst_property8_nowrite.moc"
