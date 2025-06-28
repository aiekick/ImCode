set(IMGUI_TEST_ENGINE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/imgui_test_engine)

file(GLOB_RECURSE INGUI_TEST_ENGINE_SOURCES 
	${IMGUI_TEST_ENGINE_SOURCE_DIR}/imgui_test_engine/*.h
	${IMGUI_TEST_ENGINE_SOURCE_DIR}/imgui_test_engine/*.cpp)
                 
add_library(imgui_test_engine STATIC ${INGUI_TEST_ENGINE_SOURCES})
    
set(IMGUI_TEST_ENGINE_LIBRARIES imgui_test_engine)
set(IMGUI_TEST_ENGINE_INCLUDE_DIRS ${IMGUI_TEST_ENGINE_SOURCE_DIR})

target_include_directories(imgui_test_engine PRIVATE 
	${IMGUI_INCLUDE_DIR})

target_link_libraries(imgui_test_engine ${IMGUI_LIBRARIES})

set_target_properties(imgui_test_engine PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(imgui_test_engine PROPERTIES FOLDER 3rdparty)
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    #target_compile_options(imgui PRIVATE "-Wno-everything") 
else()
    target_compile_options(imgui_test_engine PRIVATE "-Wno-everything") 
endif()
