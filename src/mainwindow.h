#ifndef KSLACK_MAINWINDOW_H
#define KSLACK_MAINWINDOW_H

#include <KXmlGuiWindow>

#include <memory>

class QWebEngineNotification;
class QWebEngineView;
class QWebEnginePermission;
class KStatusNotifierItem;
class TitlebarColorWatcher;

class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void showSignInPage();
    bool importFromFirefox();
    bool importFromChrome();
    void finishImport(bool ok);

public Q_SLOTS:
    void applyChromeColor(const QString &cssColor);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class BadgeState { None, Unread, Mention };

    void setupActions();
    void setupTrayIcon();
    void handlePermission(QWebEnginePermission permission);
    void handleNotification(std::unique_ptr<QWebEngineNotification> notification);
    void updateBadgeFromTitle(const QString &title);
    void applyBadge(BadgeState state);
    void signIn();

    QWebEngineView *m_view = nullptr;
    KStatusNotifierItem *m_trayIcon = nullptr;
    TitlebarColorWatcher *m_titlebar = nullptr;
    BadgeState m_badgeState = BadgeState::None;
    QString m_lastChromeCss;
};

#endif
