#include "mainwindow.h"
#include "clientsettings.h"
#include "ui_mainwindow.h"

#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QImage>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QStringList>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    const ConnectionDefaults defaults = ClientSettings::loadConnectionDefaults();
    ui->serverAddressEdit->setText(defaults.serverAddress);
    ui->portSpinBox->setValue(defaults.port);
    ui->usernameEdit->setText(defaults.username);
    ui->domainEdit->setText(defaults.domain);

    connect(ui->connectButton, &QPushButton::clicked, this, &MainWindow::connectToServer);
    connect(ui->disconnectButton,
            &QPushButton::clicked,
            this,
            &MainWindow::disconnectFromServer);
    connect(&rdpEventTimer, &QTimer::timeout, this, &MainWindow::processRdpEvents);
    rdpEventTimer.setInterval(16);
    rdpEventTimer.setTimerType(Qt::PreciseTimer);
    client.setDesktopUpdateHandler(
        [this](const DesktopUpdate &desktopUpdate) { displayDesktopUpdate(desktopUpdate); });
    QClipboard *clipboard = QApplication::clipboard();
    client.setClipboardTextHandler([this, clipboard](const QString &text) {
        if (clipboard->text() == text) {
            return;
        }

        QScopedValueRollback<bool> applyingClipboardText(applyingRemoteClipboard, true);
        clipboard->setText(text);
    });
    connect(clipboard, &QClipboard::dataChanged, this, [this, clipboard]() {
        if (applyingRemoteClipboard) {
            return;
        }

        if (!client.sendClipboardText(clipboard->text())) {
            handleInputError();
        }
    });
    client.sendClipboardText(clipboard->text());
    ui->remoteDesktopView->setInputHandlers(
        [this](const RdpKeyboardInput &input) {
            if (client.sendKeyboardInput(input)) {
                return true;
            }
            handleInputError();
            return false;
        },
        [this](const RdpPointerInput &input) {
            if (client.sendPointerInput(input)) {
                return true;
            }
            handleInputError();
            return false;
        });

    if (client.isInitialized()) {
        setConnectionUiState(ConnectionUiState::Disconnected);
    } else {
        showConnectionError(client.lastError());
    }
}

MainWindow::~MainWindow()
{
    rdpEventTimer.stop();
    ui->remoteDesktopView->setInputHandlers({}, {});
    client.setDesktopUpdateHandler({});
    client.setClipboardTextHandler({});
    client.disconnect();
    delete ui;
}

void MainWindow::connectToServer()
{
    ConnectionInfo connectionInfo;
    connectionInfo.serverAddress = ui->serverAddressEdit->text();
    connectionInfo.port = ui->portSpinBox->value();
    connectionInfo.username = ui->usernameEdit->text();
    connectionInfo.password = ui->passwordEdit->text();
    connectionInfo.domain = ui->domainEdit->text();
    connectionInfo.certificateVerifier = [this](const CertificateInfo &certificate) {
        return verifyServerCertificate(certificate);
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
    rdpEventTimer.start();
}

void MainWindow::disconnectFromServer()
{
    rdpEventTimer.stop();
    client.disconnect();

    if (!client.lastError().isEmpty()) {
        showConnectionError(client.lastError());
        return;
    }

    setConnectionUiState(ConnectionUiState::Disconnected);
    statusBar()->showMessage(tr("Disconnected"), 3000);
}

void MainWindow::processRdpEvents()
{
    if (client.processEvents()) {
        return;
    }

    rdpEventTimer.stop();
    showConnectionError(client.lastError());
}

void MainWindow::setConnectionUiState(ConnectionUiState state, const QString &message)
{
    const bool initialized = client.isInitialized();
    const bool connected = state == ConnectionUiState::Connected;
    const bool busy = state == ConnectionUiState::Connecting;

    ui->remoteDesktopView->setInputEnabled(connected);

    ui->serverAddressEdit->setEnabled(initialized && !connected && !busy);
    ui->portSpinBox->setEnabled(initialized && !connected && !busy);
    ui->usernameEdit->setEnabled(initialized && !connected && !busy);
    ui->passwordEdit->setEnabled(initialized && !connected && !busy);
    ui->domainEdit->setEnabled(initialized && !connected && !busy);
    ui->connectButton->setEnabled(initialized && !connected && !busy);
    ui->disconnectButton->setEnabled(initialized && connected);

    switch (state) {
    case ConnectionUiState::Disconnected:
        ui->connectionStatusValueLabel->setText(tr("Disconnected"));
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #6b7280;"));
        ui->remoteDesktopView->clearDesktop();
        ui->remoteDesktopView->setPlaceholderMessage(tr("Connect to an RDP server to begin."));
        break;
    case ConnectionUiState::Connecting:
        ui->connectionStatusValueLabel->setText(message.isEmpty() ? tr("Connecting...") : message);
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #9a6700;"));
        ui->remoteDesktopView->clearDesktop();
        ui->remoteDesktopView->setPlaceholderMessage(tr("Establishing the RDP connection..."));
        break;
    case ConnectionUiState::Connected:
        ui->connectionStatusValueLabel->setText(message.isEmpty() ? tr("Connected") : message);
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #1a7f37;"));
        ui->remoteDesktopView->setPlaceholderMessage(tr("Waiting for the remote desktop..."));
        ui->remoteDesktopView->setFocus(Qt::OtherFocusReason);
        break;
    case ConnectionUiState::Error:
        ui->connectionStatusValueLabel->setText(tr("Connection failed"));
        ui->connectionStatusValueLabel->setStyleSheet(QStringLiteral("color: #cf222e;"));
        ui->remoteDesktopView->clearDesktop();
        ui->remoteDesktopView->setPlaceholderMessage(tr("The remote desktop is unavailable."));
        break;
    }
}

void MainWindow::handleInputError()
{
    if (handlingInputError) {
        return;
    }

    handlingInputError = true;
    const QString inputError = client.lastError();
    rdpEventTimer.stop();
    client.disconnect();
    showConnectionError(inputError);
    handlingInputError = false;
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

void MainWindow::displayDesktopUpdate(const DesktopUpdate &desktopUpdate)
{
    if (desktopUpdate.pixels.isEmpty() || desktopUpdate.bytesPerLine <= 0
        || desktopUpdate.dirtyRect.isEmpty()) {
        return;
    }

    const QImage regionImage(reinterpret_cast<const uchar *>(desktopUpdate.pixels.constData()),
                             desktopUpdate.dirtyRect.width(),
                             desktopUpdate.dirtyRect.height(),
                             desktopUpdate.bytesPerLine,
                             QImage::Format_RGB32);
    ui->remoteDesktopView->updateDesktopRegion(desktopUpdate.desktopSize,
                                               desktopUpdate.dirtyRect,
                                               regionImage);
}

CertificateDecision MainWindow::verifyServerCertificate(const CertificateInfo &certificate)
{
    QStringList details;
    details << tr("Server: %1:%2").arg(certificate.host).arg(certificate.port)
            << tr("Common name: %1").arg(certificate.commonName)
            << tr("Subject: %1").arg(certificate.subject)
            << tr("Issuer: %1").arg(certificate.issuer)
            << tr("Fingerprint: %1").arg(certificate.fingerprint);

    if (certificate.hostNameMismatch) {
        details << tr("Warning: The certificate name does not match the server address.");
    }

    if (certificate.changed) {
        details << QString()
                << tr("The certificate has changed since the previous connection.")
                << tr("Previous subject: %1").arg(certificate.oldSubject)
                << tr("Previous issuer: %1").arg(certificate.oldIssuer)
                << tr("Previous fingerprint: %1").arg(certificate.oldFingerprint);
    }

    QMessageBox dialog(certificate.changed ? QMessageBox::Critical : QMessageBox::Warning,
                       certificate.changed ? tr("RDP Server Certificate Changed")
                                           : tr("Untrusted RDP Server Certificate"),
                       details.join(QLatin1Char('\n')),
                       QMessageBox::NoButton,
                       this);
    dialog.setTextFormat(Qt::PlainText);
    dialog.setInformativeText(tr("Verify the certificate details before continuing."));
    QPushButton *trustButton = dialog.addButton(tr("Trust for this connection"),
                                                QMessageBox::AcceptRole);
    dialog.addButton(QMessageBox::Cancel);
    dialog.exec();

    return dialog.clickedButton() == trustButton ? CertificateDecision::TrustOnce
                                                  : CertificateDecision::Reject;
}
