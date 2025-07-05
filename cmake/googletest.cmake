set(GOOGLE_TEST_SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/googletest)

set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(GTEST_HAS_ABSL OFF CACHE BOOL "" FORCE)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

add_subdirectory(${GOOGLE_TEST_SOURCE_DIR} EXCLUDE_FROM_ALL)

set(GOOGLE_TEST_LIBRARIES googletest GTest::gtest_main)
set(GOOGLE_TEST_INCLUDE_DIR GOOGLE_TEST_SOURCE_DIR/include)
