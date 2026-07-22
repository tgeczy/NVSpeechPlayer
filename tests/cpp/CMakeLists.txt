# TGSpeechBox C++ unit test suite.
#
# Uses doctest (single-header, vendored at tests/cpp/doctest.h) to exercise
# individual passes and helpers in isolation — i.e. the layer below the
# Python/DLL tests. The pytest suite tests behavior end-to-end through the C
# API; these tests reach inside the C++ layer to pin per-pass invariants that
# the end-to-end tests can't observe directly.
#
# Run:
#   cmake --build build-x86-nvda --config MinSizeRel --target tgsb_unit_tests
#   build-x86-nvda/MinSizeRel/tgsb_unit_tests.exe
#   # or
#   ctest --test-dir build-x86-nvda -C MinSizeRel
#
# Opt-in: the test target is only built when -DTGSB_BUILD_TESTS=ON is passed
# at configure time, so production builds aren't slowed down by it.

# Collect test source files. Each test_*.cpp is a standalone translation unit
# that includes doctest.h; one of them (test_main.cpp) defines DOCTEST_CONFIG_
# IMPLEMENT_WITH_MAIN so there's exactly one main entry point.
file(GLOB TGSB_CPP_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# Re-use the frontend source list. We re-compile the frontend into the test
# binary rather than linking against nvspFrontend.dll so tests can call
# internal (non-exported) C++ functions directly. The compile-time cost is
# acceptable because test builds are on-demand.
file(GLOB FRONTEND_CPP_FOR_TESTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../../src/frontend/*.cpp")
file(GLOB FRONTEND_PASSES_CPP_FOR_TESTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../../src/frontend/passes/*.cpp")
# DSP sources (speechPlayer) — linked so audio-capture tests can synthesize
# directly into a buffer, bypassing the platform audio sinks.
file(GLOB SPEECHPLAYER_CPP_FOR_TESTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../../src/*.cpp")

add_executable(tgsb_unit_tests
  ${TGSB_CPP_TESTS}
  ${FRONTEND_CPP_FOR_TESTS}
  ${FRONTEND_PASSES_CPP_FOR_TESTS}
  ${SPEECHPLAYER_CPP_FOR_TESTS}
)

target_include_directories(tgsb_unit_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"                         # for doctest.h
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src"                # for speechPlayer.h, frame.h, sample.h
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/frontend"       # for frontend headers
)

target_compile_features(tgsb_unit_tests PRIVATE cxx_std_17)

# Disable the DLL-export decorations when compiling the frontend sources for
# the test binary. Same flag the main nvspFrontend target uses; without it,
# __declspec(dllexport) would apply and we'd be trying to export symbols from
# an executable which MSVC warns about.
target_compile_definitions(tgsb_unit_tests PRIVATE NVSP_FRONTEND_EXPORTS=1)

# Win7 compat thunks (only if enabled — matches main build).
if(YY_THUNKS_OBJ)
  target_link_libraries(tgsb_unit_tests PRIVATE "${YY_THUNKS_OBJ}")
endif()

# normalizeNFKC() in utf8.cpp uses CFStringNormalize on Apple platforms. The
# shipping apps get CoreFoundation from the Xcode target's framework list, so
# only the standalone test binary has to ask for it.
if(APPLE)
  target_link_libraries(tgsb_unit_tests PRIVATE "-framework CoreFoundation")
endif()

# Register with CTest so `ctest` runs the binary.
add_test(NAME tgsb_unit_tests COMMAND tgsb_unit_tests)
