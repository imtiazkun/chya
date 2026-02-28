cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION 3.20)
cmake_policy(SET CMP0169 OLD)  # allow FetchContent_Populate for imgui (no CMakeLists)

project(chya VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
find_package(OpenGL REQUIRED)
find_package(SQLite3 REQUIRED)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies: GLFW + ImGui (with GLFW and OpenGL3 backends)
include(FetchContent)

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.90.1
)
FetchContent_Populate(imgui)

FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
)
FetchContent_Populate(stb)

# Font Awesome 6 (icon font for UI buttons)
FetchContent_Declare(fontawesome
  GIT_REPOSITORY https://github.com/FortAwesome/Font-Awesome.git
  GIT_TAG        6.5.2
)
FetchContent_Populate(fontawesome)
set(FA_WEBFONTS_DIR ${fontawesome_SOURCE_DIR}/webfonts)

# ImGui: build core + GLFW + OpenGL3 backend (no demo)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

set(CHYA_SOURCES src/main.cpp)
if(APPLE)
  list(APPEND CHYA_SOURCES src/folder_picker_mac.mm)
else()
  list(APPEND CHYA_SOURCES src/folder_picker_stub.cpp)
endif()

add_executable(chya
  ${CHYA_SOURCES}
  ${IMGUI_SOURCES}
)
# Copy icon font and logo next to executable so they can be loaded at runtime
add_custom_command(TARGET chya POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FA_WEBFONTS_DIR}/fa-solid-900.ttf"
    "$<TARGET_FILE_DIR:chya>/fa-solid-900.ttf"
  COMMENT "Copy Font Awesome icon font to output directory"
)
add_custom_command(TARGET chya POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/logo.png"
    "$<TARGET_FILE_DIR:chya>/logo.png"
  COMMENT "Copy logo.png to output directory"
)
target_include_directories(chya PRIVATE
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${stb_SOURCE_DIR}
)
target_link_libraries(chya PRIVATE
  glfw
  OpenGL::GL
  SQLite::SQLite3
)
if(APPLE)
  target_link_libraries(chya PRIVATE "-framework AppKit")
endif()
