cmake_minimum_required(VERSION 3.21)

project(Boreas VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Quick
    Qml
    DBus
)

qt_standard_project_setup()

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(Boreas
    main.cpp
    HardwareManager.cpp
    HardwareManager.h
    DataModels.h
    PlatformAccess.h
    SettingsStore.cpp
    SettingsStore.h
    SysfsAccess.h
    SysfsAccess.cpp
    DBusAccess.h
    DBusAccess.cpp
    RyzenAdjAccess.h
    RyzenAdjAccess.cpp
)

# Theme.qml is a QML singleton (pragma Singleton); mark it so the generated
# qmldir emits the `singleton` entry and it can be referenced as `Theme.*`.
set_source_files_properties(qml/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(Boreas
    URI BoreasApp
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/PerformanceTab.qml
        qml/LimitsTab.qml
        qml/FanCurveEditor.qml
        qml/StatusLogPanel.qml
        qml/Theme.qml
        qml/Card.qml
        qml/ProfileSelector.qml
)

target_link_libraries(Boreas
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Qml
        Qt6::DBus
)

# ---------------------------------------------------------------------------
# Testing (additive; enabled by default, can be turned off with -DBOREAS_BUILD_TESTS=OFF)
# ---------------------------------------------------------------------------
option(BOREAS_BUILD_TESTS "Build Boreas property-based and unit tests" ON)

if(BOREAS_BUILD_TESTS)
    enable_testing()

    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    # rapidcheck: property-based testing library (generation + shrinking).
    # Integrated via FetchContent since it is not packaged on this system.
    include(FetchContent)
    FetchContent_Declare(
        rapidcheck
        GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
        GIT_TAG        6e8dadfdafa3a74eabb52ead87f8787f72eccd0b
        # rapidcheck's core library does not require its ext/ test submodules
        # (catch/googletest, gated behind RC_ENABLE_TESTS which defaults OFF).
        # Skipping the recursive submodule fetch avoids a clone failure in
        # constrained environments and does not affect the built library.
        GIT_SUBMODULES ""
    )
    FetchContent_MakeAvailable(rapidcheck)

    # Property 4: persistence round-trip and profile isolation.
    qt_add_executable(tst_property4_persistence
        tests/tst_property4_persistence.cpp
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property4_persistence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property4_persistence
        PRIVATE
            Qt6::Core
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property4_persistence COMMAND tst_property4_persistence)

    # Property 7: capability monotonicity. Exercises the real HardwareManager
    # capability detection against the in-memory fakes, so it compiles the
    # HardwareManager translation unit and its concrete platform-access
    # implementations (which the production ctor references).
    qt_add_executable(tst_property7_capability
        tests/tst_property7_capability.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property7_capability PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property7_capability
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property7_capability COMMAND tst_property7_capability)

    # Property 5: status log ring buffer. Drives the private recordResult() ring
    # buffer through the public applyTemperatureThreshold() write path against
    # the in-memory fakes, so it compiles the HardwareManager translation unit
    # and its concrete platform-access implementations (referenced by the
    # production ctor). Validates Requirements 13.3, 13.4, 13.5.
    qt_add_executable(tst_property5_statuslog
        tests/tst_property5_statuslog.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property5_statuslog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property5_statuslog
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property5_statuslog COMMAND tst_property5_statuslog)

    # Task 4.2: example-based unit tests for HardwareManager persistence load
    # (default-fallback + report) and per-profile scoping of the settings
    # accessors. Links the HardwareManager translation unit and its real
    # platform-access implementations (fakes are injected at runtime), so no
    # hardware or root is required. Validates Requirements 11.5, 11.7, 12.2, 12.3.
    qt_add_executable(tst_unit_settings_scoping
        tests/tst_unit_settings_scoping.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_unit_settings_scoping PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_unit_settings_scoping
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
    )
    add_test(NAME tst_unit_settings_scoping COMMAND tst_unit_settings_scoping)

    # Task 6.4: error-path unit tests for read failures. Forces the fakes to
    # report missing sysfs files / unavailable D-Bus services and asserts that
    # readFailed is emitted, the temperature getters return the NaN sentinel,
    # and the manager never terminates. Links the HardwareManager translation
    # unit and its real platform-access implementations (fakes are injected at
    # runtime), so no hardware or root is required. Validates Requirements 3.4, 3.5.
    qt_add_executable(tst_unit_read_failures
        tests/tst_unit_read_failures.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_unit_read_failures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_unit_read_failures
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
    )
    add_test(NAME tst_unit_read_failures COMMAND tst_unit_read_failures)

    # Property 6: temperature threshold crossing. Replays random reading/threshold
    # sequences through the real HardwareManager::refreshTemperatures() against the
    # in-memory fakes, asserting the flagged-iff-reading>=threshold invariant and
    # that a timestamped crossing event fires only on the transition into the
    # crossing state. Compiles the HardwareManager translation unit and its
    # concrete platform-access implementations (referenced by the production ctor).
    # Validates Requirements 7.7, 7.8.
    qt_add_executable(tst_property6_threshold
        tests/tst_property6_threshold.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property6_threshold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property6_threshold
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property6_threshold COMMAND tst_property6_threshold)

    # Property 1: fan curve validation gating. Generates fan-curve point lists
    # biased toward the count/temperature/percent boundaries and asserts the real
    # HardwareManager::validateFanCurve reports valid iff the full predicate holds
    # (2..8 points, strictly ascending tempC, tempC in [0,105], percent in
    # [0,100]). Compiles the HardwareManager translation unit and its concrete
    # platform-access implementations (referenced by the production ctor).
    # Validates Requirements 6.3, 6.4, 6.5, 6.6.
    qt_add_executable(tst_property1_fancurve
        tests/tst_property1_fancurve.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property1_fancurve PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property1_fancurve
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property1_fancurve COMMAND tst_property1_fancurve)

    # Property 2: every write emits exactly one apply result. Drives the real
    # HardwareManager write paths (applyGpuMode, applyFanCurve, applyPowerLimits)
    # against the in-memory fakes under scripted WriteOutcomes (Ok/Timeout/
    # IoError), asserting exactly one applyResult is emitted per invocation and
    # success is true iff the outcome is Ok. Compiles the HardwareManager
    # translation unit and its concrete platform-access implementations
    # (referenced by the production ctor). Validates Requirements 2.5, 4.5, 5.11,
    # 6.10, 8.7, 9.4.
    qt_add_executable(tst_property2_applyresult
        tests/tst_property2_applyresult.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property2_applyresult PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property2_applyresult
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property2_applyresult COMMAND tst_property2_applyresult)

    # Property 9: GPU offered-modes subset and confirmation gating. Seeds a
    # random runtime-detected supported set via the real refreshGpuMode() path,
    # then asserts the C++ guard behind QML's subset/confirm gating: a confirmed
    # applyGpuMode writes via IDBusAccess::setGpuMode iff the requested mode is
    # in the supported set, and performs no write for an unsupported mode.
    # Compiles the HardwareManager translation unit and its concrete
    # platform-access implementations (referenced by the production ctor).
    # Validates Requirements 5.2, 5.3, 5.10, 5.11, 9.2, 9.3.
    qt_add_executable(tst_property9_gpumode
        tests/tst_property9_gpumode.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property9_gpumode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property9_gpumode
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property9_gpumode COMMAND tst_property9_gpumode)

    # Property 8: editing never triggers a write. Replays random sequences of
    # read-only / edit / validation calls (validateFanCurve, fanCurveFor,
    # tempThresholdFor, powerLimitsFor, setSelectedProfile, refresh*/detect*,
    # property getters) -- but never an applyXxx -- through the real
    # HardwareManager against the in-memory fakes, asserting zero write() /
    # setGpuMode / setFanCurve / save calls reach the fakes. Compiles the
    # HardwareManager translation unit and its concrete platform-access
    # implementations (referenced by the production ctor).
    # Validates Requirements 6.7, 8.6.
    qt_add_executable(tst_property8_nowrite
        tests/tst_property8_nowrite.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property8_nowrite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property8_nowrite
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property8_nowrite COMMAND tst_property8_nowrite)

    # Task 10.9: error-path unit tests for write failures. Scripts the in-memory
    # fakes to return each WriteOutcome::Status failure (Timeout/IoError/
    # PermissionDenied/Unsupported) for the power-limit, GPU-mode, and fan-curve
    # write paths and asserts the manager emits exactly one applyResult(false,
    # message) with a specific message (incl. the exact PL1/PL2 blocking-write
    # timeout string) and never crashes. Links the HardwareManager translation
    # unit and its real platform-access implementations (fakes are injected at
    # runtime), so no hardware or root is required. Validates Requirements 3.6, 8.10.
    qt_add_executable(tst_unit_write_failures
        tests/tst_unit_write_failures.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_unit_write_failures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_unit_write_failures
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
    )
    add_test(NAME tst_unit_write_failures COMMAND tst_unit_write_failures)

    # Property 3: read-back consistency after write. Drives the real
    # HardwareManager::applyPowerLimits() write + read-back path against the
    # in-memory fakes, scripting the FakeSysfsAccess read-back to either equal or
    # be perturbed away from the requested PL1/PL2 value, and asserts the
    # displayed/persisted value always equals the driver read-back (never the
    # requested value) with a discrepancy reported iff they differ. Compiles the
    # HardwareManager translation unit and its concrete platform-access
    # implementations (referenced by the production ctor).
    # Validates Requirements 8.8, 8.9, 8.10, 10.1, 10.2, 10.3.
    qt_add_executable(tst_property3_readback
        tests/tst_property3_readback.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        Fakes.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_property3_readback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_property3_readback
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
            rapidcheck
    )
    add_test(NAME tst_property3_readback COMMAND tst_property3_readback)

    # Task 16.2: representative D-Bus integration tests against a mock service.
    # Stands up a mock supergfxctl Daemon (Supported -> mode-enum indices) and a
    # presence-only mock asusd service on the SESSION bus, then exercises the
    # exact wire contract the production DBusAccess depends on: the supergfxctl
    # mode-list decode (Req 5.1) and the asusd fan-curve support probe (Req 6.11).
    # Links Qt6::DBus + Qt6::Test + DBusAccess.cpp (the real IDBusAccess impl the
    # contract is mirrored from). Skips gracefully (QSKIP) when no session bus is
    # available, so it is safe in headless CI/build sandboxes.
    qt_add_executable(tst_integration_dbus
        tests/tst_integration_dbus.cpp
        DBusAccess.cpp
        DBusAccess.h
        PlatformAccess.h
        DataModels.h
    )
    target_include_directories(tst_integration_dbus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_integration_dbus
        PRIVATE
            Qt6::Core
            Qt6::DBus
            Qt6::Test
    )
    add_test(NAME tst_integration_dbus COMMAND tst_integration_dbus)

    # Task 16.1: build + load smoke tests. main.cpp defines main() and cannot be
    # linked into a test binary, so this test reproduces the production wiring
    # directly: it constructs a HardwareManager, sets it as the `hardwareManager`
    # context property, and loads the UI via loadFromModule("BoreasApp", "Main"),
    # asserting a root object is created and the injected manager is visible to
    # it. Build-level guarantees are pinned where feasible: linking Qt6::DBus is
    # the evidence for the required QtDBus component (Req 1.4) and __cplusplus is
    # asserted to be C++17 (Req 1.5). It links Qt6::Qml/Quick for the QML engine
    # and the HardwareManager translation unit plus its concrete platform-access
    # implementations (referenced by the production ctor). The BoreasApp QML
    # module is produced by qt_add_qml_module(Boreas ...) alongside this build;
    # a filesystem-only copy of it (with the resource-redirect `prefer` lines
    # stripped, staged below) is passed in as BOREAS_QML_IMPORT_PATH so the
    # standalone test can add it to the engine import path and resolve the
    # module purely from the filesystem. This is a GUI test, so it
    # forces the offscreen QPA platform to run headless under ctest.
    # Validates Requirements 1.1, 1.2, 1.3, 1.4, 1.5.
    qt_add_executable(tst_smoke_load
        tests/tst_smoke_load.cpp
        HardwareManager.cpp
        HardwareManager.h
        SettingsStore.cpp
        SettingsStore.h
        SysfsAccess.cpp
        SysfsAccess.h
        DBusAccess.cpp
        DBusAccess.h
        RyzenAdjAccess.cpp
        RyzenAdjAccess.h
        DataModels.h
        PlatformAccess.h
    )
    target_include_directories(tst_smoke_load PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # The generated BoreasApp qmldir carries a `prefer :/qt/qml/BoreasApp/`
    # directive that redirects loadFromModule to the module resources embedded in
    # the Boreas executable -- those resources are not present in this standalone
    # test binary. Stage a filesystem-only copy of the module (qmldir with the
    # `prefer` line stripped, plus the .qml files) under the test's binary dir so
    # loadFromModule("BoreasApp", "Main") resolves purely from the filesystem.
    # The staging depends on the Boreas module being built first.
    set(_boreas_module_src "${CMAKE_CURRENT_BINARY_DIR}/BoreasApp")
    set(_boreas_smoke_import "${CMAKE_CURRENT_BINARY_DIR}/tst_smoke_load_qml")
    set(_boreas_smoke_module "${_boreas_smoke_import}/BoreasApp")
    add_custom_command(
        OUTPUT "${_boreas_smoke_module}/qmldir"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_boreas_smoke_module}/qml"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_boreas_module_src}/qml" "${_boreas_smoke_module}/qml"
        # Copy both the top-level qmldir AND the nested qml/qmldir with the
        # `prefer` (resource redirect) line removed. The generated module places
        # a `prefer :/qt/qml/BoreasApp/` directive in BOTH files; the nested one
        # redirects sibling-component (StatusLogPanel, PerformanceTab, ...)
        # resolution back to the resources embedded in the Boreas executable,
        # which this standalone test binary does not carry. Stripping both makes
        # the whole component tree resolve from the staged filesystem files.
        COMMAND ${CMAKE_COMMAND}
                -DIN_QMLDIR=${_boreas_module_src}/qmldir
                -DOUT_QMLDIR=${_boreas_smoke_module}/qmldir
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/strip_prefer_qmldir.cmake
        COMMAND ${CMAKE_COMMAND}
                -DIN_QMLDIR=${_boreas_module_src}/qml/qmldir
                -DOUT_QMLDIR=${_boreas_smoke_module}/qml/qmldir
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/strip_prefer_qmldir.cmake
        DEPENDS Boreas
        COMMENT "Staging filesystem-only BoreasApp module for tst_smoke_load"
        VERBATIM
    )
    add_custom_target(tst_smoke_load_qml_stage
        DEPENDS "${_boreas_smoke_module}/qmldir")
    add_dependencies(tst_smoke_load tst_smoke_load_qml_stage)

    target_compile_definitions(tst_smoke_load PRIVATE
        BOREAS_QML_IMPORT_PATH="${_boreas_smoke_import}"
    )
    target_link_libraries(tst_smoke_load
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
            Qt6::DBus
            Qt6::Test
    )
    add_test(NAME tst_smoke_load COMMAND tst_smoke_load)
endif()
