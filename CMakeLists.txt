cmake_minimum_required(VERSION 3.22)

project(trakdaw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE — add before Tracktion so Tracktion reuses our copy
# ---------------------------------------------------------------------------
add_subdirectory(vendor/JUCE)

# ---------------------------------------------------------------------------
# Tracktion Engine — disable examples, use our JUCE
# ---------------------------------------------------------------------------
set(TE_ADD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/tracktion_engine)

# ---------------------------------------------------------------------------
# LuaJIT — use system installation
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUAJIT REQUIRED luajit)

# ---------------------------------------------------------------------------
# Python + pybind11 — use pyenv Python; pybind11 installed via pip
# ---------------------------------------------------------------------------
set(Python3_ROOT_DIR "$ENV{HOME}/.pyenv/versions/3.12.9")
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED
    HINTS "${Python3_ROOT_DIR}/lib/python3.12/site-packages/pybind11/share/cmake/pybind11")

# ---------------------------------------------------------------------------
# trakdaw
# ---------------------------------------------------------------------------
juce_add_console_app(trakdaw
    PRODUCT_NAME "trakdaw")

juce_generate_juce_header(trakdaw)

target_sources(trakdaw PRIVATE
    src/main.cpp)

target_compile_features(trakdaw PRIVATE cxx_std_20)

# GCC 15 added -Wtemplate-body which fires in nanorange (tracktion vendor header)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(trakdaw PRIVATE -Wno-template-body)
endif()

target_include_directories(trakdaw PRIVATE
    ${LUAJIT_INCLUDE_DIRS}
    vendor/sol2/include    # sol2 headers
    vendor/cpp-httplib)    # cpp-httplib (HTTP server for /eval)

target_compile_definitions(trakdaw PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=0
    SOL_ALL_SAFETIES_ON=1
    JUCE_USE_WINRT_MIDI=0
    JUCE_JACK=1
    JUCE_ALSA=0
    JUCE_PULSEAUDIO=0)

target_link_libraries(trakdaw PRIVATE
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    tracktion::tracktion_core
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    ${LUAJIT_LIBRARIES}
    pybind11::embed)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(JACK REQUIRED jack)
    target_include_directories(trakdaw PRIVATE ${JACK_INCLUDE_DIRS})
    target_link_libraries(trakdaw PRIVATE "-latomic" ${JACK_LIBRARIES})
endif()

# GNU readline for the stdin REPL (history, line editing, reverse search).
# POSIX-only for now; Windows portability tracked in #1.
if(NOT WIN32)
    find_path(READLINE_INCLUDE_DIR readline/readline.h REQUIRED)
    find_library(READLINE_LIBRARY readline REQUIRED)
    target_include_directories(trakdaw PRIVATE ${READLINE_INCLUDE_DIR})
    target_link_libraries(trakdaw PRIVATE ${READLINE_LIBRARY})
endif()
