cmake_minimum_required(VERSION 3.16)
project(mouse_test_injector_gui CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(mouse_test_injector_gui
    src/main.cpp
    src/serial_port.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(mouse_test_injector_gui PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(mouse_test_injector_gui PRIVATE glfw OpenGL::GL)

if(APPLE)
    target_link_libraries(mouse_test_injector_gui PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreGraphics" "-framework ApplicationServices")
endif()

if(WIN32)
    target_link_libraries(mouse_test_injector_gui PRIVATE setupapi comdlg32)
    set_target_properties(mouse_test_injector_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(mouse_test_injector_gui PRIVATE /ENTRY:mainCRTStartup)
endif()
