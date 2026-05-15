cmake_minimum_required(VERSION 3.30)

project(vulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS OFF)



# compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -------------------------------------------------------
#--: Dependences
set(VULKAN_SDK_PATH "E:/OpenGL")
set(Vulakn_INCLUDE_DIR "${VULKAN_SDK_PATH}/Include")
set(Vulkan_LIBRARY "${VULKAN_SDK_PATH}/Lib/vulkan-1.lib")
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
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.hpp)

target_include_directories(${PROJECT_NAME} PRIVATE
  include
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  VULKAN_HPP_NO_CONSTRUCTORS
  VK_USE_PLATFORM_WIN32_KHR
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  Vulkan::Vulkan
  glfw
  glm
  tinyobjloader
)

set(SHADER_OUTPUT_FILES)
foreach(SHADER_SOURCE ${SHADER_SOURCE_FILES})
  get_filename_component(SHADER_NAME "${SHADER_SOURCE}" NAME)
  set(SHADER_OUTPUT "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
  add_custom_command(
    OUTPUT "${SHADER_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/shaders"
    COMMAND "${VULKAN_SDK_PATH}/Bin/glslangValidator.exe" -V "${SHADER_SOURCE}" -o "${SHADER_OUTPUT}"
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
