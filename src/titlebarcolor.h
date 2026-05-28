#ifndef KSLACK_TITLEBARCOLOR_H
#define KSLACK_TITLEBARCOLOR_H

#include <QColor>
#include <QObject>

class QTimer;
class QWebEngineView;

class TitlebarColorWatcher : public QObject
{
    Q_OBJECT

public:
    explicit TitlebarColorWatcher(QWebEngineView *view, QObject *parent = nullptr);

    void start();

    // Called from MainWindow when the page's JS observer reports a new chrome
    // colour. Bypasses the grab-based sampler.
    void applyExternal(const QColor &color);

private:
    void sample();
    void apply(const QColor &background);

    QWebEngineView *m_view;
    QTimer *m_timer;
    QColor m_lastApplied;
    bool m_ready = false;
};

#endif
