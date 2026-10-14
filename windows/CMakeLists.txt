cmake_minimum_required(VERSION 3.24)

project(MediaDuet VERSION 1.3.0 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "MediaDuet for Windows requires MSVC (C++/WinRT, Windows SDK).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MD_BUILD_TESTS "Build unit tests" ON)
option(MD_CORE_ONLY "Build only the core library and tests" OFF)
option(MD_BUILD_APP "Build the host executable" ON)

set(MD_COMMON_DEFS
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000008
  MD_VERSION="${PROJECT_VERSION}" MD_VERSION_W=L"${PROJECT_VERSION}")
set(MD_COMMON_OPTS /utf-8 /W4 /permissive- /Zc:__cplusplus /bigobj /MP /wd4100)

function(md_configure target)
  target_compile_definitions(${target} PRIVATE ${MD_COMMON_DEFS})
  target_compile_options(${target} PRIVATE ${MD_COMMON_OPTS})
endfunction()

# ---------------------------------------------------------------------------
# Core: protocol, crypto (BCrypt), link/session logic, policy, lyrics parsing.
# No WinRT or UI dependencies, so it is fully unit-testable.
# ---------------------------------------------------------------------------
add_library(md_core STATIC
  core/src/util/Bytes.cpp
  core/src/util/Log.cpp
  core/src/util/Text.cpp
  core/src/util/AppVersion.cpp
  core/src/crypto/Crypto.cpp
  core/src/protocol/Frame.cpp
  core/src/protocol/Envelope.cpp
  core/src/protocol/Messages.cpp
  core/src/link/SecureChannel.cpp
  core/src/link/Handshake.cpp
  core/src/link/Connection.cpp
  core/src/link/LinkSession.cpp
  core/src/link/ConnectionManager.cpp
  core/src/link/DedupWindow.cpp
  core/src/policy/PlaybackArbiter.cpp
  core/src/media/MediaTypes.cpp
  core/src/media/ArtworkPolicy.cpp
  core/src/lyrics/Lrc.cpp
  core/src/lyrics/Karaoke.cpp
  core/src/lyrics/LyricsClock.cpp
  core/src/overlay/SurfacePolicy.cpp
  core/src/settings/Settings.cpp
)
target_include_directories(md_core PUBLIC core/include third_party)
target_link_libraries(md_core PUBLIC bcrypt)
md_configure(md_core)

if(NOT MD_CORE_ONLY)
# ---------------------------------------------------------------------------
# Platform: WinRT/Win32 adapters (Bluetooth, LAN, GSMTC, SMTC, Core Audio,
# AudioPlaybackConnection, lyrics providers, Direct2D overlay).
# ---------------------------------------------------------------------------
add_library(md_platform STATIC
  platform/Scheduler.cpp
  platform/AppIdentity.cpp
  platform/SecureStore.cpp
  platform/Stores.cpp
  platform/bluetooth/RfcommTransport.cpp
  platform/bluetooth/BluetoothWatcher.cpp
  platform/network/StreamTransport.cpp
  platform/network/LanTransport.cpp
  platform/network/DnssdBrowser.cpp
  platform/media/GsmtcSource.cpp
  platform/media/SmtcBridge.cpp
  platform/media/Artwork.cpp
  platform/media/ArtworkLookup.cpp
  platform/media/NeteaseCompat.cpp
  platform/audio/AudioActivityMonitor.cpp
  platform/audio/A2dpSink.cpp
  platform/audio/LoopbackSender.cpp
  platform/audio/VolumeControl.cpp
  platform/lyrics/LyricsService.cpp
  platform/overlay/OverlayCommon.cpp
  platform/overlay/LyricsOverlay.cpp
  platform/overlay/TaskbarWidget.cpp
)
target_include_directories(md_platform PUBLIC platform)
target_link_libraries(md_platform PUBLIC md_core windowsapp ole32 oleaut32 user32 gdi32 propsys
  shell32 shlwapi crypt32 advapi32 d2d1 dwrite d3d11 dxgi dcomp windowscodecs ws2_32 psapi
  dwmapi uxtheme mmdevapi uiautomationcore comctl32)
md_configure(md_platform)

# ---------------------------------------------------------------------------
# Host executable (tray app, IPC server, orchestration).
# ---------------------------------------------------------------------------
if(MD_BUILD_APP)
add_executable(MediaDuet WIN32
  app/main.cpp
  app/HostApp.cpp
  app/TrayIcon.cpp
  app/IpcServer.cpp
  app/Autostart.cpp
  app/Strings.cpp
  app/Diagnostics.cpp
  app/resources/MediaDuet.rc
)
target_link_libraries(MediaDuet PRIVATE md_platform)
md_configure(MediaDuet)
set_target_properties(MediaDuet PROPERTIES LINK_FLAGS "/MANIFEST:NO")
# Rebuild the resource script when the generated icons or the manifest change.
set_source_files_properties(app/resources/MediaDuet.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/app/resources/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/app/resources/tray_on.ico;${CMAKE_CURRENT_SOURCE_DIR}/app/resources/tray_off.ico;${CMAKE_CURRENT_SOURCE_DIR}/app/resources/MediaDuet.manifest")
# Icon used for the app identity (media flyout / notifications).
add_custom_command(TARGET MediaDuet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  ${CMAKE_CURRENT_SOURCE_DIR}/app/resources/app_512.png $<TARGET_FILE_DIR:MediaDuet>/MediaDuet.png)
endif()

# Capability probe tool (Phase 0 checks, also useful for users' bug reports).
add_executable(MediaDuetProbe tools/probe/probe.cpp)
target_link_libraries(MediaDuetProbe PRIVATE windowsapp ole32 user32 mmdevapi)
md_configure(MediaDuetProbe)

# Developer tool: visual check of the lyrics overlay animation (not shipped).
add_executable(MediaDuetOverlayDemo WIN32 tools/overlay_demo/overlay_demo.cpp)
target_link_libraries(MediaDuetOverlayDemo PRIVATE md_platform)
md_configure(MediaDuetOverlayDemo)

# Test fixture: silent SMTC player for end-to-end tests (not shipped).
add_executable(MediaDuetTestPlayer WIN32 tools/test_smtc_player/test_smtc_player.cpp)
target_link_libraries(MediaDuetTestPlayer PRIVATE windowsapp ole32 user32 shell32)
md_configure(MediaDuetTestPlayer)

# Developer tool: measures lyrics hit rate (not shipped).
add_executable(MediaDuetLyricsTest tools/lyrics_test/lyrics_test.cpp)
target_link_libraries(MediaDuetLyricsTest PRIVATE md_platform)
md_configure(MediaDuetLyricsTest)
endif()

if(MD_BUILD_TESTS)
  enable_testing()
  add_executable(md_tests
    tests/test_main.cpp
    tests/test_crypto_vectors.cpp
    tests/test_frame.cpp
    tests/test_envelope.cpp
    tests/test_handshake.cpp
    tests/test_link_session.cpp
    tests/test_connection_manager.cpp
    tests/test_arbiter.cpp
    tests/test_lrc.cpp
    tests/test_settings.cpp
    tests/test_app_version.cpp
    tests/test_media.cpp
    tests/test_artwork_policy.cpp
    tests/test_karaoke.cpp
    tests/test_lyrics_clock.cpp
    tests/test_surface_policy.cpp
  )
  target_link_libraries(md_tests PRIVATE md_core)
  target_compile_definitions(md_tests PRIVATE
    MD_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/test-vectors"
    DOCTEST_CONFIG_DOUBLE_STRINGIFY)
  md_configure(md_tests)
  add_test(NAME md_tests COMMAND md_tests)
endif()
