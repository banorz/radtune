cmake_minimum_required(VERSION 3.10)
project(RadTune)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Paths to ADLX SDK
set(ADLX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/adlx_sdk")
set(ADLX_INCLUDE_DIR "${ADLX_SDK_DIR}/SDK/Include")

# Sources
set(SOURCES
    src/main.cpp
    src/ADLXHelper.cpp
    src/WinAPIs.cpp
    src/ProfileParser.cpp
    src/Scheduler.cpp
)

# Header files for better IDE integration
set(HEADERS
    src/ADLXHelper.h
    src/ProfileParser.h
    src/Scheduler.h
)

add_executable(RadTune ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(RadTune PRIVATE 
    "${ADLX_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Link libraries
if(WIN32)
    target_link_libraries(RadTune PRIVATE User32)
endif()

# ---------------------------------------------------------------------------
# RadTuneGUI - native Win32 frontend that shells out to RadTune.exe.
# Independent of ADLX (does not include or link the SDK); builds standalone.
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(RadTuneGUI WIN32
        gui/RadTuneGui.cpp
        gui/RadTuneGui.rc
    )
    target_link_libraries(RadTuneGUI PRIVATE
        User32 Gdi32 Comctl32 Comdlg32 Shell32
    )
    # We embed our own manifest (elevation + Common Controls v6) via the .rc.
    # Stop the linker from also generating one, otherwise the two collide.
    if(MSVC)
        set_target_properties(RadTuneGUI PROPERTIES LINK_FLAGS "/MANIFEST:NO")
    endif()
endif()
