// Error-path unit tests (QtTest example-based) for HardwareManager write
// failures across every WriteOutcome::Status the platform layer can report.
//
// Feature: boreas-operator-ui, Task 10.9.
// Validates: Requirements 3.6, 8.10
//
//   3.6  IF any Write_Operation fails, THEN THE Operator_UI SHALL display a
//        message identifying the affected control and the failed operation
//        within 2 seconds. Here that surface is applyResult(false, message)
//        emitted exactly once per write, with a specific, non-empty message.
//   8.10 The reported/persisted power-limit value is always the driver
//        read-back (never the merely requested value), and a discrepancy is
//        reported iff read-back differs. This test focuses on the failure
//        branches of the PL write path, including the PL1/PL2 blocking-write
//        timeout that MUST surface the exact design message.
//
// These are example-based unit tests (NOT property tests). They drive the real
// HardwareManager through the injecting constructor with the in-memory fakes
// (FakeSysfsAccess / FakeDBusAccess / FakeSettingsStore) scripted so each write
// returns a specific WriteOutcome::Status failure. No hardware or root is
// required.
//
// Coverage:
//   * applyPowerLimits: Timeout (exact message), IoError, PermissionDenied,
//     Unsupported -- for both the PL1 and PL2 write positions.
//   * applyGpuMode: Timeout / IoError / PermissionDenied / Unsupported from
//     IDBusAccess::setGpuMode.
//   * applyFanCurve: Timeout / IoError / PermissionDenied / Unsupported from
//     IDBusAccess::setFanCurve.
//   * A barrage of failing writes never crashes the manager (Req 3.6 "without
//     terminating"): reaching the end with the manager still usable is proof.
//
// Signal-timing note: the constructor runs loadPersistedSettings() and
// detectCapabilities() synchronously; NEITHER emits applyResult(). A QSignalSpy
// created on applyResult AFTER construction therefore starts empty, and each
// apply* call below produces exactly the emission under test.

#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "DataModels.h"
#include "Fakes.h"
#include "HardwareManager.h"
#include "PlatformAccess.h"

namespace {

// Canonical PL1/PL2 sysfs paths (asus-nb-wmi primary) the manager resolves via
// resolvePl1Path()/resolvePl2Path(). These match the constants in
// HardwareManager.cpp's detection/write path.
QString pl1Path() { return QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl1_spl"); }
QString pl2Path() { return QStringLiteral("/sys/devices/platform/asus-nb-wmi/ppt_pl2_sppt"); }

// The exact operator-facing message required for the PL1/PL2 blocking-write
// timeout caveat (design "PL1/PL2 blocking-write"). Kept verbatim so the test
// pins the string the UI/log will show.
QString plTimeoutMessage()
{
    return QStringLiteral("Power limit write timed out (known driver issue on some kernels)");
}

// A canonical profile name used across the write-path tests.
QString performance() { return powerProfileName(PowerProfile::Performance); }

// Seed the fakes so applyPowerLimits reaches the write step: both PL paths must
// exist for resolvePl*Path() to succeed.
void seedPowerLimitPaths(FakeSysfsAccess &sysfs)
{
    sysfs.setExists(pl1Path(), true);
    sysfs.setExists(pl2Path(), true);
    // Seed read-back values so the failure-path read-back has something to
    // report (exercises the "Current values:" suffix without changing the
    // asserted base message).
    sysfs.setValue(pl1Path(), QStringLiteral("30"));
    sysfs.setValue(pl2Path(), QStringLiteral("50"));
}

// Extract (success, message) from a single applyResult spy row.
bool successAt(const QSignalSpy &spy, int row) { return spy.at(row).at(0).toBool(); }
QString messageAt(const QSignalSpy &spy, int row) { return spy.at(row).at(1).toString(); }

// All the failure statuses this task must cover.
const QVector<WriteOutcome::Status> kFailureStatuses = {
    WriteOutcome::Status::Timeout,
    WriteOutcome::Status::IoError,
    WriteOutcome::Status::PermissionDenied,
    WriteOutcome::Status::Unsupported,
};

QString statusName(WriteOutcome::Status s)
{
    switch (s) {
    case WriteOutcome::Status::Ok:               return QStringLiteral("Ok");
    case WriteOutcome::Status::Timeout:          return QStringLiteral("Timeout");
    case WriteOutcome::Status::IoError:          return QStringLiteral("IoError");
    case WriteOutcome::Status::Unsupported:      return QStringLiteral("Unsupported");
    case WriteOutcome::Status::PermissionDenied: return QStringLiteral("PermissionDenied");
    }
    return QStringLiteral("Unknown");
}

} // namespace

class UnitWriteFailures : public QObject {
    Q_OBJECT

private slots:
    // --- applyPowerLimits: PL1 write fails with each status (Req 3.6, 8.10) ---
    void applyPowerLimitsPl1FailureEmitsSpecificApplyResult();

    // --- applyPowerLimits: PL2 write fails with each status (Req 3.6, 8.10) ---
    void applyPowerLimitsPl2FailureEmitsSpecificApplyResult();

    // --- applyPowerLimits: PL1 timeout uses the EXACT design message ----------
    void applyPowerLimitsTimeoutUsesExactMessage();

    // --- applyGpuMode: setGpuMode fails with each status (Req 3.6) ------------
    void applyGpuModeFailureEmitsSpecificApplyResult();

    // --- applyFanCurve: setFanCurve fails with each status (Req 3.6) ----------
    void applyFanCurveFailureEmitsSpecificApplyResult();

    // --- a barrage of failing writes never crashes the manager (Req 3.6) -----
    void repeatedFailingWritesDoNotCrashAndManagerStaysUsable();
};

// ----------------------------------------------------------------------------
// applyPowerLimits -- PL1 write returns each failure status
// ----------------------------------------------------------------------------

void UnitWriteFailures::applyPowerLimitsPl1FailureEmitsSpecificApplyResult()
{
    for (const WriteOutcome::Status status : kFailureStatuses) {
        FakeSysfsAccess sysfs;
        FakeDBusAccess dbus;
        FakeSettingsStore store;
        seedPowerLimitPaths(sysfs);

        // Force the FIRST write (PL1) to fail with this status. Scripting it on
        // the PL1 path specifically leaves the PL2 write path untouched.
        sysfs.setWriteOutcomeForPath(pl1Path(),
                                     WriteOutcome{status, QString()});

        HardwareManager mgr(&sysfs, &dbus, &store);

        QSignalSpy applySpy(&mgr, &HardwareManager::applyResult);
        QVERIFY(applySpy.isValid());

        mgr.applyPowerLimits(performance(), 45, 65);

        // Exactly one applyResult, reporting failure with a non-empty message
        // (Req 3.6). The context string helps pinpoint which status failed.
        const QByteArray ctx = statusName(status).toUtf8();
        QVERIFY2(applySpy.count() == 1, ctx.constData());
        QVERIFY2(!successAt(applySpy, 0), ctx.constData());
        QVERIFY2(!messageAt(applySpy, 0).isEmpty(), ctx.constData());

        // Status-specific message content (the failureMessage() mapping).
        const QString msg = messageAt(applySpy, 0);
        switch (status) {
        case WriteOutcome::Status::Timeout:
            QVERIFY2(msg.contains(plTimeoutMessage()), ctx.constData());
            break;
        case WriteOutcome::Status::PermissionDenied:
            QVERIFY2(msg.contains(QStringLiteral("Permission denied")), ctx.constData());
            QVERIFY2(msg.contains(QStringLiteral("PL1")), ctx.constData());
            break;
        case WriteOutcome::Status::Unsupported:
            QVERIFY2(msg.contains(QStringLiteral("PL1")), ctx.constData());
            QVERIFY2(msg.contains(QStringLiteral("not supported")), ctx.constData());
            break;
        case WriteOutcome::Status::IoError:
        default:
            QVERIFY2(msg.contains(QStringLiteral("PL1")), ctx.constData());
            break;
        }

        // The PL1 failure must short-circuit before PL2 is written: only the
        // PL1 path saw a write() call.
        int pl2Writes = 0;
        for (const auto &c : sysfs.writeCalls())
            if (c.path == pl2Path())
                ++pl2Writes;
        QVERIFY2(pl2Writes == 0, ctx.constData());
    }
}

// ----------------------------------------------------------------------------
// applyPowerLimits -- PL2 write returns each failure status
// ----------------------------------------------------------------------------

void UnitWriteFailures::applyPowerLimitsPl2FailureEmitsSpecificApplyResult()
{
    for (const WriteOutcome::Status status : kFailureStatuses) {
        FakeSysfsAccess sysfs;
        FakeDBusAccess dbus;
        FakeSettingsStore store;
        seedPowerLimitPaths(sysfs);

        // PL1 succeeds (default Ok), PL2 fails with this status.
        sysfs.setWriteOutcomeForPath(pl2Path(),
                                     WriteOutcome{status, QString()});

        HardwareManager mgr(&sysfs, &dbus, &store);

        QSignalSpy applySpy(&mgr, &HardwareManager::applyResult);
        QVERIFY(applySpy.isValid());

        mgr.applyPowerLimits(performance(), 45, 65);

        const QByteArray ctx = statusName(status).toUtf8();
        QVERIFY2(applySpy.count() == 1, ctx.constData());
        QVERIFY2(!successAt(applySpy, 0), ctx.constData());
        QVERIFY2(!messageAt(applySpy, 0).isEmpty(), ctx.constData());

        const QString msg = messageAt(applySpy, 0);
        switch (status) {
        case WriteOutcome::Status::Timeout:
            QVERIFY2(msg.contains(plTimeoutMessage()), ctx.constData());
            break;
        case WriteOutcome::Status::PermissionDenied:
            QVERIFY2(msg.contains(QStringLiteral("Permission denied")), ctx.constData());
            QVERIFY2(msg.contains(QStringLiteral("PL2")), ctx.constData());
            break;
        case WriteOutcome::Status::Unsupported:
            QVERIFY2(msg.contains(QStringLiteral("PL2")), ctx.constData());
            QVERIFY2(msg.contains(QStringLiteral("not supported")), ctx.constData());
            break;
        case WriteOutcome::Status::IoError:
        default:
            QVERIFY2(msg.contains(QStringLiteral("PL2")), ctx.constData());
            break;
        }

        // PL2 failure means the PL1 write happened first (success) then PL2.
        int pl1Writes = 0;
        int pl2Writes = 0;
        for (const auto &c : sysfs.writeCalls()) {
            if (c.path == pl1Path())
                ++pl1Writes;
            if (c.path == pl2Path())
                ++pl2Writes;
        }
        QVERIFY2(pl1Writes == 1, ctx.constData());
        QVERIFY2(pl2Writes == 1, ctx.constData());
    }
}

// ----------------------------------------------------------------------------
// applyPowerLimits -- PL1 Timeout surfaces the EXACT design message
// ----------------------------------------------------------------------------

void UnitWriteFailures::applyPowerLimitsTimeoutUsesExactMessage()
{
    // The PL1/PL2 blocking-write caveat requires this precise wording, and the
    // failing outcome message field must NOT override it. Script a Timeout with
    // a deliberately different message payload to prove the manager substitutes
    // the exact design string.
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore store;
    seedPowerLimitPaths(sysfs);

    sysfs.setWriteOutcomeForPath(
        pl1Path(),
        WriteOutcome{WriteOutcome::Status::Timeout,
                     QStringLiteral("ignored raw driver text")});

    HardwareManager mgr(&sysfs, &dbus, &store);

    QSignalSpy applySpy(&mgr, &HardwareManager::applyResult);
    QVERIFY(applySpy.isValid());

    mgr.applyPowerLimits(performance(), 45, 65);

    QCOMPARE(applySpy.count(), 1);
    QVERIFY(!successAt(applySpy, 0));
    // The message begins with the exact design string (a read-back "Current
    // values:" suffix may follow, so use contains rather than exact-equal).
    QVERIFY(messageAt(applySpy, 0).contains(plTimeoutMessage()));
}

// ----------------------------------------------------------------------------
// applyGpuMode -- setGpuMode returns each failure status
// ----------------------------------------------------------------------------

void UnitWriteFailures::applyGpuModeFailureEmitsSpecificApplyResult()
{
    for (const WriteOutcome::Status status : kFailureStatuses) {
        FakeSysfsAccess sysfs;
        FakeDBusAccess dbus;
        FakeSettingsStore store;

        // Reach the write step: supergfxctl available + a supported set that
        // includes the mode under test.
        dbus.setServiceAvailable(QStringLiteral("org.supergfxctl.Daemon"), true);
        dbus.setSupportedGpuModes(QStringList{QStringLiteral("Hybrid"),
                                              QStringLiteral("Integrated")});
        dbus.setActiveGpuMode(QStringLiteral("Hybrid"));
        dbus.setGpuModeOutcome(WriteOutcome{status, QString()});

        HardwareManager mgr(&sysfs, &dbus, &store);
        // Populate the runtime-detected supported set that applyGpuMode gates on.
        mgr.refreshGpuMode();

        QSignalSpy applySpy(&mgr, &HardwareManager::applyResult);
        QVERIFY(applySpy.isValid());

        mgr.applyGpuMode(QStringLiteral("Integrated"));

        const QByteArray ctx = statusName(status).toUtf8();
        // Exactly one applyResult, failure, with a specific non-empty message
        // naming the target mode (Req 3.6).
        QVERIFY2(applySpy.count() == 1, ctx.constData());
        QVERIFY2(!successAt(applySpy, 0), ctx.constData());
        const QString msg = messageAt(applySpy, 0);
        QVERIFY2(!msg.isEmpty(), ctx.constData());
        QVERIFY2(msg.contains(QStringLiteral("Integrated")), ctx.constData());
        QVERIFY2(msg.contains(QStringLiteral("Failed to switch GPU mode")), ctx.constData());

        // A failed write must NOT adopt the requested mode: activeGpuMode is
        // retained at the last-known value so the selector reverts (Req 5.12).
        QVERIFY2(mgr.activeGpuMode() == QStringLiteral("Hybrid"), ctx.constData());
    }
}

// ----------------------------------------------------------------------------
// applyFanCurve -- setFanCurve returns each failure status
// ----------------------------------------------------------------------------

void UnitWriteFailures::applyFanCurveFailureEmitsSpecificApplyResult()
{
    // A valid 2-point curve so validation passes and the write step is reached.
    QVariantList points;
    {
        QVariantMap p1; p1.insert(QStringLiteral("tempC"), 40); p1.insert(QStringLiteral("percent"), 30);
        QVariantMap p2; p2.insert(QStringLiteral("tempC"), 80); p2.insert(QStringLiteral("percent"), 75);
        points << p1 << p2;
    }

    for (const WriteOutcome::Status status : kFailureStatuses) {
        FakeSysfsAccess sysfs;
        FakeDBusAccess dbus;
        FakeSettingsStore store;

        // Fan curves must be reported supported so applyFanCurve reaches the
        // write step; then force the D-Bus write to fail.
        dbus.setFanCurvesSupported(true);
        dbus.setFanCurveOutcome(WriteOutcome{status, QString()});

        HardwareManager mgr(&sysfs, &dbus, &store);

        QSignalSpy applySpy(&mgr, &HardwareManager::applyResult);
        QVERIFY(applySpy.isValid());

        mgr.applyFanCurve(performance(), points);

        const QByteArray ctx = statusName(status).toUtf8();
        QVERIFY2(applySpy.count() == 1, ctx.constData());
        QVERIFY2(!successAt(applySpy, 0), ctx.constData());
        const QString msg = messageAt(applySpy, 0);
        QVERIFY2(!msg.isEmpty(), ctx.constData());
        // The message identifies the failing action; with per-fan curves it
        // reads "Failed to apply CPU fan curve for <profile>: ..." (CPU is the
        // default channel when applyFanCurve is called without a fan argument).
        QVERIFY2(msg.contains(QStringLiteral("Failed to apply CPU fan curve")), ctx.constData());
        QVERIFY2(msg.contains(performance()), ctx.constData());

        // A failed hardware write must NOT persist the curve for the profile.
        QVERIFY2(!store.contains(performance()), ctx.constData());
    }
}

// ----------------------------------------------------------------------------
// A barrage of interleaved failing writes never crashes the manager
// ----------------------------------------------------------------------------

void UnitWriteFailures::repeatedFailingWritesDoNotCrashAndManagerStaysUsable()
{
    FakeSysfsAccess sysfs;
    FakeDBusAccess dbus;
    FakeSettingsStore store;

    // Power limits: paths exist but every write times out.
    seedPowerLimitPaths(sysfs);
    sysfs.setWriteStatus(WriteOutcome::Status::Timeout);

    // GPU mode: service up, mode supported, but the switch errors out.
    dbus.setServiceAvailable(QStringLiteral("org.supergfxctl.Daemon"), true);
    dbus.setSupportedGpuModes(QStringList{QStringLiteral("Hybrid"),
                                          QStringLiteral("Integrated")});
    dbus.setActiveGpuMode(QStringLiteral("Hybrid"));
    dbus.setGpuModeOutcome(WriteOutcome{WriteOutcome::Status::IoError, QString()});

    // Fan curve: supported but the write is permission-denied.
    dbus.setFanCurvesSupported(true);
    dbus.setFanCurveOutcome(WriteOutcome{WriteOutcome::Status::PermissionDenied, QString()});

    HardwareManager mgr(&sysfs, &dbus, &store);
    mgr.refreshGpuMode();

    QSignalSpy applySpy(&mgr, &HardwareManager::applyResult);
    QVERIFY(applySpy.isValid());

    QVariantList curve;
    {
        QVariantMap p1; p1.insert(QStringLiteral("tempC"), 40); p1.insert(QStringLiteral("percent"), 30);
        QVariantMap p2; p2.insert(QStringLiteral("tempC"), 80); p2.insert(QStringLiteral("percent"), 75);
        curve << p1 << p2;
    }

    for (int i = 0; i < 5; ++i) {
        mgr.applyPowerLimits(performance(), 45, 65);
        mgr.applyGpuMode(QStringLiteral("Integrated"));
        mgr.applyFanCurve(performance(), curve);
    }

    // Each iteration produces exactly 3 failing applyResult emissions => 15.
    // This confirms failures keep surfacing (Req 3.6) rather than crashing.
    QCOMPARE(applySpy.count(), 5 * 3);
    for (int i = 0; i < applySpy.count(); ++i) {
        QVERIFY(!successAt(applySpy, i));
        QVERIFY(!messageAt(applySpy, i).isEmpty());
    }

    // The manager is still alive and usable after the barrage: it still answers
    // queries and accepts a selected-profile change (no crash, Req 3.6).
    QVERIFY(mgr.activeGpuMode() == QStringLiteral("Hybrid"));
    mgr.setSelectedProfile(powerProfileName(PowerProfile::Quiet));
    QCOMPARE(mgr.selectedProfile(), powerProfileName(PowerProfile::Quiet));
}

QTEST_MAIN(UnitWriteFailures)
#include "tst_unit_write_failures.moc"
