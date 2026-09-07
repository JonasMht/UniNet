# Records which source a wheel was built from, so the wheel can say whether it
# is still current. See python/<package>/_version.py for what reads this.
#
# THIS FILE IS IDENTICAL IN UniData, UniNet, UniPhys AND UniRender. Nothing in
# it is per-project: the package name is the one argument. Change it in one and
# copy it to the other three.
#
# WHY A BUILD-TIME STAMP AND NOT A RUNTIME GIT READ. Once installed, a package
# lives in site-packages with no repository above it, so at runtime there is
# nothing left to read: the commit has to be captured while the source is still
# in front of us. That is here.
#
# The file is written into the BUILD tree and installed from there. Writing it
# into the source tree would work too and is what UniNet's Slicer installer
# does -- but scikit-build-core honours .gitignore when it copies
# wheel.packages, so a generated file there has to be un-ignored to survive,
# and an un-ignored generated file is one `git status` away from being
# committed by accident. The build tree has neither problem.

function(uni_build_stamp package)
    set(_commit "")
    set(_describe "")
    set(_dirty "False")

    find_package(Git QUIET)
    if(Git_FOUND)
        # ERROR_QUIET throughout: a source tarball with no .git is a perfectly
        # normal way to build, and it must produce an unstamped wheel rather
        # than a configure error.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_VARIABLE _commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_VARIABLE _describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        # Uncommitted changes mean the commit alone does not describe what was
        # built. Saying so is the difference between a reproducible stamp and a
        # misleading one.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_VARIABLE _status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT "${_status}" STREQUAL "")
            set(_dirty "True")
        endif()
    endif()

    string(TIMESTAMP _date "%Y-%m-%dT%H:%M:%SZ" UTC)

    # A stamp already in the source tree wins. UniNet's Slicer installer writes
    # one there before it builds, and having two claim the same path in the
    # wheel is a collision; the installer's is the authoritative one because it
    # is what its own provenance checks compare against.
    if(EXISTS "${PROJECT_SOURCE_DIR}/python/${package}/_buildinfo.py")
        message(STATUS "${package}: keeping the build stamp already in the source tree")
        return()
    endif()

    set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/_buildinfo.py")
    file(WRITE "${_stamp}"
"\"\"\"Generated at build time; do not edit and do not commit.

Written by cmake/UniBuildStamp.cmake. Read by ${package}/_version.py, which
turns it into the one line this package prints when it is imported.
\"\"\"
BUILD_GIT = \"${_commit}\"
BUILD_DESCRIBE = \"${_describe}\"
BUILD_DATE = \"${_date}\"
BUILD_DIRTY = ${_dirty}
# The checkout this was built from. Compared against its HEAD at import time to
# decide whether this build is still current; ignored when the path no longer
# exists, which is the normal case on any machine that only installed a wheel.
BUILD_SOURCE = r\"${PROJECT_SOURCE_DIR}\"
")

    # DESTINATION <package>: the same place the compiled extension goes, which
    # scikit-build-core maps to the package directory inside the wheel.
    install(FILES "${_stamp}" DESTINATION "${package}")

    if(_commit STREQUAL "")
        message(STATUS "${package}: no git information; the wheel will be unstamped")
    else()
        string(SUBSTRING "${_commit}" 0 7 _short)
        message(STATUS "${package}: build stamped ${_short} (${_describe})")
    endif()
endfunction()
