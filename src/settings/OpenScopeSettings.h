#pragma once

#include <string>

struct OpenScopeSettings
{
    enum class AspectRatio
    {
        Ratio4x3,
        Ratio16x9
    };

    static constexpr double aspectRatioValue(
        AspectRatio aspectRatio) noexcept
    {
        return
            aspectRatio == AspectRatio::Ratio4x3
            ? 4.0 / 3.0
            : 16.0 / 9.0;
    }

    enum class WorkspaceView
    {
        Headless = -1,
        Matrix = 0,
        Video = 1,
        Waveform = 2,
        Vectorscope = 3
    };

    struct Control
    {
        struct Instrument
        {
            int lineNumber = 320;

            struct Waveform
            {
                int zoom = 1;
                double scrollPosition = 0.0;

                bool vintageLook = false;
                bool antiAliasing = true;
                bool colorizeIllegalLuminance = true;
                int chromaRenderIntensity = 150;

                int persistenceFrames = 5;
                int coreIntensity = 100;
                int coreWidthTenths = 10;
            };

            struct Vectorscope
            {
                bool showHundredPercentTargets = true;
                bool colorizeGamutErrors = true;

                int persistenceFrames = 5;
                int glow = 50;
            };

            Waveform waveform;
            Vectorscope vectorscope;
        };

        struct Processing
        {
            struct NoiseFilter
            {
                bool enabled = false;

                int strength = 50;
                int temporalStrength = 0;
            };

            struct LumaCompensation
            {
                bool enabled = false;

                // Hundredths of a dB at 5.8 MHz, range 0..100.
                int gainHundredthsDb = 60;
            };

            NoiseFilter noiseFilter;
            LumaCompensation lumaCompensation;
        };

        struct PcmAudio
        {
            bool enabled = false;
            bool outputEnabled = false;
            bool muteBottomTwoBits = false;

            // 0 = WASAPI, 1 = ASIO.
            int outputBackend = 0;

            // Empty means the current Windows default render endpoint.
            std::string outputDeviceId;

            // ASIO driver name, as exposed by the Steinberg host helper.
            std::string asioDriverName;

            // 0 = Auto, 1 = Off, 2 = 50/15 us.
            int deEmphasisMode = 0;

            // Receiver/playout cushion in milliseconds. 0 = Low/minimum.
            int outputBufferMs = 32;

            // Optional replacement of the chroma vectorscope viewport while
            // the PCM decoder is running. The normal vectorscope remains the
            // default and is restored whenever PCM decoding is disabled.
            bool audioScopeEnabled = false;

            // Optional semantic colour coding for the PCM Audio Scope trace.
            bool audioScopeColorized = true;
        };

        struct VideoOut
        {
            bool enabled = false;

            int width = 1920;
            int height = 1080;

            AspectRatio aspectRatio =
                AspectRatio::Ratio16x9;

            double underscan = 0.80;

            bool deinterlace = false;
        };

        Instrument instrument;
        Processing processing;
        PcmAudio pcmAudio;
        VideoOut videoOut;
    };

    struct Local
    {
        struct Display
        {
            double gamma = 0.80;

            AspectRatio aspectRatio =
                AspectRatio::Ratio16x9;

            bool deinterlace = false;
            bool lineSelectorVisible = true;
            bool safetyArea90 = false;
            bool textSafetyArea80 = false;
            bool preventDisplaySleep = false;
            bool followWssAspectRatio = false;
        };

        struct Window
        {
            int x = 100;
            int y = 100;

            int width = 1280;
            int height = 720;

            bool maximized = false;
        };

        struct Workspace
        {
            WorkspaceView view =
                WorkspaceView::Matrix;
        };

        struct Floaty
        {
            int x = 0;
            int y = 0;

            int width = 0;
            int height = 0;

            bool positionValid = false;
            bool sizeValid = false;
        };

        struct Floaties
        {
            Floaty performance;
            Floaty settings;

            bool performanceVisible = true;
            bool waveformVideoVisible = false;
        };

        struct Spout
        {
            bool videoEnabled = false;
            bool waveformEnabled = false;
            bool vectorscopeEnabled = false;
        };

        Display display;
        Window window;
        Workspace workspace;
        Floaties floaties;
        Spout spout;
    };

    Control control;
    Local local;
};