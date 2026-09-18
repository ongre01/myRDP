#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTime>
#include <QTimer>

MainWindow::MainWindow(bool startHidden, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    const QIcon applicationIcon = style()->standardIcon(QStyle::SP_ComputerIcon);
    setWindowIcon(applicationIcon);
    trayIcon = new QSystemTrayIcon(applicationIcon, this);
    trayMenu = new QMenu(this);
    trayStatusAction = trayMenu->addAction(tr("Status: Stopped"));
    trayStatusAction->setEnabled(false);
    trayMenu->addSeparator();
    QAction *showSettingsAction = trayMenu->addAction(tr("Open settings"));
    trayStartAction = trayMenu->addAction(tr("Start server"));
    trayStopAction = trayMenu->addAction(tr("Stop server"));
    trayMenu->addSeparator();
    QAction *quitAction = trayMenu->addAction(tr("Exit"));
    trayIcon->setContextMenu(trayMenu);

    connect(showSettingsAction, &QAction::triggered, this, &MainWindow::showSettings);
    connect(trayStartAction, &QAction::triggered, this, [this]() {
        tryStartServer(false);
    });
    connect(trayStopAction, &QAction::triggered, this, &MainWindow::stopServer);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);
    connect(trayIcon,
            &QSystemTrayIcon::activated,
            this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    showSettings();
                }
            });

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon->show();
    }

    QString configurationError;
    configurationLoaded = RdpServerSettings::loadDefault(&configuration,
                                                          &configurationError);
    if (configurationLoaded) {
        ui->bindAddressEdit->setText(configuration.bindAddress);
        ui->portSpinBox->setValue(static_cast<int>(configuration.port));
    }

    ui->autoStartCheckBox->setEnabled(autoStartManager.isSupported());
    ui->autoStartCheckBox->setChecked(autoStartManager.isEnabled());
    if (!autoStartManager.isSupported()) {
        ui->autoStartCheckBox->setToolTip(
            tr("Automatic startup is currently available on Windows only."));
    }

    connect(ui->startButton, &QPushButton::clicked, this, &MainWindow::startServer);
    connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::stopServer);
    connect(ui->autoStartCheckBox,
            &QCheckBox::toggled,
            this,
            &MainWindow::setAutoStartEnabled);
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
                updateStatusPresentation();
            });
    connect(&server,
            &RdpServer::clientDisconnected,
            this,
            [this](quint64, const QString &) {
                ui->clientCountLabel->setText(QString::number(server.sessionCount()));
                updateStatusPresentation();
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
        appendLog(tr("Server initialized. Starting the configured endpoint."));
        QTimer::singleShot(0, this, [this]() {
            tryStartServer(false);
        });
    }

    if (startHidden && !trayIcon->isVisible()) {
        appendLog(tr("The system tray is unavailable; the server will continue in the background."));
    }
}

MainWindow::~MainWindow()
{
    server.stop();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!quitRequested && trayIcon->isVisible()) {
        hide();
        event->ignore();
        if (!hideNotificationShown) {
            hideNotificationShown = true;
            showTrayMessage(tr("QtRdp Server"),
                            tr("The server is still running in the background."));
        }
        return;
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::startServer()
{
    tryStartServer(true);
}

void MainWindow::tryStartServer(bool interactive)
{
    if (server.isListening()) {
        return;
    }

    RdpServerConfiguration startConfiguration = configuration;
    startConfiguration.bindAddress = ui->bindAddressEdit->text();
    startConfiguration.port = static_cast<quint32>(ui->portSpinBox->value());

    if (server.start(startConfiguration)) {
        return;
    }

    const QString message = server.lastError();
    appendLog(tr("Start failed: %1").arg(message));
    statusBar()->showMessage(message);
    updateStatusPresentation();
    if (interactive && isVisible()) {
        QMessageBox::critical(this, tr("RDP Server"), message);
    } else {
        showTrayMessage(tr("RDP Server could not start"), message);
    }
}

void MainWindow::stopServer()
{
    server.stop();
    setServerUiState(false);
    ui->clientCountLabel->setText(QStringLiteral("0"));
}

void MainWindow::showSettings()
{
    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
    activateWindow();
}

void MainWindow::quitApplication()
{
    quitRequested = true;
    server.stop();
    trayIcon->hide();
    QApplication::quit();
}

void MainWindow::setAutoStartEnabled(bool enabled)
{
    QString errorMessage;
    if (!autoStartManager.setEnabled(enabled, &errorMessage)) {
        const QSignalBlocker blocker(ui->autoStartCheckBox);
        ui->autoStartCheckBox->setChecked(autoStartManager.isEnabled());
        appendLog(tr("Automatic startup update failed: %1").arg(errorMessage));
        statusBar()->showMessage(errorMessage);
        QMessageBox::warning(this, tr("Automatic startup"), errorMessage);
        return;
    }

    const QString message = enabled
                                ? tr("Automatic startup is enabled for this Windows user.")
                                : tr("Automatic startup is disabled.");
    appendLog(message);
    statusBar()->showMessage(message, 5000);
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
    updateStatusPresentation();
}

void MainWindow::updateStatusPresentation()
{
    const bool listening = server.isListening();
    const qsizetype sessionCount = listening ? server.sessionCount() : 0;
    QString status;
    if (listening) {
        QString address = ui->bindAddressEdit->text().trimmed();
        if (address.isEmpty() || address == QStringLiteral("0.0.0.0")
            || address == QStringLiteral("::")) {
            address = tr("all interfaces");
        }
        status = tr("Listening on %1:%2 — %n active session(s)",
                    nullptr,
                    static_cast<int>(sessionCount))
                     .arg(address)
                     .arg(ui->portSpinBox->value());
    } else {
        status = tr("Stopped");
    }

    trayStatusAction->setText(tr("Status: %1").arg(status));
    trayStartAction->setEnabled(!listening && server.isInitialized() && configurationLoaded);
    trayStopAction->setEnabled(listening);
    trayIcon->setToolTip(tr("QtRdp Server\n%1").arg(status));
}

void MainWindow::showTrayMessage(const QString &title, const QString &message)
{
    if (trayIcon->isVisible() && QSystemTrayIcon::supportsMessages()) {
        trayIcon->showMessage(title, message, QSystemTrayIcon::Information, 5000);
    }
}
