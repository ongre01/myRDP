#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QMessageBox>
#include <QTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    connect(ui->startButton, &QPushButton::clicked, this, &MainWindow::startServer);
    connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::stopServer);
    connect(&server,
            &RdpServer::listeningStarted,
            this,
            [this](const QString &address, quint16 port) {
                setServerUiState(true);
                appendLog(tr("Listening on %1:%2").arg(address).arg(port));
            });
    connect(&server, &RdpServer::listeningStopped, this, [this]() {
        setServerUiState(false);
        appendLog(tr("Server stopped."));
    });
    connect(&server,
            &RdpServer::clientConnected,
            this,
            [this](quint64 sessionId, const QString &peerAddress) {
                ui->clientCountLabel->setText(QString::number(server.sessionCount()));
                appendLog(tr("Session %1 accepted from %2.")
                              .arg(sessionId)
                              .arg(peerAddress.isEmpty() ? tr("unknown peer") : peerAddress));
            });
    connect(&server,
            &RdpServer::clientDisconnected,
            this,
            [this](quint64 sessionId, const QString &peerAddress) {
                ui->clientCountLabel->setText(QString::number(server.sessionCount()));
                appendLog(tr("Session %1 disconnected (%2).")
                              .arg(sessionId)
                              .arg(peerAddress.isEmpty() ? tr("unknown peer") : peerAddress));
            });
    connect(&server, &RdpServer::errorOccurred, this, [this](const QString &message) {
        appendLog(tr("Error: %1").arg(message));
        statusBar()->showMessage(message);
    });

    setServerUiState(false);
    if (!server.isInitialized()) {
        const QString message = server.lastError();
        ui->startButton->setEnabled(false);
        appendLog(tr("Initialization failed: %1").arg(message));
        statusBar()->showMessage(message);
    } else {
        appendLog(tr("Server initialized. Choose an endpoint and start listening."));
    }
}

MainWindow::~MainWindow()
{
    server.stop();
    delete ui;
}

void MainWindow::startServer()
{
    RdpServerConfiguration configuration;
    configuration.bindAddress = ui->bindAddressEdit->text();
    configuration.port = static_cast<quint16>(ui->portSpinBox->value());

    if (server.start(configuration)) {
        return;
    }

    const QString message = server.lastError();
    appendLog(tr("Start failed: %1").arg(message));
    QMessageBox::critical(this, tr("RDP Server"), message);
}

void MainWindow::stopServer()
{
    server.stop();
    setServerUiState(false);
    ui->clientCountLabel->setText(QStringLiteral("0"));
}

void MainWindow::appendLog(const QString &message)
{
    ui->eventLogEdit->appendPlainText(
        QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                                      message));
}

void MainWindow::setServerUiState(bool listening)
{
    ui->bindAddressEdit->setEnabled(!listening);
    ui->portSpinBox->setEnabled(!listening);
    ui->startButton->setEnabled(!listening && server.isInitialized());
    ui->stopButton->setEnabled(listening);
    ui->serverStateLabel->setText(listening ? tr("Listening") : tr("Stopped"));
    if (!listening) {
        ui->clientCountLabel->setText(QStringLiteral("0"));
    }
}
