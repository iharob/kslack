#include "titlebarcolor.h"

#include <QDebug>
#include <QDir>
#include <QImage>
#include <QModelIndex>
#include <QPixmap>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWebEngineView>

#include <KColorSchemeManager>
#include <KConfigGroup>
#include <KSharedConfig>

namespace
{
constexpr const char *kSchemeName = "KSlack Dynamic";
constexpr const char *kSchemeFile = "KSlackDynamic.colors";
constexpr int kSampleIntervalMs = 2000;
constexpr int kHysteresisPerChannel = 8;

QString schemeFilePath()
{
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/color-schemes");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/") + QString::fromLatin1(kSchemeFile);
}

QString rgbTriplet(const QColor &c)
{
    return QStringLiteral("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue());
}

QColor pickForeground(const QColor &bg)
{
    const double y = 0.299 * bg.red() + 0.587 * bg.green() + 0.114 * bg.blue();
    return y < 140 ? QColor(252, 252, 252) : QColor(35, 38, 41);
}

// Mirror Slack's own `.p-window--blurred .p-theme_background { filter: brightness(0.9); }`
// so the titlebar dims by exactly the same amount as the content when the
// window loses focus.
QColor dimForInactive(const QColor &c)
{
    return QColor(qRound(c.red() * 0.9), qRound(c.green() * 0.9), qRound(c.blue() * 0.9));
}

void writeSchemeFile(const QColor &bg, const QColor &fg)
{
    const QColor bgInactive = dimForInactive(bg);
    const QColor fgInactive = dimForInactive(fg);

    auto config = KSharedConfig::openConfig(schemeFilePath(), KConfig::SimpleConfig);

    KConfigGroup general(config, QStringLiteral("General"));
    general.writeEntry(QStringLiteral("ColorScheme"), QStringLiteral("KSlackDynamic"));
    general.writeEntry(QStringLiteral("Name"), QString::fromLatin1(kSchemeName));

    KConfigGroup wm(config, QStringLiteral("WM"));
    wm.writeEntry(QStringLiteral("activeBackground"), rgbTriplet(bg));
    wm.writeEntry(QStringLiteral("activeForeground"), rgbTriplet(fg));
    // Blend = background so Breeze doesn't lighten/darken the titlebar with a
    // gradient highlight; we want the exact colour Slack defines.
    wm.writeEntry(QStringLiteral("activeBlend"), rgbTriplet(bg));
    wm.writeEntry(QStringLiteral("inactiveBackground"), rgbTriplet(bgInactive));
    wm.writeEntry(QStringLiteral("inactiveForeground"), rgbTriplet(fgInactive));
    wm.writeEntry(QStringLiteral("inactiveBlend"), rgbTriplet(bgInactive));

    auto writeColorGroup = [&](const QString &parentName) {
        KConfigGroup parent(config, parentName);
        parent.writeEntry(QStringLiteral("BackgroundNormal"), rgbTriplet(bg));
        parent.writeEntry(QStringLiteral("BackgroundAlternate"), rgbTriplet(bg));
        parent.writeEntry(QStringLiteral("ForegroundNormal"), rgbTriplet(fg));
        parent.writeEntry(QStringLiteral("ForegroundInactive"), rgbTriplet(fg));
        // [<parent>][Inactive] — use the sub-group API; passing the literal
        // string "<parent>][Inactive" makes KConfig escape the brackets and
        // KWin never finds the group.
        KConfigGroup inactive = parent.group(QStringLiteral("Inactive"));
        inactive.writeEntry(QStringLiteral("BackgroundNormal"), rgbTriplet(bgInactive));
        inactive.writeEntry(QStringLiteral("BackgroundAlternate"), rgbTriplet(bgInactive));
        inactive.writeEntry(QStringLiteral("ForegroundNormal"), rgbTriplet(fgInactive));
        inactive.writeEntry(QStringLiteral("ForegroundInactive"), rgbTriplet(fgInactive));
    };
    writeColorGroup(QStringLiteral("Colors:Header"));
    writeColorGroup(QStringLiteral("Colors:Window"));

    config->sync();
}

bool close(const QColor &a, const QColor &b)
{
    return qAbs(a.red() - b.red()) <= kHysteresisPerChannel
        && qAbs(a.green() - b.green()) <= kHysteresisPerChannel
        && qAbs(a.blue() - b.blue()) <= kHysteresisPerChannel;
}
}

TitlebarColorWatcher::TitlebarColorWatcher(QWebEngineView *view, QObject *parent)
    : QObject(parent)
    , m_view(view)
    , m_timer(new QTimer(this))
{
    connect(view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (ok)
            m_ready = true;
    });
}

void TitlebarColorWatcher::start()
{
    // Write a sensible baseline so KColorSchemeManager can find the scheme on first activate.
    const QColor fallback(0x4A, 0x15, 0x4B);
    writeSchemeFile(fallback, pickForeground(fallback));
    apply(fallback);
    // The JS-side observer (mainwindow.cpp) drives all subsequent updates via
    // applyExternal(). The grab-based sampler stays compiled but unused.
}

void TitlebarColorWatcher::applyExternal(const QColor &color)
{
    if (!color.isValid())
        return;
    if (m_lastApplied.isValid() && close(color, m_lastApplied))
        return;
    apply(color);
}

void TitlebarColorWatcher::sample()
{
    if (!m_ready || !m_view || m_view->size().isEmpty()) {
        qWarning() << "[kslack/titlebar] sample skipped — not ready or no size";
        return;
    }

    const QPixmap pixmap = m_view->grab();
    if (pixmap.isNull()) {
        qWarning() << "[kslack/titlebar] grab() returned null pixmap";
        return;
    }

    const QImage image = pixmap.toImage();
    qWarning() << "[kslack/titlebar] grabbed" << image.width() << "x" << image.height();
    if (image.height() < 1 || image.width() < 8)
        return;

    const int w = image.width();
    const int xStart = w / 10;
    const int xEnd = w - xStart;
    quint64 r = 0, g = 0, b = 0;
    int n = 0;
    for (int x = xStart; x < xEnd; ++x) {
        const QRgb p = image.pixel(x, 0);
        r += qRed(p);
        g += qGreen(p);
        b += qBlue(p);
        ++n;
    }
    if (n == 0)
        return;

    const QColor bg(int(r / n), int(g / n), int(b / n));
    if (m_lastApplied.isValid() && close(bg, m_lastApplied))
        return;

    apply(bg);
}

void TitlebarColorWatcher::apply(const QColor &background)
{
    const bool sameAsLast = m_lastApplied.isValid()
        && background.rgb() == m_lastApplied.rgb();

    const QColor fg = pickForeground(background);
    writeSchemeFile(background, fg);

    auto *mgr = KColorSchemeManager::instance();
    const auto idx = mgr->indexForScheme(QString::fromLatin1(kSchemeName));
    qWarning().noquote() << "[kslack/titlebar] apply" << background.name()
                          << "fg=" << fg.name()
                          << "schemeIndex.isValid=" << idx.isValid();
    if (idx.isValid() && !sameAsLast) {
        // KWin treats activate-same-scheme-twice as a no-op for the decoration,
        // so flip through the default scheme first to force a re-read.
        mgr->activateScheme(QModelIndex());
        mgr->activateScheme(idx);
    }

    m_lastApplied = background;
}
