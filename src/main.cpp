#include <QApplication>
#include <QIcon>

#include <KAboutData>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedString>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--autoplay-policy=no-user-gesture-required");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    // DevTools-Protocol port for ad-hoc inspection during development.
    qputenv("QTWEBENGINE_REMOTE_DEBUGGING", "9222");

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
    window->show();

    QObject::connect(&service, &KDBusService::activateRequested, window, [window]() {
        window->show();
        window->raise();
        window->activateWindow();
    });

    return app.exec();
}
