// Error-path unit tests (QtTest example-based) for HardwareManager read-only
// telemetry failures.
//
// Feature: boreas-operator-ui, Task 6.4.
// Validates: Requirements 3.4, 3.5
//
//   3.4 If a required daemon, D-Bus service, or sysfs file is missing during a
//       Read_Operation, HardwareManager reports the failure to the Operator_UI
//       WITHOUT terminating the application.
//   3.5 If any Read_Operation fails, the failure is surfaced (here via the
//       readFailed(controlId, message) signal) identifying the affected control
//       and the failed operation.
//
// These are example-based unit tests (NOT property tests). They drive
// HardwareManager through the injecting constructor with in-memory fakes
// (FakeSysfsAccess / FakeDBusAccess / FakeSettingsStore) scripted so the backing
// sysfs paths and D-Bus services are ABSENT. No hardware or root is required.
//
// Design references: "Error Handling -> Read failures" (readFailed + NaN
// sentinel getters) and "HardwareManager read-only telemetry" (refreshActive-
// Profile / refreshGpuMode / refreshTemperatures never crash on a missing
// dependency).
//
// Signal-timing note: the constructor runs loadPersistedSettings() and
// detectCapabilities() synchronously; NEITHER emits readFailed(). A QSignalSpy
// created on readFailed AFTER construction therefore starts empty, and each
// refresh call below produces the emission(s) under test. The manager instance
// stays alive for the whole test body, which is itself the "does not crash /
// does not terminate" assertion (a crash or std::terminate would fail the run).

#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QString>
#include <QVariantList>

#include <cmath>

#include "DataModels.h"
#include "Fakes.h"
#include "HardwareManager.h"
#include "PlatformAccess.h"

namespace {

// Canonical control-id keys the manager emits with readFailed(...).
QString powerProfileKey()    { return controlIdKey(ControlId::PowerProfile); }
QString gpuModeKey()         { return controlIdKey(ControlId::GpuMode); }
QString temperatureReadKey() { return controlIdKey(ControlId::TemperatureRead); }

// Extract the controlId (first arg) from a QSignalSpy row of readFailed.
QString controlIdAt(const QSignalSpy &spy, int row)
{
    return spy.at(row).at(0).toString();
}

// True iff the spy recorded at least one readFailed whose controlId matches.
bool spyHasControlId(const QSignalSpy &spy, const QString &controlId)
{
    for (int i = 0; i < spy.count(); ++i) {
        if (controlIdAt(spy, i) == controlId)
            return true;
    }
    return false;
}

} // namespace

class UnitReadFailures : public QObject {
    Q_OBJECT

private slots:
    // --- refreshActiveProfile(): missing platform_profile (Req 3.4, 3.5) ------
    void refreshActiveProfileMissingFileEmitsReadFailed();

    // --- refreshGpuMode(): supergfxctl service unavailable (Req 3.4, 3.5) -----
    void refreshGpuModeMissingServiceEmitsReadFailed();

    // --- refreshTemperatures(): missing hwmon paths (Req 3.4, 3.5) ------------
    void refreshTemperaturesMissingSensorsEmitReadFailedAndNaN();

    // --- getters default to the NaN sentinel before any successful read -------
    void temperatureGettersReturnNanSentinelWhenUnavailable();

    // --- the manager survives an interleaved barrage of failing reads ---------
    void repeatedFailingReadsDoNotCrashAndManagerStaysUsable();
};

// ----------------------------------------------------------------------------
// refreshActiveProfile() -- missing platform_profile sysfs attribute
// ----------------------------------------------------------------------------

void UnitReadFailures::refreshActiveProfileMissingFileEmitsReadFailed()
{
    // Empty fakes: no sysfs paths seeded, so /sys/firmware/acpi/platform_profile
    // does not exist. This models the missing-file read failure (Req 3.4).
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore store;

    HardwareManager mgr(&sysfs, &dbus, &store);

    // Spy AFTER construction so only the refresh call's emission is captured.
    QSignalSpy readFailedSpy(&mgr, &HardwareManager::readFailed);
    QVERIFY(readFailedSpy.isValid());

    mgr.refreshActiveProfile();

    // Exactly one readFailed identifying the PowerProfile control (Req 3.5).
    QCOMPARE(readFailedSpy.count(), 1);
    QCOMPARE(controlIdAt(readFailedSpy, 0), powerProfileKey());
    // The message names the failed operation (non-empty, human-readable).
    QVERIFY(!readFailedSpy.at(0).at(1).toString().isEmpty());

    // No spurious active-profile value was adopted from a failed read.
    QVERIFY(mgr.activePowerProfile().isEmpty());
}

// ----------------------------------------------------------------------------
// refreshGpuMode() -- supergfxctl D-Bus service unavailable
// ----------------------------------------------------------------------------

void UnitReadFailures::refreshGpuModeMissingServiceEmitsReadFailed()
{
    // Empty fakes: no D-Bus service marked available, so supergfxctl is absent.
    // This models the missing-service read failure (Req 3.4).
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore store;

    HardwareManager mgr(&sysfs, &dbus, &store);

    QSignalSpy readFailedSpy(&mgr, &HardwareManager::readFailed);
    QVERIFY(readFailedSpy.isValid());

    mgr.refreshGpuMode();

    // Exactly one readFailed identifying the GpuMode control (Req 3.5).
    QCOMPARE(readFailedSpy.count(), 1);
    QCOMPARE(controlIdAt(readFailedSpy, 0), gpuModeKey());
    QVERIFY(!readFailedSpy.at(0).at(1).toString().isEmpty());

    // Nothing was adopted for active mode or the supported-mode list.
    QVERIFY(mgr.activeGpuMode().isEmpty());
    QVERIFY(mgr.supportedGpuModes().isEmpty());
}

// ----------------------------------------------------------------------------
// refreshTemperatures() -- missing hwmon input paths
// ----------------------------------------------------------------------------

void UnitReadFailures::refreshTemperaturesMissingSensorsEmitReadFailedAndNaN()
{
    // Empty fakes: neither hwmon CPU nor GPU temp input exists, so both reads
    // fail (Req 3.4) and each getter returns the NaN sentinel.
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore store;

    HardwareManager mgr(&sysfs, &dbus, &store);

    QSignalSpy readFailedSpy(&mgr, &HardwareManager::readFailed);
    QVERIFY(readFailedSpy.isValid());

    mgr.refreshTemperatures();

    // Both CPU and GPU reads failed: two readFailed emissions, both tagged with
    // the TemperatureRead control id (Req 3.5).
    QCOMPARE(readFailedSpy.count(), 2);
    QVERIFY(spyHasControlId(readFailedSpy, temperatureReadKey()));
    for (int i = 0; i < readFailedSpy.count(); ++i) {
        QCOMPARE(controlIdAt(readFailedSpy, i), temperatureReadKey());
        QVERIFY(!readFailedSpy.at(i).at(1).toString().isEmpty());
    }

    // Getters return the NaN sentinel so QML renders "—" rather than a stale
    // value (design "Error Handling -> Read failures").
    QVERIFY(std::isnan(mgr.cpuTemperature()));
    QVERIFY(std::isnan(mgr.gpuTemperature()));
}

// ----------------------------------------------------------------------------
// Getters default to NaN before any successful read
// ----------------------------------------------------------------------------

void UnitReadFailures::temperatureGettersReturnNanSentinelWhenUnavailable()
{
    // Immediately after construction (before any poll) the temperature getters
    // are the NaN sentinel: no reading has succeeded yet.
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore store;

    HardwareManager mgr(&sysfs, &dbus, &store);

    QVERIFY(std::isnan(mgr.cpuTemperature()));
    QVERIFY(std::isnan(mgr.gpuTemperature()));
}

// ----------------------------------------------------------------------------
// The manager survives a barrage of interleaved failing reads
// ----------------------------------------------------------------------------

void UnitReadFailures::repeatedFailingReadsDoNotCrashAndManagerStaysUsable()
{
    // Drive every read-only telemetry path repeatedly against absent
    // dependencies. The app must never terminate (Req 3.4); reaching the end of
    // this test with the manager still responsive is the proof.
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore store;

    HardwareManager mgr(&sysfs, &dbus, &store);

    QSignalSpy readFailedSpy(&mgr, &HardwareManager::readFailed);
    QVERIFY(readFailedSpy.isValid());

    for (int i = 0; i < 5; ++i) {
        mgr.refreshActiveProfile();
        mgr.refreshGpuMode();
        mgr.refreshTemperatures();
    }

    // Every iteration produced: 1 (profile) + 1 (gpu) + 2 (cpu/gpu temps) = 4
    // failures. Five iterations => 20. This confirms failures keep surfacing
    // (Req 3.5) rather than silently going away or crashing.
    QCOMPARE(readFailedSpy.count(), 5 * 4);

    // The manager is still alive and usable: it still answers queries and
    // accepts a profile change after the barrage (no crash, Req 3.4).
    QVERIFY(std::isnan(mgr.cpuTemperature()));
    QVERIFY(std::isnan(mgr.gpuTemperature()));
    QVERIFY(mgr.activePowerProfile().isEmpty());

    mgr.setSelectedProfile(powerProfileName(PowerProfile::Performance));
    QCOMPARE(mgr.selectedProfile(), powerProfileName(PowerProfile::Performance));
}

QTEST_MAIN(UnitReadFailures)
#include "tst_unit_read_failures.moc"
