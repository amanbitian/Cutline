cmake_minimum_required(VERSION 3.24)

project(Cutline VERSION 0.1.0 LANGUAGES C CXX)

option(CUTLINE_BUILD_NATIVE_APP "Build the optional Qt Quick desktop host" ON)
option(CUTLINE_BUILD_TESTS "Build Cutline native unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Cutline's own code is built warning-clean at a high level. Third-party code is
# exempt (see third_party/sqlite3), which is the whole reason this is a function
# applied per target rather than a global flag.
function(cutline_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

option(CUTLINE_USE_VENDORED_SQLITE "Build the in-tree SQLite amalgamation instead of a system package" ON)

if(CUTLINE_USE_VENDORED_SQLITE)
  add_subdirectory(third_party/sqlite3)
else()
  find_package(SQLite3 3.35 REQUIRED)
endif()

# FFmpeg is optional. Without it the core, the compositor, and every test still
# build and run against the synthetic media source; only reading real files is
# unavailable. CUTLINE_FFMPEG_ROOT points at a directory holding include/ and
# lib/, which is the layout of both a vcpkg install and a prebuilt package.
set(CUTLINE_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.tools/ffmpeg"
    CACHE PATH "Directory containing FFmpeg include/ and lib/")
option(CUTLINE_ENABLE_FFMPEG "Build the FFmpeg media provider when it can be found" ON)

set(CUTLINE_FFMPEG_FOUND OFF)
if(CUTLINE_ENABLE_FFMPEG)
  find_path(CUTLINE_FFMPEG_INCLUDE_DIR libavformat/avformat.h
            HINTS "${CUTLINE_FFMPEG_ROOT}/include" PATH_SUFFIXES include)
  set(CUTLINE_FFMPEG_LIBS "")
  foreach(component avformat avcodec avutil swscale swresample)
    find_library(CUTLINE_FFMPEG_${component}
                 NAMES ${component}
                 HINTS "${CUTLINE_FFMPEG_ROOT}/lib" PATH_SUFFIXES lib)
    if(CUTLINE_FFMPEG_${component})
      list(APPEND CUTLINE_FFMPEG_LIBS "${CUTLINE_FFMPEG_${component}}")
    endif()
  endforeach()
  list(LENGTH CUTLINE_FFMPEG_LIBS CUTLINE_FFMPEG_LIB_COUNT)
  if(CUTLINE_FFMPEG_INCLUDE_DIR AND CUTLINE_FFMPEG_LIB_COUNT EQUAL 5)
    set(CUTLINE_FFMPEG_FOUND ON)
    message(STATUS "FFmpeg found: ${CUTLINE_FFMPEG_INCLUDE_DIR}")
  else()
    message(STATUS "FFmpeg not found: building without file decoding (synthetic media still works)")
  endif()
endif()

add_library(cutline_core STATIC
  core/time/RationalTime.cpp
  core/anim/Keyframe.cpp
  core/db/Sql.cpp
  core/db/ChangeSet.cpp
  core/jobs/PriorityScheduler.cpp
  core/resource/QuotaManager.cpp
  core/model/Types.cpp
  core/project/Schema.cpp
  core/util/Json.cpp
  core/util/JsonParse.cpp
  core/util/XmlParse.cpp
  core/util/Sha256.cpp
  effects/EffectRegistry.cpp
  effects/GraphicsDocument.cpp
  effects/MaskDocument.cpp
  core/commands/Command.cpp
  core/commands/CommandService.cpp
  core/project/Consolidate.cpp
  core/project/ProjectStore.cpp
  media/TimestampMap.cpp
  media/VideoFrame.cpp
  media/AudioBuffer.cpp
  media/Source.cpp
  media/SyntheticSource.cpp
  media/Ingest.cpp
  media/ProxyWorkflow.cpp
  media/IngestWorkflow.cpp
  media/Writer.cpp
  exporter/ExportWorker.cpp
  exporter/ExportPresets.cpp
  exporter/ExportQueue.cpp
  exporter/ExportValidation.cpp
  captions/Captions.cpp
  speech/Transcript.cpp
  speech/Whisper.cpp
  interchange/Interchange.cpp
  interchange/Otio.cpp
  interchange/Edl.cpp
  interchange/FcpXml.cpp
  playback/DecodePool.cpp
  playback/PlaybackEngine.cpp
  render/CubeLut.cpp
  render/AngleMonitor.cpp
  render/BlendModes.cpp
  render/Compositor.cpp
  render/ColorEffects.cpp
  render/ColorManagement.cpp
  render/CaptionRender.cpp
  render/Filters.cpp
  render/FlowCache.cpp
  render/Graphics.cpp
  render/GpuDevice.cpp
  render/D3D11Compositor.cpp
  render/MeshWarp.cpp
  render/NoiseReduction.cpp
  render/OpticalFlow.cpp
  render/RenderCache.cpp
  render/RenderGraph.cpp
  render/RenderGraphBuilder.cpp
  render/RollingShutter.cpp
  render/Scopes.cpp
  render/Stabilizer.cpp
  render/TextRaster.cpp
  render/Tracking.cpp
  render/Transitions.cpp
  render/TrackingData.cpp
  timeline/TimelineCompiler.cpp
  ui/EditPlanner.cpp
  ui/Inspector.cpp
  ui/Jobs.cpp
  ui/Layout.cpp
  ui/LutLibrary.cpp
  ui/LookPack.cpp
  ui/Monitor.cpp
  ui/Preferences.cpp
  ui/MaskEditor.cpp
  ui/Multicam.cpp
  ui/TextEdit.cpp
  ui/Transitions.cpp
  ui/GraphicsDesigner.cpp
  ui/AudioWorkflow.cpp
  ui/Ramp.cpp
  ui/Shortcuts.cpp
  ui/TimelineView.cpp
  ui/Transport.cpp
  timeline/Multicam.cpp
  timeline/SequenceLoader.cpp
  audio/AudioClock.cpp
  audio/AudioMixer.cpp
  audio/TimeStretch.cpp
  audio/Dsp.cpp
  audio/AudioSink.cpp
  audio/WasapiSink.cpp
  audio/AudioCapture.cpp
)
add_library(Cutline::core ALIAS cutline_core)

target_include_directories(cutline_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(cutline_core PUBLIC SQLite::SQLite3)
target_compile_features(cutline_core PUBLIC cxx_std_20)

cutline_set_warnings(cutline_core)

# WASAPI is the Windows output backend. Without it (or on another platform) the
# offline sink is used, so playback is silent but everything else still runs.
if(WIN32)
  target_compile_definitions(cutline_core PUBLIC CUTLINE_HAVE_WASAPI)
  target_link_libraries(cutline_core PUBLIC ole32 avrt gdi32 user32 d3d11 dxgi d3dcompiler)
endif()

if(CUTLINE_FFMPEG_FOUND)
  target_sources(cutline_core PRIVATE media/ffmpeg/FFmpegSource.cpp media/ffmpeg/FFmpegWriter.cpp)
  # SYSTEM so FFmpeg's own headers do not trip Cutline's warning level.
  target_include_directories(cutline_core SYSTEM PUBLIC "${CUTLINE_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(cutline_core PUBLIC ${CUTLINE_FFMPEG_LIBS})
  target_compile_definitions(cutline_core PUBLIC CUTLINE_HAVE_FFMPEG)

  # The prebuilt package is shared, so the DLLs have to sit beside the test and
  # demo binaries for them to start at all.
  file(GLOB CUTLINE_FFMPEG_DLLS "${CUTLINE_FFMPEG_ROOT}/bin/*.dll")
  if(CUTLINE_FFMPEG_DLLS)
    file(COPY ${CUTLINE_FFMPEG_DLLS} DESTINATION "${CMAKE_BINARY_DIR}")
  endif()
endif()

# Console walkthrough of the whole native stack. Built always, because it needs
# no Qt and is the quickest way to check the core end to end.
add_executable(cutline_demo app/demo_main.cpp)
target_link_libraries(cutline_demo PRIVATE Cutline::core)
cutline_set_warnings(cutline_demo)

# Render benchmark. Built in Release for meaningful numbers; see scripts/bench.bat.
add_executable(cutline_bench app/bench_main.cpp)
target_link_libraries(cutline_bench PRIVATE Cutline::core)
cutline_set_warnings(cutline_bench)

if(CUTLINE_BUILD_NATIVE_APP)
  find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Qml Quick QuickControls2)
  if(Qt6_FOUND)
    qt_standard_project_setup(REQUIRES 6.5)
    qt_policy(SET QTP0004 NEW)
    set_source_files_properties(app/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    qt_add_executable(Cutline app/main.cpp)
    qt_add_qml_module(Cutline
      URI Cutline
      VERSION 1.0
      SOURCES
        app/Session.h
        app/Session.cpp
        app/SessionMulticam.cpp
        app/SessionExport.cpp
        app/SessionMasks.cpp
        app/SessionTranscript.cpp
        app/SessionCaptions.cpp
        app/SessionAudio.cpp
        app/SessionIngest.cpp
        app/SessionTransitions.cpp
        app/SessionGraphics.cpp
        app/MulticamItem.h
        app/MulticamItem.cpp
        app/MonitorItem.h
        app/MonitorItem.cpp
        app/ScopeItem.h
        app/ScopeItem.cpp
        app/TimelineItem.h
        app/TimelineItem.cpp
        app/DockController.h
        app/DockController.cpp
      QML_FILES
        app/qml/Main.qml
        app/qml/Theme.qml
        app/qml/Chip.qml
        app/qml/Check.qml
        app/qml/NumberBox.qml
        app/qml/ParameterRow.qml
        app/qml/LutBrowser.qml
        app/qml/DockArea.qml
        app/qml/PanelGroup.qml
        app/qml/SpeedDialog.qml
        app/qml/ColorSpaceMenu.qml
        app/qml/IngestDialog.qml
        app/qml/TransitionDialog.qml
        app/qml/SequenceColorDialog.qml
        app/qml/RampDialog.qml
        app/qml/ExportDialog.qml
        app/qml/panels/ExportsPanel.qml
        app/qml/panels/AudioMixerPanel.qml
        app/qml/panels/ScopesPanel.qml
        app/qml/panels/TranscriptPanel.qml
        app/qml/panels/AudioEssentialsPanel.qml
        app/qml/panels/CaptionsPanel.qml
        app/qml/panels/GraphicsPanel.qml
        app/qml/panels/GraphicDesignerPanel.qml
        app/qml/MulticamSetupDialog.qml
        app/qml/panels/MulticamPanel.qml
        app/qml/CommandPalette.qml
        app/qml/ShortcutsDialog.qml
        app/qml/PreferencesDialog.qml
        app/qml/panels/PlaceholderPanel.qml
        app/qml/panels/ProjectPanel.qml
        app/qml/panels/MonitorPanel.qml
        app/qml/panels/TimelinePanel.qml
        app/qml/panels/InspectorPanel.qml
        app/qml/panels/EffectsPanel.qml
        app/qml/panels/HistoryPanel.qml
        app/qml/panels/JobsPanel.qml
    )
    target_include_directories(Cutline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
    target_link_libraries(Cutline PRIVATE Cutline::core Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    cutline_set_warnings(Cutline)

    # The window's own tests drive the same C++ classes the QML uses (Session, the timeline and monitor items, the dock
    # controller) with simulated mouse and keys on an off-screen window. They need no QML module.
    if(BUILD_TESTING AND CUTLINE_BUILD_TESTS)
      find_package(Qt6 6.5 QUIET COMPONENTS Test)
      if(Qt6Test_FOUND)
        qt_add_executable(cutline_app_tests
          tests/app/app_tests.cpp
          app/Session.cpp
          app/SessionMulticam.cpp
        app/SessionExport.cpp
          app/SessionMasks.cpp
          app/SessionTranscript.cpp
          app/SessionCaptions.cpp
          app/SessionAudio.cpp
          app/SessionIngest.cpp
          app/SessionTransitions.cpp
          app/SessionGraphics.cpp
          app/MulticamItem.cpp
          app/MonitorItem.cpp
          app/TimelineItem.cpp
          app/DockController.cpp
        )
        target_include_directories(cutline_app_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
        target_link_libraries(cutline_app_tests PRIVATE Cutline::core Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
        cutline_set_warnings(cutline_app_tests)
        add_test(NAME cutline_app_tests COMMAND cutline_app_tests)
        set_tests_properties(cutline_app_tests PROPERTIES ENVIRONMENT
          "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_FONTDIR=C:/Windows/Fonts;CUTLINE_GOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden;CUTLINE_FIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/fixtures")
        if(WIN32)
          # CTest starts the app test directly, outside build-app.bat, so make the matching Qt runtime and
          # platform plugin discoverable without requiring a developer command prompt or a hand-edited PATH.
          set_tests_properties(cutline_app_tests PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
      endif()
    endif()
  else()
    message(STATUS "Qt 6.5+ was not found: Cutline desktop host is disabled; native core and tests remain available.")
  endif()
endif()

include(CTest)
if(BUILD_TESTING AND CUTLINE_BUILD_TESTS)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Fixtures.cmake")
  cutline_generate_fixtures("${CMAKE_CURRENT_BINARY_DIR}/fixtures")

  # One place to declare a test binary. CUTLINE_GOLDEN_DIR points the
  # golden-image tests at the source tree so they can run from any build
  # directory, and CUTLINE_FIXTURE_DIR does the same for generated media.
  function(cutline_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Cutline::core)
    cutline_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT
      "CUTLINE_GOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden;CUTLINE_FIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  endfunction()

  cutline_add_test(cutline_core_tests tests/native/core_tests.cpp)
  cutline_add_test(cutline_store_tests tests/native/store_tests.cpp)
  cutline_add_test(cutline_timeline_tests tests/native/timeline_tests.cpp)
  cutline_add_test(cutline_media_tests tests/native/media_tests.cpp)
  cutline_add_test(cutline_render_tests tests/native/render_tests.cpp)
  cutline_add_test(cutline_audio_tests tests/native/audio_tests.cpp)
  cutline_add_test(cutline_playback_tests tests/native/playback_tests.cpp)
  cutline_add_test(cutline_export_tests tests/native/export_tests.cpp)
  cutline_add_test(cutline_interchange_tests tests/native/interchange_tests.cpp)
  cutline_add_test(cutline_ui_tests tests/native/ui_tests.cpp)
  cutline_add_test(cutline_gpu_tests tests/native/gpu_tests.cpp)
  cutline_add_test(cutline_speech_tests tests/native/speech_tests.cpp)
endif()
