cmake_minimum_required(VERSION 3.20)

set(PROJECT ImCodeTests)
project(${PROJECT})

# GoogleTest requires at least C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# App Sources
file(GLOB_RECURSE SRC_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.*)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX src FILES ${SRC_SOURCES})

add_executable(${PROJECT} ${SRC_SOURCES})

target_link_libraries(
	${PROJECT}
	GTest::gtest_main
	${IM_CODE_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(${PROJECT})
