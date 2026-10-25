set(WF_GENERATED_DIR "${PROJECT_SOURCE_DIR}/generated" CACHE PATH "recompiled sources (python tools/recomp/recomp.py)")
if(EXISTS "${WF_GENERATED_DIR}/sources.cmake")
  include("${WF_GENERATED_DIR}/sources.cmake")
else()
  message(STATUS "generated/ not found: building windfall.exe without the game module")
endif()

include(FetchContent)
FetchContent_Declare(miniz
  URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5)
FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/releases/download/11.2.0/fmt-11.2.0.zip
  URL_HASH SHA256=203eb4e8aa0d746c62d8f903df58e0419e3751591bb53ff971096eaa0ebd4ec3)
FetchContent_Declare(vma
  URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
  URL_HASH SHA256=c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e
  SOURCE_SUBDIR none)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
  URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
FetchContent_Declare(zstd
  URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
  URL_HASH SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
  SOURCE_SUBDIR none)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/f65bcf481ab34cd07d3909aab1479f409fa79f2f.tar.gz
  URL_HASH SHA256=fc2f2f72e18a680b9ba141194ff5b35c940a11e58784d5d22aac7c8d7224592b
  SOURCE_SUBDIR none)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(miniz fmt SDL3)
FetchContent_MakeAvailable(vma zstd imgui)
add_library(miniz_lib STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(miniz_lib PUBLIC "${miniz_SOURCE_DIR}")
target_compile_options(miniz_lib PRIVATE /w)

file(GLOB ZSTD_SOURCES "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c" "${zstd_SOURCE_DIR}/lib/decompress/*.c")
add_library(zstd_lib STATIC ${ZSTD_SOURCES})
target_include_directories(zstd_lib PUBLIC "${zstd_SOURCE_DIR}/lib")
target_compile_definitions(zstd_lib PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)
target_compile_options(zstd_lib PRIVATE /w /O2)

add_library(imgui_lib STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp" "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp" "${imgui_SOURCE_DIR}/imgui_demo.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(imgui_lib PUBLIC "${imgui_SOURCE_DIR}")
target_compile_options(imgui_lib PRIVATE /w)

find_package(Vulkan REQUIRED)
get_filename_component(VULKAN_SDK_LIB_DIR "${Vulkan_LIBRARY}" DIRECTORY)
set(WF_GLSLANG_LIBS
  "${VULKAN_SDK_LIB_DIR}/glslang.lib" "${VULKAN_SDK_LIB_DIR}/SPIRV.lib"
  "${VULKAN_SDK_LIB_DIR}/glslang-default-resource-limits.lib"
  "${VULKAN_SDK_LIB_DIR}/SPIRV-Tools-opt.lib" "${VULKAN_SDK_LIB_DIR}/SPIRV-Tools.lib")

if(WF_GENERATED_SOURCES)
  add_library(windfall_game SHARED ${WF_GENERATED_SOURCES})
  target_include_directories(windfall_game PRIVATE include)
  target_compile_definitions(windfall_game PRIVATE WF_FREESTANDING=1)
  target_compile_options(windfall_game PRIVATE /GS- /Zl /GR- /EHs-c- /clang:-O2 /clang:-ffreestanding /clang:-ffp-contract=off
    /clang:-mfma /clang:-fno-strict-aliasing /clang:-mno-stack-arg-probe /bigobj /w)
  set_target_properties(windfall_game PROPERTIES MSVC_RUNTIME_LIBRARY "" LINK_FLAGS "/NOENTRY /NODEFAULTLIB")
endif()

# Code vendored from Cemu (MPL-2.0, src/gpu/cemu) with Windfall shims (src/gpu/cemu_shim).
set(CEMU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/cemu")
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS "${CEMU_DIR}/*.cpp")
list(FILTER CEMU_SOURCES EXCLUDE REGEX "GX2_Event\.cpp$|GX2_Memory\.cpp$|GX2_TilingAperture\.cpp$")
add_library(cemu_vendored STATIC ${CEMU_SOURCES})
target_include_directories(cemu_vendored PUBLIC src/gpu/cemu_shim src/gpu/cemu_shim/Cafe src/gpu/cemu src/gpu/cemu/Cafe src include)
target_link_libraries(cemu_vendored PUBLIC fmt::fmt-header-only imgui_lib zstd_lib miniz_lib Vulkan::Headers ${WF_GLSLANG_LIBS})
target_include_directories(cemu_vendored PRIVATE "${vma_SOURCE_DIR}/include")
target_compile_options(cemu_vendored PRIVATE "/FICommon/precompiled.h" /w /clang:-fno-strict-aliasing)
target_compile_definitions(cemu_vendored PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS VK_NO_PROTOTYPES ENABLE_VULKAN)
target_compile_options(cemu_vendored PRIVATE /clang:-msse4.1)
target_compile_definitions(cemu_vendored PUBLIC EMULATOR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  EMULATOR_VERSION_MINOR=${PROJECT_VERSION_MINOR} EMULATOR_VERSION_PATCH=${PROJECT_VERSION_PATCH})

add_executable(windfall
  src/main.cpp
  src/core/log.cpp
  src/core/memory.cpp
  src/core/rpx_loader.cpp
  src/cpu/recomp_support.cpp
  src/cpu/interp.cpp
  src/debug/watchdog.cpp
  src/debug/profiler.cpp
  src/debug/explorer.cpp
  src/os/sched.cpp
  src/hle/hle.cpp
  src/hle/guest_printf.cpp
  src/hle/cemu_bridge.cpp
  src/hle/gx2_windfall.cpp
  src/hle/fs.cpp
  src/hle/erreula.cpp
  src/hle/ax_windfall.cpp
  src/audio/sdl_audio.cpp
  src/hle/pad.cpp
  src/hle/swkbd.cpp
  src/input/input.cpp
  src/enhancements/fps60.cpp
  src/hle/stubs.cpp
  src/hle/coreinit/heap.cpp
  src/hle/coreinit/misc.cpp
  src/hle/coreinit/sync.cpp
  src/hle/coreinit/thread.cpp
  src/hle/coreinit/time.cpp
  src/gpu/window.cpp
  src/gpu/gpu.cpp
  src/gpu/cemu_glue.cpp
  src/gpu/gpu_dump.cpp
  src/core/cemu_install.cpp
  src/core/setup.cpp
  src/core/game_module.cpp
  src/core/game_build.cpp
  src/core/setup_window.cpp
  src/core/archive.cpp
  src/mods/mods.cpp
  src/enhancements/graphic_packs.cpp
  src/enhancements/cheats.cpp
  src/enhancements/playtest.cpp
  src/net/multiplayer.cpp
  src/net/discord_rpc.cpp
  src/core/places.cpp
  src/net/remote_render.cpp
  src/enhancements/randomizer.cpp
  src/ui/settings.cpp
  src/ui/game_art.cpp
  src/ui/menu.cpp
  src/ui/sfx.cpp
  res/windfall.rc
)
target_include_directories(windfall PRIVATE src)
if(TARGET windfall_game)
  add_dependencies(windfall windfall_game)  # building the exe also builds the developer's game module
endif()
target_link_libraries(windfall PRIVATE cemu_vendored miniz_lib SDL3::SDL3-static
  bcrypt shell32 ole32 winmm ntdll ws2_32 winhttp dbghelp d3d11 dxgi d3dcompiler dwmapi)
target_compile_definitions(windfall PRIVATE
  WINDFALL_VERSION="${PROJECT_VERSION}"
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
)
target_compile_options(windfall PRIVATE /W4 /clang:-mfma /clang:-fno-strict-aliasing
  -Wno-unused-parameter -Wno-deprecated-declarations -Wno-missing-field-initializers -Wno-unused-variable
  -Wno-unused-function -Wno-nullability-completeness)

if(EXISTS "${WF_GENERATED_DIR}/cputest.cpp")
  add_executable(windfall_cputest "${WF_GENERATED_DIR}/cputest.cpp")
  target_include_directories(windfall_cputest PRIVATE include)
  target_compile_options(windfall_cputest PRIVATE /clang:-O2 /clang:-ffp-contract=off /clang:-mfma /w)
endif()
