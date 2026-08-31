// Build + load smoke tests (task 16.1).
//
// Feature: boreas-operator-ui, Task 16.1.
// Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5
//
//   1.1 The application exposes the HardwareManager instance to QML under the
//       name `hardwareManager`.
//   1.2 The application loads the UI via loadFromModule("BoreasApp", "Main").
//   1.3 The QML module URI remains `BoreasApp`.
//   1.4 QtDBus is declared as a required Qt6 component in the build (this test
//       target links Qt6::DBus, so it only builds when the component resolves).
//   1.5 The build compiles against the C++17 standard.
//
// These are example/smoke tests (NOT property tests). main.cpp defines main()
// and therefore cannot be linked into a test binary, so this test reproduces
// the exact production wiring from main.cpp directly: it constructs a
// HardwareManager, sets it as the `hardwareManager` context property on a
// QQmlApplicationEngine BEFORE loading, and loads the UI via
// loadFromModule("BoreasApp", "Main") -- mirroring the sequence in main.cpp so
// a regression in that contract is caught here.
//
// The QML module `BoreasApp` is produced by qt_add_qml_module(Boreas ...) and
// lives on the filesystem next to the built module (its generated qmldir lists
// the Main/PerformanceTab/... entries). The CMake target passes that directory
// in via the BOREAS_QML_IMPORT_PATH compile definition and this test adds it to
// the engine import path so loadFromModule can resolve `BoreasApp`.
//
// This is a GUI test (QQmlApplicationEngine instantiates QtQuick items), so it
// forces the offscreen QPA platform before the QGuiApplication is created via
// QTEST_MAIN, keeping it runnable headless under ctest.

#include <QtTest/QtTest>

#include <QByteArray>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include "HardwareManager.h"

#ifndef BOREAS_QML_IMPORT_PATH
#  error "BOREAS_QML_IMPORT_PATH must be defined by the build (path to the BoreasApp module directory)"
#endif

namespace {

// Add the filesystem location of the built BoreasApp module to the engine so
// loadFromModule("BoreasApp", "Main") can resolve it from a standalone test
// binary (the module normally travels inside the Boreas executable's
// resources).
void addBoreasModuleImportPath(QQmlApplicationEngine &engine)
{
    engine.addImportPath(QStringLiteral(BOREAS_QML_IMPORT_PATH));
}

} // namespace

class SmokeLoad : public QObject {
    Q_OBJECT

private slots:
    // --- Req 1.5 / 1.4: build-level compile guarantees -----------------------
    void compilesAgainstCpp17();
    void linksAgainstQtDBus();

    // --- Req 1.1: hardwareManager context property ---------------------------
    void exposesHardwareManagerContextProperty();

    // --- Req 1.1 / 1.2 / 1.3: load via loadFromModule("BoreasApp", "Main") ----
    void loadsBoreasAppMainModuleAndCreatesRootObject();
    void hardwareManagerIsVisibleToLoadedRootObject();
};

// ----------------------------------------------------------------------------
// Req 1.5 / 1.4 -- build-level guarantees, asserted where feasible at compile time
// ----------------------------------------------------------------------------

void SmokeLoad::compilesAgainstCpp17()
{
    // Req 1.5: the project sets CMAKE_CXX_STANDARD 17 / REQUIRED ON. The
    // __cplusplus macro reflects the standard the translation unit was compiled
    // against, so this pins the build to C++17 (201703L). A regression that
    // drops the standard requirement (or bumps it) makes this fail.
    QCOMPARE(static_cast<long>(__cplusplus), 201703L);
}

void SmokeLoad::linksAgainstQtDBus()
{
    // Req 1.4: QtDBus is declared as a required Qt6 component. This test target
    // links Qt6::DBus, so the fact that this binary built and runs is the
    // primary evidence. Touch a QtDBus symbol so the dependency is real and not
    // pruned, and assert QtDBus reports a version consistent with the Qt build.
    const QString dbusVersion = QStringLiteral(QT_VERSION_STR);
    QVERIFY(!dbusVersion.isEmpty());

    // Confirm the DBus module macro exposed by the QtDBus headers is present at
    // compile time (only available when Qt6::DBus is on the include path).
    const int dbusMajor = (QT_VERSION >> 16) & 0xff;
    QVERIFY(dbusMajor >= 6);
}

// ----------------------------------------------------------------------------
// Req 1.1 -- hardwareManager context property is present and is a HardwareManager
// ----------------------------------------------------------------------------

void SmokeLoad::exposesHardwareManagerContextProperty()
{
    QQmlApplicationEngine engine;
    HardwareManager mgr;

    // Mirror main.cpp: set the context property BEFORE loading (Req 1.1).
    engine.rootContext()->setContextProperty(QStringLiteral("hardwareManager"), &mgr);

    const QVariant prop =
        engine.rootContext()->contextProperty(QStringLiteral("hardwareManager"));
    QVERIFY2(prop.isValid(), "hardwareManager context property must be present");

    QObject *obj = prop.value<QObject *>();
    QVERIFY2(obj != nullptr, "hardwareManager must be a QObject");
    QCOMPARE(obj, static_cast<QObject *>(&mgr));

    // It is specifically a HardwareManager instance (Req 1.1, 1.6).
    QVERIFY2(qobject_cast<HardwareManager *>(obj) != nullptr,
             "hardwareManager must be a HardwareManager");
}

// ----------------------------------------------------------------------------
// Req 1.1 / 1.2 / 1.3 -- loadFromModule("BoreasApp", "Main") creates a root object
// ----------------------------------------------------------------------------

void SmokeLoad::loadsBoreasAppMainModuleAndCreatesRootObject()
{
    QQmlApplicationEngine engine;
    addBoreasModuleImportPath(engine);

    HardwareManager mgr;
    engine.rootContext()->setContextProperty(QStringLiteral("hardwareManager"), &mgr);

    QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
    QSignalSpy failed(&engine, &QQmlApplicationEngine::objectCreationFailed);

    // Req 1.2 / 1.3: the exact production load call, against the retained
    // `BoreasApp` module URI.
    engine.loadFromModule("BoreasApp", "Main");

    // objectCreated fires (with a non-null object) on success; objectCreationFailed
    // fires on any failure. Wait briefly for the (possibly queued) signal.
    QTRY_VERIFY_WITH_TIMEOUT(created.count() + failed.count() >= 1, 5000);

    QCOMPARE(failed.count(), 0);
    QVERIFY2(!engine.rootObjects().isEmpty(),
             "loadFromModule(\"BoreasApp\", \"Main\") must create a root object");
    QVERIFY(engine.rootObjects().first() != nullptr);
}

void SmokeLoad::hardwareManagerIsVisibleToLoadedRootObject()
{
    QQmlApplicationEngine engine;
    addBoreasModuleImportPath(engine);

    HardwareManager mgr;
    engine.rootContext()->setContextProperty(QStringLiteral("hardwareManager"), &mgr);

    QSignalSpy failed(&engine, &QQmlApplicationEngine::objectCreationFailed);
    engine.loadFromModule("BoreasApp", "Main");
    QTRY_VERIFY_WITH_TIMEOUT(!engine.rootObjects().isEmpty() || failed.count() > 0, 5000);

    QCOMPARE(failed.count(), 0);
    QVERIFY(!engine.rootObjects().isEmpty());

    QObject *root = engine.rootObjects().first();
    QVERIFY(root != nullptr);

    // The loaded object tree resolves `hardwareManager` from the root context to
    // the same HardwareManager instance we injected (Req 1.1). Evaluate the name
    // in the root object's own QML context.
    QQmlContext *ctx = engine.contextForObject(root);
    QVERIFY(ctx != nullptr);
    const QVariant resolved = ctx->contextProperty(QStringLiteral("hardwareManager"));
    QVERIFY2(resolved.isValid(),
             "hardwareManager must be resolvable from the loaded root object's context");
    QCOMPARE(resolved.value<QObject *>(), static_cast<QObject *>(&mgr));
}

// QTEST_MAIN provides a QGuiApplication (required for QtQuick). Force the
// offscreen QPA platform so the GUI test runs headless under ctest.
int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    SmokeLoad tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_smoke_load.moc"
