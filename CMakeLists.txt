cmake_minimum_required(VERSION 3.22)

project(Cadenza VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CADENZA_BUILD_TESTS "Build Cadenza unit tests" ON)

# ------------------------------------------------------------
# Optional FluidSynth. If not found, AudioEngine uses
# NullSynthEngine (logs notes, emits no sound). The build always works.
# ------------------------------------------------------------
find_package(FluidSynth QUIET)
if(NOT FluidSynth_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FLUIDSYNTH QUIET IMPORTED_TARGET fluidsynth)
    endif()
endif()

if(FluidSynth_FOUND OR FLUIDSYNTH_FOUND)
    message(STATUS "Cadenza: FluidSynth available - real synth enabled")
    set(CADENZA_HAVE_FLUIDSYNTH ON)
else()
    message(STATUS "Cadenza: FluidSynth NOT found - using NullSynthEngine fallback")
    set(CADENZA_HAVE_FLUIDSYNTH OFF)
endif()

# ============================================================
# cadenza_core : pure C++ library, no JUCE, no FluidSynth.
# ============================================================
add_library(cadenza_core STATIC
    Source/ApplicationState.cpp
    Source/BridgeRouter.cpp
    Source/Json/Json.cpp
    Source/Midi/ChordRecognizer.cpp
    Source/Midi/ChordTypes.cpp
    Source/Midi/ArrangerMidiRouter.cpp
    Source/Midi/LiveMelodyVoice.cpp
    Source/Midi/RightHand.cpp
    Source/Midi/MidiControlMap.cpp
    Source/Midi/GmInstruments.cpp
    Source/Arranger/Style.cpp
    Source/Arranger/PatternNoteMerge.cpp
    Source/Arranger/StyleLibraryIndex.h
    Source/Arranger/StyleLibraryIndex.cpp
    Source/Arranger/StyleLoader.cpp
    Source/Arranger/StyParser.cpp
    Source/Arranger/PatternTransposer.cpp
    Source/Arranger/DrumKeyMap.cpp
    Source/Arranger/StyleRecorder.cpp
    Source/Arranger/RuntimePlayback.cpp
    Source/Arranger/PlaybackDiagnostics.cpp
    Source/Arranger/Song.cpp
    Source/Arranger/SongLoader.cpp
    Source/Arranger/SongPlayer.cpp
    Source/Arranger/SectionButtons.cpp
    Source/Arranger/SectionFlow.cpp
    Source/Arranger/SectionChangeQueue.cpp
    Source/Arranger/StyleChangeQueue.cpp
    Source/Audio/MidiChannel.cpp
    Source/Audio/MixerModel.cpp
    Source/Audio/MasterCompressor.cpp
    Source/Audio/PolishedMaster.cpp
    Source/Audio/VoiceMap.cpp
    Source/Audio/Transport.cpp
    Source/Audio/TransportCommandMailbox.cpp
    Source/Audio/ChordAnalysis.cpp
    Source/Settings/SettingsStore.cpp
    Source/UI/StylePartBarWorkflow.cpp
    Source/UI/StylePartNoteWorkflow.cpp
    Source/UI/StylePartPianoRollGeometry.cpp
    Source/UI/FirstLaunchChecklist.cpp
)

target_include_directories(cadenza_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

# ============================================================
# Tests
# ============================================================
if(CADENZA_BUILD_TESTS)
    enable_testing()

    function(cadenza_add_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE cadenza_core)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    cadenza_add_test(cadenza_core_tests        tests/BridgeRouterTests.cpp)
    cadenza_add_test(cadenza_json_tests        tests/JsonTests.cpp)
    cadenza_add_test(cadenza_chord_tests       tests/ChordRecognizerTests.cpp)
    cadenza_add_test(cadenza_style_tests       tests/StyleLoaderTests.cpp)
    cadenza_add_test(cadenza_transpose_tests   tests/PatternTransposerTests.cpp)
    cadenza_add_test(cadenza_style_sound_correctness_tests tests/StyleSoundCorrectnessTests.cpp)
    cadenza_add_test(cadenza_transport_tests   tests/TransportTests.cpp)
    cadenza_add_test(cadenza_audioblock_tests  tests/AudioBlockPipelineTests.cpp)
    cadenza_add_test(cadenza_runtime_tests     tests/RuntimePlaybackTests.cpp)
    cadenza_add_test(cadenza_drum_keymap_tests tests/DrumKeyMapTests.cpp)
    cadenza_add_test(cadenza_octave_tests      tests/OctaveRoutingTests.cpp)
    cadenza_add_test(cadenza_livemelody_tests  tests/LiveMelodyVoiceTests.cpp)
    cadenza_add_test(cadenza_righthand_tests    tests/RightHandTests.cpp)
    cadenza_add_test(cadenza_midimap_tests      tests/MidiControlMapTests.cpp)
    cadenza_add_test(cadenza_sections_tests     tests/SectionButtonsTests.cpp)
    cadenza_add_test(cadenza_section_flow_tests tests/SectionFlowTests.cpp)
    cadenza_add_test(cadenza_section_queue_tests tests/SectionChangeQueueTests.cpp)
    cadenza_add_test(cadenza_style_queue_tests tests/StyleChangeQueueTests.cpp)
    cadenza_add_test(cadenza_transport_command_tests tests/TransportCommandMailboxTests.cpp)
    cadenza_add_test(cadenza_mixer_tests        tests/MixerModelTests.cpp)
    cadenza_add_test(cadenza_compressor_tests   tests/MasterCompressorTests.cpp)
    cadenza_add_test(cadenza_voicemap_tests     tests/VoiceMapTests.cpp)
    cadenza_add_test(cadenza_gm_tests           tests/GmInstrumentsTests.cpp)
    cadenza_add_test(cadenza_settings_tests    tests/SettingsStoreTests.cpp)
    cadenza_add_test(cadenza_song_tests        tests/SongLoaderTests.cpp)
    cadenza_add_test(cadenza_songplayer_tests  tests/SongPlayerTests.cpp)
    cadenza_add_test(cadenza_arranger_router_tests tests/ArrangerMidiRouterTests.cpp)
    cadenza_add_test(cadenza_ots_tests         tests/OtsRecallTests.cpp)
    cadenza_add_test(cadenza_recorder_tests    tests/StyleRecorderTests.cpp)
    cadenza_add_test(cadenza_part_editor_tests tests/StylePartPianoRollTests.cpp)
    cadenza_add_test(cadenza_part_workflow_tests tests/StylePartBarWorkflowTests.cpp)
    cadenza_add_test(cadenza_note_workflow_tests tests/StylePartNoteWorkflowTests.cpp)
    cadenza_add_test(cadenza_audio_chord_tests tests/AudioChordAnalysisTests.cpp)
    cadenza_add_test(cadenza_first_launch_tests tests/FirstLaunchChecklistTests.cpp)
    cadenza_add_test(cadenza_polished_master_tests tests/PolishedMasterTests.cpp)
endif()

# ============================================================
# tools/sty-converter — standalone CLI that converts Yamaha .sty
# (or plain .mid) files into Cadenza's .cstyle JSON format.
# Pure C++, depends only on cadenza_core.
# ============================================================
add_executable(sty-to-cstyle
    tools/sty-converter/main.cpp
)
target_link_libraries(sty-to-cstyle PRIVATE cadenza_core)

add_executable(style-probe
    tools/style-probe/main.cpp
)
target_link_libraries(style-probe PRIVATE cadenza_core)

add_executable(ab-transpose
    tools/ab-transpose/main.cpp
)
target_link_libraries(ab-transpose PRIVATE cadenza_core)

add_executable(style-scan
    tools/style-scan/main.cpp
)
target_link_libraries(style-scan PRIVATE cadenza_core)
if(MSVC)
    # /EHa lets _set_se_translator turn native faults (access violations in the
    # parser on a malformed .sty) into catchable C++ exceptions so the batch
    # scan survives a bad file instead of aborting the whole run.
    target_compile_options(style-scan PRIVATE /EHa)
endif()

if(CADENZA_BUILD_TESTS)
    add_test(
        NAME style_probe_separate_drum_channels
        COMMAND style-probe
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/style-probe-shared-drums.cstyle"
            C
            mainA
            1
    )
    # RHY1 (main kit) and RHY2 (sub-rhythm) each get their own drum channel so
    # they have independent mixer strips: drums -> playCh 10, rhythm2 -> playCh 9.
    set_tests_properties(style_probe_separate_drum_channels PROPERTIES
        PASS_REGULAR_EXPRESSION "part 'rhythm2' srcCh=9 role=rhythm2 playCh=9"
        FAIL_REGULAR_EXPRESSION "rhythm2 srcCh=9 role=rhythm2 playCh=10"
    )

    add_test(
        NAME style_probe_stable_note_names
        COMMAND style-probe
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/style-probe-note-range.cstyle"
            C
            mainA
            2
    )
    set_tests_properties(style_probe_stable_note_names PROPERTIES
        PASS_REGULAR_EXPRESSION "played range A1\\(33\\)..E3\\(52\\)"
    )

    add_test(
        NAME style_probe_diagnostics_none
        COMMAND style-probe
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/style-probe-note-range.cstyle"
            C
            mainA
            1
    )
    set_tests_properties(style_probe_diagnostics_none PROPERTIES
        PASS_REGULAR_EXPRESSION "Diagnostics: none"
    )

    add_test(
        NAME style_probe_diagnostics_one_warning
        COMMAND style-probe
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/style-probe-one-warning.cstyle"
            C
            mainA
            1
    )
    set_tests_properties(style_probe_diagnostics_one_warning PROPERTIES
        PASS_REGULAR_EXPRESSION "Diagnostics: 1 warning"
    )

    add_test(
        NAME style_probe_diagnostics_plural_warnings
        COMMAND style-probe
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/style-probe-two-warnings.cstyle"
            C
            mainA
            1
    )
    set_tests_properties(style_probe_diagnostics_plural_warnings PROPERTIES
        PASS_REGULAR_EXPRESSION "Diagnostics: 2 warnings"
    )

    add_executable(cadenza_sty_tests
        tools/sty-converter/StyParserTests.cpp
    )
    target_link_libraries(cadenza_sty_tests PRIVATE cadenza_core)
    add_test(NAME cadenza_sty_tests COMMAND cadenza_sty_tests)
endif()

# ============================================================
# Native JUCE GUI app (built only if lib/JUCE is checked out).
# ============================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/JUCE/CMakeLists.txt")
    add_subdirectory(lib/JUCE)

    # ----- plugin-probe: headless CLI to verify VST3 hosting works -----
    juce_add_console_app(plugin-probe PRODUCT_NAME "plugin-probe")
    target_sources(plugin-probe PRIVATE tools/plugin-probe/main.cpp)
    target_compile_definitions(plugin-probe PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1)   # runDispatchLoopUntil while samplers load
    target_link_libraries(plugin-probe PRIVATE
        juce::juce_audio_processors_headless
        juce::juce_recommended_config_flags)

    juce_add_gui_app(Cadenza
        PRODUCT_NAME "Cadenza Workstation"
        COMPANY_NAME "Cadenza"
        NEEDS_CURL TRUE
    )

    target_sources(Cadenza
        PRIVATE
            Source/Main.cpp
            Source/MainComponent.cpp
            Source/Audio/SynthEngine.cpp
            Source/Audio/Metronome.cpp
            Source/Audio/AudioEngine.cpp
            Source/Audio/MasterEq.cpp
            Source/Audio/MasterGlue.cpp
            Source/Audio/PluginHost.cpp
            Source/Midi/MidiRouter.cpp
            Source/Arranger/MidiStyleConverter.cpp
            Source/Arranger/StyleEngine.cpp
            Source/UI/CadenzaLookAndFeel.cpp
            Source/UI/NativePanel.cpp
            Source/UI/FirstLaunchAssistant.cpp
            Source/UI/StylePartEditor.cpp
            Source/UI/StylePartPatternImport.cpp
            Source/UI/StylePartPianoRoll.cpp
            Source/Ai/StyleGenerator.cpp
    )

    target_compile_definitions(Cadenza
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_PLUGINHOST_VST3=1
    )

    # Optional ASIO (low-latency, bypasses the Windows audio stack). The Steinberg
    # ASIO SDK is proprietary and cannot be bundled, so ASIO turns on automatically
    # only when the SDK is present. Drop it at lib/asiosdk, or pass -DASIO_SDK_DIR=<path>.
    set(ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK root (contains common/iasiodrv.h)")
    if(NOT ASIO_SDK_DIR)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/asiosdk/common/iasiodrv.h")
            set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/asiosdk")
        else()
            # Find the SDK anywhere under lib/ regardless of the extracted folder name.
            file(GLOB_RECURSE _asio_hdr "${CMAKE_CURRENT_SOURCE_DIR}/lib/*/common/iasiodrv.h")
            if(_asio_hdr)
                list(GET _asio_hdr 0 _asio_first)
                get_filename_component(_asio_common "${_asio_first}" DIRECTORY)
                get_filename_component(ASIO_SDK_DIR "${_asio_common}" DIRECTORY)
            endif()
        endif()
    endif()
    if(MSVC AND ASIO_SDK_DIR AND EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
        message(STATUS "Cadenza: ASIO SDK found (${ASIO_SDK_DIR}) -> ASIO ENABLED")
        target_compile_definitions(Cadenza PRIVATE JUCE_ASIO=1)
        target_include_directories(Cadenza PRIVATE
            "${ASIO_SDK_DIR}/common"
            "${ASIO_SDK_DIR}/host"
            "${ASIO_SDK_DIR}/host/pc")
    else()
        message(STATUS "Cadenza: ASIO SDK not found -> ASIO disabled "
                       "(set ASIO_SDK_DIR or place it at lib/asiosdk to enable)")
    endif()

    if(CADENZA_HAVE_FLUIDSYNTH)
        target_compile_definitions(Cadenza PRIVATE CADENZA_HAVE_FLUIDSYNTH=1)
        if(FluidSynth_FOUND)
            target_link_libraries(Cadenza PRIVATE FluidSynth::libfluidsynth)
        elseif(FLUIDSYNTH_FOUND)
            target_link_libraries(Cadenza PRIVATE PkgConfig::FLUIDSYNTH)
        endif()
    endif()

    target_link_libraries(Cadenza
        PRIVATE
            cadenza_core
            juce::juce_audio_utils
            juce::juce_audio_devices
            juce::juce_audio_formats
            juce::juce_audio_processors
            juce::juce_gui_extra

    )

    if(CADENZA_BUILD_TESTS)
        juce_add_console_app(cadenza_pattern_import_tests
            PRODUCT_NAME "cadenza_pattern_import_tests"
            NEEDS_CURL FALSE
        )
        target_sources(cadenza_pattern_import_tests PRIVATE
            tests/StylePartPatternImportTests.cpp
            Source/UI/StylePartPatternImport.cpp
        )
        target_link_libraries(cadenza_pattern_import_tests
            PRIVATE
                cadenza_core
                juce::juce_audio_basics
        )
        add_test(NAME cadenza_pattern_import_tests
                 COMMAND cadenza_pattern_import_tests)

        juce_add_console_app(cadenza_midi_style_converter_tests
            PRODUCT_NAME "cadenza_midi_style_converter_tests"
            NEEDS_CURL FALSE
        )
        target_sources(cadenza_midi_style_converter_tests PRIVATE
            tests/MidiStyleConverterTests.cpp
            Source/Arranger/MidiStyleConverter.cpp
        )
        target_link_libraries(cadenza_midi_style_converter_tests
            PRIVATE
                cadenza_core
                juce::juce_audio_basics
        )
        add_test(NAME cadenza_midi_style_converter_tests
                 COMMAND cadenza_midi_style_converter_tests)

        juce_add_console_app(cadenza_ai_style_validation_tests
            PRODUCT_NAME "cadenza_ai_style_validation_tests"
            NEEDS_CURL TRUE
        )
        target_sources(cadenza_ai_style_validation_tests PRIVATE
            tests/AiStyleValidationTests.cpp
            Source/Ai/StyleGenerator.cpp
        )
        target_link_libraries(cadenza_ai_style_validation_tests
            PRIVATE
                cadenza_core
                juce::juce_core
        )
        add_test(NAME cadenza_ai_style_validation_tests
                 COMMAND cadenza_ai_style_validation_tests)
    endif()

    add_custom_command(TARGET Cadenza POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:Cadenza>/resources"
        COMMENT "Copying Cadenza resources next to the executable"
    )
else()
    message(WARNING "lib/JUCE was not found, so the native host target is skipped. Clone JUCE into lib/JUCE to build Cadenza.exe.")
endif()
