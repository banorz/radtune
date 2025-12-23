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
)

# Header files for better IDE integration
set(HEADERS
    src/ADLXHelper.h
    src/ProfileParser.h
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
