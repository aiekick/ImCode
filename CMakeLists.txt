cmake_minimum_required(VERSION 3.20)

set(PROJECT TestApp)

project(${PROJECT} CXX)
enable_language(C CXX)

option(USE_IM_CODE_TESTING "Enable ImCode testing" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

## some defines for debug mode (before 3rdparty.cmake)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")

set(FINAL_BIN_DIR ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_PDB_OUTPUT_DIRECTORY_DEBUG "${FINAL_BIN_DIR}/debug")
set(CMAKE_PDB_OUTPUT_DIRECTORY_RELEASE "${FINAL_BIN_DIR}/release")
set(CMAKE_PDB_OUTPUT_DIRECTORY_MINSIZEREL "${FINAL_BIN_DIR}/minsizerel")
set(CMAKE_PDB_OUTPUT_DIRECTORY_RELWITHDEBINFO "${FINAL_BIN_DIR}/reldeb")

set(CMAKE_INSTALL_PREFIX ${FINAL_BIN_DIR})
set(CMAKE_INSTALL_BINDIR ${FINAL_BIN_DIR})

set(ARCH x64)

add_definitions(-DIMGUI_APP_GLFW_GL3)
add_definitions(-DIMGUI_ENABLE_TEST_ENGINE)
add_definitions(-DIMGUI_IMPL_OPENGL_LOADER_GLAD)
add_definitions(-DIMGUI_TEST_ENGINE_ENABLE_CAPTURE)
add_definitions(-DIMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL)
include(cmake/3rdparty.cmake)

# CMake Sources
file(GLOB_RECURSE CMAKE_SOURCES ${CMAKE_SOURCE_DIR}/cmake/*.*)
source_group(TREE ${CMAKE_SOURCE_DIR}/cmake PREFIX cmake FILES ${CMAKE_SOURCES})

# App Sources
file(GLOB_RECURSE SRC_SOURCES ${CMAKE_SOURCE_DIR}/src/*.*)
source_group(TREE ${CMAKE_SOURCE_DIR}/src PREFIX src FILES ${SRC_SOURCES})

set(IMGUI_IMPL_SOURCES)
file(GLOB IMGUI_IMPL_SOURCES 
	#${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_glfw.cpp
	#${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_glfw.h
	#${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_opengl3.cpp
	#${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_opengl3.h
	#${CMAKE_SOURCE_DIR}/3rdparty/imgui/misc/freetype/imgui_freetype.cpp
	#${CMAKE_SOURCE_DIR}/3rdparty/imgui/misc/freetype/imgui_freetype.h
	${CMAKE_SOURCE_DIR}/3rdparty/imgui_test_engine/shared/imgui_app.cpp
	${CMAKE_SOURCE_DIR}/3rdparty/imgui_test_engine/shared/imgui_app.h)
source_group(src FILES ${MAIN_SOURCES})
source_group(src/ImGuiImpl FILES ${IMGUI_IMPL_SOURCES})

add_executable(${PROJECT} 
	${SRC_SOURCES}
	${CMAKE_SOURCES}
	${EZ_LIBS_SOURCES}
	${IMGUI_IMPL_SOURCES})

target_link_libraries(${PROJECT}
	${GLAD_LIBRARIES}
	${GLFW_LIBRARIES}
	${IMGUI_LIBRARIES}
	${IMGUI_TEST_ENGINE_LIBRARIES}
	${IM_CODE_LIBRARIES}
	${OPENGL_LIBRARIES}
)

target_include_directories(${PROJECT} PRIVATE 
	${CMAKE_SOURCE_DIR}/3rdparty
	${CMAKE_SOURCE_DIR}/src
	${IMGUI_INCLUDE_DIR}
	${IMGUI_TEST_ENGINE_INCLUDE_DIRS}
	${IM_CODE_INCLUDE_DIRS}
)

target_compile_definitions(${PROJECT} PRIVATE 
	IMGUI_DEFINE_MATH_OPERATORS
)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}")
install(TARGETS ${PROJECT} DESTINATION ${CMAKE_INSTALL_PREFIX})

if(NOT CMAKE_DEBUG_POSTFIX)
	set(CMAKE_DEBUG_POSTFIX _debug)
endif()
if(NOT CMAKE_RELEASE_POSTFIX)
	set(CMAKE_RELEASE_POSTFIX) ## to config in file
endif()
if(NOT CMAKE_MINSIZEREL_POSTFIX)
	set(CMAKE_MINSIZEREL_POSTFIX _minsizerel)
endif()
if(NOT CMAKE_RELWITHDEBINFO_POSTFIX)
	set(CMAKE_RELWITHDEBINFO_POSTFIX _reldeb)
endif()

set_target_properties(${PROJECT} PROPERTIES OUTPUT_NAME "${PROJECT}_${CMAKE_SYSTEM_NAME}_${ARCH}")
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${FINAL_BIN_DIR}")
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FINAL_BIN_DIR}")
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${FINAL_BIN_DIR}")
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${FINAL_BIN_DIR}")

########################################################
################# BUILD INCREMENT TOOL #################
########################################################

## only for Debug
add_subdirectory(3rdparty/buildinc)
add_custom_command(
		TARGET ${PROJECT} PRE_BUILD
		COMMAND BuildInc ${PROJECT} $<$<CONFIG:Debug>:${CMAKE_SOURCE_DIR}/src/headers/${PROJECT}Build.h> -ff ${CMAKE_SOURCE_DIR}/src/res/big.flf --no-help
)

########################################################
##### TESTS ############################################
########################################################

if(USE_IM_CODE_TESTING)
	enable_testing()
	include(cmake/googletest.cmake)
	add_subdirectory(tests)
endif()
