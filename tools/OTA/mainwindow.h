#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QByteArray>
#include <QMainWindow>
#include <QList>
#include <QString>

#include <cstdint>

class QByteArray;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSerialPort;
class QSerialPortInfo;
class QSplitter;
class QVBoxLayout;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class ThemeMode {
        System,
        Dark,
        Light
    };

    enum class UpgradeState {
        Idle,
        Running,
        Cancelling,
        Completed,
        Failed
    };

    enum class ProtocolCommand : quint8 {
        Handshake = 0x01,
        Start = 0x02,
        Data = 0x03,
        End = 0x04,
        Jump = 0x05,
        Ack = 0x80,
        Nack = 0x81
    };

    struct ParsedFrame {
        quint8 command = 0;
        quint8 sequence = 0;
        QByteArray payload;
    };

    struct AckInfo {
        quint8 originalCommand = 0;
        quint8 errorCode = 0;
        quint8 deviceState = 0;
    };

    enum class SerialMonitorEntryKind {
        System,
        Tx,
        Rx
    };

    struct SerialMonitorEntry {
        SerialMonitorEntryKind kind = SerialMonitorEntryKind::System;
        qint64 timestampMs = 0;
        QByteArray payload;
        QString message;
    };

    void buildUi();
    void setupDashboardStyle();
    void applyTheme(ThemeMode mode);
    ThemeMode resolveActiveTheme(ThemeMode requestedMode) const;
    QString buildStyleSheet(ThemeMode mode) const;
    void refreshMetricTileStyles();
    void populatePlaceholders();
    void wireInteractions();
    void refreshSerialPorts();
    void toggleSerialPort();
    void sendSerialText();
    void handleSerialReadyRead();
    void handleSerialError();
    void updateSerialUiState(bool isOpen);
    void updateUpgradeUiState(UpgradeState state);
    void loadFirmwareFile(const QString &filePath);
    QByteArray loadImageFile(const QString &filePath);
    QByteArray loadIntelHexFile(const QString &filePath);
    bool applyTargetAddressFromInput(bool reloadFirmware);
    bool parseTargetAddress(const QString &text, quint32 *address, QString *errorMessage = nullptr) const;
    bool validateFirmwareRange(quint32 startAddress, qsizetype imageSize, QString *errorMessage = nullptr) const;
    QString formatTargetAddress(quint32 address) const;
    void updateTargetAddressDisplay();
    void updateMaxFirmwareSizeDisplay();
    quint32 currentMaxFirmwareSize() const;
    void refreshFirmwareMeta();
    void startUpgrade();
    void stopUpgrade();
    bool performUpgrade();
    bool tryHandshake(int retries);
    bool sendCommand(ProtocolCommand command, quint8 sequence, const QByteArray &payload, AckInfo *ackInfo = nullptr);
    QByteArray buildFrame(ProtocolCommand command, quint8 sequence, const QByteArray &payload) const;
    bool waitForAck(ProtocolCommand command, quint8 sequence, AckInfo *ackInfo);
    bool readFrame(ParsedFrame *frame, int timeoutMs, int maxPayloadLength = -1);
    AckInfo parseAckPayload(const QByteArray &payload, ProtocolCommand expectedCommand, bool *ok = nullptr) const;
    quint16 crc16Ibm(const QByteArray &data) const;
    quint8 nextSequence(quint8 sequence) const;
    QString formatPortDescription(const QSerialPortInfo &info) const;
    QByteArray encodeSerialText(const QString &text) const;
    QString decodeReceivedData(const QByteArray &data) const;
    QString describeBootState(quint8 state) const;
    QString describeErrorCode(quint8 code) const;
    void appendSerialMonitorEntry(SerialMonitorEntryKind kind, const QByteArray &payload = QByteArray(), const QString &message = QString());
    QString formatSerialMonitorEntry(const SerialMonitorEntry &entry) const;
    void rerenderSerialMonitorHistory();
    void appendLog(QPlainTextEdit *target, const QString &message);
    QFrame *createCard(const QString &title, const QString &subtitle, QVBoxLayout **bodyLayout);
    QFrame *createMetricTile(
        const QString &title,
        const QString &value,
        const QString &note,
        const QString &accent
    );

    Ui::MainWindow *ui;
    QComboBox *comboTheme = nullptr;
    QComboBox *comboPort = nullptr;
    QComboBox *comboBaud = nullptr;
    QComboBox *comboDataBits = nullptr;
    QComboBox *comboParity = nullptr;
    QComboBox *comboStopBits = nullptr;
    QComboBox *comboEncoding = nullptr;
    QLineEdit *editFilePath = nullptr;
    QLineEdit *editTargetAddress = nullptr;
    QLineEdit *editTx = nullptr;
    QLabel *labelFileMeta = nullptr;
    QLabel *labelTransportState = nullptr;
    QLabel *labelPortDescription = nullptr;
    QLabel *labelFirmwareType = nullptr;
    QLabel *labelImageSize = nullptr;
    QLabel *labelChunkSize = nullptr;
    QLabel *labelMaxDownloadSize = nullptr;
    QLabel *labelProtocolState = nullptr;
    QPlainTextEdit *textOtaLog = nullptr;
    QPlainTextEdit *textSerialMonitor = nullptr;
    QProgressBar *progressUpgrade = nullptr;
    QFrame *tileTransport = nullptr;
    QFrame *tileFirmware = nullptr;
    QFrame *tileTarget = nullptr;
    QFrame *tileMaxDownloadSize = nullptr;
    QPushButton *btnRefreshPorts = nullptr;
    QPushButton *btnOpenPort = nullptr;
    QPushButton *btnBrowseFile = nullptr;
    QPushButton *btnStartUpgrade = nullptr;
    QPushButton *btnStopUpgrade = nullptr;
    QPushButton *btnSendText = nullptr;
    QPushButton *btnClearSerial = nullptr;
    QCheckBox *chkTimestamp = nullptr;
    QCheckBox *chkHexDisplay = nullptr;
    QSerialPort *serialPort = nullptr;

    QByteArray firmwareImage;
    QByteArray serialRxBuffer;
    QList<SerialMonitorEntry> serialMonitorHistory;
    QString firmwareFilePath;
    quint32 targetAddress = 0x08004000u;
    quint32 firmwareValidatedAddress = 0x08004000u;
    bool firmwareLoadedFromHex = false;
    ThemeMode currentTheme = ThemeMode::System;
    UpgradeState currentUpgradeState = UpgradeState::Idle;
    bool themeUpdateInProgress = false;
    bool cancelUpgradeRequested = false;
};

#endif // MAINWINDOW_H
