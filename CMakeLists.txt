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

# 构建版本标识：编译时取当前 git 提交短哈希，写入日志方便区分新旧构建
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE SIDEBAND_BUILD_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT SIDEBAND_BUILD_COMMIT)
    set(SIDEBAND_BUILD_COMMIT "unknown")
endif()
add_compile_definitions(SIDEBAND_BUILD_COMMIT="${SIDEBAND_BUILD_COMMIT}")

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
    uiautomationcore  # UIA（阶段二：自绘 caret 应用兜底取插入符位置）
)

# 嵌入 Manifest (DPI Awareness)
if(MSVC)
    target_link_options(moonlight_sideband PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/moonlight_sideband.manifest"
    )
    # /utf-8 必须加：本仓库所有源文件都是 UTF-8 无 BOM，而 MSVC 默认按系统 ANSI
    # 代码页解释源文件。在英文系统上（GitHub Actions runner 就是，ACP=1252）
    # L"打开日志" 这类宽字符字面量会被逐字节从 CP1252 转成 UTF-16，托盘菜单显示成
    # "æ‰"å¼€æ—¥å¿—"。窄字符串之所以看起来没事，是因为源代码页与执行代码页相同、
    # 字节原样透传，只有宽字面量需要真实转码才暴露出来。
    # /utf-8 等价于 /source-charset:utf-8 /execution-charset:utf-8。
    #
    # /EHa：显示驱动（虚拟显示器）可能在 CCD 查询时写穿我们传入的栈缓冲区，
    # 引发访问违规（0xC0000005）。/EHa 让 catch(...) 也能接住 SEH 异常，
    # MonitorLoop / 服务器主循环 / 各工作线程的 catch(...) 就能把这类驱动
    # 异常降级为"记录日志 + 本轮重试"，而不是整个进程崩溃。
    target_compile_options(moonlight_sideband PRIVATE /W4 /utf-8 /EHa)
endif()

# Windows 子系统 (无控制台黑框)。
# 注意：因此进程没有控制台 —— 不要指望 std::cout 或 SIGINT，
# 退出请走托盘菜单（见 TrayIcon）。
set_target_properties(moonlight_sideband PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
)

# Unicode 支持 + 目标 Windows 版本。
#
# 0x0A00 = Windows 10。各项下限：
#   WSAPoll / SIO_KEEPALIVE_VALS   需要 0x0600 (Vista)
#   GetDpiForMonitor               需要 0x0603 (8.1) —— 低于此值 shellscalingapi.h
#                                  不声明它，CursorModule 会报 C3861
#   manifest 里声明的 PerMonitorV2 需要 Windows 10 1703+
# 取最高的那个，与 manifest 保持一致。
add_compile_definitions(
    UNICODE _UNICODE
    NOMINMAX WIN32_LEAN_AND_MEAN
    _WINSOCK_DEPRECATED_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000000
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

# ============================================================
#  文本插入符探针（可选测试客户端）
#
#  连上服务端后解析并打印每个文本光标包的 Y%/高度/来源/包间隔，
#  用于真机验证 caret 追踪（Win32 + UIA 两级路径）。
#    cmake -S . -B build -DSIDEBAND_BUILD_CARET_PROBE=ON
#    cmake --build build --config Release --target caret_probe
# ============================================================
option(SIDEBAND_BUILD_CARET_PROBE "Build the caret text-cursor probe test client" OFF)

if(SIDEBAND_BUILD_CARET_PROBE)
    add_executable(caret_probe tools/caret_probe.cpp)
    target_link_libraries(caret_probe PRIVATE ws2_32)
    if(MSVC)
        target_compile_options(caret_probe PRIVATE /utf-8 /W4)
    endif()
endif()

if(SIDEBAND_BUILD_TESTS)
    enable_testing()
    add_executable(sideband_tests tests/test_units.cpp)
    target_include_directories(sideband_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(sideband_tests PRIVATE /utf-8)
    endif()
    # 测试目标是控制台程序，不套用上面的 /SUBSYSTEM:WINDOWS
    add_test(NAME sideband_tests COMMAND sideband_tests)
endif()
