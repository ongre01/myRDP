#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QMessageBox>
#include <QStatusBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    connect(ui->connectButton, &QPushButton::clicked, this, &MainWindow::connectToServer);
    connect(ui->disconnectButton,
            &QPushButton::clicked,
            this,
            &MainWindow::disconnectFromServer);

    if (client.isInitialized()) {
        setConnectionUiState(ConnectionUiState::Disconnected);
    } else {
        showConnectionError(client.lastError());
    }
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::connectToServer()
{
    const ConnectionInfo connectionInfo {
        ui->serverAddressEdit->text(),
        ui->portSpinBox->value()
    };

    setConnectionUiState(ConnectionUiState::Connecting,
                         tr("Connecting to %1:%2...")
                             .arg(connectionInfo.serverAddress.trimmed())
                             .arg(connectionInfo.port));

    if (!client.connectToServer(connectionInfo)) {
        showConnectionError(client.lastError());
        return;
    }

    const QString endpoint = tr("%1:%2")
                                 .arg(connectionInfo.serverAddress.trimmed())
                                 .arg(connectionInfo.port);
    setConnectionUiState(ConnectionUiState::Connected, tr("Connected to %1").arg(endpoint));
    statusBar()->showMessage(tr("Connected to %1").arg(endpoint));
}

void MainWindow::disconnectFromServer()
{
    client.disconnect();

    if (!client.lastError().isEmpty()) {
        showConnectionError(client.lastError());
        return;
    }

    setConnectionUiState(ConnectionUiState::Disconnected);
    statusBar()->showMessage(tr("Disconnected"), 3000);
}

void MainWindow::setConnectionUiState(ConnectionUiState state, const QString &message)
{
    const bool initialized = client.isInitialized();
    const bool connected = state == ConnectionUiState::Connected;
    const bool busy = state == ConnectionUiState::Connecting;

    ui->serverAddressEdit->setEnabled(initialized && !connected && !busy);
    ui->portSpinBox->setEnabled(initialized && !connected && !busy);
    ui->connectButton->setEnabled(initialized && !connected && !busy);
    ui->disconnectButton->setEnabled(initialized && connected);

    switch (state) {
    case ConnectionUiState::Disconnected:
        ui->connectionStatusValueLabel->setText(tr("Disconnected"));
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #6b7280;"));
        ui->remoteDesktopMessageLabel->setText(tr("Connect to an RDP server to begin."));
        break;
    case ConnectionUiState::Connecting:
        ui->connectionStatusValueLabel->setText(message.isEmpty() ? tr("Connecting...") : message);
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #9a6700;"));
        ui->remoteDesktopMessageLabel->setText(tr("Establishing the RDP connection..."));
        break;
    case ConnectionUiState::Connected:
        ui->connectionStatusValueLabel->setText(message.isEmpty() ? tr("Connected") : message);
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #1a7f37;"));
        ui->remoteDesktopMessageLabel->setText(tr("Waiting for the remote desktop..."));
        break;
    case ConnectionUiState::Error:
        ui->connectionStatusValueLabel->setText(tr("Connection failed"));
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #cf222e;"));
        ui->remoteDesktopMessageLabel->setText(tr("The remote desktop is unavailable."));
        break;
    }
}

void MainWindow::showConnectionError(const QString &message)
{
    const QString errorMessage = message.isEmpty()
                                     ? tr("An unknown RDP connection error occurred.")
                                     : message;
    setConnectionUiState(ConnectionUiState::Error);
    statusBar()->showMessage(errorMessage);
    QMessageBox::critical(this, tr("RDP Connection Error"), errorMessage);
}
