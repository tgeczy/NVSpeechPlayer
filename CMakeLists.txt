cmake_minimum_required(VERSION 3.21)

project(speechPlayer)

# Threads are needed on POSIX (LockableObject uses std::recursive_mutex).
# On Windows this is not required.
if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

# ---------------------------------------
# Global MSVC settings (lean + UTF-8)
# ---------------------------------------
if(MSVC)
  # Make the compiler treat source files as UTF-8 (important for IPA symbols).
  add_compile_options(/utf-8)

  # Prefer dynamic MSVC runtime (smaller DLLs than /MT in most cases).
  # If you rely on /MT for deployment reasons, remove this line.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

  # Release optimizations and dead code stripping.
  add_compile_options("$<$<CONFIG:Release>:/O2>" "$<$<CONFIG:Release>:/DNDEBUG>")
  add_compile_options("$<$<CONFIG:Release>:/Gy>" "$<$<CONFIG:Release>:/Gw>")
  add_compile_options("$<$<CONFIG:Release>:/GL>")

  add_link_options("$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
  add_link_options("$<$<CONFIG:Release>:/LTCG>")
endif()

# -------------------------
# speechPlayer.dll (DSP)
# -------------------------
file(GLOB SRC_CPP CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

set(SPEECHPLAYER_SOURCES ${SRC_CPP})
if(WIN32)
  # Export list for the legacy DLL ABI.
  list(APPEND SPEECHPLAYER_SOURCES "src/speechPlayer.def")
endif()

add_library(speechPlayer SHARED ${SPEECHPLAYER_SOURCES})

target_include_directories(speechPlayer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(speechPlayer PRIVATE cxx_std_17)

# Windows waveOut dependency (as in the original project).
if(WIN32)
  target_link_libraries(speechPlayer PRIVATE winmm)
else()
  target_link_libraries(speechPlayer PRIVATE Threads::Threads)
endif()

# -------------------------
# nvspFrontend.dll (IPA -> Frames)
# -------------------------
file(GLOB FRONTEND_CPP CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/*.cpp")
file(GLOB FRONTEND_PASSES_CPP CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/passes/*.cpp")

add_library(nvspFrontend SHARED ${FRONTEND_CPP} ${FRONTEND_PASSES_CPP})

target_include_directories(nvspFrontend PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend"
)

target_compile_features(nvspFrontend PRIVATE cxx_std_17)

target_compile_definitions(nvspFrontend PRIVATE NVSP_FRONTEND_EXPORTS=1)

# Optional: make the DLL name predictable.
set_target_properties(nvspFrontend PROPERTIES OUTPUT_NAME "nvspFrontend")

# -------------------------
# Optional command-line tools
# -------------------------
option(NVSP_BUILD_TOOLS "Build optional command-line tools (e.g. Speech Dispatcher helpers)" OFF)
if(NVSP_BUILD_TOOLS)
  add_executable(nvspRender tools/nvspRender.cpp)
  target_include_directories(nvspRender PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend"
  )
  target_compile_features(nvspRender PRIVATE cxx_std_17)
  target_link_libraries(nvspRender PRIVATE speechPlayer nvspFrontend)
endif()

# -------------------------
# Win32 phoneme editor GUI
# -------------------------
if(WIN32)
  add_subdirectory(tools/nvspPhonemeEditorWin32)
endif()

# If nvspFrontend needs WinMM too (it usually doesn't), uncomment:
# if(WIN32)
#   target_link_libraries(nvspFrontend PRIVATE winmm)
# endif()
