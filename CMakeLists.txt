cmake_minimum_required(VERSION 3.25)

project(OpenScope
    VERSION 0.9.6
    LANGUAGES C CXX
)

set(DECKLINK_SDK_ROOT
    ""
    CACHE PATH
    "Path to the Blackmagic DeckLink SDK"
)

if(NOT DECKLINK_SDK_ROOT)
    message(FATAL_ERROR
        "DECKLINK_SDK_ROOT is not set. "
        "Set it in CMakeUserPresets.json or via -DDECKLINK_SDK_ROOT=..."
    )
endif()

if(NOT EXISTS "${DECKLINK_SDK_ROOT}/Win/include/DeckLinkAPI.idl")
    message(FATAL_ERROR
        "DeckLinkAPI.idl not found below: ${DECKLINK_SDK_ROOT}"
    )
endif()


set(ASIO_SDK_ROOT "" CACHE PATH "Path to Steinberg ASIO SDK")

if(NOT ASIO_SDK_ROOT AND DEFINED ENV{ASIO_SDK_ROOT})
    file(TO_CMAKE_PATH "$ENV{ASIO_SDK_ROOT}" _asio_env_root)
    if(EXISTS "${_asio_env_root}/common/asio.cpp")
        set(ASIO_SDK_ROOT "${_asio_env_root}" CACHE PATH "Path to Steinberg ASIO SDK" FORCE)
    endif()
endif()

if(NOT ASIO_SDK_ROOT)
    set(_asio_candidates
        "C:/SDK/ASIOSDK"
        "C:/SDK/asiosdk"
        "C:/Dev/ASIOSDK"
        "C:/Dev/asiosdk"
        "${CMAKE_CURRENT_SOURCE_DIR}/ASIOSDK"
        "${CMAKE_CURRENT_SOURCE_DIR}/../ASIOSDK"
        "$ENV{USERPROFILE}/Documents/ASIOSDK"
        "$ENV{USERPROFILE}/Downloads/ASIOSDK"
    )
    foreach(_candidate IN LISTS _asio_candidates)
        file(TO_CMAKE_PATH "${_candidate}" _candidate_norm)
        if(EXISTS "${_candidate_norm}/common/asio.cpp" AND
           EXISTS "${_candidate_norm}/host/asiodrivers.cpp" AND
           EXISTS "${_candidate_norm}/host/pc/asiolist.cpp")
            set(ASIO_SDK_ROOT "${_candidate_norm}" CACHE PATH "Path to Steinberg ASIO SDK" FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT ASIO_SDK_ROOT)
    message(FATAL_ERROR
        "Steinberg ASIO SDK was not found. Set ASIO_SDK_ROOT to the SDK root containing common/, host/ and host/pc/.")
endif()

message(STATUS "Using ASIO SDK: ${ASIO_SDK_ROOT}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Spout2 CONFIG REQUIRED)

# Prefer the static DirectX Spout target so OpenScope does not gain
# an extra runtime DLL. Fall back to the dynamic target when needed.
if(TARGET Spout2::SpoutDX_static)
    set(OPENSCOPE_SPOUT_TARGET Spout2::SpoutDX_static)
elseif(TARGET Spout2::SpoutDX)
    set(OPENSCOPE_SPOUT_TARGET Spout2::SpoutDX)
else()
    message(FATAL_ERROR
        "Spout2 was found, but no SpoutDX target is available. "
        "Install vcpkg port spout2[dx]."
    )
endif()

qt_standard_project_setup()

set(OPENSCOPE_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

file(MAKE_DIRECTORY
    "${OPENSCOPE_GENERATED_DIR}"
)

set(OPENSCOPE_DELTA 0)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in"
    "${OPENSCOPE_GENERATED_DIR}/Version.h"
    @ONLY
)

if(WIN32)
    set(OPENSCOPE_ICON_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/OpenScope.ico"
    )

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenScopeAppIcon.rc.in"
        "${OPENSCOPE_GENERATED_DIR}/OpenScopeAppIcon.rc"
        @ONLY
    )
endif()

set(DECKLINK_IDL
    "${DECKLINK_SDK_ROOT}/Win/include/DeckLinkAPI.idl"
)

set(DECKLINK_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/decklink_generated"
)

set(DECKLINK_GENERATED_HEADER
    "${DECKLINK_GENERATED_DIR}/DeckLinkAPI_h.h"
)

set(DECKLINK_GENERATED_IID
    "${DECKLINK_GENERATED_DIR}/DeckLinkAPI_i.c"
)

add_custom_command(
    OUTPUT
        "${DECKLINK_GENERATED_HEADER}"
        "${DECKLINK_GENERATED_IID}"

    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${DECKLINK_GENERATED_DIR}"

    COMMAND midl
        /nologo
        /env x64
        /h DeckLinkAPI_h.h
        /iid DeckLinkAPI_i.c
        /out "${DECKLINK_GENERATED_DIR}"
        "${DECKLINK_IDL}"

    DEPENDS
        "${DECKLINK_IDL}"

    VERBATIM
)

add_custom_target(DeckLinkApiGenerated
    DEPENDS
        "${DECKLINK_GENERATED_HEADER}"
        "${DECKLINK_GENERATED_IID}"
)

qt_add_executable(OpenScope
src/main.cpp
resources/OpenScope.qrc

src/MainWindow.h
src/MainWindow.cpp
src/ScopeViewport.h
src/ScopeViewport.cpp
src/ScopeWorkspace.h
src/ScopeWorkspace.cpp
src/VectorscopeSettings.h
src/TestPatternGenerator.h
src/TestPatternGenerator.cpp
src/VideoEngine.h
src/VideoEngine.cpp
src/VideoSource.h
src/VideoSource.cpp

src/diagnostics/TraceLogger.h
src/diagnostics/TraceLogger.cpp

    src/sources/philips/PhilipsPatternRomDecoder.h
    src/sources/philips/PhilipsPatternRomDecoder.cpp
    src/sources/philips/PhilipsPatternRomSource.h
    src/sources/philips/PhilipsPatternRomSource.cpp
src/DeckLinkProbe.h
src/DeckLinkProbe.cpp
src/DeckLinkInputCallback.h
src/DeckLinkInputCallback.cpp

src/analysis/Analyzer.h
src/analysis/VectorscopeAnalyzer.h
src/analysis/VectorscopeAnalyzer.cpp
src/analysis/WssDecoder.h
src/analysis/WssDecoder.cpp
src/analysis/PcmVideoDecoder.h
src/analysis/PcmVideoDecoder.cpp
src/analysis/HamPcmV2Decoder.h
src/analysis/HamPcmV2Decoder.cpp

src/output/SpoutOutput.h
src/output/SpoutOutput.cpp
src/output/WasapiPcmOutput.h
src/output/WasapiPcmOutput.cpp
src/output/HamWasapiPcmOutput.h
src/output/HamWasapiPcmOutput.cpp
src/output/AsioPcmOutput.h
src/output/AsioPcmOutput.cpp
src/output/BufferNudgeController.h
src/output/BufferNudgeController.cpp
"${ASIO_SDK_ROOT}/common/asio.cpp"
"${ASIO_SDK_ROOT}/host/asiodrivers.cpp"
"${ASIO_SDK_ROOT}/host/pc/asiolist.cpp"

src/processing/SignalReconstructor.h
src/processing/SignalReconstructor.cpp
src/processing/SignalReconstructorAvx2.cpp
src/processing/VideoDeinterlacer.cpp
src/processing/VideoDeinterlacer.h
src/processing/NoiseReducer.h
src/processing/NoiseReducer.cpp
src/processing/LumaHighFrequencyCompensator.h
src/processing/LumaHighFrequencyCompensator.cpp
src/processing/LumaHighFrequencyCompensatorAvx2.cpp

src/rendering/WaveformRenderer.h
src/rendering/WaveformRenderer.cpp
src/rendering/WaveformGraticule.h
src/rendering/WaveformGraticule.cpp
src/rendering/VectorscopeGraticule.h
src/rendering/VectorscopeGraticule.cpp
src/rendering/VectorscopeRenderer.h
src/rendering/VectorscopeRenderer.cpp

src/settings/OpenScopeSettings.h
src/settings/SettingsService.h
src/settings/SettingsService.cpp
src/settings/SettingsStorage.h
src/settings/SettingsStorage.cpp

src/standards/ColorBars.h
src/standards/ColorMatrices.h
src/standards/VideoStandard.h

src/ui/RenderLoadBar.h
src/ui/RenderLoadBar.cpp

src/util/CpuFeatures.h
src/util/CpuFeatures.cpp
src/util/PerformanceStats.h

src/video/DisplayConverter.h
src/video/DisplayConverter.cpp
src/video/FrameBufferPool.h
src/video/FrameBufferPool.cpp
src/video/ReconstructedLumaFrame.h
src/video/Uyvy422ToYuv444Converter.h
src/video/Uyvy422ToYuv444Converter.cpp
src/video/V210ToYuv444Converter.h
src/video/V210ToYuv444Converter.cpp
src/video/VideoConverter.h
src/video/Yuv444Frame.h
src/video/ProgressiveLumaPair.h
src/video/DisplayPerformance.h

src/widgets/PerformanceWidget.h
src/widgets/PerformanceWidget.cpp
src/widgets/VectorscopeWidget.h
src/widgets/VectorscopeWidget.cpp
src/widgets/VideoWidget.h
src/widgets/VideoWidget.cpp
src/widgets/WaveformWidget.h
src/widgets/WaveformWidget.cpp
src/widgets/YSpectrumWindow.h
src/widgets/YSpectrumWindow.cpp
src/widgets/ControlWidget.h
src/widgets/ControlWidget.cpp
src/widgets/PcmAudioMeter.h
src/widgets/PcmAudioMeter.cpp
src/widgets/PcmAudioScopeWidget.h
src/widgets/PcmAudioScopeWidget.cpp

"${DECKLINK_GENERATED_IID}"
)

# ASIO SDK asio.h requires this to expose ASIOSampleRate as native double.
target_compile_definitions(OpenScope PRIVATE IEEE754_64FLOAT=1)


set(_asio_sdk_sources
    "${ASIO_SDK_ROOT}/common/asio.cpp"
    "${ASIO_SDK_ROOT}/host/asiodrivers.cpp"
    "${ASIO_SDK_ROOT}/host/pc/asiolist.cpp"
)
set_source_files_properties(${_asio_sdk_sources}
    PROPERTIES
        COMPILE_DEFINITIONS "interface=struct;IEEE754_64FLOAT=1"
        COMPILE_OPTIONS "/UUNICODE;/U_UNICODE;/FIWindows.h;/FIObjbase.h"
)

target_include_directories(OpenScope PRIVATE
    "${ASIO_SDK_ROOT}/common"
    "${ASIO_SDK_ROOT}/host"
    "${ASIO_SDK_ROOT}/host/pc"
)


if(WIN32)
    target_sources(OpenScope
        PRIVATE
            "${OPENSCOPE_GENERATED_DIR}/OpenScopeAppIcon.rc"
    )
endif()

if(WIN32)
    set_target_properties(OpenScope PROPERTIES
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    )
endif()

# Keep optimized MSVC Release builds symbolized so production-only crashes
# can be resolved to OpenScope functions and source lines in Visual Studio.
if(MSVC)
    target_compile_options(OpenScope PRIVATE
        $<$<CONFIG:Release>:/Zi>
    )

    target_link_options(OpenScope PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/processing/SignalReconstructorAvx2.cpp"
    PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2"
)

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/processing/LumaHighFrequencyCompensatorAvx2.cpp"
    PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2"
)

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/processing/NoiseReducer.cpp"
    PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2"
)

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/WaveformRenderer.cpp"
    PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2"
)

add_dependencies(OpenScope DeckLinkApiGenerated)

target_include_directories(OpenScope
    PRIVATE
        "${DECKLINK_SDK_ROOT}/Win/include"
        "${DECKLINK_GENERATED_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${OPENSCOPE_GENERATED_DIR}"
)

target_link_libraries(OpenScope
    PRIVATE
        Qt6::Widgets
        ${OPENSCOPE_SPOUT_TARGET}
        d3d11
        dxgi
        ole32
        comsuppw
        uuid
        winmm
)

if(WIN32)
    add_custom_command(TARGET OpenScope POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenScope>/platforms"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE_DIR:OpenScope>/platforms/"
    )
endif()
install(
    TARGETS OpenScope
    RUNTIME DESTINATION bin
)

set(OPENSCOPE_RUNTIME_DIR
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin"
)

install(
    FILES
        "${OPENSCOPE_RUNTIME_DIR}/Qt6Core.dll"
        "${OPENSCOPE_RUNTIME_DIR}/Qt6Gui.dll"
        "${OPENSCOPE_RUNTIME_DIR}/Qt6Widgets.dll"

        "${OPENSCOPE_RUNTIME_DIR}/brotlicommon.dll"
        "${OPENSCOPE_RUNTIME_DIR}/brotlidec.dll"
        "${OPENSCOPE_RUNTIME_DIR}/bz2.dll"
        "${OPENSCOPE_RUNTIME_DIR}/double-conversion.dll"
        "${OPENSCOPE_RUNTIME_DIR}/freetype.dll"
        "${OPENSCOPE_RUNTIME_DIR}/harfbuzz.dll"
        "${OPENSCOPE_RUNTIME_DIR}/icudt78.dll"
        "${OPENSCOPE_RUNTIME_DIR}/icuin78.dll"
        "${OPENSCOPE_RUNTIME_DIR}/icuuc78.dll"
        "${OPENSCOPE_RUNTIME_DIR}/libpng16.dll"
        "${OPENSCOPE_RUNTIME_DIR}/md4c.dll"
        "${OPENSCOPE_RUNTIME_DIR}/pcre2-16.dll"
        "${OPENSCOPE_RUNTIME_DIR}/z.dll"
        "${OPENSCOPE_RUNTIME_DIR}/zstd.dll"

    DESTINATION
        bin
)
install(
    FILES
        "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
    DESTINATION
        bin/platforms
)