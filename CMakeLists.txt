cmake_minimum_required(VERSION 3.25)

project(ai-deco-mod VERSION 1.0.0 LANGUAGES CXX)

# Find Geode
find_package(Geode REQUIRED)

# Force the runner to fetch the correct Android loader binaries manually
execute_process(COMMAND geode sdk install-binaries)

# Setup mod files
setup_geode_mod(
    ID "jetjet090909.aideco"
    NAME "AI Deco Assistant"
    DEVELOPER "jetjet090909"
    DESCRIPTION "Automates level decoration using Gemini."
    VERSION 1.0.0
)

# Source files
add_geode_mod_src(src/main.cpp)
