#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QMessageBox>
#include <QTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    QString configurationError;
    configurationLoaded = RdpServerSettings::loadDefault(&configuration,
                                                          &configurationError);
    if (configurationLoaded) {
        ui->bindAddressEdit->setText(configuration.bindAddress);
        ui->portSpinBox->setValue(static_cast<int>(configuration.port));
    }

    connect(ui->startButton, &QPushButton::clicked, this, &MainWindow::startServer);
    connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::stopServer);
    connect(&server,
            &RdpServer::listeningStarted,
            this,
            [this](const QString &, quint16) {
                setServerUiState(true);
            });
    connect(&server, &RdpServer::listeningStopped, this, [this]() {
        setServerUiState(false);
    });
    connect(&server,
            &RdpServer::clientConnected,
            this,
            [this](quint64, const QString &) {
                ui->clientCountLabel->setText(QString::number(server.sessionCount()));
            });
    connect(&server,
            &RdpServer::clientDisconnected,
            this,
            [this](quint64, const QString &) {
                ui->clientCountLabel->setText(QString::number(server.sessionCount()));
            });
    connect(&server, &RdpServer::errorOccurred, this, [this](const QString &message) {
        statusBar()->showMessage(message);
    });
    connect(&server,
            &RdpServer::logMessage,
            this,
            [this](RdpServerLogLevel level,
                   const QString &category,
                   const QString &message) {
                appendLog(QStringLiteral("[%1] [%2] %3")
                              .arg(rdpServerLogLevelName(level), category, message));
            });

    setServerUiState(false);
    if (!configurationLoaded) {
        ui->startButton->setEnabled(false);
        appendLog(tr("Configuration failed: %1").arg(configurationError));
        statusBar()->showMessage(configurationError);
    } else if (!server.isInitialized()) {
        const QString message = server.lastError();
        ui->startButton->setEnabled(false);
        appendLog(tr("Initialization failed: %1").arg(message));
        statusBar()->showMessage(message);
    } else {
        appendLog(tr("Configuration loaded from %1.")
                      .arg(RdpServerSettings::defaultFilePath()));
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
    RdpServerConfiguration startConfiguration = configuration;
    startConfiguration.bindAddress = ui->bindAddressEdit->text();
    startConfiguration.port = static_cast<quint32>(ui->portSpinBox->value());

    if (server.start(startConfiguration)) {
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
    ui->startButton->setEnabled(!listening && server.isInitialized() && configurationLoaded);
    ui->stopButton->setEnabled(listening);
    ui->serverStateLabel->setText(listening ? tr("Listening") : tr("Stopped"));
    if (!listening) {
        ui->clientCountLabel->setText(QStringLiteral("0"));
    }
}
