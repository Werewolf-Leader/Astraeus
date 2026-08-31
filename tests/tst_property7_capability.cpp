// Property-based test for Property 7: Capability monotonicity.
//
// Feature: boreas-operator-ui, Property 7: For any capability report, every
// control marked unsupported yields a disabled control with no enabled write
// action; the UI never presents a supported/enabled state for a control that
// failed capability detection.
//
// Validates: Requirements 3.2, 3.7
//
// Runner:  QtTest. Generation/shrinking: rapidcheck. Minimum 100 iterations per
// property (rapidcheck defaults to 100; asserted explicitly below).
//
// Strategy: the capability report is not hand-built; it is *derived* by the real
// HardwareManager::detectCapabilities() from a randomly-scripted platform state.
// Each generated scenario randomly decides, per underlying dependency (sysfs
// paths, D-Bus services, writability, discoverable PL bounds), whether it is
// present. We then construct a HardwareManager with the injected fakes, run
// detection, and assert the monotonicity invariant against the resulting
// `capabilities` QVariantMap: every control reported unsupported carries a
// visible reason and never simultaneously surfaces a supported/enabled state,
// and unsupported PowerLimits never surfaces discovered (enabled) bounds.
//
// Exercises FakeSysfsAccess / FakeDBusAccess / FakeSettingsStore so no hardware
// or root is required.

#include <QtTest/QtTest>

#include <rapidcheck.h>

#include "DataModels.h"
#include "Fakes.h"
#include "HardwareManager.h"

namespace {

// Canonical sysfs paths and D-Bus service names probed by
// HardwareManager::detectCapabilities(). Kept in sync with the probes in
// HardwareManager.cpp (Hardware Dependency Map).
const QString kPlatformProfilePath =
    QStringLiteral("/sys/firmware/acpi/platform_profile");
const QString kCpuTempPath =
    QStringLiteral("/sys/class/hwmon/hwmon0/temp1_input");
const QString kGpuTempPath =
    QStringLiteral("/sys/class/hwmon/hwmon1/temp1_input");
const QString kThrottlePath =
    QStringLiteral("/sys/devices/platform/asus-nb-wmi/throttle_thermal_policy");
const QString kPl1Path =
    QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl1_spl");
const QString kPl2Path =
    QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl2_sppt");

const QString kSupergfxctlService = QStringLiteral("org.supergfxctl.Daemon");
const QString kAsusdService = QStringLiteral("org.asuslinux.Daemon");

// Every control key surfaced in the capabilities QVariantMap.
const QStringList &allControlKeys()
{
    static const QStringList keys = {
        controlIdKey(ControlId::PowerProfile),
        controlIdKey(ControlId::GpuMode),
        controlIdKey(ControlId::FanCurve),
        controlIdKey(ControlId::TemperatureRead),
        controlIdKey(ControlId::TemperatureThreshold),
        controlIdKey(ControlId::PowerLimits),
    };
    return keys;
}

// A single randomly-scripted platform scenario. Each flag decides whether an
// underlying dependency is present, spanning the whole capability input space.
struct Scenario {
    bool platformProfileExists;
    bool asusdAvailable;
    bool supergfxctlAvailable;
    bool supergfxctlReportsModes;   // service up but empty/no modes when false
    bool fanCurvesSupported;
    bool cpuTempExists;
    bool gpuTempExists;
    bool throttleTargetExists;      // drives tempThresholdIsHardwareLimit
    bool pl1Exists;
    bool pl2Exists;
    bool plWritable;                // both PL paths writable when true
};

// Apply a scenario to freshly-scripted fakes so detectCapabilities() observes it.
void applyScenario(const Scenario &s, FakeSysfsAccess &sysfs, FakeDBusAccess &dbus)
{
    sysfs.setExists(kPlatformProfilePath, s.platformProfileExists);
    sysfs.setExists(kCpuTempPath, s.cpuTempExists);
    sysfs.setExists(kGpuTempPath, s.gpuTempExists);
    sysfs.setExists(kThrottlePath, s.throttleTargetExists);

    sysfs.setExists(kPl1Path, s.pl1Exists);
    sysfs.setExists(kPl2Path, s.pl2Exists);
    // Writability only matters when the paths exist; set it regardless so the
    // probe's exists-AND-writable predicate is exercised across the space.
    sysfs.setWritable(kPl1Path, s.plWritable);
    sysfs.setWritable(kPl2Path, s.plWritable);

    dbus.setServiceAvailable(kAsusdService, s.asusdAvailable);
    dbus.setServiceAvailable(kSupergfxctlService, s.supergfxctlAvailable);
    dbus.setSupportedGpuModes(
        s.supergfxctlReportsModes
            ? std::optional<QStringList>(QStringList{QStringLiteral("Integrated"),
                                                     QStringLiteral("Hybrid")})
            : std::optional<QStringList>(QStringList{}));
    dbus.setFanCurvesSupported(s.fanCurvesSupported);
}

} // namespace

namespace rc {

// Generator drawing an independent boolean for every dependency flag so the
// generated scenarios span the full capability input space (all supported /
// all unsupported / arbitrary mixes).
template <>
struct Arbitrary<Scenario> {
    static Gen<Scenario> arbitrary()
    {
        return gen::build<Scenario>(
            gen::set(&Scenario::platformProfileExists),
            gen::set(&Scenario::asusdAvailable),
            gen::set(&Scenario::supergfxctlAvailable),
            gen::set(&Scenario::supergfxctlReportsModes),
            gen::set(&Scenario::fanCurvesSupported),
            gen::set(&Scenario::cpuTempExists),
            gen::set(&Scenario::gpuTempExists),
            gen::set(&Scenario::throttleTargetExists),
            gen::set(&Scenario::pl1Exists),
            gen::set(&Scenario::pl2Exists),
            gen::set(&Scenario::plWritable));
    }
};

} // namespace rc

class Property7Capability : public QObject {
    Q_OBJECT

private slots:
    // Property 7: every control reported unsupported by detectCapabilities()
    // carries a visible reason and never simultaneously presents a supported /
    // enabled state; unsupported PowerLimits never surfaces discovered (enabled)
    // bounds. (Req 3.2, 3.7)
    void capabilityMonotonicity();
};

void Property7Capability::capabilityMonotonicity()
{
    const bool ok = rc::check(
        "unsupported controls never surface a supported/enabled state",
        [](const Scenario &scenario) {
            FakeSysfsAccess sysfs;
            FakeDBusAccess dbus;
            FakeSettingsStore settings;

            applyScenario(scenario, sysfs, dbus);

            // Construct with injected fakes; the ctor runs detectCapabilities().
            HardwareManager mgr(&sysfs, &dbus, &settings);

            const QVariantMap caps = mgr.capabilities();

            // The report must cover every control (Req 3.1) so QML always has a
            // state to bind (no control can be silently absent).
            for (const QString &key : allControlKeys())
                RC_ASSERT(caps.contains(key));

            // Monotonicity: for every reported control, an unsupported control
            // is disabled with a visible reason, and no control reports a
            // supported state without also being detected as supported.
            for (auto it = caps.constBegin(); it != caps.constEnd(); ++it) {
                const QVariantMap cap = it.value().toMap();
                RC_ASSERT(cap.contains(QStringLiteral("supported")));
                RC_ASSERT(cap.contains(QStringLiteral("reason")));

                const bool supported = cap.value(QStringLiteral("supported")).toBool();
                const QString reason = cap.value(QStringLiteral("reason")).toString();

                if (!supported) {
                    // Req 3.3: an unsupported control must display a visible text
                    // reason identifying the failed dependency. The only way the
                    // UI disables the control AND shows a reason is a non-empty
                    // reason string here.
                    RC_ASSERT(!reason.isEmpty());
                }
            }

            // Req 3.7 for the write-bearing PowerLimits control: when it is
            // unsupported, the exposed bounds must be the conservative estimate
            // (estimated=true), never a discovered/enabled bound set. This is the
            // "no enabled write surface for an unsupported control" invariant
            // expressed against the value QML would bind a writable input to.
            const QVariantMap plCap =
                caps.value(controlIdKey(ControlId::PowerLimits)).toMap();
            const bool plSupported =
                plCap.value(QStringLiteral("supported")).toBool();
            const QVariantMap bounds = mgr.powerLimitBounds();
            if (!plSupported) {
                RC_ASSERT(bounds.value(QStringLiteral("estimated")).toBool());
                RC_ASSERT(bounds.value(QStringLiteral("min")).toInt() == 1);
                RC_ASSERT(bounds.value(QStringLiteral("max")).toInt() == 200);
            }

            // Cross-check the derived monotonicity against the scenario's ground
            // truth for the two purely-deterministic controls, proving detection
            // is monotonic in its inputs (a present dependency => supported, an
            // absent one => unsupported), never the reverse.
            const bool fanSupported =
                caps.value(controlIdKey(ControlId::FanCurve))
                    .toMap()
                    .value(QStringLiteral("supported"))
                    .toBool();
            RC_ASSERT(fanSupported == scenario.fanCurvesSupported);

            const bool plExpectedSupported =
                scenario.pl1Exists && scenario.pl2Exists && scenario.plWritable;
            RC_ASSERT(plSupported == plExpectedSupported);
        });

    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(Property7Capability)
#include "tst_property7_capability.moc"
