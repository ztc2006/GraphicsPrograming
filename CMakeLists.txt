cmake_minimum_required(VERSION 3.30)

project(vulkan LANGUAGES CXX)

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

# -------------------------------------------------------
#--: Targets

file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB SHADER_SOURCE_FILES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/shaders/*.vert"
  "${CMAKE_SOURCE_DIR}/shaders/*.frag"
)

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
  deps/stb
  deps/imgui
  deps/imgui/backends
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  VULKAN_HPP_NO_CONSTRUCTORS
)
if(WIN32)
  target_compile_definitions(${PROJECT_NAME} PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
  Vulkan::Vulkan
  glfw
  glm
  tinyobjloader
)

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
