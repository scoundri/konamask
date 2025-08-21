cmake_minimum_required(VERSION 3.16)
project(konamask VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# allow overriding install prefixes and vcpkg toolchain via -DCMAKE_TOOLCHAIN_FILE
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# debug output
message(STATUS "Configuring konamask for ${CMAKE_SYSTEM_NAME} ${CMAKE_CXX_COMPILER_ID}")

# add local cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/Modules")

# prefer vcpkg-provided libraries if toolchain is set
find_package(Threads REQUIRED)  # for -pthread

# attempt to find common deps via pkg-config on Unix or vcpkg/find_package on Windows
if(WIN32)
  message(STATUS "Windows build: getting dependencies...")
  # try to find PortAudio, SDL2, CURL, Vulkan etc.
  find_package(PortAudio QUIET)
  find_package(SDL2 QUIET)
  find_package(CURL QUIET)
  find_package(Vulkan QUIET)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PKG_ESPEAK espeak-ng REQUIRED)
  pkg_check_modules(PKG_PORTAUDIO portaudio-2.0 REQUIRED)
  pkg_check_modules(PKG_SDL2 sdl2 REQUIRED)
  pkg_check_modules(PKG_PULSE libpulse-simple QUIET)
  pkg_check_modules(PKG_GTK gtk+-3.0 QUIET)
endif()

# find PortAudio (cross-platform)
if(NOT PortAudio_FOUND)
  find_package(portaudio QUIET)
endif()

# find curl/vulkan/sdl using CMake modules/link by name fallback
if(NOT CURL_FOUND)
  find_package(CURL QUIET)
endif()
if(NOT Vulkan_FOUND)
  find_package(Vulkan QUIET)
endif()

# find espeak-ng and vosk
find_package(ESPEAK_NG QUIET)
find_package(VOSK QUIET)

# if not found, allow user specification of ESPEAK_ROOT and VOSK_ROOT:
if(NOT ESPEAK_NG_FOUND)
  if(DEFINED ENV{ESPEAK_ROOT})
    set(ESPEAK_NG_ROOT $ENV{ESPEAK_ROOT})
    find_package(ESPEAK_NG REQUIRED PATHS ${ESPEAK_NG_ROOT} NO_DEFAULT_PATH)
  endif()
endif()
if(NOT VOSK_FOUND)
  if(DEFINED ENV{VOSK_ROOT})
    set(VOSK_ROOT $ENV{VOSK_ROOT})
    find_package(VOSK REQUIRED PATHS ${VOSK_ROOT} NO_DEFAULT_PATH)
  endif()
endif()

# collect sources
file(GLOB SRC_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB BACKEND_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/imgui_backends/*.cpp)
list(APPEND SRC_FILES ${BACKEND_SRC})

add_executable(${PROJECT_NAME} ${SRC_FILES})

# add include dirs from found packages
if(PKG_ESPEAK_INCLUDEDIR)
  target_include_directories(${PROJECT_NAME} PRIVATE ${PKG_ESPEAK_INCLUDEDIR})
endif()
if(ESPEAK_NG_INCLUDE_DIR)
  target_include_directories(${PROJECT_NAME} PRIVATE ${ESPEAK_NG_INCLUDE_DIR})
endif()
if(VOSK_INCLUDE_DIR)
  target_include_directories(${PROJECT_NAME} PRIVATE ${VOSK_INCLUDE_DIR})
endif()
if(PortAudio_INCLUDE_DIRS)
  target_include_directories(${PROJECT_NAME} PRIVATE ${PortAudio_INCLUDE_DIRS})
endif()
if(SDL2_INCLUDE_DIRS)
  target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
endif()
if(CURL_INCLUDE_DIRS)
  target_include_directories(${PROJECT_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
endif()

# link libraries
if(PKG_ESPEAK_LIBRARIES)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${PKG_ESPEAK_LIBRARIES})
endif()
if(ESPEAK_NG_LIBRARY)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${ESPEAK_NG_LIBRARY})
endif()

if(VOSK_LIBRARY)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${VOSK_LIBRARY})
endif()

# PortAudio
if(PortAudio_LIBRARIES)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${PortAudio_LIBRARIES})
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE portaudio)
endif()

# SDL2, curl, Vulkan, threads
if(SDL2_LIBRARIES)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL2_LIBRARIES})
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE SDL2)
endif()

if(CURL_LIBRARIES)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${CURL_LIBRARIES})
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE curl)
endif()

if(Vulkan_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE vulkan)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

find_library(IMGUI_LIB imgui)
if(IMGUI_LIB)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${IMGUI_LIB})
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE imgui)
endif()

# runtime output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output
)

# definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<PLATFORM_ID:Windows>:_WIN32>)

# install target (not required)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
