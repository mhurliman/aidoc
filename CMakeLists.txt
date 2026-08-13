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

target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d12
    dxgi
    nlohmann_json::nlohmann_json
)

# ------------------------------------------------------------------------------------------------
# .NET hosting: embed the C# physics sim (Sim.Host.dll from the sailboat-sim repo) via nethost.
# ------------------------------------------------------------------------------------------------
file(GLOB NETHOST_NATIVE_DIRS
    "C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/*/runtimes/win-x64/native")
if(NOT NETHOST_NATIVE_DIRS)
    message(FATAL_ERROR "nethost pack not found. Install the .NET SDK (win-x64 host pack).")
endif()
list(SORT NETHOST_NATIVE_DIRS)
list(GET NETHOST_NATIVE_DIRS -1 NETHOST_DIR)   # newest version
message(STATUS "Using nethost from: ${NETHOST_DIR}")

target_include_directories(${PROJECT_NAME} PRIVATE ${NETHOST_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE "${NETHOST_DIR}/nethost.lib")

# Build Sim.Host (C#) alongside WaveSim, and copy its managed assemblies next to the exe.
set(SAILBOAT_SIM_DIR "${CMAKE_SOURCE_DIR}/../sailboat-sim"
    CACHE PATH "Path to the sailboat-sim repo (provides the Sim.Host physics DLL)")
set(SIM_HOST_PROJECT "${SAILBOAT_SIM_DIR}/src/Sim.Host/Sim.Host.csproj")
set(SIM_HOST_OUTPUT_DIR "${SAILBOAT_SIM_DIR}/src/Sim.Host/bin/$<CONFIG>/net9.0")

add_custom_target(sim_host ALL
    COMMAND dotnet build "${SIM_HOST_PROJECT}" -c $<CONFIG> --nologo -v quiet
    COMMENT "Building Sim.Host (C# physics sim)"
    VERBATIM
)
add_dependencies(${PROJECT_NAME} sim_host)

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

# Copy the managed sim assemblies + runtime config and nethost.dll next to the exe.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SIM_HOST_OUTPUT_DIR}/Sim.Host.dll"
        "${SIM_HOST_OUTPUT_DIR}/Sim.Host.runtimeconfig.json"
        "${SIM_HOST_OUTPUT_DIR}/Sim.Host.deps.json"
        "${SIM_HOST_OUTPUT_DIR}/Sim.Core.dll"
        "${SIM_HOST_OUTPUT_DIR}/Sim.Models.dll"
        "${SIM_HOST_OUTPUT_DIR}/Sim.Telemetry.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NETHOST_DIR}/nethost.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMENT "Copying Sim.Host managed assemblies + nethost.dll to output directory"
)
