#include "mainwindow.h"

#include "titlebarcolor.h"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMenuBar>
#include <QWebEngineNewWindowRequest>
#include <QMessageBox>
#include <QNetworkCookie>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineCookieStore>
#include <QWebEngineNotification>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QWidget>

#include <KActionCollection>
#include <KLocalizedString>
#include <KNotification>
#include <KStandardAction>
#include <KStatusNotifierItem>

static const QString userAgent = QStringLiteral(
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36");

static const QString slackUrl = QStringLiteral("https://app.slack.com/");
static const QString slackSignInUrl = QStringLiteral("https://slack.com/signin");

static bool isSlackHost(const QString &host)
{
    return host == QStringLiteral("app.slack.com")
        || host == QStringLiteral("slack.com")
        || host.endsWith(QStringLiteral(".slack.com"));
}

static void openInBrowser(const QUrl &url)
{
    // QDesktopServices::openUrl() goes through QtDBus / xdg-desktop-portal
    // and silently fails from inside QtWebEngine processes under Wayland.
    // Spawn xdg-open directly instead.
    if (QProcess::startDetached(QStringLiteral("xdg-open"), {url.toString()}))
        return;
    QDesktopServices::openUrl(url);
}

class SlackPage : public QWebEnginePage
{
public:
    SlackPage(QWebEngineProfile *profile, MainWindow *parent)
        : QWebEnginePage(profile, parent)
        , m_owner(parent) {}

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        qWarning().noquote() << "[kslack] nav" << (isMainFrame ? "main" : "sub")
                          << "type=" << int(type) << url.toString();
        if (!isMainFrame)
            return true;

        // "I've logged in" button callback from the landing page.
        if (url.scheme() == QStringLiteral("kslack")
            && url.host() == QStringLiteral("auth-done")) {
            QTimer::singleShot(0, m_owner, [w = m_owner]() {
                w->finishImport(w->importFromFirefox() || w->importFromChrome());
            });
            return false;
        }

        // Our own landing pages.
        const auto scheme = url.scheme();
        if (scheme == QStringLiteral("about") || scheme == QStringLiteral("data"))
            return true;

        // Only intercept *user-clicked* external links (LinkClicked = 0). Those
        // get punted to the system browser. Everything else — redirects from
        // Slack's server to IdPs (RedirectNavigation = 6), back/forward, reload,
        // OtherNavigation — stays inside our view, so the OAuth round-trip
        // completes in our cookie jar.
        if (type == NavigationTypeLinkClicked && !isSlackHost(url.host())) {
            openInBrowser(url);
            return false;
        }

        return true;
    }

private:
    MainWindow *m_owner;
};

MainWindow::MainWindow(QWidget *parent)
    : KXmlGuiWindow(parent)
{
    auto *profile = new QWebEngineProfile(QStringLiteral("kslack"), this);
    profile->setHttpUserAgent(userAgent);
    // Slack sets the d-s session cookie with no expiry, so a normal browser
    // drops it on shutdown and you have to re-auth on every cold start.
    // ForcePersistentCookies keeps session cookies on disk too.
    profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    profile->setNotificationPresenter([this](std::unique_ptr<QWebEngineNotification> n) {
        handleNotification(std::move(n));
    });
    profile->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);

    m_view = new QWebEngineView(this);
    m_view->setPage(new SlackPage(profile, this));

    QWebEngineScript script;
    script.setSourceCode(QStringLiteral(
        "(function() {"
        "  var style = document.createElement('style');"
        "  style.textContent = 'html, body { margin: 0 !important; padding: 0 !important; }';"
        "  document.documentElement.appendChild(style);"
        "})();"
    ));
    script.setInjectionPoint(QWebEngineScript::DocumentReady);
    script.setWorldId(QWebEngineScript::ApplicationWorld);
    script.setRunsOnSubFrames(false);
    m_view->page()->scripts().insert(script);

    connect(m_view->page(), &QWebEnginePage::permissionRequested,
            this, &MainWindow::handlePermission);

    connect(m_view->page(), &QWebEnginePage::newWindowRequested,
            this, [this](QWebEngineNewWindowRequest &request) {
                const QUrl url = request.requestedUrl();
                qWarning().noquote() << "[kslack] popup" << url.toString();
                if (isSlackHost(url.host())) {
                    m_view->load(url);
                    return;
                }
                // Slack JS asked for window.open(<OAuth url>). Honour it with
                // a top-level view that *shares our profile*, exactly like
                // Slack desktop does with an Electron BrowserWindow. The
                // OAuth callback at oauth2.slack.com lands in the same
                // cookie jar as our main view.
                auto *profile = m_view->page()->profile();
                auto *popup = new QWebEngineView(profile);
                popup->setAttribute(Qt::WA_DeleteOnClose);
                popup->resize(560, 720);
                popup->setWindowTitle(i18n("Sign in — KSlack"));
                popup->setWindowIcon(QIcon::fromTheme(QStringLiteral("kslack")));
                request.openIn(popup->page());
                connect(popup->page(), &QWebEnginePage::windowCloseRequested,
                        popup, &QWidget::close);
                connect(popup, &QWebEngineView::urlChanged, this,
                        [this, popup](const QUrl &newUrl) {
                            if (!isSlackHost(newUrl.host()))
                                return;
                            const auto path = newUrl.path();
                            if (path.contains(QStringLiteral("/client"))
                                || path.contains(QStringLiteral("/ssb/signin_redirect"))
                                || path.contains(QStringLiteral("claimDeviceMultipass"))
                                || path.contains(QStringLiteral("auth.loginMagic"))) {
                                QTimer::singleShot(800, popup, &QWidget::close);
                                m_view->load(QUrl(slackUrl));
                            }
                        });
                popup->show();
            });

    connect(m_view, &QWebEngineView::titleChanged,
            this, &MainWindow::updateBadgeFromTitle);

    // /ssb/redirect is a "tell desktop to load workspace" page that hangs
    // without Electron IPC. Jump to /client/ ourselves — with plain Chrome
    // UA Slack's web client renders normally there.
    connect(m_view, &QWebEngineView::urlChanged, this, [this](const QUrl &url) {
        if (isSlackHost(url.host()) && url.path().endsWith(QStringLiteral("/ssb/redirect"))) {
            QTimer::singleShot(400, m_view, [this]() {
                m_view->load(QUrl(QStringLiteral("https://app.slack.com/client/")));
            });
        }
    });

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view);
    setCentralWidget(container);

    m_view->load(QUrl(slackUrl));

    setupActions();
    setupTrayIcon();
    setupGUI(ToolBar | Keys | Save | Create, QStringLiteral("kslackui.rc"));

    menuBar()->setVisible(false);
    menuBar()->setMaximumHeight(0);
    setProperty("_breeze_no_separator", true);

    m_titlebar = new TitlebarColorWatcher(m_view, this);
    m_titlebar->start();

    resize(1280, 800);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupActions()
{
    KStandardAction::quit(qApp, &QCoreApplication::quit, actionCollection());

    auto *reload = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                               i18n("&Reload"), this);
    actionCollection()->addAction(QStringLiteral("reload"), reload);
    actionCollection()->setDefaultShortcut(reload, Qt::Key_F5);
    connect(reload, &QAction::triggered, this, [this]() {
        if (m_view)
            m_view->reload();
    });

    auto *signInAct = new QAction(QIcon::fromTheme(QStringLiteral("system-users")),
                                  i18n("&Sign In via Browser..."), this);
    actionCollection()->addAction(QStringLiteral("sign_in_browser"), signInAct);
    connect(signInAct, &QAction::triggered, this, &MainWindow::signIn);
}

void MainWindow::setupTrayIcon()
{
    m_trayIcon = new KStatusNotifierItem(this);
    m_trayIcon->setCategory(KStatusNotifierItem::Communications);
    if (QIcon::hasThemeIcon(QStringLiteral("kslack-tray")))
        m_trayIcon->setIconByName(QStringLiteral("kslack-tray"));
    else
        m_trayIcon->setIconByName(QStringLiteral("kslack"));
    m_trayIcon->setStandardActionsEnabled(true);
    m_trayIcon->setToolTipTitle(i18n("KSlack"));
    m_trayIcon->setToolTipSubTitle(i18n("Slack Client"));

    connect(m_trayIcon, &KStatusNotifierItem::activateRequested, this, [this]() {
        if (isVisible() && !isMinimized()) {
            hide();
        } else {
            show();
            raise();
            activateWindow();
        }
    });
}

void MainWindow::handlePermission(QWebEnginePermission permission)
{
    const auto origin = permission.origin();
    if (isSlackHost(origin.host())) {
        permission.grant();
        return;
    }
    permission.deny();
}

void MainWindow::handleNotification(std::unique_ptr<QWebEngineNotification> webNotification)
{
    std::shared_ptr<QWebEngineNotification> notification(webNotification.release());

    auto *knotify = new KNotification(QStringLiteral("webNotification"),
                                       KNotification::Persistent, this);
    knotify->setTitle(notification->title().toHtmlEscaped());
    knotify->setText(notification->message().toHtmlEscaped());

    const auto icon = notification->icon();
    if (!icon.isNull())
        knotify->setPixmap(QPixmap::fromImage(icon));

    auto *defaultAction = knotify->addDefaultAction(i18n("Open"));
    connect(defaultAction, &KNotificationAction::activated, this, [this, notification]() {
        notification->click();
        show();
        raise();
        activateWindow();
    });
    connect(knotify, &KNotification::closed, this, [notification]() {
        notification->close();
    });

    knotify->sendEvent();
}

void MainWindow::updateBadgeFromTitle(const QString &title)
{
    BadgeState state = BadgeState::None;
    const QString trimmed = title.trimmed();
    if (trimmed.startsWith(QLatin1Char('('))) {
        state = BadgeState::Mention;
    } else if (trimmed.startsWith(QLatin1Char('*'))
               || trimmed.contains(QStringLiteral("• "))
               || trimmed.contains(QStringLiteral(" * "))) {
        state = BadgeState::Unread;
    }
    applyBadge(state);
}

void MainWindow::applyBadge(BadgeState state)
{
    if (!m_trayIcon || state == m_badgeState)
        return;
    m_badgeState = state;

    const QString base = QIcon::hasThemeIcon(QStringLiteral("kslack-tray"))
        ? QStringLiteral("kslack-tray")
        : QStringLiteral("kslack");

    if (state == BadgeState::None) {
        m_trayIcon->setIconByName(base);
        m_trayIcon->setToolTipSubTitle(i18n("Slack Client"));
        return;
    }

    const int size = 64;
    QPixmap canvas = QIcon::fromTheme(base).pixmap(size, size);
    if (canvas.isNull()) {
        canvas = QPixmap(size, size);
        canvas.fill(Qt::transparent);
    }

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(state == BadgeState::Mention
        ? QColor(0xE0, 0x1E, 0x5A)
        : QColor(0x1D, 0x9B, 0xD1));
    const int d = 26;
    p.drawEllipse(QRect(size - d - 2, size - d - 2, d, d));
    p.end();

    m_trayIcon->setIconByPixmap(QIcon(canvas));
    m_trayIcon->setToolTipSubTitle(state == BadgeState::Mention
        ? i18n("Mentions waiting")
        : i18n("Unread messages"));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_trayIcon) {
        hide();
        event->ignore();
        return;
    }
    KXmlGuiWindow::closeEvent(event);
}

void MainWindow::signIn()
{
    openInBrowser(QUrl(slackSignInUrl));
    showSignInPage();
}

void MainWindow::showSignInPage()
{
    m_view->setHtml(QStringLiteral(
        "<html><body style='background:#1a1d21;color:#fff;font-family:sans-serif;"
        "display:flex;justify-content:center;align-items:center;height:100vh;margin:0'>"
        "<div style='text-align:center;max-width:480px;padding:0 24px'>"
        "<h2 style='margin-bottom:16px'>Finish signing in in your browser</h2>"
        "<p style='color:#abadb1;line-height:1.6;margin-bottom:8px'>"
        "Your default browser has opened the Slack sign-in page. "
        "Enter your workspace URL, then complete the email magic-link or SSO flow there.</p>"
        "<p style='color:#abadb1;line-height:1.6;margin-bottom:32px'>"
        "When you see your Slack workspace loaded in the browser, come back here and click below "
        "so KSlack can copy the session over.</p>"
        "<a href='kslack://auth-done' style='"
        "display:inline-block;background:#007a5a;color:#fff;font-weight:600;"
        "padding:12px 32px;border-radius:4px;text-decoration:none;font-size:15px"
        "'>I've signed in</a>"
        "</div></body></html>"
    ));
}

void MainWindow::finishImport(bool ok)
{
    if (ok) {
        m_view->load(QUrl(slackUrl));
        return;
    }
    QMessageBox::warning(this, i18n("Import Failed"),
        i18n("Could not find Slack cookies in Firefox or Chrome.\n"
             "Make sure you completed sign-in in your default browser, "
             "then try again."));
}

bool MainWindow::importFromFirefox()
{
    QDir firefoxDir(QDir::homePath() + QStringLiteral("/.mozilla/firefox"));
    if (!firefoxDir.exists())
        return false;

    const auto profiles = firefoxDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &profile : profiles) {
        const auto cookiesDb = firefoxDir.filePath(profile + QStringLiteral("/cookies.sqlite"));
        if (!QFile::exists(cookiesDb))
            continue;

        const auto tempDb = QDir::tempPath() + QStringLiteral("/kslack-import.sqlite");
        QFile::remove(tempDb);
        if (!QFile::copy(cookiesDb, tempDb))
            continue;

        QProcess proc;
        proc.start(QStringLiteral("sqlite3"),
                   {QStringLiteral("-separator"), QStringLiteral("|"), tempDb,
                    QStringLiteral("SELECT name, value, host, path, expiry, isSecure, isHttpOnly "
                                   "FROM moz_cookies WHERE host LIKE '%slack.com'")});
        proc.waitForFinished(5000);
        QFile::remove(tempDb);

        const auto output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (output.isEmpty())
            continue;

        auto *store = m_view->page()->profile()->cookieStore();
        bool found = false;

        const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const auto &line : lines) {
            const auto parts = line.split(QLatin1Char('|'));
            if (parts.size() < 7)
                continue;

            QNetworkCookie cookie;
            cookie.setName(parts[0].toUtf8());
            cookie.setValue(parts[1].toUtf8());
            cookie.setDomain(parts[2]);
            cookie.setPath(parts[3]);
            const auto expiry = parts[4].toLongLong();
            if (expiry > 0)
                cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(expiry));
            cookie.setSecure(parts[5] == QLatin1String("1"));
            cookie.setHttpOnly(parts[6].trimmed() == QLatin1String("1"));

            store->setCookie(cookie, QUrl(slackUrl));
            found = true;
        }

        if (found)
            return true;
    }
    return false;
}

bool MainWindow::importFromChrome()
{
    struct BrowserInfo {
        QString configDir;
        QString walletFolder;
    };
    const BrowserInfo browsers[] = {
        {QDir::homePath() + QStringLiteral("/.config/google-chrome"), QStringLiteral("Chrome Safe Storage")},
        {QDir::homePath() + QStringLiteral("/.config/chromium"), QStringLiteral("Chromium Safe Storage")},
    };

    for (const auto &browser : browsers) {
        const auto cookiesDb = browser.configDir + QStringLiteral("/Default/Cookies");
        if (!QFile::exists(cookiesDb))
            continue;

        QProcess kwallet;
        kwallet.start(QStringLiteral("kwallet-query"),
                      {QStringLiteral("-f"), browser.walletFolder,
                       QStringLiteral("-r"), browser.walletFolder,
                       QStringLiteral("kdewallet")});
        kwallet.waitForFinished(5000);
        const auto password = QString::fromUtf8(kwallet.readAllStandardOutput()).trimmed();
        if (password.isEmpty())
            continue;

        const auto tempDb = QDir::tempPath() + QStringLiteral("/kslack-chrome.sqlite");
        QFile::remove(tempDb);
        if (!QFile::copy(cookiesDb, tempDb))
            continue;

        QProcess sqlite;
        sqlite.start(QStringLiteral("sqlite3"),
                     {QStringLiteral("-separator"), QStringLiteral("|"), tempDb,
                      QStringLiteral("SELECT name, hex(encrypted_value), host_key, path, "
                                     "expires_utc, is_secure, is_httponly "
                                     "FROM cookies WHERE host_key LIKE '%slack.com'")});
        sqlite.waitForFinished(5000);
        QFile::remove(tempDb);

        const auto output = QString::fromUtf8(sqlite.readAllStandardOutput()).trimmed();
        if (output.isEmpty())
            continue;

        QProcess deriveKey;
        deriveKey.start(QStringLiteral("openssl"),
                        {QStringLiteral("kdf"), QStringLiteral("-keylen"), QStringLiteral("16"),
                         QStringLiteral("-kdfopt"), QStringLiteral("digest:SHA1"),
                         QStringLiteral("-kdfopt"), QStringLiteral("pass:") + password,
                         QStringLiteral("-kdfopt"), QStringLiteral("salt:saltysalt"),
                         QStringLiteral("-kdfopt"), QStringLiteral("iter:1"),
                         QStringLiteral("PBKDF2")});
        deriveKey.waitForFinished(5000);
        const auto keyHex = QString::fromUtf8(deriveKey.readAllStandardOutput()).trimmed()
                                .remove(QLatin1Char(':')).remove(QLatin1Char('\n'));
        if (keyHex.isEmpty())
            continue;

        auto *store = m_view->page()->profile()->cookieStore();
        bool found = false;

        const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const auto &line : lines) {
            const auto parts = line.split(QLatin1Char('|'));
            if (parts.size() < 7)
                continue;

            const auto encHex = parts[1];
            // Chrome v11 prefix "v11" = 763131 in hex
            if (!encHex.startsWith(QStringLiteral("763131")))
                continue;
            const auto cipherHex = encHex.mid(6);

            QProcess decrypt;
            decrypt.start(QStringLiteral("bash"),
                          {QStringLiteral("-c"),
                           QStringLiteral("echo '%1' | xxd -r -p | "
                                          "openssl enc -d -aes-128-cbc -K '%2' "
                                          "-iv '20202020202020202020202020202020'")
                               .arg(cipherHex, keyHex)});
            decrypt.waitForFinished(2000);
            const auto value = QString::fromUtf8(decrypt.readAllStandardOutput());
            if (value.isEmpty())
                continue;

            QNetworkCookie cookie;
            cookie.setName(parts[0].toUtf8());
            cookie.setValue(value.toUtf8());
            cookie.setDomain(parts[2]);
            cookie.setPath(parts[3]);
            const auto expiresUtc = parts[4].toLongLong();
            if (expiresUtc > 0) {
                // Chrome expires_utc is microseconds since 1601-01-01.
                const auto unixSecs = (expiresUtc / 1000000) - 11644473600LL;
                cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(unixSecs));
            }
            cookie.setSecure(parts[5] == QLatin1String("1"));
            cookie.setHttpOnly(parts[6].trimmed() == QLatin1String("1"));

            store->setCookie(cookie, QUrl(slackUrl));
            found = true;
        }

        if (found)
            return true;
    }
    return false;
}
