set(IM_CODE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/ImCode)

file(GLOB_RECURSE IM_CODE_SOURCES 
	${IM_CODE_SOURCE_DIR}/*.h
	${IM_CODE_SOURCE_DIR}/*.cpp)
                 
add_library(ImCode STATIC ${IM_CODE_SOURCES})
    
set(IM_CODE_LIBRARIES ImCode)

target_include_directories(ImCode PRIVATE 
	${IMGUI_INCLUDE_DIR})

target_link_libraries(ImCode ${IMGUI_LIBRARIES})

set_target_properties(ImCode PROPERTIES LINKER_LANGUAGE CXX)

