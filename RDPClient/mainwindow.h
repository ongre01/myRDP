#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "rdpclient.h"

#include <QMainWindow>
#include <QTimer>

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
    void connectToServer();
    void disconnectFromServer();
    void processRdpEvents();

private:
    enum class ConnectionUiState
    {
        Disconnected,
        Connecting,
        Connected,
        Error
    };

    void setConnectionUiState(ConnectionUiState state, const QString &message = QString());
    void showConnectionError(const QString &message);
    void handleInputError();
    CertificateDecision verifyServerCertificate(const CertificateInfo &certificate);
    void displayDesktopUpdate(const DesktopUpdate &desktopUpdate);

    Ui::MainWindow *ui;
    RdpClient client;
    QTimer rdpEventTimer;
    bool handlingInputError = false;
};
#endif // MAINWINDOW_H
