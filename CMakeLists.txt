cmake_minimum_required(VERSION 3.20)
project(aidoc LANGUAGES CXX)

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
    src/Renderer.cpp
    src/util/InputManager.cpp
    src/util/OrbitCamera.cpp
    src/util/FlyCamera.cpp
    src/gfx/DescriptorHeap.cpp
    src/gfx/LinearAllocator.cpp
    src/gfx/CommandContext.cpp
    src/gfx/ComputeContext.cpp
    src/gfx/GraphicsContext.cpp
    src/gfx/ColorBuffer.cpp
    src/gfx/DepthBuffer.cpp
    src/gfx/PixelBuffer.cpp
    src/gfx/GpuBuffer.cpp
    src/assets/Texture.cpp
    src/assets/Material.cpp
    src/assets/Mesh.cpp
    src/scene/Scene.cpp
    src/assets/ResourceManager.cpp
    src/water/FFTWaterSurface.cpp
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

target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d12
    dxgi
    nlohmann_json::nlohmann_json
)

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
        DEPENDS ${SHADER_FILE} ${COMMON_HLSLI}
        COMMENT "Compiling VS: ${SHADER_NAME}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${PS_OUTPUT}
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain ${DXC_SHADER_FLAGS} -Fo ${PS_OUTPUT} ${SHADER_FILE}
        DEPENDS ${SHADER_FILE} ${COMMON_HLSLI}
        COMMENT "Compiling PS: ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND SHADER_OUTPUTS ${VS_OUTPUT} ${PS_OUTPUT})
endforeach()

# Root signature blobs — compiled with rootsig_1_1 profile; loaded directly by CreateRootSignature.
set(RS_SOURCES
    "PbrRootSig.hlsl|pbr_rs"
    "FFT/FFTRootSig.hlsl|fft_compute_rs"
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
        DEPENDS ${RS_FILE}
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
