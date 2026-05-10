cmake_minimum_required(VERSION 3.30)

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

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

file(GLOB_RECURSE MODULE_FILES CONFIGURE_DEPENDS "src/*.cppm")
set(VULKAN_HPP_MODULE "${VULKAN_SDK_PATH}/Include/vulkan/vulkan.cppm")

if(EXISTS "${VULKAN_HPP_MODULE}")
  list(APPEND MODULE_FILES "${VULKAN_HPP_MODULE}")
endif()

add_executable(${PROJECT_NAME} )
target_sources(${PROJECT_NAME} PRIVATE ${SOURCE_FILES})
if(MODULE_FILES)
  target_sources(${PROJECT_NAME} PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS
      "${CMAKE_SOURCE_DIR}/src"
      "${VULKAN_SDK_PATH}/Include"
    FILES
      ${MODULE_FILES}
  )
endif()

if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
  set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_MODULE_STD ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  get_filename_component(CLANG_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(CLANG_ROOT_DIR "${CLANG_BIN_DIR}" DIRECTORY)
  set(LIBCXX_STD_MODULE "${CLANG_ROOT_DIR}/share/libc++/v1/std.cppm")
  set(LIBCXX_STD_COMPAT_MODULE "${CLANG_ROOT_DIR}/share/libc++/v1/std.compat.cppm")
  set(LIBCXX_STD_PCM "${CMAKE_BINARY_DIR}/std.pcm")
  set(LIBCXX_STD_COMPAT_PCM "${CMAKE_BINARY_DIR}/std.compat.pcm")

  if(EXISTS "${LIBCXX_STD_MODULE}" AND EXISTS "${LIBCXX_STD_COMPAT_MODULE}")
    add_custom_command(
      OUTPUT "${LIBCXX_STD_PCM}"
      COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -stdlib=libc++ -Wno-reserved-module-identifier --precompile "${LIBCXX_STD_MODULE}" -o "${LIBCXX_STD_PCM}"
      DEPENDS "${LIBCXX_STD_MODULE}"
      COMMENT "Precompiling libc++ std module"
      VERBATIM
    )

    add_custom_command(
      OUTPUT "${LIBCXX_STD_COMPAT_PCM}"
      COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -stdlib=libc++ -Wno-reserved-module-identifier -fmodule-file=std=${LIBCXX_STD_PCM} --precompile "${LIBCXX_STD_COMPAT_MODULE}" -o "${LIBCXX_STD_COMPAT_PCM}"
      DEPENDS "${LIBCXX_STD_COMPAT_MODULE}" "${LIBCXX_STD_PCM}"
      COMMENT "Precompiling libc++ std.compat module"
      VERBATIM
    )

    add_custom_target(cxx_std_module DEPENDS "${LIBCXX_STD_PCM}" "${LIBCXX_STD_COMPAT_PCM}")
    set_source_files_properties(${SOURCE_FILES} ${MODULE_FILES} PROPERTIES OBJECT_DEPENDS "${LIBCXX_STD_PCM};${LIBCXX_STD_COMPAT_PCM}")
    add_dependencies(${PROJECT_NAME} cxx_std_module)

    target_compile_options(${PROJECT_NAME} PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>"
      "$<$<COMPILE_LANGUAGE:CXX>:-fmodule-file=std=${LIBCXX_STD_PCM}>"
      "$<$<COMPILE_LANGUAGE:CXX>:-fmodule-file=std.compat=${LIBCXX_STD_COMPAT_PCM}>"
    )
    target_link_options(${PROJECT_NAME} PRIVATE -stdlib=libc++)

    message(STATUS "Using manual libc++ std module fallback: ${LIBCXX_STD_MODULE} and ${LIBCXX_STD_COMPAT_MODULE}")
  else()
    message(STATUS "Toolchain does not provide import std support; CXX_MODULE_STD remains disabled")
  endif()
else()
  message(STATUS "Toolchain does not provide import std support; CXX_MODULE_STD remains disabled")
endif()

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
