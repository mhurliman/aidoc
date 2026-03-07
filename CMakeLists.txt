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
FetchContent_MakeAvailable(tinygltf imgui)

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/App.cpp
    src/OrbitCamera.cpp
    src/FlyCamera.cpp
    src/Mesh.cpp
    src/Scene.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${tinygltf_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d12
    dxgi
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

# Shader compilation - output to a fixed build directory
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

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
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo ${VS_OUTPUT} ${SHADER_FILE}
        DEPENDS ${SHADER_FILE}
        COMMENT "Compiling VS: ${SHADER_NAME}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${PS_OUTPUT}
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo ${PS_OUTPUT} ${SHADER_FILE}
        DEPENDS ${SHADER_FILE}
        COMMENT "Compiling PS: ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND SHADER_OUTPUTS ${VS_OUTPUT} ${PS_OUTPUT})
endforeach()

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
    COMMENT "Copying shaders and models to output directory"
)
