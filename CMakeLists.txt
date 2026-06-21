cmake_minimum_required(VERSION 3.25)

# 1. Tell CMake where to look for Geode's structural files
if(NOT DEFINED GEODE_SDK)
    if(DEFINED ENV{GEODE_SDK})
        set(GEODE_SDK $ENV{GEODE_SDK})
    else()
        message(FATAL_ERROR "GEODE_SDK environment variable not set!")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${GEODE_SDK}/cmake")

project(ai-deco-mod VERSION 1.0.0 LANGUAGES CXX)

# 2. Safely find the Geode setup files now that paths are linked
find_package(Geode REQUIRED)

# 3. Force the runner to fetch the correct Android loader binaries manually
execute_process(COMMAND geode sdk install-binaries)

# 4. Initialize and build the mod packaging layout
setup_geode_mod(
    ID "jetjet090909.aideco"
    NAME "AI Deco Assistant"
    DEVELOPER "jetjet090909"
    DESCRIPTION "Automates level decoration using Gemini."
    VERSION 1.0.0
)

# Source files
add_geode_mod_src(src/main.cpp)
