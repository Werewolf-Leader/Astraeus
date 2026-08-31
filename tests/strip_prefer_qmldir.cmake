# Helper for the tst_smoke_load target (task 16.1).
#
# Copies a generated qmldir (IN_QMLDIR) to OUT_QMLDIR with any `prefer ...` line
# removed. The BoreasApp module's generated qmldir contains
# `prefer :/qt/qml/BoreasApp/`, which redirects loadFromModule to the module
# resources embedded in the Boreas executable. The standalone smoke-test binary
# does not carry those resources, so the smoke test loads the module from a
# staged filesystem copy; stripping the prefer directive makes loadFromModule
# resolve the .qml files sitting next to this qmldir instead of the missing
# resource path.
file(READ "${IN_QMLDIR}" _contents)
# Drop any line beginning with optional whitespace + `prefer`.
string(REGEX REPLACE "(^|\n)[ \t]*prefer[^\n]*" "\\1" _contents "${_contents}")
file(WRITE "${OUT_QMLDIR}" "${_contents}")
