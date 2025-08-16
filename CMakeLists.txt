cmake_minimum_required(VERSION 3.16)
project(konamask LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# default to debug if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (default Debug)" FORCE)
endif()

# compiler warnings and debug symbols
add_compile_options(-Wall -Wextra)
# Keep debug info (CMake adds -g for debug builds)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-g)
endif()

# pkg-config to find libraries
find_package(PkgConfig REQUIRED)

# list of pkg-config modules
set(PKG_MODULES
  espeak-ng
  portaudio-2.0
  sdl2
  libpulse-simple
  gtk+-3.0
  appindicator3-0.1
)

foreach(_m ${PKG_MODULES})
  pkg_check_modules(${_m}_PKG QUIET ${_m})
  if(NOT ${_m}_PKG_FOUND)
    message(FATAL_ERROR "pkg-config could not find package: ${_m}. Install it or adjust PKG_CONFIG_PATH.")
  endif()
endforeach()

# collect include dirs and libs from pkg-config results
set(PKG_INCLUDE_DIRS "")
set(PKG_LIBS "")
foreach(_m ${PKG_MODULES})
  list(APPEND PKG_INCLUDE_DIRS $${_m}_PKG_INCLUDE_DIRS)
  list(APPEND PKG_LIBS $${_m}_PKG_LIBRARIES)
endforeach()

# linux testing
#list(APPEND PKG_INCLUDE_DIRS
#  ${CMAKE_SOURCE_DIR}/include
#  /usr/include/glib-2.0
#  /usr/include/gtk-3.0
#)

# convert lists to proper CMake lists (pkg-config may return space-separated strings)
separate_arguments(PKG_INCLUDE_DIRS)
separate_arguments(PKG_LIBS)

# project sources (src/*.cpp)
file(GLOB SRC_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

# linux testing
#file(GLOB BACKEND_SRC ${CMAKE_SOURCE_DIR}/third_party/imgui_backends/*.cpp)
#if(BACKEND_SRC)
#  list(APPEND SRC_FILES ${BACKEND_SRC})
#endif()

# build
add_executable(${PROJECT_NAME} ${SRC_FILES})

# add include directories
target_include_directories(${PROJECT_NAME} PRIVATE ${PKG_INCLUDE_DIRS})

# link libraries from pkg-config
target_link_libraries(${PROJECT_NAME} PRIVATE ${PKG_LIBS})

# link libraries
# vosk, pthread, imgui, vulkan, curl
# pthread -> Threads package
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# link libraries by name (will require them to be on linker path)
find_library(VOSK_LIB vosk REQUIRED)
if(VOSK_LIB)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${VOSK_LIB})
else()
  message(WARNING "Could not find libvosk via find_library; linking 'vosk' by name")
  target_link_libraries(${PROJECT_NAME} PRIVATE vosk)
endif()

# imgui, vulkan, curl: try find, otherwise link by name
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE vulkan)
endif()

find_package(CURL QUIET)
if(CURL_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE curl)
endif()

# imgui often doesn't provide a CMake config, link by name
find_library(IMGUI_LIB imgui)
if(IMGUI_LIB)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${IMGUI_LIB})
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE imgui)
endif()

# set the runtime output directory to ./output - remove if buggy
set_target_properties(${PROJECT_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output
)

# linux testing
#target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/include)

# provide user-friendly build/run targets
add_custom_target(run
  COMMAND ${CMAKE_COMMAND} -E echo "Running ${PROJECT_NAME}"
  COMMAND ${CMAKE_COMMAND} -E env $<TARGET_FILE:${PROJECT_NAME}>
  DEPENDS ${PROJECT_NAME}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# remove output directory on 'make clean' handled by CMake in build tree
add_custom_target(distclean
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_SOURCE_DIR}/output
  COMMENT "Remove generated output directory"
)
