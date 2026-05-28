#include "titlebarcolor.h"

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

void writeSchemeFile(const QColor &bg, const QColor &fg)
{
    auto config = KSharedConfig::openConfig(schemeFilePath(), KConfig::SimpleConfig);

    KConfigGroup general(config, QStringLiteral("General"));
    general.writeEntry(QStringLiteral("ColorScheme"), QStringLiteral("KSlackDynamic"));
    general.writeEntry(QStringLiteral("Name"), QString::fromLatin1(kSchemeName));

    KConfigGroup wm(config, QStringLiteral("WM"));
    wm.writeEntry(QStringLiteral("activeBackground"), rgbTriplet(bg));
    wm.writeEntry(QStringLiteral("activeForeground"), rgbTriplet(fg));
    wm.writeEntry(QStringLiteral("activeBlend"), rgbTriplet(fg));
    wm.writeEntry(QStringLiteral("inactiveBackground"), rgbTriplet(bg));
    wm.writeEntry(QStringLiteral("inactiveForeground"), rgbTriplet(fg));
    wm.writeEntry(QStringLiteral("inactiveBlend"), rgbTriplet(fg));

    for (const QString &section : {QStringLiteral("Colors:Header"),
                                   QStringLiteral("Colors:Header][Inactive"),
                                   QStringLiteral("Colors:Window")}) {
        KConfigGroup g(config, section);
        g.writeEntry(QStringLiteral("BackgroundNormal"), rgbTriplet(bg));
        g.writeEntry(QStringLiteral("BackgroundAlternate"), rgbTriplet(bg));
        g.writeEntry(QStringLiteral("ForegroundNormal"), rgbTriplet(fg));
        g.writeEntry(QStringLiteral("ForegroundInactive"), rgbTriplet(fg));
    }

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
    m_timer->setInterval(kSampleIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &TitlebarColorWatcher::sample);

    connect(view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok)
            return;
        m_ready = true;
        QTimer::singleShot(250, this, &TitlebarColorWatcher::sample);
    });
    connect(view, &QWebEngineView::urlChanged, this, [this](const QUrl &) {
        if (m_ready)
            QTimer::singleShot(250, this, &TitlebarColorWatcher::sample);
    });
}

void TitlebarColorWatcher::start()
{
    // Write a sensible baseline so KColorSchemeManager can find the scheme on first activate.
    const QColor fallback(0x4A, 0x15, 0x4B);
    writeSchemeFile(fallback, pickForeground(fallback));
    apply(fallback);
    m_timer->start();
}

void TitlebarColorWatcher::sample()
{
    if (!m_ready || !m_view || m_view->size().isEmpty())
        return;

    const QPixmap pixmap = m_view->grab();
    if (pixmap.isNull())
        return;

    const QImage image = pixmap.toImage();
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
    const QColor fg = pickForeground(background);
    writeSchemeFile(background, fg);

    auto *mgr = KColorSchemeManager::instance();
    const auto idx = mgr->indexForScheme(QString::fromLatin1(kSchemeName));
    if (idx.isValid())
        mgr->activateScheme(idx);

    m_lastApplied = background;
}
