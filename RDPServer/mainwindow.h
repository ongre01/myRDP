#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "autostartmanager.h"
#include "rdpserver.h"

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
class QAction;
class QCloseEvent;
class QMenu;
class QSystemTrayIcon;
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(bool startHidden = false, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void startServer();
    void stopServer();
    void showSettings();
    void quitApplication();
    void setAutoStartEnabled(bool enabled);

private:
    void tryStartServer(bool interactive);
    void appendLog(const QString &message);
    void setServerUiState(bool listening);
    void updateStatusPresentation();
    void showTrayMessage(const QString &title, const QString &message);

    Ui::MainWindow *ui;
    RdpServer server;
    RdpServerConfiguration configuration;
    bool configurationLoaded = false;
    bool quitRequested = false;
    bool hideNotificationShown = false;
    AutoStartManager autoStartManager;
    QSystemTrayIcon *trayIcon = nullptr;
    QMenu *trayMenu = nullptr;
    QAction *trayStatusAction = nullptr;
    QAction *trayStartAction = nullptr;
    QAction *trayStopAction = nullptr;
};
#endif // MAINWINDOW_H
