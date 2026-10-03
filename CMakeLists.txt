cmake_minimum_required(VERSION 3.30)

project(vulkan LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS OFF)



# compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -------------------------------------------------------
#--: Dependences
find_package(Vulkan REQUIRED)

add_subdirectory(${CMAKE_SOURCE_DIR}/deps/glfw)
add_subdirectory(${CMAKE_SOURCE_DIR}/deps/glm)
add_subdirectory(${CMAKE_SOURCE_DIR}/deps/tinyobjloader)

# Pinned offline dependency; only the importer adapter consumes its types.
add_library(cgltf STATIC deps/cgltf/cgltf.cpp)
target_include_directories(cgltf SYSTEM PUBLIC deps/cgltf)
add_library(vma STATIC deps/vma/vma.cpp)
target_include_directories(vma SYSTEM PUBLIC deps/vma)
target_link_libraries(vma PUBLIC Vulkan::Vulkan)

# -------------------------------------------------------
#--: Targets

file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB SHADER_SOURCE_FILES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/shaders/*.vert"
  "${CMAKE_SOURCE_DIR}/shaders/*.frag"
)

# Fingerprint the actual project code used for an A/B measurement. Reconfigure
# when code changes so incremental builds cannot keep a stale fingerprint.
file(GLOB_RECURSE BASELINE_INPUTS CONFIGURE_DEPENDS "src/*.cpp" "src/*.hpp" "shaders/*.vert" "shaders/*.frag")
list(APPEND BASELINE_INPUTS "${CMAKE_SOURCE_DIR}/deps/cgltf/cgltf.h" "${CMAKE_SOURCE_DIR}/deps/cgltf/cgltf.cpp" "${CMAKE_SOURCE_DIR}/deps/vma/vk_mem_alloc.h" "${CMAKE_SOURCE_DIR}/deps/vma/vma.cpp")
list(SORT BASELINE_INPUTS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BASELINE_INPUTS})
set(BASELINE_HASH_INPUT "")
foreach(BASELINE_INPUT IN LISTS BASELINE_INPUTS)
  file(SHA256 "${BASELINE_INPUT}" BASELINE_FILE_HASH)
  file(RELATIVE_PATH BASELINE_RELATIVE "${CMAKE_SOURCE_DIR}" "${BASELINE_INPUT}")
  string(APPEND BASELINE_HASH_INPUT "${BASELINE_RELATIVE}:${BASELINE_FILE_HASH}\n")
endforeach()
string(SHA256 BASELINE_SOURCE_HASH "${BASELINE_HASH_INPUT}")
configure_file(cmake/build_info.hpp.in generated/build_info.hpp @ONLY)

add_executable(${PROJECT_NAME} )
target_sources(${PROJECT_NAME} PRIVATE ${SOURCE_FILES})
target_sources(${PROJECT_NAME} PRIVATE
  deps/imgui/imgui.cpp
  deps/imgui/imgui_draw.cpp
  deps/imgui/imgui_tables.cpp
  deps/imgui/imgui_widgets.cpp
  deps/imgui/backends/imgui_impl_glfw.cpp
  deps/imgui/backends/imgui_impl_vulkan.cpp
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.hpp)

target_include_directories(${PROJECT_NAME} PRIVATE
  include
  "${CMAKE_BINARY_DIR}/generated"
  deps/stb
  deps/imgui
  deps/imgui/backends
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  VULKAN_HPP_NO_CONSTRUCTORS
  VULKAN_RESOURCE_ROOT="${CMAKE_BINARY_DIR}"
  VULKAN_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
if(WIN32)
  target_compile_definitions(${PROJECT_NAME} PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
  Vulkan::Vulkan
  glfw
  glm
  tinyobjloader
  cgltf
  vma
)

if(BUILD_TESTING)
  # Optional real Vulkan regression. Keep display requirements out of CPU CTest.
  option(VULKAN_GPU_TESTS "Build graphical scene transaction regression" OFF)
  if(VULKAN_GPU_TESTS)
    add_executable(gpu_scene_transaction_test
      tests/gpu_scene_transaction_test.cpp tests/hdr_output_regression.cpp
      src/resource_ledger.cpp src/gpu_allocator.cpp src/device.cpp src/swap_chain.cpp src/renderer.cpp src/hdr_output.cpp src/material_gpu_store.cpp
      src/texture.cpp src/texture_cache.cpp src/upload_batch.cpp src/hdr_image.cpp src/hdr_ibl.cpp src/stb_image.cpp
      src/mesh.cpp src/gltf_loader.cpp)
    target_include_directories(gpu_scene_transaction_test PRIVATE src deps/stb)
    target_compile_definitions(gpu_scene_transaction_test PRIVATE
      VULKAN_HPP_NO_CONSTRUCTORS
      TEST_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/uv_transform.gltf")
    target_precompile_headers(gpu_scene_transaction_test PRIVATE src/pch.hpp)
    target_link_libraries(gpu_scene_transaction_test PRIVATE Vulkan::Vulkan glfw glm cgltf vma)
    # Exercise the actual Application loading/preview lifecycle with a real backend.
    get_target_property(VIEWER_TEST_SOURCES ${PROJECT_NAME} SOURCES)
    list(FILTER VIEWER_TEST_SOURCES EXCLUDE REGEX "(^|/)main\\.cpp$")
    get_target_property(VIEWER_TEST_INCLUDES ${PROJECT_NAME} INCLUDE_DIRECTORIES)
    get_target_property(VIEWER_TEST_DEFINITIONS ${PROJECT_NAME} COMPILE_DEFINITIONS)
    get_target_property(VIEWER_TEST_LIBRARIES ${PROJECT_NAME} LINK_LIBRARIES)
    add_executable(viewer_async_scene_test tests/viewer_async_scene_test.cpp ${VIEWER_TEST_SOURCES})
    target_include_directories(viewer_async_scene_test PRIVATE src ${VIEWER_TEST_INCLUDES})
    target_compile_definitions(viewer_async_scene_test PRIVATE ${VIEWER_TEST_DEFINITIONS}
      TEST_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/uv_transform.gltf"
      TEST_ADAPTER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/cgltf"
      TEST_BAD_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/sampler_bad_filter.gltf"
      TEST_SIHEYUAN_PATH="${CMAKE_SOURCE_DIR}/assets/models/siheyuan/source/siheyuan.glb")
    target_link_libraries(viewer_async_scene_test PRIVATE ${VIEWER_TEST_LIBRARIES})
    target_precompile_headers(viewer_async_scene_test PRIVATE src/pch.hpp)
    add_test(NAME viewer_async_scene COMMAND viewer_async_scene_test)
    set_tests_properties(viewer_async_scene PROPERTIES
      LABELS gpu SKIP_RETURN_CODE 77 TIMEOUT 90
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME gpu_scene_transaction COMMAND gpu_scene_transaction_test)
    set_tests_properties(gpu_scene_transaction PROPERTIES
      LABELS gpu SKIP_RETURN_CODE 77 TIMEOUT 60
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
  endif()
  add_executable(resource_ledger_test tests/resource_ledger_test.cpp src/resource_ledger.cpp)
  target_include_directories(resource_ledger_test PRIVATE src)
  add_test(NAME resource_ledger COMMAND resource_ledger_test)

  add_executable(measurement_test tests/measurement_test.cpp src/viewer_options.cpp src/benchmark_report.cpp src/resource_ledger.cpp)
  target_include_directories(measurement_test PRIVATE src)
  add_test(NAME measurement COMMAND measurement_test)
  find_program(BASH_EXECUTABLE bash)
  if(BASH_EXECUTABLE AND UNIX)
    add_test(NAME run_launcher COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/run_launcher_test.sh" "${CMAKE_SOURCE_DIR}")
  endif()
  add_executable(gltf_geometry_regression_test tests/gltf_geometry_regression_test.cpp src/gltf_loader.cpp src/mesh.cpp)
  target_include_directories(gltf_geometry_regression_test PRIVATE src)
  target_compile_definitions(gltf_geometry_regression_test PRIVATE VULKAN_HPP_NO_CONSTRUCTORS TEST_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/uv_transform.gltf")
  target_link_libraries(gltf_geometry_regression_test PRIVATE Vulkan::Vulkan glm cgltf)
  add_test(NAME gltf_geometry_regression COMMAND gltf_geometry_regression_test)

  add_executable(gltf_adapter_test tests/gltf_adapter_test.cpp src/gltf_loader.cpp src/mesh.cpp)
  target_include_directories(gltf_adapter_test PRIVATE src)
  target_compile_definitions(gltf_adapter_test PRIVATE VULKAN_HPP_NO_CONSTRUCTORS
    TEST_ADAPTER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/cgltf")
  target_link_libraries(gltf_adapter_test PRIVATE Vulkan::Vulkan glm cgltf)
  add_test(NAME gltf_adapter COMMAND gltf_adapter_test)

  add_executable(gltf_material_semantics_test
    tests/gltf_material_semantics_test.cpp
    src/gltf_loader.cpp
    src/mesh.cpp
  )
  target_include_directories(gltf_material_semantics_test PRIVATE src)
  target_compile_definitions(gltf_material_semantics_test PRIVATE
    VULKAN_HPP_NO_CONSTRUCTORS
    TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/import_tests/gltf_external"
    TEST_SIHEYUAN_PATH="${CMAKE_SOURCE_DIR}/assets/models/siheyuan/source/siheyuan.glb"
  )
  target_link_libraries(gltf_material_semantics_test PRIVATE
    Vulkan::Vulkan
    glm
    cgltf
  )
  add_test(NAME gltf_material_semantics COMMAND gltf_material_semantics_test)

  add_executable(hdr_image_test
    tests/hdr_image_test.cpp
    src/hdr_image.cpp
    src/stb_image.cpp
  )
  target_include_directories(hdr_image_test PRIVATE src deps/stb)
  target_compile_definitions(hdr_image_test PRIVATE
    TEST_HDR_PATH="${CMAKE_SOURCE_DIR}/assets/environments/environment.hdr"
  )
  add_test(NAME hdr_image COMMAND hdr_image_test)

  add_executable(hdr_ibl_test
    tests/hdr_ibl_test.cpp
    src/hdr_ibl.cpp
  )
  target_include_directories(hdr_ibl_test PRIVATE src)
  target_link_libraries(hdr_ibl_test PRIVATE glm)
  add_test(NAME hdr_ibl COMMAND hdr_ibl_test)
endif()

set(SHADER_OUTPUT_FILES)

if(TARGET Vulkan::glslangValidator)
  set(GLSLANG_VALIDATOR "$<TARGET_FILE:Vulkan::glslangValidator>")
else()
  find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator glslangValidator.exe
    HINTS
      "$ENV{VULKAN_SDK}/Bin"
  )
endif()

if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found. Install Vulkan SDK or add glslangValidator to PATH.")
endif()

foreach(SHADER_SOURCE ${SHADER_SOURCE_FILES})
  get_filename_component(SHADER_NAME "${SHADER_SOURCE}" NAME)
  set(SHADER_OUTPUT "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
  add_custom_command(
    OUTPUT "${SHADER_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${SHADER_SOURCE}" -o "${SHADER_OUTPUT}"
    DEPENDS "${SHADER_SOURCE}"
    COMMENT "Compiling shader ${SHADER_NAME}"
    VERBATIM
  )
  list(APPEND SHADER_OUTPUT_FILES "${SHADER_OUTPUT}")
endforeach()

if(SHADER_OUTPUT_FILES)
  add_custom_target(compile_shaders ALL DEPENDS ${SHADER_OUTPUT_FILES})
  add_dependencies(${PROJECT_NAME} compile_shaders)
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_SOURCE_DIR}/texture"
          "${CMAKE_BINARY_DIR}/texture"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_SOURCE_DIR}/assets"
          "${CMAKE_BINARY_DIR}/assets"
)

if(TARGET gpu_scene_transaction_test)
  add_dependencies(gpu_scene_transaction_test ${PROJECT_NAME})
endif()

if(TARGET viewer_async_scene_test)
  add_dependencies(viewer_async_scene_test ${PROJECT_NAME})
endif()
