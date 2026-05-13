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
# LuaJIT — pkg-config on Linux, manual paths on Windows
# ---------------------------------------------------------------------------
if(WIN32)
    set(LUAJIT_DIR "${CMAKE_SOURCE_DIR}/vendor/LuaJIT/src")
    set(LUAJIT_INCLUDE_DIRS "${LUAJIT_DIR}")
    set(LUAJIT_LIBRARIES "${LUAJIT_DIR}/lua51.lib")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LUAJIT REQUIRED luajit)
endif()

# ---------------------------------------------------------------------------
# Python + pybind11 — pyenv on Linux, auto-detect on Windows
# ---------------------------------------------------------------------------
if(NOT WIN32)
    set(Python3_ROOT_DIR "$ENV{HOME}/.pyenv/versions/3.12.9")
endif()
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# Ask the active Python where pybind11 installed its CMake config
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import pybind11, sys; sys.stdout.write(pybind11.get_cmake_dir())"
    OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PYBIND11_CMAKE_DIR_RESULT)
if(NOT PYBIND11_CMAKE_DIR_RESULT EQUAL 0)
    message(FATAL_ERROR "pybind11 not found via Python ${Python3_EXECUTABLE}. Run: ${Python3_EXECUTABLE} -m pip install pybind11")
endif()
find_package(pybind11 CONFIG REQUIRED HINTS "${PYBIND11_CMAKE_DIR}")

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
    JUCE_USE_WINRT_MIDI=0)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(trakdaw PRIVATE
        JUCE_JACK=1
        JUCE_ALSA=1               # also gates Linux MIDI (juce_Midi_linux.cpp)
        JUCE_PULSEAUDIO=0)
endif()

target_link_libraries(trakdaw PRIVATE
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    tracktion::tracktion_core
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_osc
    ${LUAJIT_LIBRARIES}
    pybind11::embed)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(JACK REQUIRED jack)
    target_include_directories(trakdaw PRIVATE ${JACK_INCLUDE_DIRS})
    target_link_libraries(trakdaw PRIVATE "-latomic" ${JACK_LIBRARIES})
endif()

# Vendored line editor (history, multi-line, completion, syntax highlighting).
# Single-file amalgamation — builds on Linux, macOS, and Windows with no
# system dep. Replaces GNU readline; closes the cross-platform-REPL part of #1.
target_sources(trakdaw PRIVATE vendor/isocline/src/isocline.c)
target_include_directories(trakdaw PRIVATE vendor/isocline/include)

# Windows: copy lua51.dll next to the built exe (LuaJIT is built as a DLL).
if(WIN32)
    add_custom_command(TARGET trakdaw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUAJIT_DIR}/lua51.dll"
            "$<TARGET_FILE_DIR:trakdaw>/lua51.dll")
endif()
