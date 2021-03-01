include(FetchContent)

# GPDS
if (DEPENDENCY_GPDS_DOWNLOAD)
    FetchContent_Declare(
        gpds
        GIT_REPOSITORY https://github.com/simulton/gpds
        GIT_TAG        develop
    )
    FetchContent_MakeAvailable(gpds)

    set(DEPENDENCY_GPDS_TARGET "gpds-shared")
endif()

# Qt5
find_package(
    Qt5
    REQUIRED
    COMPONENTS
        Core
        Gui
        Widgets
)
