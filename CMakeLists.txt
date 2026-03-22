cmake_minimum_required(VERSION 3.12)


# Project
project(mrvnradiant VERSION 0.0)


# Options
option(BUILD_PLUGINS "Build plugins" ON)
option(BUILD_RADIANT "Build radiant" ON)
option(BUILD_TOOLS   "Build tools" ON)
option(INSTALL_DLLS  "Install DLLs" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Why are you trying to do this on linux
if(NOT WIN32)
    set(INSTALL_DLLS OFF)
endif()

# Install dlls is separate from the build process
if(INSTALL_DLLS)
    set(BUILD_PLUGINS OFF)
    set(BUILD_RADIANT OFF)
    set(BUILD_TOOLS   OFF)
endif()

# Github actions moment
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# Build system setup
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compile flags
set(CMAKE_CXX_FLAGS_RELEASE "-Os")
set(CMAKE_C_FLAGS_RELEASE "")

if(UNIX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    add_compile_definitions(RADIANT_EXECUTABLE="")
    add_compile_definitions(POSIX)
    add_compile_definitions(XWINDOWS)
endif()

if(WIN32)
    add_compile_definitions(RADIANT_EXECUTABLE=".exe")
    add_compile_definitions(WIN32)
    add_compile_definitions(_WIN32)
endif()

if(${MSVC})
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /MANIFEST:NO")
endif()

# Debug
if(CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_definitions(_DEBUG)
else()
    add_compile_definitions(NDEBUG)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/install)


# Radiant definitions
add_compile_definitions(RADIANT_VERSION="0.0")
add_compile_definitions(RADIANT_MAJOR_VERSION=0)
add_compile_definitions(RADIANT_MINOR_VERSION=0)
add_compile_definitions(RADIANT_ABOUTMSG="Dev build")

# Remap definitions
add_compile_definitions(Q3MAP_VERSION="Dev build")

# Compile defs
add_compile_definitions(QT_NO_KEYWORDS)

# Find dependencies
if (${MSVC}) # Visual studio
    set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/windeps/cmake)
    set(CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/windeps/qt)
    find_package(Glib2 REQUIRED)
    find_package(LibPng REQUIRED)
    find_package(LibXml2 REQUIRED)
    find_package(ZLib REQUIRED)
    find_package(LibJpeg REQUIRED)
    find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
    find_package(Threads REQUIRED)
else() # Msys / Linux
    include(FindPkgConfig)
    pkg_check_modules(GLIB glib-2.0 REQUIRED)
    pkg_check_modules(PNG libpng REQUIRED)
    pkg_check_modules(LIBXML2 libxml-2.0 REQUIRED)
    pkg_check_modules(ZLIB zlib REQUIRED)
    pkg_check_modules(JPEG libjpeg REQUIRED)

    find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
    find_package(Threads REQUIRED)
endif()

# ── Vulkan ────────────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

# ── VulkanMemoryAllocator (header-only, fetched automatically) ────────────────
include(FetchContent)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE
)
set(VMA_STATIC_VULKAN_FUNCTIONS  OFF CACHE BOOL "" FORCE)
set(VMA_DYNAMIC_VULKAN_FUNCTIONS ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# ── Build-time GLSL → SPIR-V compilation ─────────────────────────────────────
find_program(GLSLC_EXECUTABLE
    NAMES glslc
    HINTS "${Vulkan_GLSLC_EXECUTABLE}" ENV VULKAN_SDK
    PATH_SUFFIXES bin
)

if(NOT GLSLC_EXECUTABLE)
    message(WARNING "glslc not found — shaders won't be compiled automatically.  "
                    "Install the Vulkan SDK or set VULKAN_SDK.")
endif()

set(SHADER_SRC_DIR  "${CMAKE_SOURCE_DIR}/install/shaders")
set(SHADER_SPV_DIR  "${CMAKE_SOURCE_DIR}/install/shaders/spv")
file(MAKE_DIRECTORY "${SHADER_SPV_DIR}")

set(_SHADER_SOURCES
    "${SHADER_SRC_DIR}/lighting_dbs_omni.vert"
    "${SHADER_SRC_DIR}/lighting_dbs_omni.frag"
    "${SHADER_SRC_DIR}/zfill.vert"
    "${SHADER_SRC_DIR}/zfill.frag"
    "${SHADER_SRC_DIR}/skybox.vert"
    "${SHADER_SRC_DIR}/skybox.frag"
)

set(SHADER_SPIRV_OUTPUTS)
if(GLSLC_EXECUTABLE)
    foreach(_SHADER ${_SHADER_SOURCES})
        get_filename_component(_NAME "${_SHADER}" NAME)
        set(_SPV "${SHADER_SPV_DIR}/${_NAME}.spv")
        add_custom_command(
            OUTPUT  "${_SPV}"
            COMMAND "${GLSLC_EXECUTABLE}" "${_SHADER}" -o "${_SPV}"
            DEPENDS "${_SHADER}"
            COMMENT "Compiling SPIR-V: ${_NAME}"
            VERBATIM
        )
        list(APPEND SHADER_SPIRV_OUTPUTS "${_SPV}")
    endforeach()
    add_custom_target(radiant_shaders ALL DEPENDS ${SHADER_SPIRV_OUTPUTS})
endif()

# ── Global include dirs ────────────────────────────────────────────────────────
# VMA is FetchContent-populated — expose its include dir globally so ivk.h
# (in include/) can find <vk_mem_alloc.h> from any target.
FetchContent_GetProperties(VulkanMemoryAllocator SOURCE_DIR _VMA_SRC)
include_directories(include libs ${_VMA_SRC}/include ${CMAKE_SOURCE_DIR})


# Libs
if(BUILD_RADIANT OR BUILD_TOOLS OR BUILD_PLUGINS)
    add_subdirectory(libs)
endif()

# Contrib
if(BUILD_PLUGINS)
    add_subdirectory(contrib)
endif()

# Plugins && Radiant
if(BUILD_RADIANT)
    add_subdirectory(plugins)
    add_subdirectory(radiant)
endif()

# Tools
if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# https://stackoverflow.com/questions/11944060/how-to-detect-target-architecture-using-cmake
if(INSTALL_DLLS)
    execute_process( COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE ARCHITECTURE )
    execute_process( COMMAND sh ./install-dlls-msys2-mingw.sh ${ARCHITECTURE})
endif()
