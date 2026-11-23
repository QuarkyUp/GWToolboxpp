add_executable(replay_check EXCLUDE_FROM_ALL
    main.cpp
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayFile.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayFile.h"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayObserve.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayObserve.h"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayState.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayState.h"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayWire.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/ReplayWire.h")

target_include_directories(replay_check PRIVATE "${PROJECT_SOURCE_DIR}/GWToolboxdll")
target_compile_options(replay_check PRIVATE -Wall -Wextra)
# The entry point is wmain, which MinGW only links when told to.
target_link_options(replay_check PRIVATE -municode)
