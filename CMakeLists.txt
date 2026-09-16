cmake_minimum_required(VERSION 3.22)

project(OrchDelay VERSION 0.1.0)

# Build-timestamp label (see cmake/GenerateOrchDelayBuildInfo.cmake): a plain
# custom target with no OUTPUT/DEPENDS tracking, so it reruns and rewrites
# the header on every single build, not just every configure or only when a
# source file happens to change.
set(OrchDelayGeneratedDir "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${OrchDelayGeneratedDir}")

add_custom_target(OrchDelayGenerateBuildInfo ALL
    COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT_FILE=${OrchDelayGeneratedDir}/OrchDelayBuildInfo.h"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateOrchDelayBuildInfo.cmake"
    BYPRODUCTS "${OrchDelayGeneratedDir}/OrchDelayBuildInfo.h"
    COMMENT "Stamping OrchDelay build timestamp"
)

# Path to your JUCE folder.
add_subdirectory("C:/JUCE/JUCE" JUCE)

juce_add_plugin(OrchDelay
    COMPANY_NAME "JohnPascu"
    PLUGIN_MANUFACTURER_CODE Jpas
    PLUGIN_CODE Odly

    FORMATS VST3 Standalone

    PRODUCT_NAME "OrchDelay"
    PLUGIN_NAME "OrchDelay"
    BUNDLE_ID "com.johnpascu.OrchDelay"

    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_REPLACES_VST2 FALSE

    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(OrchDelay)

target_sources(OrchDelay
    PRIVATE
        Source/OrchDelayLogic.cpp
        Source/OrchDelayProcessor.cpp
        Source/OrchDelayEditor.cpp
        Source/OrchDelayLink.cpp
)

add_dependencies(OrchDelay OrchDelayGenerateBuildInfo)
target_include_directories(OrchDelay PRIVATE ${OrchDelayGeneratedDir})

target_compile_definitions(OrchDelay
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(OrchDelay
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# JUCE 9 VST3 wrapper warning control.
if (TARGET OrchDelay_VST3)
    target_compile_definitions(OrchDelay_VST3
        PRIVATE
            JUCE_VST3_CAN_REPLACE_VST2=0
    )
endif()

# Developer-only verification target for the hold/transform/propose logic.
# Pure: no GUI, no plugin host, just odly:: operating on plain data.
juce_add_console_app(OrchDelayLogicCheck
    PRODUCT_NAME "OrchDelayLogicCheck"
)

juce_generate_juce_header(OrchDelayLogicCheck)

target_sources(OrchDelayLogicCheck
    PRIVATE
        Tools/OrchDelayLogicCheck.cpp
        Source/OrchDelayLogic.cpp
)

target_link_libraries(OrchDelayLogicCheck
    PRIVATE
        juce::juce_audio_basics

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_include_directories(OrchDelayLogicCheck
    PRIVATE
        Source
)
