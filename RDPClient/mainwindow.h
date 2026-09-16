#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "rdpclient.h"

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
    void connectToServer();
    void disconnectFromServer();

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
    CertificateDecision verifyServerCertificate(const CertificateInfo &certificate);

    Ui::MainWindow *ui;
    RdpClient client;
};
#endif // MAINWINDOW_H
