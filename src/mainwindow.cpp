#include "mainwindow.h"

#include "titlebarcolor.h"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QStatusBar>
#include <QWebEngineNewWindowRequest>
#include <QMessageBox>
#include <QNetworkCookie>
#include <QPainter>
#include <optional>
#include <QPixmap>
#include <QProcess>
#include <QSet>
#include <QSharedPointer>
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
#include <KConfigGroup>
#include <KLocalizedString>
#include <KNotification>
#include <KSharedConfig>
#include <KStandardAction>
#include <KStatusNotifierItem>

static const QString userAgent = QStringLiteral(
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36");

static const QString slackUrl = QStringLiteral("https://app.slack.com/");
static const QString slackSignInUrl = QStringLiteral("https://slack.com/signin");

static QString readLastTeam()
{
    KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("Session"));
    return config.readEntry(QStringLiteral("lastTeam"), QString());
}

static QString lastWorkspaceUrl()
{
    const QString teamId = readLastTeam();
    if (teamId.isEmpty())
        return slackUrl;
    return QStringLiteral("https://app.slack.com/client/") + teamId;
}

// Extract the T-prefixed team ID from a Slack client URL, or empty.
static QString teamIdFromUrl(const QUrl &url)
{
    if (url.host() != QStringLiteral("app.slack.com"))
        return {};
    const auto segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.size() < 2 || segments[0] != QStringLiteral("client"))
        return {};
    const auto teamId = segments[1];
    if (teamId.size() < 2 || teamId[0] != QLatin1Char('T'))
        return {};
    return teamId;
}

static void rememberWorkspace(const QUrl &url)
{
    const QString teamId = teamIdFromUrl(url);
    if (teamId.isEmpty())
        return;
    KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("Session"));
    if (config.readEntry(QStringLiteral("lastTeam"), QString()) == teamId)
        return;
    config.writeEntry(QStringLiteral("lastTeam"), teamId);
    config.sync();
}

static std::optional<QColor> cachedColorFor(const QString &teamId)
{
    if (teamId.isEmpty())
        return std::nullopt;
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("WorkspaceThemes"));
    const QString hex = group.readEntry(teamId, QString());
    if (hex.isEmpty())
        return std::nullopt;
    QColor c(hex);
    if (!c.isValid())
        return std::nullopt;
    return c;
}

static void setCachedColorFor(const QString &teamId, const QColor &c)
{
    if (teamId.isEmpty() || !c.isValid())
        return;
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("WorkspaceThemes"));
    const QString hex = c.name();
    if (group.readEntry(teamId, QString()) == hex)
        return;
    group.writeEntry(teamId, hex);
    group.sync();
}

static bool isSlackHost(const QString &host)
{
    return host == QStringLiteral("app.slack.com")
        || host == QStringLiteral("slack.com")
        || host.endsWith(QStringLiteral(".slack.com"));
}

// Hosts that participate in Slack's sign-in flow. We keep them in our view
// (and thus our cookie jar) even on user-initiated link clicks, because
// punting them to a separate browser context breaks the OAuth state and
// you get cryptic 400 errors from the IdP.
static bool isAuthProviderHost(const QString &host)
{
    return host.endsWith(QStringLiteral(".google.com"))
        || host.endsWith(QStringLiteral(".gstatic.com"))
        || host == QStringLiteral("appleid.apple.com")
        || host == QStringLiteral("login.microsoftonline.com")
        || host.endsWith(QStringLiteral(".onelogin.com"))
        || host.endsWith(QStringLiteral(".okta.com"))
        || host.endsWith(QStringLiteral(".auth0.com"))
        || host.endsWith(QStringLiteral(".duosecurity.com"));
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
        if (type == NavigationTypeLinkClicked
            && !isSlackHost(url.host())
            && !isAuthProviderHost(url.host())) {
            openInBrowser(url);
            return false;
        }

        return true;
    }

    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString &message,
                                  int lineNumber,
                                  const QString &sourceID) override
    {
        // Color-watcher channel: messages starting with our marker carry
        // the chrome colour the JS observer just read. Forward to the owner.
        if (message.startsWith(QStringLiteral("__kslack_chrome__:"))) {
            // Body is "rgb(R, G, B)|via=<selector>". Strip the via= for the slot.
            const QString body = message.mid(18);
            const int sep = body.indexOf(QStringLiteral("|via="));
            const QString colorOnly = sep > 0 ? body.left(sep) : body;
            qWarning().noquote() << "[kslack/chrome] reported" << body;
            QMetaObject::invokeMethod(m_owner, "applyChromeColor",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, colorOnly));
            return;
        }
        if (message.startsWith(QStringLiteral("__kslack_debug__:"))) {
            qWarning().noquote() << "[kslack/chrome] top chain"
                                 << message.mid(17);
            return;
        }
        QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber, sourceID);
    }

private:
    MainWindow *m_owner;
};

MainWindow::MainWindow(QWidget *parent)
    : KXmlGuiWindow(parent)
{
    auto *profile = new QWebEngineProfile(QStringLiteral("kslack"), this);
    profile->setHttpUserAgent(userAgent);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);

    // ForcePersistentCookies keeps the on-disk file but Chromium still marks
    // rows from no-expiry Set-Cookie headers as is_persistent=0 and drops
    // them on restart. Slack relies on a session-scoped `d-s` cookie for
    // "active device" state — without it we hit the sign-in flow on every
    // cold start. Intercept Slack session cookies as they're added and
    // re-set them with a far-future expiry so is_persistent flips to 1.
    auto *cookies = profile->cookieStore();
    auto seen = QSharedPointer<QSet<QString>>::create();
    connect(cookies, &QWebEngineCookieStore::cookieAdded, this,
            [cookies, seen](const QNetworkCookie &cookie) {
        const QString domain = cookie.domain();
        if (!domain.endsWith(QStringLiteral("slack.com")))
            return;
        if (cookie.expirationDate().isValid())
            return; // already persistent, leave alone
        const QString key = domain + QLatin1Char('|') + cookie.path()
                            + QLatin1Char('|') + QString::fromUtf8(cookie.name());
        if (seen->contains(key))
            return; // we just re-inserted this one — skip to avoid a loop
        seen->insert(key);
        QNetworkCookie persistent = cookie;
        persistent.setExpirationDate(QDateTime::currentDateTime().addYears(1));
        cookies->deleteCookie(cookie);
        cookies->setCookie(persistent);
    });
    profile->setNotificationPresenter([this](std::unique_ptr<QWebEngineNotification> n) {
        handleNotification(std::move(n));
    });
    profile->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);

    m_view = new QWebEngineView(this);
    m_view->setPage(new SlackPage(profile, this));
    // No Chromium-default right-click menu (Reload / Back / Inspect ...).
    // Slack itself provides the only context menus that should appear in chat.
    m_view->setContextMenuPolicy(Qt::NoContextMenu);

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

    // Chrome-colour observer: read the *unfiltered* background-color of the
    // .p-theme_background element. In IA4 Slack, every layer above it is
    // transparent, so this element's bg is exactly what gets painted at the
    // top — except when the window is blurred, Slack adds
    //   .p-window--blurred .p-theme_background { filter: brightness(0.9); }
    // which we deliberately ignore (we want titlebar invariant to focus).
    // The MutationObserver is anchored on .p-theme_background itself, so
    // body's p-window--blurred class swings never trigger us.
    QWebEngineScript colorObserver;
    colorObserver.setSourceCode(QStringLiteral(
        "(function() {"
        "  var last = '';"
        "  var timer = null;"
        "  var inner = null;"
        "  function report() {"
        "    timer = null;"
        "    var el = document.querySelector('.p-theme_background');"
        "    if (!el) return;"
        "    var c = getComputedStyle(el).backgroundColor;"
        "    if (c && c !== last) {"
        "      last = c;"
        "      console.log('__kslack_chrome__:' + c);"
        "    }"
        "  }"
        "  function schedule() {"
        "    if (timer) return;"
        "    timer = setTimeout(report, 50);"
        "  }"
        "  function attach(el) {"
        "    if (inner) return;"
        "    inner = new MutationObserver(schedule);"
        "    inner.observe(el, { attributes: true });"
        "    schedule();"
        "  }"
        "  var existing = document.querySelector('.p-theme_background');"
        "  if (existing) {"
        "    attach(existing);"
        "  } else {"
        "    var wait = new MutationObserver(function() {"
        "      var el = document.querySelector('.p-theme_background');"
        "      if (el) {"
        "        wait.disconnect();"
        "        attach(el);"
        "      }"
        "    });"
        "    wait.observe(document.body || document.documentElement,"
        "                 { childList: true, subtree: true });"
        "  }"
        "  setInterval(report, 5000);"
        "})();"
    ));
    colorObserver.setInjectionPoint(QWebEngineScript::DocumentReady);
    colorObserver.setWorldId(QWebEngineScript::MainWorld);
    colorObserver.setRunsOnSubFrames(false);
    m_view->page()->scripts().insert(colorObserver);

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

    m_titlebar = new TitlebarColorWatcher(m_view, this);
    m_titlebar->start();

    // Track which workspace we're on (used by applyChromeColor to key the
    // cache writes). We deliberately do NOT apply the cached color here —
    // urlChanged fires *before* Slack tears down the old workspace and
    // mounts the new one, so changing the titlebar at this point looks
    // jarring against the still-visible old content. Instead, the JS
    // observer on .p-theme_background fires when the new workspace's
    // theme is actually painted, and applyChromeColor takes it from there.
    // (Cold start is a different story — m_view hasn't loaded anything,
    // so we pre-render from the cache *before* m_view->load(), further down.)
    connect(m_view, &QWebEngineView::urlChanged, this, [this](const QUrl &url) {
        const QString teamId = teamIdFromUrl(url);
        if (teamId.isEmpty() || teamId == m_currentTeamId)
            return;
        m_currentTeamId = teamId;
        // Force the next JS report through the dedupe check even if the
        // rgb string happens to match the previous workspace's — we want
        // the cache write keyed to the new team ID.
        m_lastChromeCss.clear();
    });

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

    connect(m_view, &QWebEngineView::urlChanged, this, &rememberWorkspace);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view);
    setCentralWidget(container);

    // Apply the cached color for lastTeam before kicking off the load, so the
    // titlebar is right from the moment the window first paints.
    const QString lastTeam = readLastTeam();
    if (auto c = cachedColorFor(lastTeam)) {
        m_currentTeamId = lastTeam;
        m_titlebar->applyExternal(*c);
    }

    m_view->load(QUrl(lastWorkspaceUrl()));

    setupActions();
    setupTrayIcon();
    setupGUI(ToolBar | Keys | Save | Create, QStringLiteral("kslackui.rc"));

    menuBar()->setVisible(false);
    menuBar()->setMaximumHeight(0);
    if (auto *sb = findChild<QStatusBar *>()) {
        sb->setVisible(false);
        sb->setMaximumHeight(0);
    }
    setProperty("_breeze_no_separator", true);

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

    auto *signInAct = new QAction(QIcon::fromTheme(QStringLiteral("system-users")),
                                  i18n("Sign In via Browser..."), this);
    connect(signInAct, &QAction::triggered, this, &MainWindow::signIn);
    m_trayIcon->contextMenu()->addAction(signInAct);

    auto *autostart = new QAction(i18n("Start automatically at login"), this);
    autostart->setCheckable(true);
    autostart->setChecked(isAutostartEnabled());
    connect(autostart, &QAction::toggled, this, &MainWindow::setAutostartEnabled);
    m_trayIcon->contextMenu()->addAction(autostart);

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

static QString autostartDesktopPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/autostart/org.kde.kslack.desktop");
}

bool MainWindow::isAutostartEnabled() const
{
    return QFile::exists(autostartDesktopPath());
}

void MainWindow::setAutostartEnabled(bool enabled)
{
    const QString path = autostartDesktopPath();
    QFile::remove(path);
    if (!enabled)
        return;

    QDir().mkpath(QFileInfo(path).absolutePath());

    const QString installed = QStandardPaths::locate(
        QStandardPaths::ApplicationsLocation, QStringLiteral("org.kde.kslack.desktop"));
    if (!installed.isEmpty()) {
        QFile::copy(installed, path);
        return;
    }

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=KSlack\n"
            "Exec=kslack\n"
            "Icon=kslack\n"
            "Terminal=false\n"
            "X-GNOME-Autostart-enabled=true\n");
    }
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

static QColor parseCssColor(const QString &css)
{
    const QString trimmed = css.trimmed();
    // QColor parses #RGB / #RRGGBB / #RRGGBBAA and named colours directly.
    QColor c(trimmed);
    if (c.isValid())
        return c;
    // Fall back to rgb()/rgba() syntax.
    const auto open = trimmed.indexOf(QLatin1Char('('));
    const auto close = trimmed.indexOf(QLatin1Char(')'));
    if (open <= 0 || close <= open)
        return QColor();
    const auto parts = trimmed.mid(open + 1, close - open - 1)
                              .split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() < 3)
        return QColor();
    bool ok1, ok2, ok3;
    const int r = parts[0].trimmed().toInt(&ok1);
    const int g = parts[1].trimmed().toInt(&ok2);
    const int b = parts[2].trimmed().toInt(&ok3);
    if (!ok1 || !ok2 || !ok3)
        return QColor();
    return QColor(r, g, b);
}

void MainWindow::applyChromeColor(const QString &cssColor)
{
    if (cssColor == m_lastChromeCss)
        return;
    m_lastChromeCss = cssColor;
    if (!m_titlebar)
        return;
    const QColor c = parseCssColor(cssColor);
    if (!c.isValid())
        return;
    qWarning().noquote() << "[kslack/chrome]" << cssColor << "->" << c.name();
    m_titlebar->applyExternal(c);
    setCachedColorFor(m_currentTeamId, c);
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
