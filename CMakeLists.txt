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
    src/Win32Util.cpp
    src/Config.cpp
    src/TrayIcon.cpp
    modules/CursorModule.cpp
    modules/DisplayModule.cpp
)

add_executable(moonlight_sideband ${SOURCES})

# 链接 Windows 系统库
target_link_libraries(moonlight_sideband PRIVATE
    ws2_32      # Winsock2 / WSAPoll
    gdi32       # GDI
    user32      # User32
    gdiplus     # GDI+ (PNG 编码)
    shcore      # DPI Awareness
    ole32       # COM (IStream)
    advapi32    # Registry
    shell32     # 托盘图标 / CommandLineToArgvW
    bcrypt      # BCryptGenRandom (令牌生成)
)

# 嵌入 Manifest (DPI Awareness)
if(MSVC)
    target_link_options(moonlight_sideband PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/moonlight_sideband.manifest"
    )
    target_compile_options(moonlight_sideband PRIVATE /W4)
endif()

# Windows 子系统 (无控制台黑框)。
# 注意：因此进程没有控制台 —— 不要指望 std::cout 或 SIGINT，
# 退出请走托盘菜单（见 TrayIcon）。
set_target_properties(moonlight_sideband PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
)

# Unicode 支持 + 目标 Windows 版本。
# _WIN32_WINNT 0x0601 (Windows 7) 是 WSAPoll / SIO_KEEPALIVE_VALS 所需的最低值。
add_compile_definitions(
    UNICODE _UNICODE
    NOMINMAX WIN32_LEAN_AND_MEAN
    _WINSOCK_DEPRECATED_NO_WARNINGS
    _WIN32_WINNT=0x0601
    WINVER=0x0601
)

# 输出目录
set_target_properties(moonlight_sideband PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)

# ============================================================
#  单元测试（可选）
#
#  只覆盖与 Windows 无关的纯逻辑：JSON 字段解析、协议包构造。
#  这两块恰好是最容易出静默错误、又最不需要真机验证的部分。
#    cmake -S . -B build -DSIDEBAND_BUILD_TESTS=ON
#    cmake --build build --target sideband_tests
#    ctest --test-dir build
# ============================================================
option(SIDEBAND_BUILD_TESTS "Build unit tests for platform-independent logic" OFF)

if(SIDEBAND_BUILD_TESTS)
    enable_testing()
    add_executable(sideband_tests tests/test_units.cpp)
    target_include_directories(sideband_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # 测试目标是控制台程序，不套用上面的 /SUBSYSTEM:WINDOWS
    add_test(NAME sideband_tests COMMAND sideband_tests)
endif()
