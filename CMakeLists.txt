# ============================================================================
#  EHsc · CMakeLists.txt
#  纯 C++ 命令行哈希工具（版本 2.0.0bate）—— 零第三方依赖，仅使用 Windows 原生 API。
#
#  ⚠ 安全提示：本程序中的 CRC32、MD5、SHA-1 仅可用于文件完整性/传输错误检查，
#     三者均已完全不适合任何安全用途（详见 README.md 第 2 节）。
#
#  快速开始（Visual Studio 2026 / 2022）：
#    1) 用 VS「打开文件夹」直接打开本目录（VS 内置 CMake 支持，会自动配置）
#    2) 或命令行：
#         cmake -S . -B build -G "Visual Studio 18 2026" -A x64
#         cmake --build build --config Release
#    3) 或直接双击 build.bat（自动查找 VS 环境并用 Ninja 构建）
# ============================================================================
cmake_minimum_required(VERSION 3.16)

# 版本号：CMake 的 VERSION 字段只接受纯数字，完整版本串单独定义并传给编译器
set(EHSC_VERSION_STRING "2.1.1")

project(EHsc
        VERSION 2.1.1
        DESCRIPTION "纯 C++ 文件哈希 / 校验 / 比较命令行工具（无第三方依赖）"
        LANGUAGES CXX)

# --------------------------------------------------------------- 语言标准 --
# 默认 C++20；若工具链不支持，可 -DEHSC_CXX_STANDARD=17 回退。
set(EHSC_CXX_STANDARD "20" CACHE STRING "C++ 标准版本 (17 或 20)")
set(CMAKE_CXX_STANDARD ${EHSC_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------------------------- 独立可执行文件（/MT） --
# 静态链接 C/C++ 运行库：生成的 EHsc.exe 不依赖 MSVCP140.dll / VCRUNTIME140.dll
# 等 VC++ 运行库，只依赖 Windows 自带系统 DLL，可单独拷到任意机器直接运行。
# （若想改回 /MD，需要目标机器安装 VC++ 可再发行组件包。）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
    "MSVC 运行库：静态(/MT) 或动态(/MD)" FORCE)

if(NOT WIN32)
    message(FATAL_ERROR "EHsc 依赖 Windows 原生 API（文件对话框 / 控制台 / CreateFileW），仅支持 Windows。")
endif()

# ------------------------------------------------------------------ 目标 --
add_executable(EHsc
    src/main.cpp
    src/ui.cpp
    src/core.cpp
    src/report.cpp
    src/hashes.cpp
    src/watch.cpp
    src/win32_utils.cpp
)

target_include_directories(EHsc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 控制台程序 + 启用 UTF-8 源文件与执行字符集 + 多线程静态运行库
if(MSVC)
    target_compile_definitions(EHsc PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        EHSC_VERSION="${EHSC_VERSION_STRING}"
    )
    target_compile_options(EHsc PRIVATE
        /utf-8                 # 源码与执行字符集均为 UTF-8（中文字面量必需）
        /W4                    # 高警告等级
        /permissive-           # 严格标准一致性
        /Zc:__cplusplus
        /MP                    # 多进程编译
        /EHsc                  # 标准 C++ 异常模型（与本项目同名，纯属巧合）
        $<$<CONFIG:Release>:/O2 /Oi /GL>
        $<$<CONFIG:Release>:/DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:/O2>
    )
    target_link_options(EHsc PRIVATE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )
else()
    target_compile_options(EHsc PRIVATE -Wall -Wextra -O2)
endif()

# 需要链接的 Windows 系统库（均为系统自带，不是第三方依赖）
target_link_libraries(EHsc PRIVATE
    ole32        # COM：IFileOpenDialog / IFileSaveDialog
    shell32      # CommandLineToArgvW / Shell 相关
    comdlg32     # 通用对话框过滤器结构
    user32       # 控制台窗口 / FillConsoleOutput 等
    advapi32     # 系统信息
)

set_target_properties(EHsc PROPERTIES
    OUTPUT_NAME "EHsc"
    WIN32_EXECUTABLE OFF          # 保留控制台窗口
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ------------------------------------------------------- UTF-8 编译期检查 --
# 若忘记 /utf-8（MSVC），hashes.h 中的 static_assert 会直接报错提示。

# ------------------------------------------------------------- 安装/说明 --
install(TARGETS EHsc RUNTIME DESTINATION bin)

message(STATUS "EHsc ${EHSC_VERSION_STRING}: C++${EHSC_CXX_STANDARD}, 编译器 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
