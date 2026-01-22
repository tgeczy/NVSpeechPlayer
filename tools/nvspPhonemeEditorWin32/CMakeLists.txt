cmake_minimum_required(VERSION 3.21)

# This is a Win32 GUI utility for editing NVSP YAML packs.
# It is intentionally kept "plain Win32" (no WPF/MFC/Qt).

set(NVSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_executable(nvspPhonemeEditor WIN32
  main.cpp
  AppController.cpp
  AppController.h
  chunking.cpp
  chunking.h
  phonemizer_cli.cpp
  phonemizer_cli.h
  Dialogs.cpp
  Dialogs.h
  WinUtils.cpp
  WinUtils.h
  AccessibilityUtils.cpp
  AccessibilityUtils.h
  yaml_edit.cpp
  yaml_edit.h
  nvsp_runtime.cpp
  nvsp_runtime.h
  process_util.cpp
  process_util.h
  wav_writer.cpp
  wav_writer.h
  resource.h
  resources.rc

  # Reuse NVSP's small YAML + UTF-8 helpers (no external deps).
  "${NVSP_ROOT}/src/frontend/yaml_min.cpp"
  "${NVSP_ROOT}/src/frontend/yaml_min.h"
  "${NVSP_ROOT}/src/frontend/utf8.cpp"
  "${NVSP_ROOT}/src/frontend/utf8.h"
)

target_include_directories(nvspPhonemeEditor PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${NVSP_ROOT}/src"
  "${NVSP_ROOT}/src/frontend"
)

target_compile_features(nvspPhonemeEditor PRIVATE cxx_std_17)

# For IPA symbols in string literals.
if(MSVC)
  target_compile_options(nvspPhonemeEditor PRIVATE /utf-8)
endif()

if(WIN32)
  target_link_libraries(nvspPhonemeEditor PRIVATE
    comctl32
    oleacc
    ole32
    shell32
    shlwapi
    winmm
  )
endif()
