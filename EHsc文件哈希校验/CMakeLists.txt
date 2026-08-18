cmake_minimum_required(VERSION 3.16)

project(OfflineFileHash LANGUAGES CXX)

add_executable(OfflineFileHash
    main.cpp
    hashes.cpp
    hashes.hpp
)

target_compile_features(OfflineFileHash PRIVATE cxx_std_17)

target_compile_definitions(OfflineFileHash PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(OfflineFileHash PRIVATE comdlg32)

if(MSVC)
    target_compile_options(OfflineFileHash PRIVATE
        /W4
        /O2
        /EHsc
        /utf-8
    )
else()
    target_compile_options(OfflineFileHash PRIVATE
        -Wall
        -Wextra
        -O3
    )
endif()
















