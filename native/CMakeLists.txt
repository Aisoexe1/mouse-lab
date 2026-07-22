cmake_minimum_required(VERSION 3.16)
project(mouse_test_injector_gui CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

# --- Embed weapon JSON files as C arrays ---
set(WEAPONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../weapons")
file(GLOB WEAPON_FILES CONFIGURE_DEPENDS "${WEAPONS_DIR}/*.json")
set(EMBED_CPP "${CMAKE_CURRENT_BINARY_DIR}/weapons_embedded.cpp")
file(WRITE "${EMBED_CPP}" "#include \"weapons_embedded.h\"\n// Auto-generated at configure time — do not edit\n\n")
set(WEAPON_COUNT 0)
set(WEAPON_ENTRIES "")
foreach(WF ${WEAPON_FILES})
    get_filename_component(WBASE "${WF}" NAME)
    get_filename_component(WNAME "${WF}" NAME_WE)
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" WIDENT "${WNAME}")
    file(READ "${WF}" WHEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," WBYTES "${WHEX}")
    file(APPEND "${EMBED_CPP}" "static const char kWD_${WIDENT}[] = {${WBYTES}0x00};\n")
    list(APPEND WEAPON_ENTRIES "  {\"${WBASE}\", kWD_${WIDENT}, sizeof(kWD_${WIDENT})-1}")
    math(EXPR WEAPON_COUNT "${WEAPON_COUNT}+1")
endforeach()
if(WEAPON_FILES)
    string(JOIN ",\n" WEAPON_TABLE ${WEAPON_ENTRIES})
    file(APPEND "${EMBED_CPP}" "\nconst EmbeddedWeapon kEmbeddedWeapons[] = {\n${WEAPON_TABLE}\n};\n")
else()
    file(APPEND "${EMBED_CPP}" "\nconst EmbeddedWeapon kEmbeddedWeapons[1] = {};\n")
endif()
file(APPEND "${EMBED_CPP}" "const int kEmbeddedWeaponCount = ${WEAPON_COUNT};\n")

add_executable(mouse_test_injector_gui
    src/main.cpp
    src/serial_port.cpp
    "${EMBED_CPP}"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_compile_definitions(mouse_test_injector_gui PRIVATE HAVE_EMBEDDED_WEAPONS)

target_link_libraries(mouse_test_injector_gui PRIVATE glfw OpenGL::GL)

if(APPLE)
    target_link_libraries(mouse_test_injector_gui PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreGraphics" "-framework ApplicationServices")
endif()

if(WIN32)
    target_link_libraries(mouse_test_injector_gui PRIVATE setupapi comdlg32)
    set_target_properties(mouse_test_injector_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(mouse_test_injector_gui PRIVATE /ENTRY:mainCRTStartup)
endif()
