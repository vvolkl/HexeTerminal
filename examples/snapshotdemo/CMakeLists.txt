set(SAMPLE_TERMINAL_HEADERS ${SAMPLE_TERMINAL_HEADERS}
)


#set(IMPLOT_HEADERS ../implot/implot.h ../implot/implot_internal.h)
#set(IMPLOT_SRC     ../implot/implot.cpp ../implot/implot_items.cpp ../implot/implot_demo.cpp)
#
#if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../implot/backends")
#list(APPEND IMPLOT_HEADERS ../implot/backends/implot_backend.h ../implot/backends/implot_impl_opengl3.h)
#list(APPEND IMPLOT_SRC ../implot/backends/implot_backend.h ../implot/backends/implot_impl_opengl3.cpp)
#endif()
#
##add_library(implot ${IMPLOT_HEADERS} ${IMPLOT_SRC})
#target_link_libraries(implot PUBLIC imgui)
##target_compile_definitions(implot PUBLIC IMPLOT_DEBUG IMPLOT_DLL_EXPORT IMPLOT_BACKEND_ENABLE_OPENGL3 IMGUI_IMPL_OPENGL_LOADER_GLAD)
#set_property(TARGET implot PROPERTY CXX_STANDARD 11)
#  target_compile_options(implot PRIVATE -Wall -Wextra -pedantic -Werror -mavx2 -Ofast)

#include_directories(../implot/)

set(SAMPLE_TERMINAL_SOURCES ${SAMPLE_TERMINAL_SOURCES}
    "snapshotdemo.cpp" "networkwidget.cpp" "folderwatch.cpp" 
)

add_executable(snapshotdemo ${EXAMPLE_HEADERS} ${EXAMPLE_SOURCES} ${SAMPLE_TERMINAL_HEADERS} ${SAMPLE_TERMINAL_SOURCES})
target_link_libraries(snapshotdemo PUBLIC SDL2::SDL2main SDL2::SDL2 imgui  freetype HexeTerminal ImGuiTerminal GLEW::GLEW OpenGL::GL pthread)
target_include_directories(snapshotdemo PRIVATE "${imgui_SOURCE_DIR}/examples/")
target_include_directories(snapshotdemo PRIVATE "$implot/")
add_custom_command(TARGET snapshotdemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FONTS_DIRECTORY}"
        $<TARGET_FILE_DIR:snapshotdemo>
)
