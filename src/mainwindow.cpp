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
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStatusBar>
#include <QWebEngineNewWindowRequest>
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
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
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

// We present TWO user agents and swap per navigation host (see
// userAgentForHost): the Slack desktop UA on Slack and most hosts, the Firefox
// UA on identity-provider hosts during sign-in. Each solves a problem the other
// would re-create, so neither can be used alone.
//
// authUserAgent — Firefox/Gecko. Google blocks Google-account sign-in ("Continue
// with Google") from embedded browsers with "this browser or app may not be
// secure". A Chrome UA does not help: we are Chromium under the hood, so
// QtWebEngine emits Sec-CH-UA client hints that Google cross-checks against the
// UA; the embedded-Chromium signature does not match real Chrome and the check
// fails. Firefox is Gecko, sends no Sec-CH-UA, so the UA stands alone and
// sign-in goes through. Same fix qutebrowser (also QtWebEngine) ships as a
// quirk. We scope it to auth-provider hosts so it only covers the OAuth round
// trip — using it everywhere would put Slack back into plain browser mode.
static const QString authUserAgent = QStringLiteral(
    "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) "
    "Gecko/20100101 Firefox/140.0");

// desktopUserAgent — impersonate the Slack Electron desktop app (SSB). The
// Slack/Electron/Slack_SSB tokens are what app.slack.com sniffs to render its
// desktop client (isDesktop() === true) instead of the browser experience. Once
// in desktop mode Slack reaches for an Electron IPC bridge; we satisfy the parts
// it hard-depends on with an injected window.desktop stub (see the bridge script
// in the constructor), without which /client fails to mount.
//
// The version matters: Slack ships a support matrix in its bundle with a current
// minimum (slack 4.41.0) and an upcoming `next` minimum (slack 4.44.00). A
// version below `next` triggers the "this version is no longer compatible"
// deprecation banner, so we report comfortably above it. The embedded Chrome
// token (138) is already above Slack's `next` chrome min (137). The Linux distro
// deprecation only fires when the UA carries an Ubuntu/Red Hat token, which ours
// deliberately does not.
static const QString desktopUserAgent = QStringLiteral(
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Slack/4.45.0 Chrome/138.0.7204.97 Electron/37.2.3 Safari/537.36 "
    "Slack_SSB/4.45.0");

static const QString slackUrl = QStringLiteral("https://app.slack.com/");

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

// Per-host User-Agent, applied at the network layer for every request. The
// profile UA stays the desktop (SSB) UA the whole session; we only override the
// outgoing User-Agent *header* on identity-provider hosts, so their embedded-
// browser OAuth checks (Google's especially) pass. This replaces the previous
// scheme of swapping the global profile UA inside acceptNavigationRequest, which
// re-entered QtWebEngine and crashed (SIGTRAP) on any main-frame navigation that
// needed a swap. An interceptor never mutates the profile and never alters the
// request method, so it crashes nothing and leaves OAuth/SAML POSTs intact.
class UAInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (isAuthProviderHost(info.requestUrl().host()))
            info.setHttpHeader(QByteArrayLiteral("User-Agent"), authUserAgent.toUtf8());
    }
};

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

        // The per-host User-Agent is applied by UAInterceptor at the network
        // layer — never by mutating the profile UA here, which re-enters
        // QtWebEngine and crashes (SIGTRAP) on any navigation that needs a swap.
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
        // Tray badge: Slack's window.desktop.dock.setBadgeCount(n) is forwarded
        // here as "mention" (n > 0) or "none". See the desktopBridge script.
        if (message.startsWith(QStringLiteral("__kslack_badge__:"))) {
            QMetaObject::invokeMethod(m_owner, "updateBadge",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, message.mid(17)));
            return;
        }
        // KSLACK_BRIDGE_DEBUG diagnostic: every swallowed window.desktop call and
        // the web Notification probes, for re-discovering the bridge surface.
        if (message.startsWith(QStringLiteral("__kslack_probe__:"))) {
            qWarning().noquote() << "[kslack/probe]" << message.mid(17);
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
    // Start on the desktop UA so the first Slack load renders the desktop
    // client. acceptNavigationRequest swaps to the Firefox UA for IdP hosts
    // during sign-in and back again (see userAgentForHost).
    profile->setHttpUserAgent(desktopUserAgent);
    profile->setUrlRequestInterceptor(new UAInterceptor(this));
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
    // Pre-grant the Notifications permission for the Slack client origin. In SSB
    // desktop mode Slack assumes the host app already holds notification
    // permission (the real Electron app does) and creates notifications via the
    // web Notification API without first calling Notification.requestPermission().
    // QtWebEngine then leaves the permission in the default ("ask") state and
    // never invokes our presenter, so notifications silently never appear.
    // Granting it up front (persisted on disk) makes Notification.permission read
    // "granted", so new Notification(...) flows to handleNotification(). The
    // request-time handler (handlePermission) still covers any explicit prompt.
    profile->setPersistentPermissionsPolicy(
        QWebEngineProfile::PersistentPermissionsPolicy::StoreOnDisk);
    for (const auto &origin : { "https://app.slack.com", "https://slack.com" }) {
        auto permission = profile->queryPermission(
            QUrl::fromUserInput(QString::fromLatin1(origin)),
            QWebEnginePermission::PermissionType::Notifications);
        if (permission.isValid())
            permission.grant();
    }
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

    // Desktop interop bridge stub. With the Slack_SSB UA, Slack's
    // client renders in desktop mode and calls getDesktopState() — which is
    // bindDesktopMethod(["redux","getState"]) → lodash.get(window,
    // "desktop.redux.getState")(...path). Without an Electron preload this is
    // undefined, the Client component destructures the undefined return
    // (`const { darwin } = getDesktopState("environment")`) and the whole UI
    // fails to mount. bindDesktopMethod tolerates *missing* methods (null-safe
    // get + logs), so we only need to satisfy redux.getState plus the
    // windowToken source (window.browserWindowId). Inject at DocumentCreation
    // in the main world so it's in place before Slack's bundle evaluates.
    // Set KSLACK_BRIDGE_DEBUG=1 to turn the bridge into a diagnostic: every
    // window.desktop.* call Slack makes (other than the ones we implement) is
    // logged through the console marker channel as [kslack/probe]. This is the
    // supported way to re-discover the bridge surface when a Slack update moves
    // things — see project_desktop_spoof memory.
    const bool bridgeDebug = qEnvironmentVariableIsSet("KSLACK_BRIDGE_DEBUG");

    QWebEngineScript desktopBridge;
    desktopBridge.setSourceCode(QStringLiteral(
        "(function() {"
        "  if (window.__kslackDesktop) return;"
        "  window.__kslackDesktop = true;"
        "  var KSLACK_DEBUG = ") + QLatin1String(bridgeDebug ? "true" : "false") + QStringLiteral(";"
        "  if (!window.browserWindowId) window.browserWindowId = 1;"
        "  var STATE = {"
        "    environment: {"
        "      platform: 'linux', arch: 'x64', platformVersion: '6.0.0',"
        "      appVersion: '4.45.0', version: '4.45.0', releaseChannel: 'prod',"
        "      uuid: 'kslack-0000-0000-0000-000000000001',"
        "      sessionId: 'kslack-0000-0000-0000-000000000002',"
        "      isGpuCompositionAvailable: false, isStore: false"
        "    },"
        "    settings: { zoomLevel: 0, useHwAcceleration: true, releaseChannelOverride: null },"
        "    app: { hwAccelAvailability: 'unavailable' }"
        "  };"
        "  function getState() {"
        "    var node = STATE;"
        "    for (var i = 0; i < arguments.length; i++) {"
        "      if (node == null) return undefined;"
        "      node = node[arguments[i]];"
        "    }"
        "    return node;"
        "  }"
        "  function noop() {}"
        // Tray badge: Slack drives the desktop badge through
        // bindDesktopMethod(['dock','setBadgeCount']) → window.desktop.dock
        // .setBadgeCount(n), where n is the unread-highlight (mention) count
        // Slack itself would show as the dock badge (it follows the user's
        // "badge for: all activity / mentions only" setting). We forward the
        // on/off state to the C++ side, which lights the tray dot. A real nested
        // object is required (not the Proxy fallback) because bindDesktopMethod
        // resolves the method with lodash.get(window,'desktop.dock.setBadgeCount')
        // and a missing leaf is silently dropped.
        "  function reportBadge(count) {"
        "    var on = (typeof count === 'number') ? count > 0 : !!count;"
        "    if (KSLACK_DEBUG) { try { console.log('__kslack_probe__:dock.setBadgeCount|' + JSON.stringify([count])); } catch (e) {} }"
        "    try { console.log('__kslack_badge__:' + (on ? 'mention' : 'none')); } catch (e) {}"
        "  }"
        // Diagnostic only (KSLACK_BRIDGE_DEBUG): a recursive, callable Proxy that
        // logs every access/call path without ever throwing, so Slack's boot
        // side-effects survive. makeProbe('desktop.foo') logs desktop.foo(args),
        // and chains: desktop.foo.bar() logs that full path.
        "  function probeLog(path, args) {"
        "    var a;"
        "    try { a = JSON.stringify(Array.prototype.slice.call(args)); }"
        "    catch (e) { a = '<unstringifiable>'; }"
        "    try { console.log('__kslack_probe__:' + path + '|' + a); } catch (e) {}"
        "  }"
        "  function makeProbe(path) {"
        "    var fn = function() { probeLog(path, arguments); return undefined; };"
        "    return new Proxy(fn, {"
        "      get: function(t, prop) {"
        "        if (prop === 'then' || typeof prop === 'symbol') return undefined;"
        "        if (prop in t) return t[prop];"
        "        return makeProbe(path + '.' + String(prop));"
        "      },"
        "      apply: function(t, thiz, args) { probeLog(path, args); return undefined; }"
        "    });"
        "  }"
        // desktop.dock namespace: implement setBadgeCount, keep every other
        // dock.* access a no-op (or probe) so we never throw, mirroring the
        // top-level bridge's crash-tolerance.
        "  function makeDock() {"
        "    var impl = { setBadgeCount: function(count) { reportBadge(count); } };"
        "    return new Proxy(function() {}, {"
        "      get: function(t, prop) {"
        "        if (prop in impl) return impl[prop];"
        "        if (prop === 'then' || typeof prop === 'symbol') return undefined;"
        "        if (prop in t) return t[prop];"
        "        return KSLACK_DEBUG ? makeProbe('desktop.dock.' + String(prop)) : noop;"
        "      }"
        "    });"
        "  }"
        // desktop.notice namespace. Notifications themselves use the web
        // Notification API (which our setNotificationPresenter handles), but
        // before showing one Slack calls desktop.notice.getNotificationWarnings()
        // and *awaits the result* (getParsedNotificationWarnings → .then). A bare
        // no-op returns undefined, so `.then` throws and the whole messageReceived
        // handler aborts — no notification. Return a resolved empty warning list so
        // the flow proceeds. (We intentionally leave app.canShowHtmlNotifications
        // falsy so Slack uses the web Notification API rather than a custom HTML
        // notification window we can't host.)
        "  function makeNotice() {"
        "    var impl = { getNotificationWarnings: function() { return Promise.resolve([]); } };"
        "    return new Proxy(function() {}, {"
        "      get: function(t, prop) {"
        "        if (prop in impl) return impl[prop];"
        "        if (prop === 'then' || typeof prop === 'symbol') return undefined;"
        "        if (prop in t) return t[prop];"
        "        return KSLACK_DEBUG ? makeProbe('desktop.notice.' + String(prop)) : noop;"
        "      }"
        "    });"
        "  }"
        // desktop.screen namespace. Slack's SSB build mirrors Electron's `screen`
        // module here. Several desktop-gated surfaces (e.g. the Activity panel,
        // which positions its popover relative to the display layout) call
        // window.desktop.screen.getAllDisplays() DIRECTLY and immediately .map()
        // over the result. Our Proxy fallback returns undefined, so `.map` throws
        // ("Cannot read properties of undefined (reading 'map')") and the whole
        // handler aborts — the rail icon never activates. Return a single
        // Electron-shaped Display describing the page viewport so the math works.
        // We derive the geometry from window.screen at call time rather than
        // hard-coding it, so it tracks the real window. All distances are in
        // density-independent (DIP) pixels, matching Electron.
        "  function makeDisplay() {"
        "    var w = (window.screen && window.screen.width) || window.innerWidth || 1920;"
        "    var h = (window.screen && window.screen.height) || window.innerHeight || 1080;"
        "    var aw = (window.screen && window.screen.availWidth) || w;"
        "    var ah = (window.screen && window.screen.availHeight) || h;"
        "    var dpr = window.devicePixelRatio || 1;"
        "    return {"
        "      id: 1, label: 'kslack-display-0',"
        "      bounds: { x: 0, y: 0, width: w, height: h },"
        "      workArea: { x: 0, y: 0, width: aw, height: ah },"
        "      size: { width: w, height: h },"
        "      workAreaSize: { width: aw, height: ah },"
        "      scaleFactor: dpr, rotation: 0, internal: true,"
        "      touchSupport: 'unknown', monochrome: false, accelerometerSupport: 'unknown',"
        "      colorDepth: 24, colorSpace: 'srgb', depthPerComponent: 8, displayFrequency: 60"
        "    };"
        "  }"
        "  function makeScreen() {"
        "    var impl = {"
        "      getAllDisplays: function() { return [makeDisplay()]; },"
        "      getPrimaryDisplay: function() { return makeDisplay(); },"
        "      getDisplayMatching: function() { return makeDisplay(); },"
        "      getDisplayNearestPoint: function() { return makeDisplay(); },"
        "      getCursorScreenPoint: function() { return { x: 0, y: 0 }; }"
        "    };"
        "    return new Proxy(function() {}, {"
        "      get: function(t, prop) {"
        "        if (prop in impl) return impl[prop];"
        "        if (prop === 'then' || typeof prop === 'symbol') return undefined;"
        "        if (prop in t) return t[prop];"
        "        return KSLACK_DEBUG ? makeProbe('desktop.screen.' + String(prop)) : noop;"
        "      }"
        "    });"
        "  }"
        "  var real = {"
        "    redux: {"
        "      getState: getState,"
        "      dispatchUpdate: noop,"
        "      subscribe: function() { return noop; }"
        "    },"
        "    dock: makeDock(),"
        "    notice: makeNotice(),"
        "    screen: makeScreen()"
        "  };"
        // Some desktop methods are called DIRECTLY (window.desktop.foo()),
        // bypassing the null-safe bindDesktopMethod — a missing one throws and
        // aborts its boot side-effect (we hit exposeWorkspaceDelegate, then
        // setGlobalDelegate, …). Rather than chase each, wrap in a Proxy: known
        // keys resolve to the real bridge, every other property is a callable
        // no-op (or, with KSLACK_DEBUG, a logging probe). 'then' returns undefined
        // so the object isn't mistaken for a thenable when awaited.
        "  window.desktop = new Proxy(real, {"
        "    get: function(target, prop) {"
        "      if (prop in target) return target[prop];"
        "      if (prop === 'then' || typeof prop === 'symbol') return undefined;"
        "      if (KSLACK_DEBUG) return makeProbe('desktop.' + String(prop));"
        "      return noop;"
        "    }"
        "  });"
        // Notifications go through the standard web Notification API
        // (new Notification(title,{body,icon,tag})), which QtWebEngine routes to
        // our setNotificationPresenter — no bridge shim needed. Under
        // KSLACK_BRIDGE_DEBUG, log construction + permission requests so the
        // notification path stays observable if a Slack update changes it.
        "  if (KSLACK_DEBUG && window.Notification) {"
        "    try {"
        "      window.Notification = new Proxy(window.Notification, {"
        "        construct: function(t, args) {"
        "          try { console.log('__kslack_probe__:Notification.new|' + JSON.stringify(args)); } catch (e) {}"
        "          return new (Function.prototype.bind.apply(t, [null].concat(args)))();"
        "        },"
        "        get: function(t, prop) {"
        "          if (prop === 'requestPermission') {"
        "            return function() {"
        "              try { console.log('__kslack_probe__:Notification.requestPermission|[]'); } catch (e) {}"
        "              return t.requestPermission.apply(t, arguments);"
        "            };"
        "          }"
        "          return t[prop];"
        "        }"
        "      });"
        "    } catch (e) {}"
        "  }"
        "})();"
    ));
    desktopBridge.setInjectionPoint(QWebEngineScript::DocumentCreation);
    desktopBridge.setWorldId(QWebEngineScript::MainWorld);
    desktopBridge.setRunsOnSubFrames(true);
    m_view->page()->scripts().insert(desktopBridge);

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
        "  var absentTimer = null;"
        "  var attrObs = null;"
        "  var watched = null;"
        "  function report() {"
        "    timer = null;"
        "    var el = document.querySelector('.p-theme_background');"
        "    if (!el) {"
        // The theme element is gone — we've navigated out of a workspace (the
        // launcher, sign-in, settings). Debounce it: a workspace *switch*
        // detaches and re-mounts the element within a frame or two, and we
        // don't want to flash the default titlebar in between.
        "      if (!absentTimer) {"
        "        absentTimer = setTimeout(function() {"
        "          absentTimer = null;"
        "          if (!document.querySelector('.p-theme_background') && last !== 'none') {"
        "            last = 'none';"
        "            console.log('__kslack_chrome__:none');"
        "          }"
        "        }, 800);"
        "      }"
        "      return;"
        "    }"
        "    if (absentTimer) { clearTimeout(absentTimer); absentTimer = null; }"
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
        // Track the current .p-theme_background node. A workspace switch swaps
        // it for a fresh element, so re-anchor the attribute observer whenever
        // the node identity changes (and tear it down when it disappears).
        "  function sync() {"
        "    var el = document.querySelector('.p-theme_background');"
        "    if (el !== watched) {"
        "      if (attrObs) { attrObs.disconnect(); attrObs = null; }"
        "      watched = el;"
        "      if (el) {"
        "        attrObs = new MutationObserver(schedule);"
        "        attrObs.observe(el, { attributes: true });"
        "      }"
        "    }"
        "    schedule();"
        "  }"
        "  var tree = new MutationObserver(sync);"
        "  tree.observe(document.body || document.documentElement,"
        "               { childList: true, subtree: true });"
        "  setInterval(report, 5000);"
        "  sync();"
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
                // This popup only ever opens on an IdP (Slack URLs took the
                // branch above). It shares our profile; UAInterceptor already
                // serves the Firefox UA to identity-provider hosts at the network
                // layer, so no global profile-UA change is needed here (and the
                // profile UA must stay the desktop UA so the main view keeps Slack
                // in SSB mode).
                auto *popup = new QWebEngineView(profile);
                popup->setAttribute(Qt::WA_DeleteOnClose);
                popup->resize(560, 720);
                popup->setWindowTitle(i18n("Sign in — KSlack"));
                popup->setWindowIcon(QIcon::fromTheme(QStringLiteral("kslack")));
                request.openIn(popup->page());

                // The OAuth round-trip ends by handing control back to Slack.
                // The popup only ever opens on a *non-Slack* IdP (the isSlackHost
                // branch above keeps Slack URLs in the main view), so the first
                // time the popup lands back on any slack.com host the session
                // cookie is already in our shared jar and the flow is done.
                // Earlier we only matched a handful of paths (/client, …); a
                // brand-new-workspace handoff can land on a different path — or
                // via window.open() — and dead-end the popup on a spinner even
                // though login fully succeeded. Treat any return to Slack as
                // completion: close the popup and reload the main view, which
                // now resolves to the freshly authorised workspace.
                auto done = std::make_shared<bool>(false);
                auto finish = [this, popup, done](const QUrl &landed) {
                    if (*done)
                        return;
                    *done = true;
                    qWarning().noquote() << "[kslack] popup handoff" << landed.toString();
                    m_view->load(QUrl(slackUrl));
                    QTimer::singleShot(800, popup, &QWidget::close);
                };

                connect(popup->page(), &QWebEnginePage::windowCloseRequested,
                        popup, &QWidget::close);
                // A window.open() from the handoff page must not dead-end: if it
                // targets Slack, that *is* the handoff; otherwise keep it inside
                // the same popup so the flow stays visible.
                connect(popup->page(), &QWebEnginePage::newWindowRequested,
                        this, [popup, finish](QWebEngineNewWindowRequest &req) {
                            const QUrl u = req.requestedUrl();
                            qWarning().noquote() << "[kslack] popup window.open" << u.toString();
                            if (isSlackHost(u.host())) {
                                finish(u);
                                return;
                            }
                            req.openIn(popup->page());
                        });
                connect(popup, &QWebEngineView::urlChanged, this,
                        [finish](const QUrl &newUrl) {
                            if (isSlackHost(newUrl.host()))
                                finish(newUrl);
                        });
                popup->show();
            });

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

    // /ssb/redirect is a "tell desktop to load workspace" page that, in the real
    // app, the Electron main process handles over IPC. Our window.desktop stub
    // doesn't drive that handoff, so jump to /client/ ourselves — it renders
    // normally there under our desktop UA + bridge.
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

    QString contents;
    const QString installed = QStandardPaths::locate(
        QStandardPaths::ApplicationsLocation, QStringLiteral("org.kde.kslack.desktop"));
    if (!installed.isEmpty()) {
        QFile in(installed);
        if (in.open(QIODevice::ReadOnly | QIODevice::Text))
            contents = QString::fromUtf8(in.readAll());
    }
    if (contents.isEmpty()) {
        contents = QStringLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=KSlack\n"
            "Exec=kslack\n"
            "Icon=kslack\n"
            "Terminal=false\n"
            "X-GNOME-Autostart-enabled=true\n");
    }

    // Autostart launches go straight to the tray.
    static const QRegularExpression execLine(
        QStringLiteral("^(Exec=.*)$"), QRegularExpression::MultilineOption);
    const auto m = execLine.match(contents);
    if (m.hasMatch() && !m.captured(1).contains(QStringLiteral("--hidden")))
        contents.replace(m.capturedStart(1), m.capturedLength(1),
                         m.captured(1) + QStringLiteral(" --hidden"));

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        file.write(contents.toUtf8());
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

void MainWindow::updateBadge(const QString &state)
{
    // Driven by window.desktop.dock.setBadgeCount via the bridge (see the
    // desktopBridge script). Modern Slack only reports the unread-highlight
    // (mention) count to the desktop badge, so the tray dot is mention-only.
    applyBadge(state == QStringLiteral("mention")
                   ? BadgeState::Mention
                   : BadgeState::None);
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

    if (cssColor == QLatin1String("none")) {
        // The page reports no theme element — we're outside any workspace.
        // Ignore it until a live theme has been seen at least once this
        // session: during cold start the page is still loading and we want the
        // pre-applied cached colour to stay put rather than flashing back to
        // the default scheme for the whole load.
        if (!m_chromeReported)
            return;
        qWarning().noquote() << "[kslack/chrome] no theme -> reset titlebar";
        m_titlebar->reset();
        return;
    }

    const QColor c = parseCssColor(cssColor);
    if (!c.isValid())
        return;
    qWarning().noquote() << "[kslack/chrome]" << cssColor << "->" << c.name();
    m_chromeReported = true;
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

void MainWindow::changeEvent(QEvent *event)
{
    // KWin on Wayland paints an unfocused window's titlebar with our scheme's
    // *active* colour (it never reads the inactive group of a per-window
    // scheme), so we dim it ourselves whenever the window loses focus.
    if (event->type() == QEvent::ActivationChange && m_titlebar)
        m_titlebar->setWindowActive(isActiveWindow());
    KXmlGuiWindow::changeEvent(event);
}
