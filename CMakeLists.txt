cmake_minimum_required(VERSION 3.17)
project(wkSetResCustom)

set(CMAKE_CXX_STANDARD 20)

set(WORM_TRACKER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../wkWormsTracker")

add_library(hacklib STATIC
        "${WORM_TRACKER_ROOT}/lib/hacklib/ExeFile.cpp"
        "${WORM_TRACKER_ROOT}/lib/hacklib/ExeFile_WIN32.cpp"
        "${WORM_TRACKER_ROOT}/lib/hacklib/PatternScanner.cpp"
        "${WORM_TRACKER_ROOT}/lib/hacklib/Memory_WIN32.cpp")

link_directories("${WORM_TRACKER_ROOT}/lib")
include_directories("${WORM_TRACKER_ROOT}/include" "${WORM_TRACKER_ROOT}/lib/hacklib")

configure_file(
        resource.rc.in
        resource.rc
)

add_library(wkSetResCustom SHARED
        src/Chat.cpp
        src/Chat.h
        src/Config.cpp
        src/Config.h
        src/Debugf.h
        src/Diagnostics.cpp
        src/Diagnostics.h
        src/dllmain.cpp
        src/Frontend.cpp
        src/Frontend.h
        src/Hooks.cpp
        src/Hooks.h
        src/RegistryMonitor.cpp
        src/RegistryMonitor.h
        src/ResolutionManager.cpp
        src/ResolutionManager.h
        src/W2App.cpp
        src/W2App.h
        ${CMAKE_CURRENT_BINARY_DIR}/resource.rc
)

set_property(TARGET wkSetResCustom hacklib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(wkSetResCustom hacklib capstone Polyhook_2 version advapi32)

set(WA_DEPLOY_DIR "C:/Program Files/Worms Armageddon/Team17/Worms Armageddon")

if(EXISTS "${WA_DEPLOY_DIR}/WA.exe")
    add_custom_command(TARGET wkSetResCustom POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:wkSetResCustom>" "${WA_DEPLOY_DIR}/wkSetResCustom.dll"
            COMMENT "Deploying wkSetResCustom.dll to ${WA_DEPLOY_DIR}")
endif()
