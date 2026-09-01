cmake_minimum_required(VERSION 3.20)
project(WaveSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v2.9.4
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(tinygltf imgui nlohmann_json)

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/App.cpp
    src/SimBridge.cpp
    src/Renderer.cpp
    src/util/InputManager.cpp
    src/util/OrbitCamera.cpp
    src/util/FlyCamera.cpp
    src/gfx/DescriptorHeap.cpp
    src/gfx/GpuProfiler.cpp
    src/gfx/LinearAllocator.cpp
    src/gfx/CommandContext.cpp
    src/gfx/ComputeContext.cpp
    src/gfx/GraphicsContext.cpp
    src/gfx/ColorBuffer.cpp
    src/gfx/DepthBuffer.cpp
    src/gfx/ShadowMap.cpp
    src/gfx/PixelBuffer.cpp
    src/gfx/GpuBuffer.cpp
    src/assets/Texture.cpp
    src/assets/Material.cpp
    src/assets/Mesh.cpp
    src/assets/ObjLoader.cpp
    src/scene/Scene.cpp
    src/assets/ResourceManager.cpp
    src/water/FFTWaterSurface.cpp
    src/gfx/Atmosphere.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${tinygltf_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_compile_options(${PROJECT_NAME} PRIVATE /GR-)

# Prevent <windows.h> from defining min()/max() macros that collide with std::min/std::max.
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)

# D3D12 GPU-based validation. Off by default because it costs ~11 ms a frame in this scene, which
# is most of a Debug frame budget; the ordinary debug layer is unaffected and stays on in Debug.
option(GPU_BASED_VALIDATION "Enable D3D12 GPU-based validation in Debug builds (very slow)" OFF)
if(GPU_BASED_VALIDATION)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_GPU_BASED_VALIDATION)
endif()

target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d12
    dxgi
    nlohmann_json::nlohmann_json
)

# The sim lives in this repo as a git submodule; override the cache entry only to build against
# a checkout somewhere else.
set(SAILBOAT_SIM_DIR "${CMAKE_SOURCE_DIR}/external/sailboat-sim"
    CACHE PATH "Path to the sailboat-sim repo (provides the native sim client library)")

# An uninitialised submodule is an empty directory, which would otherwise surface much later as a
# confusing "project file does not exist" from dotnet partway through the build.
if(NOT EXISTS "${SAILBOAT_SIM_DIR}/src/Sim.Core/Sim.Core.csproj")
    message(FATAL_ERROR
        "sailboat-sim not found at ${SAILBOAT_SIM_DIR} - the sim is a git submodule. "
        "Initialise it with: git submodule update --init --recursive")
endif()

# ------------------------------------------------------------------------------------------------
# The sim's native client library: Sim.Net.Client publishes under NativeAOT to one shared library
# exporting the sim_view_* / sim_client_* C ABI. SimClient.h is the authoritative contract; see
# external/sailboat-sim/docs/engine-integration.md.
# ------------------------------------------------------------------------------------------------
set(SIM_NATIVE_RID "win-x64" CACHE STRING
    "Runtime identifier to publish the sim's native client library for")

# The sim is published Release even for a Debug renderer, and that is a deliberate 14x: a Debug
# NativeAOT build steps in 3.46 ms against 0.24 ms optimised (measured), and at 400 Hz the frame
# loop cannot afford it - it burns the whole per-frame step budget and the boat sails in slow
# motion while the display sits at ~10 fps. You debug this renderer, not the sim, so the dependency
# stays optimised. Turn it on only to actually step through sim code.
option(SIM_DEBUG_RUNTIME
    "Publish the sim native library in Debug too (~14x slower to step; for debugging sim code)" OFF)
if(SIM_DEBUG_RUNTIME)
    # dotnet knows only Debug and Release, so the extra configs of a multi-config generator get Release.
    set(SIM_DOTNET_CONFIG "$<IF:$<CONFIG:Debug>,Debug,Release>")
    set(SIM_LIB_CONFIG_Debug   Debug)
else()
    set(SIM_DOTNET_CONFIG "Release")
    set(SIM_LIB_CONFIG_Debug   Release)
endif()
set(SIM_LIB_CONFIG_Release Release)

set(SIM_CLIENT_PROJECT "${SAILBOAT_SIM_DIR}/src/Sim.Net.Client")

# PublishAot is what emits the native library, so this is `publish` and not `build`. It is
# deliberately not given -o: `publish -o <dir>` copies the .dll out but leaves the import .lib
# behind, and MSVC needs the .lib to link. Both sit together in the intermediate native/ directory
# that IMPORTED_IMPLIB points at below.
# NativeAOT links through findvcvarsall.bat, which calls vcvarsall.bat with its STDOUT sent to NUL
# but its stderr left alone. vcvarsall invokes a bare "vswhere.exe", so when the VS Installer
# directory is not on PATH - which it is not in a plain shell, only in a Developer Command Prompt -
# that lookup fails and prints to stderr. MSBuild captures the Exec with ConsoleToMSBuild="true",
# splits the result on "#", and uses element [0] as the linker directory, so the error TEXT ends up
# spliced into CppLinker and the link dies with a nonsense command. Putting vswhere on PATH removes
# the noise at its source. The failure is invisible until something forces a fresh native link,
# because dotnet skips the step while its output is current.
find_program(VSWHERE_EXECUTABLE vswhere
    HINTS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
          "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer")

# The directory is handed to a .cmd wrapper rather than set here. A Windows PATH is semicolon-
# separated and CMake treats semicolons as list separators, so a "PATH=..." argument gets torn into
# one argument per entry; the wrapper also prepends to the live %PATH% instead of replacing it,
# which keeps the MSVC environment MSBuild gives a custom build step.
if(VSWHERE_EXECUTABLE)
    get_filename_component(VSWHERE_DIR "${VSWHERE_EXECUTABLE}" DIRECTORY)
    file(TO_NATIVE_PATH "${VSWHERE_DIR}" SIM_VSWHERE_DIR)
    message(STATUS "Using vswhere from: ${VSWHERE_DIR}")
else()
    # Not fatal: a Developer Command Prompt already has both vswhere and the toolchain on PATH.
    set(SIM_VSWHERE_DIR "")
    message(WARNING
        "vswhere.exe not found. If the NativeAOT link fails with a mangled linker path, build from "
        "a Developer Command Prompt or install the Visual Studio Installer.")
endif()

add_custom_target(sim_native ALL
    COMMAND "${CMAKE_SOURCE_DIR}/tools/publish-sim-native.cmd"
            "${SIM_VSWHERE_DIR}" "${SIM_CLIENT_PROJECT}" ${SIM_DOTNET_CONFIG} ${SIM_NATIVE_RID}
    COMMENT "Publishing Sim.Net.Client (NativeAOT sim client library)"
    VERBATIM
)
add_dependencies(${PROJECT_NAME} sim_native)

# An imported target rather than a bare path. Handed the full path to a library whose name it does
# not recognise, target_link_libraries silently decomposes it back into -L<dir> -l<name> and the
# linker then hunts for a file that was never going to exist - a trap the sim's integration guide
# calls out, because the error reads as a missing library while the file sits right there.
add_library(sim_net_client SHARED IMPORTED)
foreach(SIM_CFG Debug Release)
    string(TOUPPER ${SIM_CFG} SIM_CFG_UPPER)
    # Not ${SIM_CFG}: a Debug renderer links the Release library unless SIM_DEBUG_RUNTIME is on.
    set(SIM_NATIVE_DIR
        "${SIM_CLIENT_PROJECT}/bin/${SIM_LIB_CONFIG_${SIM_CFG}}/net9.0/${SIM_NATIVE_RID}/native")
    set_target_properties(sim_net_client PROPERTIES
        IMPORTED_IMPLIB_${SIM_CFG_UPPER}   "${SIM_NATIVE_DIR}/Sim.Net.Client.lib"
        IMPORTED_LOCATION_${SIM_CFG_UPPER} "${SIM_NATIVE_DIR}/Sim.Net.Client.dll")
endforeach()
set_target_properties(sim_net_client PROPERTIES
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    MAP_IMPORTED_CONFIG_MINSIZEREL     Release)

# SimClient.h includes SimProtocol.h by name and the two headers live beside their own C# sources
# rather than in a shared include tree, so both directories go on the include path.
target_include_directories(${PROJECT_NAME} PRIVATE
    "${SIM_CLIENT_PROJECT}"
    "${SAILBOAT_SIM_DIR}/src/Sim.Net.Protocol"
)
target_link_libraries(${PROJECT_NAME} PRIVATE sim_net_client)

# Find dxc shader compiler
find_program(DXC_EXECUTABLE dxc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
)
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "dxc.exe not found. Install the Windows SDK or Vulkan SDK.")
endif()
message(STATUS "Found dxc: ${DXC_EXECUTABLE}")

option(SHADER_DEBUG_SYMBOLS "Compile shaders with debug symbols (-Zi -Qembed_debug -Od)" OFF)
if(SHADER_DEBUG_SYMBOLS)
    set(DXC_SHADER_FLAGS -Zi -Qembed_debug -Od)
    message(STATUS "Shader debug symbols: ON")
else()
    set(DXC_SHADER_FLAGS)
endif()

# Shader compilation - output to a fixed build directory
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

set(COMMON_HLSLI "${CMAKE_SOURCE_DIR}/shaders/Common.hlsli")
set(SHADOW_HLSLI "${CMAKE_SOURCE_DIR}/shaders/Shadow.hlsli")

set(SHADER_SOURCES
    "${CMAKE_SOURCE_DIR}/shaders/triangle.hlsl"
    "${CMAKE_SOURCE_DIR}/shaders/phong.hlsl"
    "${CMAKE_SOURCE_DIR}/shaders/pbr.hlsl"
)

set(SHADER_OUTPUTS)
foreach(SHADER_FILE ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)

    set(VS_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}_vs.cso")
    set(PS_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}_ps.cso")
    add_custom_command(
        OUTPUT ${VS_OUTPUT}
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain ${DXC_SHADER_FLAGS} -Fo ${VS_OUTPUT} ${SHADER_FILE}
        DEPENDS ${SHADER_FILE} ${COMMON_HLSLI} ${SHADOW_HLSLI}
        COMMENT "Compiling VS: ${SHADER_NAME}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${PS_OUTPUT}
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain ${DXC_SHADER_FLAGS} -Fo ${PS_OUTPUT} ${SHADER_FILE}
        DEPENDS ${SHADER_FILE} ${COMMON_HLSLI} ${SHADOW_HLSLI}
        COMMENT "Compiling PS: ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND SHADER_OUTPUTS ${VS_OUTPUT} ${PS_OUTPUT})
endforeach()

# Root signature blobs — compiled with rootsig_1_1 profile; loaded directly by CreateRootSignature.
set(RS_SOURCES
    "PbrRootSig.hlsl|pbr_rs"
    "FFT/FFTRootSig.hlsl|fft_compute_rs"
    "FFT/DownsampleRootSig.hlsl|downsample_rs"
    "Water/WaterRenderRootSig.hlsl|water_render_rs"
)
foreach(RS_ENTRY ${RS_SOURCES})
    string(REPLACE "|" ";" RS_PARTS "${RS_ENTRY}")
    list(GET RS_PARTS 0 RS_FILE_REL)
    list(GET RS_PARTS 1 RS_NAME)
    set(RS_FILE "${CMAKE_SOURCE_DIR}/shaders/${RS_FILE_REL}")
    set(RS_OUTPUT "${SHADER_OUTPUT_DIR}/${RS_NAME}.cso")
    add_custom_command(
        OUTPUT ${RS_OUTPUT}
        COMMAND ${DXC_EXECUTABLE} -T rootsig_1_1 -E ROOTSIG -Fo ${RS_OUTPUT} ${RS_FILE}
        DEPENDS ${RS_FILE} ${COMMON_HLSLI}
                "${CMAKE_SOURCE_DIR}/shaders/Water/WaterCommon.hlsli"
                "${CMAKE_SOURCE_DIR}/shaders/FFT/FFTCommon.hlsli"
        COMMENT "Compiling RS: ${RS_NAME}"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${RS_OUTPUT})
endforeach()

# FFT compute shaders: name|entry_point pairs
set(FFT_COMMON_INC "${CMAKE_SOURCE_DIR}/shaders/FFT/FFTCommon.hlsli")
set(COMPUTE_SHADERS
    "FFT/PhillipsSpectrum.hlsl|phillips_spectrum_cs|main"
    "FFT/DynamicSpectrum.hlsl|dynamic_spectrum_cs|main"
    "FFT/Precompute.hlsl|precompute_cs|main"
    "FFT/IFFTHorz.hlsl|ifft_horz_cs|main"
    "FFT/IFFTVert.hlsl|ifft_vert_cs|main"
    "FFT/Permute.hlsl|permute_cs|main"
    "FFT/Downsample.hlsl|downsample_cs|main"
    "FFT/WaterViz.hlsl|water_viz_cs|main"
    "utils/CopyTexture2D.hlsl|copy_texture_cs|main"
)

foreach(CS_ENTRY ${COMPUTE_SHADERS})
    string(REPLACE "|" ";" CS_PARTS "${CS_ENTRY}")
    list(GET CS_PARTS 0 CS_REL)
    list(GET CS_PARTS 1 CS_NAME)
    list(GET CS_PARTS 2 CS_EP)
    set(CS_FILE "${CMAKE_SOURCE_DIR}/shaders/${CS_REL}")
    set(CS_OUTPUT "${SHADER_OUTPUT_DIR}/${CS_NAME}.cso")
    add_custom_command(
        OUTPUT ${CS_OUTPUT}
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E ${CS_EP} ${DXC_SHADER_FLAGS} -Fo ${CS_OUTPUT} ${CS_FILE}
        DEPENDS ${CS_FILE} ${FFT_COMMON_INC}
        COMMENT "Compiling CS: ${CS_NAME}"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${CS_OUTPUT})
endforeach()

# Shadow depth-only caster shader (VS only)
set(SHADOW_VS_OUTPUT "${SHADER_OUTPUT_DIR}/ShadowDepth_vs.cso")
add_custom_command(
    OUTPUT ${SHADOW_VS_OUTPUT}
    COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain ${DXC_SHADER_FLAGS} -Fo ${SHADOW_VS_OUTPUT}
            "${CMAKE_SOURCE_DIR}/shaders/ShadowDepth.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/ShadowDepth.hlsl"
    COMMENT "Compiling VS: ShadowDepth"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${SHADOW_VS_OUTPUT})

# Water surface vertex and pixel shaders
set(WATER_COMMON_INC "${CMAKE_SOURCE_DIR}/shaders/Water/WaterCommon.hlsli")

set(WATER_VS_OUTPUT "${SHADER_OUTPUT_DIR}/water_surface_vs.cso")
add_custom_command(
    OUTPUT ${WATER_VS_OUTPUT}
    COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain ${DXC_SHADER_FLAGS} -Fo ${WATER_VS_OUTPUT}
            "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceVS.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceVS.hlsl" ${WATER_COMMON_INC}
    COMMENT "Compiling VS: water_surface"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${WATER_VS_OUTPUT})

set(WATER_VS_FULL_OUTPUT "${SHADER_OUTPUT_DIR}/water_surface_vs_full.cso")
add_custom_command(
    OUTPUT ${WATER_VS_FULL_OUTPUT}
    COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain ${DXC_SHADER_FLAGS} -Fo ${WATER_VS_FULL_OUTPUT}
            "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceVS_Full.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceVS_Full.hlsl" ${WATER_COMMON_INC}
    COMMENT "Compiling VS: water_surface_vs_full"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${WATER_VS_FULL_OUTPUT})

set(WATER_HS_OUTPUT "${SHADER_OUTPUT_DIR}/water_surface_hs.cso")
add_custom_command(
    OUTPUT ${WATER_HS_OUTPUT}
    COMMAND ${DXC_EXECUTABLE} -T hs_6_0 -E HSMain ${DXC_SHADER_FLAGS} -Fo ${WATER_HS_OUTPUT}
            "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceHS.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceHS.hlsl" ${WATER_COMMON_INC}
    COMMENT "Compiling HS: water_surface"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${WATER_HS_OUTPUT})

set(WATER_DS_OUTPUT "${SHADER_OUTPUT_DIR}/water_surface_ds.cso")
add_custom_command(
    OUTPUT ${WATER_DS_OUTPUT}
    COMMAND ${DXC_EXECUTABLE} -T ds_6_0 -E DSMain ${DXC_SHADER_FLAGS} -Fo ${WATER_DS_OUTPUT}
            "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceDS.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfaceDS.hlsl" ${WATER_COMMON_INC}
    COMMENT "Compiling DS: water_surface"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${WATER_DS_OUTPUT})

set(WATER_PS_OUTPUT "${SHADER_OUTPUT_DIR}/water_surface_ps.cso")
add_custom_command(
    OUTPUT ${WATER_PS_OUTPUT}
    COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain ${DXC_SHADER_FLAGS} -Fo ${WATER_PS_OUTPUT}
            "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfacePS.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/Water/WaterSurfacePS.hlsl" ${WATER_COMMON_INC}
    COMMENT "Compiling PS: water_surface"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${WATER_PS_OUTPUT})

# Atmosphere shaders
set(ATM_COMMON_INC "${CMAKE_SOURCE_DIR}/shaders/Atmosphere/AtmosphereCommon.hlsli")

set(ATM_SHADERS
    "Atmosphere/AtmosphereComputeRootSig.hlsl|atm_compute_rs|rootsig_1_1|ROOTSIG"
    "Atmosphere/AtmosphereRenderRootSig.hlsl|atm_render_rs|rootsig_1_1|ROOTSIG"
    "Atmosphere/AtmEnvCubemapRootSig.hlsl|atm_envcube_rs|rootsig_1_1|ROOTSIG"
)
foreach(RS_ENTRY ${ATM_SHADERS})
    string(REPLACE "|" ";" RS_PARTS "${RS_ENTRY}")
    list(GET RS_PARTS 0 RS_REL)
    list(GET RS_PARTS 1 RS_NAME)
    list(GET RS_PARTS 2 RS_PROFILE)
    list(GET RS_PARTS 3 RS_EP)
    set(RS_FILE "${CMAKE_SOURCE_DIR}/shaders/${RS_REL}")
    set(RS_OUT  "${SHADER_OUTPUT_DIR}/${RS_NAME}.cso")
    add_custom_command(
        OUTPUT ${RS_OUT}
        COMMAND ${DXC_EXECUTABLE} -T ${RS_PROFILE} -E ${RS_EP} ${DXC_SHADER_FLAGS} -Fo ${RS_OUT} ${RS_FILE}
        DEPENDS ${RS_FILE} ${ATM_COMMON_INC}
        COMMENT "Compiling RS: ${RS_NAME}"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${RS_OUT})
endforeach()

set(ATM_CS_SHADERS
    "Atmosphere/TransmittanceLUT.hlsl|atm_transmittance_cs|main"
    "Atmosphere/SkyViewLUT.hlsl|atm_skyview_cs|main"
    "Atmosphere/AtmEnvCubemapCS.hlsl|atm_envcube_cs|main"
)
foreach(CS_ENTRY ${ATM_CS_SHADERS})
    string(REPLACE "|" ";" CS_PARTS "${CS_ENTRY}")
    list(GET CS_PARTS 0 CS_REL)
    list(GET CS_PARTS 1 CS_NAME)
    list(GET CS_PARTS 2 CS_EP)
    set(CS_FILE "${CMAKE_SOURCE_DIR}/shaders/${CS_REL}")
    set(CS_OUT  "${SHADER_OUTPUT_DIR}/${CS_NAME}.cso")
    add_custom_command(
        OUTPUT ${CS_OUT}
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E ${CS_EP} ${DXC_SHADER_FLAGS} -Fo ${CS_OUT} ${CS_FILE}
        DEPENDS ${CS_FILE} ${ATM_COMMON_INC}
        COMMENT "Compiling CS: ${CS_NAME}"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${CS_OUT})
endforeach()

set(ATM_VS_OUT "${SHADER_OUTPUT_DIR}/atm_sky_render_vs.cso")
add_custom_command(
    OUTPUT ${ATM_VS_OUT}
    COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain ${DXC_SHADER_FLAGS} -Fo ${ATM_VS_OUT}
            "${CMAKE_SOURCE_DIR}/shaders/Atmosphere/SkyRenderVS.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/Atmosphere/SkyRenderVS.hlsl" ${ATM_COMMON_INC}
    COMMENT "Compiling VS: atm_sky_render"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${ATM_VS_OUT})

set(ATM_PS_OUT "${SHADER_OUTPUT_DIR}/atm_sky_render_ps.cso")
add_custom_command(
    OUTPUT ${ATM_PS_OUT}
    COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain ${DXC_SHADER_FLAGS} -Fo ${ATM_PS_OUT}
            "${CMAKE_SOURCE_DIR}/shaders/Atmosphere/SkyRenderPS.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/Atmosphere/SkyRenderPS.hlsl" ${ATM_COMMON_INC}
    COMMENT "Compiling PS: atm_sky_render"
    VERBATIM
)
list(APPEND SHADER_OUTPUTS ${ATM_PS_OUT})

add_custom_target(shaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(${PROJECT_NAME} shaders)

# Copy compiled shaders and model files next to the executable after build
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_OUTPUT_DIR}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/models
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/models
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/scenes
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/scenes
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/textures
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/textures
    COMMENT "Copying shaders, models, scenes, and textures to output directory"
)

# The sim's native client library next to the exe. NativeAOT output is self-contained, so this is
# the only runtime file the sim needs.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:sim_net_client>"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMENT "Copying the sim's native client library to the output directory"
)
