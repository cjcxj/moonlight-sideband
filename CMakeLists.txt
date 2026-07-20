cmake_minimum_required(VERSION 3.15)
project(moonlight_sideband LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态链接运行时 (MT)，减少 DLL 依赖
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 头文件搜索路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
)

# 源文件
set(SOURCES
    main.cpp
    src/SidebandServer.cpp
    src/SidebandSession.cpp
    modules/CursorModule.cpp
    modules/DisplayModule.cpp
)

add_executable(moonlight_sideband ${SOURCES})

# 链接 Windows 系统库
target_link_libraries(moonlight_sideband PRIVATE
    ws2_32      # Winsock2
    gdi32       # GDI
    user32      # User32
    gdiplus     # GDI+ (PNG 编码)
    shcore      # DPI Awareness
    ole32       # COM (IStream)
    advapi32    # Registry
    shell32     # Shell
)

# 嵌入 Manifest (DPI Awareness)
if(MSVC)
    target_link_options(moonlight_sideband PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/moonlight_sideband.manifest"
    )
endif()

# Windows 子系统 (无控制台黑框)
set_target_properties(moonlight_sideband PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
)

# Unicode 支持
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WINSOCK_DEPRECATED_NO_WARNINGS)

# 输出目录
set_target_properties(moonlight_sideband PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)
