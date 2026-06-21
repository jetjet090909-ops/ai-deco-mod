cmake_minimum_required(VERSION 3.13)

# Change this if your repository folder has a different name
project(ai-deco-mod)

include(FetchContent)

FetchContent_Declare(
    Geode
    GIT_REPOSITORY https://github.com/geode-sdk/geode.git
    GIT_TAG        v4.0.0-beta.1
)

FetchContent_MakeAvailable(Geode)

add_library(${PROJECT_NAME} SHARED src/main.cpp)

setup_geode_mod(${PROJECT_NAME})
