add_executable(replay_check EXCLUDE_FROM_ALL
    main.cpp
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayFile.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayFile.h"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayMessages.h"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayObserve.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayObserve.h"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayState.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayState.h"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayWire.cpp"
    "${PROJECT_SOURCE_DIR}/GWToolboxdll/Utils/Replay/ReplayWire.h")

target_include_directories(replay_check PRIVATE
    "${PROJECT_SOURCE_DIR}/GWToolboxdll"
    "${PROJECT_SOURCE_DIR}/Dependencies/GWCA/include")
target_compile_options(replay_check PRIVATE -Wall -Wextra)
# The entry point is wmain, which MinGW only links when told to.
target_link_options(replay_check PRIVATE -municode)
