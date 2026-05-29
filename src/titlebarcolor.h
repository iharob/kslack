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

    // Drop back to the user's default colour scheme. Called when we navigate
    // out of a workspace (no .p-theme_background to track), so the titlebar
    // shows the regular UI colour instead of a stale workspace tint.
    void reset();

    // Track main-window focus. KWin 6.x on Wayland renders an unfocused
    // window's titlebar with the per-window scheme's *active* colour (it never
    // reads the scheme's inactive group), so we dim it ourselves here instead
    // of relying on KWin to honour inactiveBackground.
    void setWindowActive(bool active);

private:
    void sample();
    void apply(const QColor &background);
    // Recompute the effective titlebar colour from the base colour and the
    // current focus state, then push it through apply().
    void render();

    QWebEngineView *m_view;
    QTimer *m_timer;
    // The full, focus-independent workspace colour last reported by the page.
    QColor m_baseColor;
    // The colour actually written to the scheme (base when focused, dimmed when
    // not). Used to dedupe redundant re-activations.
    QColor m_lastApplied;
    bool m_ready = false;
    bool m_windowActive = true;
    // Whether our dynamic scheme (rather than the user's default) is currently
    // the active one. Lets reset() no-op when already default, and forces
    // apply() to re-activate after a reset even if the colour is unchanged.
    bool m_schemeActive = false;
};

#endif
