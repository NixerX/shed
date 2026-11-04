cmake_minimum_required(VERSION 3.22)
project(Shed VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at your JUCE checkout:  cmake -B build -DJUCE_DIR=/path/to/JUCE
# If not set, we expect JUCE as a subfolder (git clone https://github.com/juce-framework/JUCE.git)
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE")
add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)

option(SHED_JACK "Enable JACK audio on Linux (needs libjack-jackd2-dev)" OFF)

juce_add_gui_app(Shed
    PRODUCT_NAME "Shed"
    COMPANY_NAME "JCS"
    BUNDLE_ID com.jcs.shed
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Shed needs audio input to record.")

juce_generate_juce_header(Shed)

target_sources(Shed PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/ArrangeView.cpp
    Source/PianoRoll.cpp
    Source/MainComponent.cpp)

target_compile_definitions(Shed PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Shed,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Shed,JUCE_VERSION>"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_AU=1
    JUCE_USE_LAME_AUDIO_FORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(SHED_JACK)
    target_compile_definitions(Shed PRIVATE JUCE_JACK=1)
endif()

target_link_libraries(Shed PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
