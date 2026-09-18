#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "rdpserver.h"

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void startServer();
    void stopServer();

private:
    void appendLog(const QString &message);
    void setServerUiState(bool listening);

    Ui::MainWindow *ui;
    RdpServer server;
    RdpServerConfiguration configuration;
    bool configurationLoaded = false;
};
#endif // MAINWINDOW_H
