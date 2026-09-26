cmake_minimum_required(VERSION 3.22.1)
project(ps1recomp_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../..)
set(RUNTIME ${ROOT}/ps1Runtime)
set(FMT ${ROOT}/third_party/fmt/include)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(ps1recomp_android SHARED
    main_android.cpp
    renderer_android.cpp
    ${RUNTIME}/src/gpu/gpu.cpp
    ${RUNTIME}/src/metrics.cpp
)

target_include_directories(ps1recomp_android PRIVATE
    ${RUNTIME}/include
    ${FMT}
    ${ANDROID_NDK}/sources/android/native_app_glue
)

target_compile_definitions(ps1recomp_android PRIVATE FMT_HEADER_ONLY=1)

target_compile_options(ps1recomp_android PRIVATE
    -Wall
    -Wextra
    -ffunction-sections
    -fdata-sections
)

target_link_options(ps1recomp_android PRIVATE -Wl,--gc-sections)

target_link_libraries(ps1recomp_android
    native_app_glue
    android
    log
    EGL
    GLESv2
)
