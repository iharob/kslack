#include <QApplication>
#include <QIcon>

#include <KAboutData>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedString>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    // --disable-features=UserAgentClientHint stops Chromium from emitting the
    // Sec-CH-UA / Sec-CH-UA-Platform client-hint headers. Those headers carry
    // the *real* embedded-Chromium identity and contradict the Firefox UA we
    // spoof (see userAgent in mainwindow.cpp), which is what makes Google's
    // "this browser or app may not be secure" block flap intermittently on the
    // add-a-Google-account sign-in. With the hints gone, the Firefox UA stands
    // alone with nothing to give us away and the sign-in is reliable. Firefox
    // never sends these hints anyway, so this is consistent, not suspicious.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            "--autoplay-policy=no-user-gesture-required "
            "--disable-features=UserAgentClientHint");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");

    QApplication app(argc, argv);

    KAboutData aboutData(
        QStringLiteral("kslack"),
        i18n("KSlack"),
        QStringLiteral("1.0.0"), // x-release-please-version
        i18n("Slack client for KDE"),
        KAboutLicense::GPL_V3,
        i18n("© 2026 KSlack contributors"),
        QString(),
        QString()
    );
    aboutData.setDesktopFileName(QStringLiteral("org.kde.kslack"));

    KAboutData::setApplicationData(aboutData);
    auto icon = QIcon::fromTheme(QStringLiteral("kslack"));
    if (icon.isNull())
        icon = QIcon(QStringLiteral(":/icons/kslack.svg"));
    QApplication::setWindowIcon(icon);

    KCrash::initialize();

    KDBusService service(KDBusService::Unique);

    auto *window = new MainWindow;
    if (!QCoreApplication::arguments().contains(QStringLiteral("--hidden")))
        window->show();

    QObject::connect(&service, &KDBusService::activateRequested, window, [window]() {
        window->show();
        window->raise();
        window->activateWindow();
    });

    return app.exec();
}
