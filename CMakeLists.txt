cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(JobSystem)

file(GLOB_RECURSE SRC_FILES "Code/Source/*.cpp" "Code/Source*.hpp")

add_executable(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)


target_sources(${PROJECT_NAME} PRIVATE ${SRC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE "Code/Include")