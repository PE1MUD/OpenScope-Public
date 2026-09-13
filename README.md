# OpenScope

OpenScope is a Windows-based software waveform monitor, vectorscope and video analysis toolbox focused on PAL/625-line SD video.

The current public baseline is **OpenScope 0.9.5**.

## Features

- PAL/625-line video capture and display
- Waveform monitor
- Vectorscope
- Fullscreen and multi-view workspace
- Spout output for video, waveform and vectorscope
- WSS decoding with optional automatic 4:3 / 16:9 aspect-ratio following
- Illegal-luminance indication
- Blackmagic DeckLink / Intensity device selection
- Support for systems with multiple Blackmagic capture devices
- Sony PCM-F1 / EIAJ PCM decoding
- Ham PCM 2.0 decoding
- PCM Audio Scope with:
  - Left/right PPM-style meters
  - Stereo phase/correlation display
  - Goniometer
- MUDTW metering
- WASAPI decoded-audio output
- ASIO decoded-audio output
- Performance and worker timing diagnostics

## Supported video hardware

OpenScope currently targets Blackmagic Design capture hardware through the DeckLink SDK.

The application is designed to remain usable when no Blackmagic driver or device is available, so non-capture functionality can still be accessed.

## Blackmagic Desktop Video driver

OpenScope uses Blackmagic Design DeckLink / Intensity capture hardware through the DeckLink API.

To use Blackmagic capture hardware, install the current **Blackmagic Desktop Video** software and driver package first.

Download it from Blackmagic Design:

https://www.blackmagicdesign.com/support

The Desktop Video package installs the required device drivers and the Desktop Video Setup utility.

OpenScope can still start without the Blackmagic driver installed, but Blackmagic capture devices will not be available.

## PCM audio

OpenScope can decode PCM embedded in video, including Sony PCM-F1 / EIAJ-style signals and Ham PCM 2.0.

Decoded audio can be monitored through the built-in PCM Audio Scope and sent to either:

- WASAPI
- ASIO

Sony PCM/EIAJ audio uses a 44.1 kHz source rate. Ham PCM 2.0 uses 48 kHz.

## ASIO

OpenScope supports ASIO using the Steinberg ASIO SDK.

The Steinberg ASIO SDK is **not distributed with OpenScope** and must be obtained separately from Steinberg.

CMake can locate the SDK through `ASIO_SDK_ROOT` or a supported common SDK location.

The selected SDK root must contain at least:

```text
common/asio.cpp
host/asiodrivers.cpp
host/pc/asiolist.cpp
```

ASIO is a trademark or registered trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries.

## Philips ROM sets

OpenScope can use external Philips PM5644 ROM sets for supported test-pattern and reference functions.

The ROM layout and model-specific configuration files in `romsets/` are part of OpenScope and are included in the repository. These `.ini` files describe how OpenScope should interpret the corresponding ROM contents.

The actual Philips ROM images are **not distributed with OpenScope**.

Users must provide their own legally obtained ROM dumps and place the required `.bin` files in the appropriate subdirectory under `romsets/`.

For example:

```text
romsets/
├── PM5644G00/
│   ├── PM5644G00.ini
│   └── <ROM image>.bin
├── PM5644G913/
│   ├── Indian_head.ini
│   └── <ROM image>.bin
└── PM5644G924/
    ├── 16x9_v1.ini
    ├── 16x9_v2.ini
    └── <ROM image>.bin


## Build environment

OpenScope is currently developed and built with:

- Windows 11
- Visual Studio 2022
- C++20
- CMake
- Qt 6
- Blackmagic DeckLink SDK
- Steinberg ASIO SDK for ASIO support

A typical workflow is:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Depending on your local setup, CMake paths for Qt, the DeckLink SDK and ASIO SDK may need to be supplied explicitly.

## Release notes

See:

```text
releasenotesV0.9.5.md
```

for the current release notes.

## Project status

OpenScope 0.9.5 is the first public baseline of the project.

Development prior to this public baseline was kept in a private repository. The public repository therefore starts from 0.9.5 without the earlier development history.

## License

OpenScope is licensed under the **GNU General Public License v3.0 only**.

See [LICENSE](LICENSE) for the full license text.

SPDX identifier:

```text
GPL-3.0-only
```

The Steinberg ASIO SDK is not distributed as part of OpenScope.
