#include "util/CpuFeatures.h"
#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QTimer>
#include "MainWindow.h"
#include "DeckLinkProbe.h"

int main(int argc, char* argv[])
{
    Q_INIT_RESOURCE(OpenScope);

    QApplication app(argc, argv);

    const QIcon openScopeIcon(
        QStringLiteral(":/branding/OpenScopeLogo.png"));

    app.setWindowIcon(openScopeIcon);
    if (!CpuFeatures::supportsAvx2Fma())
    {
        QMessageBox::critical(
            nullptr,
            "Unsupported CPU",
            "OpenScope requires a CPU with AVX2 and FMA support.");

        return 1;
    }
    MainWindow window;
    window.setWindowIcon(openScopeIcon);

    // Start in a safe no-DeckLink state.  In particular this keeps the
    // Blackmagic source and hardware-only controls disabled until probing
    // has actually succeeded.
    window.setBlackmagicDeviceName({});
    window.show();

    // Do not probe DeckLink before the Qt event loop has started.  Some
    // missing/old Desktop Video installations can take a long time (or fail)
    // while COM/DeckLink is being instantiated.  Probing here used to happen
    // immediately after show(), before Windows had a chance to paint the main
    // window, which made OpenScope appear to silently fail at startup.
    QTimer::singleShot(
        100,
        &window,
        [&window]()
        {
            window.setBlackmagicDeviceName(
                deckLinkProbe(window.videoEngine()));
        });

    return app.exec();
}