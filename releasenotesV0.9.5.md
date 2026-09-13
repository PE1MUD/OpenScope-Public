# OpenScope 0.9.5

OpenScope 0.9.5 is a major consolidation release that brings together the work done since the 0.8.5 baseline: improved SD video handling, WSS/aspect-ratio support, PCM decoding and audio monitoring, MUDTW metering, ASIO output, Blackmagic multi-device handling, and a broad set of UI, performance and robustness improvements.

## Highlights

- Improved Blackmagic DeckLink / Intensity device handling, including systems with multiple capture cards.
- Persistent Blackmagic input-device selection.
- More robust startup and source switching when Blackmagic hardware or drivers are unavailable.
- WSS decoding for 625-line sources with optional automatic 4:3 / 16:9 aspect-ratio following.
- Expanded Sony PCM-F1 / EIAJ PCM support.
- Ham PCM 2.0 decoding support.
- PCM audio monitoring with PPM meters, phase/correlation display and goniometer.
- MUDTW meter display with improved fullscreen scaling and presentation.
- ASIO as an alternative decoded-PCM output path alongside WASAPI.
- Continued waveform, vectorscope, fullscreen and profiler polish.

## Video and source handling

OpenScope remains focused on PAL/625-line SD video, with stable 720x576 capture and presentation.

The source path has been made more tolerant of real-world configurations:

- Blackmagic device selection is explicit instead of assuming a single installed card.
- The selected Blackmagic device is stored persistently.
- Multi-device systems no longer need to rely on enumeration order.
- Startup remains usable when no Blackmagic driver or capture device is available.
- Non-Blackmagic sources remain selectable when capture hardware is unavailable.
- Source-dependent controls are enabled and disabled more consistently.

The earlier 0.8.5 work is retained, including the `625/50d` naming for deinterlaced PAL presentation and source-independent Y frequency-response correction.

## WSS and aspect ratio

625-line WSS decoding was added after the 0.8.5 baseline.

- WSS is detected from the PAL source signal.
- OpenScope can optionally follow the transmitted aspect-ratio information.
- Stable 4:3 material remains 4:3.
- Anamorphic 16:9 signalling can automatically switch the video display to 16:9.
- Loss of WSS restores the user's manual aspect-ratio selection.
- The detected WSS line can be hidden from the displayed video image without altering the raw captured data used by the scopes.

## Waveform and vectorscope

The waveform and vectorscope continue to receive substantial performance and presentation work.

- Waveform rendering and worker scheduling have been refined for lower render cost and more predictable frame delivery.
- Performance diagnostics expose the real worker/render phases more accurately.
- Stale profiler information is cleared when a view is disabled.
- Waveform operation remains valid when used for Spout-only output.
- Vectorscope positioning and information-panel behaviour are more stable during resize.
- Illegal-luminance indication uses the corrected lower threshold of `0.280 V`.
- Fullscreen behaviour, zoom/pan handling and viewport scaling have received further polish.

The 0.8.5 `View FPS` diagnostics remain available for Video, Waveform, Vectorscope and their Spout outputs.

## PCM decoding

OpenScope now includes substantially expanded PCM-over-video support.

Supported work includes:

- Sony PCM-F1 / EIAJ-style PCM decoding.
- 14-bit and 16-bit PCM handling.
- Ham PCM 2.0 decoding.
- More robust PCM lock and reacquisition behaviour.
- Improved valid-line accounting and field handling.
- PCM control/header handling and de-emphasis behaviour.
- Decoder reference/slicer improvements for difficult or non-ideal source levels.
- PCM audio metering and status presentation integrated into the application.

The PCM path has been refined over many incremental builds, particularly around field/line accounting, mode switching, lock robustness and transport quality.

## PCM Audio Scope

Decoded PCM can be monitored visually in OpenScope.

The Audio Scope includes:

- Left/right PPM-style level meters.
- Stereo phase/correlation indication.
- Goniometer display.
- Compact PCM status presentation.
- Fullscreen-aware scaling.
- Improved phosphor-style presentation and graticule layout.

The MUDTW presentation has also been refined for fullscreen use, with wider/taller meter segments while retaining visible black separation between adjacent segments. The phase row now uses the same horizontal extent as the MUDTW meter panel.

## MUDTW

MUDTW metering is integrated as a compact broadcast-style level display.

Recent refinements include:

- Improved proportional scaling.
- Better fullscreen geometry.
- Wider meter segments in F11 mode.
- Preserved black gaps between segments.
- Consistent alignment between MUDTW and the Audio Scope/phase display.

Normal windowed MUDTW geometry remains deliberately more compact than the fullscreen presentation.

## Audio output

Decoded PCM can be sent to either WASAPI or ASIO.

### WASAPI

The existing WASAPI output path remains available, including its receiver-buffer control.

### ASIO

ASIO was added as a second PCM output backend.

- ASIO driver enumeration is available in PCM Audio settings.
- Sony PCM/EIAJ audio is treated as 44.1 kHz source audio.
- Ham PCM 2.0 uses 48 kHz source audio.
- The ASIO device clock is the playback master.
- An elastic linear ASRC compensates for source-clock versus device-clock drift.
- Buffer position control is based on the actual ASIO driver quantum rather than fixed millisecond thresholds.
- The WASAPI receiver-buffer slider is disabled while ASIO output is selected because ASIO buffering is derived automatically from the driver.

The Steinberg ASIO SDK is not bundled with OpenScope. CMake expects an external SDK via `ASIO_SDK_ROOT` or one of the supported common SDK locations.

## Fullscreen and UI

Fullscreen and workspace behaviour have been polished throughout the 0.8.x and 0.9.x work.

- F11 remains true windowless fullscreen.
- Config/F2 behaviour is restricted to contexts where it is useful.
- Quad mode remains stable and keeps its fourth viewport intact.
- Scope and meter layouts scale more consistently between windowed and fullscreen operation.
- Video, waveform and vectorscope views retain their intended aspect and interaction behaviour.
- MUDTW and Audio Scope presentation now make better use of the available fullscreen area.

## Performance and diagnostics

A large amount of work since 0.8.5 has gone into making performance behaviour both faster and easier to understand.

- Worker activity and render phases are reported more accurately.
- Waveform and vectorscope rendering paths have been progressively reduced and parallelised where useful.
- Display and profiler timing are less prone to stale or misleading values.
- The application remains usable on relatively modest hardware for local PAL video, waveform and vectorscope work, while heavier Spout/OBS workflows naturally require more headroom.

## Robustness

This release also rolls up many smaller fixes covering:

- startup without capture hardware;
- device enumeration and selection;
- source switching;
- fullscreen state management;
- viewport geometry;
- PCM mode switching;
- PCM lock/reacquisition;
- audio-device selection;
- ASIO buffering;
- settings persistence;
- profiler lifecycle;
- stale-state cleanup;
- UI enable/disable logic.

## Build notes

OpenScope is built with CMake and Qt 6.

For ASIO support, the Steinberg ASIO SDK must be supplied separately. The SDK root must contain the standard ASIO source files used by the build, including:

- `common/asio.cpp`
- `host/asiodrivers.cpp`
- `host/pc/asiolist.cpp`

## Release baseline

OpenScope 0.9.5 is intended as the new clean baseline after the long 0.8.x / early 0.9.x development cycle.

It consolidates the video, scope, PCM, audio-output, MUDTW, fullscreen, device-selection and performance work into one release point before further feature development continues.
