#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QStyleHints>
#include <QTextCursor>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

constexpr quint32 kDefaultAppAddress = 0x08004000u;
constexpr int kDefaultChunkSize = 128;
constexpr int kDefaultTimeoutMs = 1000;
constexpr int kReadPollIntervalMs = 50;
constexpr int kHandshakeRetries = 5;

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , serialPort(new QSerialPort(this))
{
    ui->setupUi(this);
    buildUi();
    setupDashboardStyle();
    populatePlaceholders();
    wireInteractions();
    refreshSerialPorts();
}

MainWindow::~MainWindow()
{
    if (serialPort->isOpen()) {
        serialPort->close();
    }
    delete ui;
}

void MainWindow::buildUi()
{
    setWindowTitle("OTA升级控制台");
    resize(1480, 920);
    setMinimumSize(1280, 800);

    auto *centralHost = new QWidget(this);
    auto *root = new QVBoxLayout(centralHost);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(18);
    setCentralWidget(centralHost);

    auto *hero = new QFrame(centralHost);
    hero->setObjectName("heroPanel");
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(26, 22, 26, 22);
    heroLayout->setSpacing(16);

    auto *heroTextLayout = new QVBoxLayout;
    heroTextLayout->setSpacing(6);
    auto *title = new QLabel("OTA升级控制台", hero);
    auto *subtitle = new QLabel("面向物联网工程场景的串口升级与调试界面", hero);
    title->setObjectName("heroTitle");
    subtitle->setObjectName("heroSubtitle");
    heroTextLayout->addWidget(title);
    heroTextLayout->addWidget(subtitle);
    heroTextLayout->addStretch();

    comboTheme = new QComboBox(this);
    comboTheme->setObjectName("themeCombo");
    comboTheme->setMinimumWidth(136);
    comboTheme->setView(new QListView(comboTheme));
    comboTheme->addItem("跟随系统", static_cast<int>(ThemeMode::System));
    comboTheme->addItem("深色模式", static_cast<int>(ThemeMode::Dark));
    comboTheme->addItem("浅色模式", static_cast<int>(ThemeMode::Light));

    auto *themeWrap = new QVBoxLayout;
    themeWrap->setSpacing(8);
    auto *themeLabel = new QLabel("界面主题", hero);
    themeLabel->setObjectName("themeLabel");
    themeWrap->addWidget(themeLabel, 0, Qt::AlignRight);
    themeWrap->addWidget(comboTheme, 0, Qt::AlignRight);
    themeWrap->addStretch();

    heroLayout->addLayout(heroTextLayout, 1);
    heroLayout->addLayout(themeWrap);
    root->addWidget(hero);

    auto *content = new QHBoxLayout;
    content->setSpacing(18);
    root->addLayout(content, 1);

    auto *leftScroll = new QScrollArea(this);
    leftScroll->setObjectName("sideScroll");
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    leftScroll->setFixedWidth(446);

    auto *leftPanel = new QFrame(leftScroll);
    leftPanel->setObjectName("sidePanel");
    leftPanel->setMinimumWidth(430);
    leftPanel->setMaximumWidth(430);

    auto *leftColumn = new QVBoxLayout(leftPanel);
    leftColumn->setContentsMargins(0, 0, 0, 0);
    leftColumn->setSpacing(18);
    leftScroll->setWidget(leftPanel);
    content->addWidget(leftScroll, 0);

    auto *rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(18);
    content->addLayout(rightColumn, 1);

    comboPort = new QComboBox(this);
    comboBaud = new QComboBox(this);
    comboDataBits = new QComboBox(this);
    comboParity = new QComboBox(this);
    comboStopBits = new QComboBox(this);
    for (QComboBox *box : {comboPort, comboBaud, comboDataBits, comboParity, comboStopBits}) {
        box->setMinimumHeight(36);
        box->setView(new QListView(box));
    }
    comboBaud->setEditable(true);
    comboBaud->setInsertPolicy(QComboBox::NoInsert);

    editFilePath = new QLineEdit(this);
    editFilePath->setReadOnly(true);
    editFilePath->setPlaceholderText("选择 .bin 或 .hex 固件文件");
    editTx = new QLineEdit(this);
    editTx->setPlaceholderText("输入要发送的调试内容");

    btnRefreshPorts = new QPushButton("扫描", this);
    btnOpenPort = new QPushButton("打开", this);
    btnBrowseFile = new QPushButton("选择文件", this);
    btnStartUpgrade = new QPushButton("开始升级", this);
    btnStopUpgrade = new QPushButton("停止", this);
    btnSendText = new QPushButton("发送", this);
    btnClearSerial = new QPushButton("清空", this);
    btnSwapPanels = new QPushButton("切换上下位置", this);
    btnOpenPort->setObjectName("primaryButton");
    btnBrowseFile->setObjectName("accentButton");
    btnStartUpgrade->setObjectName("primaryButton");
    btnStopUpgrade->setObjectName("dangerButton");
    btnSendText->setObjectName("accentButton");
    btnSwapPanels->setObjectName("accentButton");

    auto addLabel = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setObjectName("fieldLabel");
        return label;
    };

    QVBoxLayout *serialBody = nullptr;
    auto *serialCard = createCard("串口连接", "通过下拉框选择端口与波特率，并查看设备描述信息", &serialBody);
    leftColumn->addWidget(serialCard);

    serialBody->addWidget(addLabel("端口选择"));
    auto *portRow = new QHBoxLayout;
    portRow->setSpacing(10);
    portRow->addWidget(comboPort, 1);
    btnRefreshPorts->setFixedWidth(88);
    portRow->addWidget(btnRefreshPorts);
    serialBody->addLayout(portRow);

    labelPortDescription = new QLabel(
        "端口名：--\n"
        "描述：等待扫描串口\n"
        "厂商：--\n"
        "序列号：--\n"
        "VID/PID：--",
        serialCard
    );
    labelPortDescription->setObjectName("infoText");
    labelPortDescription->setWordWrap(true);
    serialBody->addWidget(labelPortDescription);

    serialBody->addWidget(addLabel("波特率"));
    auto *baudRow = new QHBoxLayout;
    baudRow->setSpacing(10);
    baudRow->addWidget(comboBaud, 1);
    btnOpenPort->setFixedWidth(120);
    baudRow->addWidget(btnOpenPort);
    serialBody->addLayout(baudRow);

    auto *paramGrid = new QGridLayout;
    paramGrid->setHorizontalSpacing(10);
    paramGrid->setVerticalSpacing(8);
    paramGrid->addWidget(addLabel("数据位"), 0, 0);
    paramGrid->addWidget(addLabel("校验位"), 0, 1);
    paramGrid->addWidget(addLabel("停止位"), 0, 2);
    paramGrid->addWidget(comboDataBits, 1, 0);
    paramGrid->addWidget(comboParity, 1, 1);
    paramGrid->addWidget(comboStopBits, 1, 2);
    serialBody->addLayout(paramGrid);

    labelTransportState = new QLabel("当前未应用串口配置", serialCard);
    labelTransportState->setObjectName("statusChipIdle");
    labelTransportState->setAlignment(Qt::AlignCenter);
    labelTransportState->setMinimumHeight(34);
    serialBody->addWidget(labelTransportState);

    QVBoxLayout *fileBody = nullptr;
    auto *fileCard = createCard("固件文件", "选择升级文件并查看基础信息", &fileBody);
    leftColumn->addWidget(fileCard);

    auto *fileRow = new QHBoxLayout;
    fileRow->setSpacing(10);
    fileRow->addWidget(editFilePath, 1);
    btnBrowseFile->setFixedWidth(110);
    fileRow->addWidget(btnBrowseFile);
    fileBody->addLayout(fileRow);

    labelFileMeta = new QLabel("当前未加载固件文件。", fileCard);
    labelFileMeta->setObjectName("infoText");
    labelFileMeta->setWordWrap(true);
    fileBody->addWidget(labelFileMeta);

    auto createInfoCell = [this](const QString &titleText, const QString &valueText, QLabel **valueLabel) {
        auto *box = new QFrame(this);
        box->setObjectName("infoCell");
        auto *layout = new QVBoxLayout(box);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(2);
        auto *titleLabel = new QLabel(titleText, box);
        titleLabel->setObjectName("infoCellTitle");
        auto *value = new QLabel(valueText, box);
        value->setObjectName("infoCellValue");
        layout->addWidget(titleLabel);
        layout->addWidget(value);
        *valueLabel = value;
        return box;
    };

    auto *fileInfoGrid = new QGridLayout;
    fileInfoGrid->setHorizontalSpacing(10);
    fileInfoGrid->setVerticalSpacing(10);
    fileInfoGrid->addWidget(createInfoCell("文件类型", "--", &labelFirmwareType), 0, 0);
    fileInfoGrid->addWidget(createInfoCell("文件大小", "--", &labelImageSize), 0, 1);
    fileInfoGrid->addWidget(createInfoCell("分包大小", QString("%1 字节").arg(kDefaultChunkSize), &labelChunkSize), 1, 0);
    fileInfoGrid->addWidget(createInfoCell("协议状态", "就绪", &labelProtocolState), 1, 1);
    fileBody->addLayout(fileInfoGrid);

    QVBoxLayout *actionBody = nullptr;
    auto *actionCard = createCard("升级控制", "执行握手、开始传输、分包发送、结束校验与可选跳转", &actionBody);
    leftColumn->addWidget(actionCard);

    progressUpgrade = new QProgressBar(this);
    progressUpgrade->setRange(0, 100);
    progressUpgrade->setValue(0);
    progressUpgrade->setFormat("就绪  %p%");
    actionBody->addWidget(progressUpgrade);

    auto *upgradeButtons = new QHBoxLayout;
    upgradeButtons->setSpacing(10);
    upgradeButtons->addWidget(btnStartUpgrade, 1);
    upgradeButtons->addWidget(btnStopUpgrade, 1);
    actionBody->addLayout(upgradeButtons);
    leftColumn->addStretch();

    auto *metricRow = new QHBoxLayout;
    metricRow->setSpacing(14);
    tileTransport = createMetricTile("当前链路", "UART", "串口调试已接入", "#19c2ff");
    tileFirmware = createMetricTile("固件格式", ".BIN / .HEX", "兼容 Intel HEX", "#58fba8");
    tileTarget = createMetricTile("目标地址", "0x08004000", "STM32F103 应用起始地址", "#ffb347");
    metricRow->addWidget(tileTransport);
    metricRow->addWidget(tileFirmware);
    metricRow->addWidget(tileTarget);
    rightColumn->addLayout(metricRow);

    auto *workspaceCard = new QFrame(this);
    workspaceCard->setObjectName("cardFrame");
    auto *workspaceLayout = new QVBoxLayout(workspaceCard);
    workspaceLayout->setContentsMargins(18, 16, 18, 16);
    workspaceLayout->setSpacing(12);

    auto *workspaceHeader = new QHBoxLayout;
    workspaceHeader->setSpacing(12);
    auto *workspaceTitleWrap = new QVBoxLayout;
    workspaceTitleWrap->setSpacing(4);
    auto *workspaceTitle = new QLabel("工作区布局", workspaceCard);
    workspaceTitle->setObjectName("cardTitle");
    auto *workspaceSubtitle = new QLabel("保持一体化卡片风格，可拖动分割条调整大小，并可一键切换上下顺序", workspaceCard);
    workspaceSubtitle->setObjectName("cardSubtitle");
    workspaceSubtitle->setWordWrap(true);
    workspaceTitleWrap->addWidget(workspaceTitle);
    workspaceTitleWrap->addWidget(workspaceSubtitle);
    workspaceHeader->addLayout(workspaceTitleWrap, 1);
    workspaceHeader->addWidget(btnSwapPanels, 0, Qt::AlignTop);
    workspaceLayout->addLayout(workspaceHeader);

    workspaceSplitter = new QSplitter(Qt::Vertical, workspaceCard);
    workspaceSplitter->setObjectName("workspaceSplitter");
    workspaceSplitter->setChildrenCollapsible(false);
    workspaceSplitter->setHandleWidth(10);

    QVBoxLayout *otaBody = nullptr;
    cardUpgradeLog = createCard("升级日志", "显示串口扫描、协议阶段、分包进度与错误信息", &otaBody);
    textOtaLog = new QPlainTextEdit(this);
    textOtaLog->setReadOnly(true);
    textOtaLog->setObjectName("telemetryPanel");
    otaBody->addWidget(textOtaLog, 1);

    QVBoxLayout *debugBody = nullptr;
    cardSerialDebug = createCard("串口调试窗口", "用于查看串口收发内容，并手动发送调试数据", &debugBody);
    textSerialMonitor = new QPlainTextEdit(this);
    textSerialMonitor->setReadOnly(true);
    textSerialMonitor->setObjectName("terminalPanel");
    debugBody->addWidget(textSerialMonitor, 1);

    auto *sendRow = new QHBoxLayout;
    sendRow->setSpacing(10);
    btnSendText->setFixedWidth(88);
    btnClearSerial->setFixedWidth(88);
    sendRow->addWidget(editTx, 1);
    sendRow->addWidget(btnSendText);
    sendRow->addWidget(btnClearSerial);
    debugBody->addLayout(sendRow);

    auto *optionRow = new QHBoxLayout;
    optionRow->setSpacing(16);
    chkTimestamp = new QCheckBox("显示时间戳", this);
    chkHexDisplay = new QCheckBox("HEX 显示", this);
    chkTimestamp->setChecked(true);
    optionRow->addWidget(chkTimestamp);
    optionRow->addWidget(chkHexDisplay);
    optionRow->addStretch();
    debugBody->addLayout(optionRow);

    workspaceSplitter->addWidget(cardUpgradeLog);
    workspaceSplitter->addWidget(cardSerialDebug);
    workspaceSplitter->setStretchFactor(0, 1);
    workspaceSplitter->setStretchFactor(1, 1);
    workspaceSplitter->setSizes({320, 280});
    workspaceLayout->addWidget(workspaceSplitter, 1);
    rightColumn->addWidget(workspaceCard, 1);
}

void MainWindow::setupDashboardStyle()
{
    applyTheme(currentTheme);
}

void MainWindow::applyTheme(ThemeMode mode)
{
    if (themeUpdateInProgress) {
        currentTheme = mode;
        return;
    }

    themeUpdateInProgress = true;
    currentTheme = mode;
    const ThemeMode activeTheme = resolveActiveTheme(mode);
    setStyleSheet(buildStyleSheet(activeTheme));
    refreshMetricTileStyles();
    themeUpdateInProgress = false;
}

MainWindow::ThemeMode MainWindow::resolveActiveTheme(ThemeMode requestedMode) const
{
    if (requestedMode != ThemeMode::System) {
        return requestedMode;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme colorScheme = QGuiApplication::styleHints()->colorScheme();
    if (colorScheme == Qt::ColorScheme::Dark) {
        return ThemeMode::Dark;
    }
    if (colorScheme == Qt::ColorScheme::Light) {
        return ThemeMode::Light;
    }
#endif

    const QColor windowColor = palette().color(QPalette::Window);
    return qGray(windowColor.rgb()) < 128 ? ThemeMode::Dark : ThemeMode::Light;
}

QString MainWindow::buildStyleSheet(ThemeMode mode) const
{
    if (mode == ThemeMode::Light) {
        return
            "QMainWindow { background: #eef4f8; }"
            "QWidget { color: #163047; font-family: 'Segoe UI', 'Microsoft YaHei UI'; font-size: 10.5pt; }"
            "#heroPanel { border: 1px solid rgba(91,145,186,0.25); border-radius: 22px; background-color: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #f6fbff, stop:1 #eaf4fb); }"
            "#heroTitle { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 21pt; font-weight: 700; color: #0f2a3f; }"
            "#heroSubtitle, #themeLabel { color: rgba(32,67,94,0.72); font-size: 10pt; }"
            "#sideScroll, #sidePanel { background: transparent; }"
            "#themeCombo { min-height: 38px; }"
            "#badgeAccent, #badgeOutline, #statusChipIdle { padding: 7px 14px; border-radius: 15px; font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 9pt; }"
            "#badgeAccent { color: #ffffff; background-color: #1b8cff; }"
            "#badgeOutline { color: #25608c; border: 1px solid rgba(37,96,140,0.25); background-color: rgba(255,255,255,0.7); }"
            "#statusChipIdle { color: #25608c; border: 1px solid rgba(37,96,140,0.22); background-color: rgba(255,255,255,0.8); }"
            "#cardFrame { border: 1px solid rgba(91,145,186,0.18); border-radius: 20px; background-color: rgba(255,255,255,0.86); }"
            "#cardTitle { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 13pt; color: #14314a; }"
            "#cardSubtitle, #fieldLabel { color: rgba(32,67,94,0.74); font-size: 9pt; }"
            "#infoCell { border: 1px solid rgba(91,145,186,0.16); border-radius: 12px; background-color: rgba(244,249,253,0.95); }"
            "#infoCellTitle { color: rgba(32,67,94,0.72); font-size: 8.6pt; }"
            "#infoCellValue { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 11pt; color: #14314a; }"
            "#metricTile { border-radius: 20px; }"
            "#metricTitle { color: rgba(32,67,94,0.72); font-size: 9pt; }"
            "#metricValue { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 17pt; color: #14314a; }"
            "#metricNote { color: rgba(32,67,94,0.60); font-size: 8.7pt; }"
            "#workspaceSplitter::handle { background-color: rgba(91,145,186,0.20); border-radius: 4px; margin: 3px 0; }"
            "#workspaceSplitter::handle:hover { background-color: rgba(27,140,255,0.32); }"
            "#infoText { color: #14314a; background-color: rgba(244,249,253,0.95); border: 1px solid rgba(91,145,186,0.16); border-radius: 12px; padding: 10px 12px; }"
            "QLineEdit, QComboBox, QPlainTextEdit { border: 1px solid rgba(91,145,186,0.22); border-radius: 12px; padding: 8px 10px; background-color: rgba(255,255,255,0.96); selection-background-color: #7cc4ff; }"
            "QComboBox { min-height: 36px; }"
            "QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus { border: 1px solid rgba(27,140,255,0.68); }"
            "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 28px; border: none; }"
            "QComboBox QAbstractItemView { background-color: #ffffff; color: #14314a; border: 1px solid rgba(27,140,255,0.24); selection-background-color: rgba(124,196,255,0.30); }"
            "#telemetryPanel, #terminalPanel { border-radius: 16px; padding: 12px; background-color: #fbfdff; border: 1px solid rgba(91,145,186,0.18); font-family: 'Consolas', 'Cascadia Mono'; font-size: 10pt; }"
            "QPushButton { min-height: 38px; border-radius: 14px; padding: 0 18px; border: 1px solid rgba(91,145,186,0.18); background-color: rgba(243,248,252,0.96); color: #163047; }"
            "QPushButton:hover { border-color: rgba(27,140,255,0.30); background-color: rgba(232,243,252,0.98); }"
            "QPushButton:pressed { background-color: rgba(220,235,248,0.98); }"
            "#primaryButton { color: #ffffff; font-weight: 700; border: none; background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1b8cff, stop:1 #31b2ff); }"
            "#accentButton { color: #1b6ea8; border: 1px solid rgba(27,110,168,0.24); background-color: rgba(237,247,255,0.96); }"
            "#dangerButton { color: #ffffff; border: none; background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ef6b73, stop:1 #d94e61); }"
            "QProgressBar { min-height: 24px; border-radius: 12px; text-align: center; color: #163047; background-color: rgba(238,244,249,0.96); border: 1px solid rgba(91,145,186,0.18); }"
            "QProgressBar::chunk { border-radius: 12px; background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1b8cff, stop:1 #31b2ff); }"
            "QCheckBox { color: rgba(22,48,71,0.86); }"
            "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px; border: 1px solid rgba(91,145,186,0.28); background-color: #ffffff; }"
            "QCheckBox::indicator:checked { background-color: #1b8cff; }"
            "QScrollBar:vertical { width: 10px; margin: 2px; border: none; background: transparent; }"
            "QScrollBar::handle:vertical { min-height: 28px; border-radius: 5px; background-color: rgba(91,145,186,0.48); }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }";
    }

    return
        "QMainWindow { background: #07111a; }"
        "QWidget { color: #dbe8f7; font-family: 'Segoe UI', 'Microsoft YaHei UI'; font-size: 10.5pt; }"
        "#heroPanel { border: 1px solid rgba(65,190,255,0.28); border-radius: 22px; background-color: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 rgba(11,32,47,240), stop:0.55 rgba(12,25,38,235), stop:1 rgba(22,44,61,225)); }"
        "#heroTitle { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 21pt; font-weight: 700; letter-spacing: 2px; color: #eef8ff; }"
        "#heroSubtitle, #themeLabel { color: rgba(203,224,242,0.72); font-size: 10pt; }"
        "#sideScroll, #sidePanel { background: transparent; }"
        "#themeCombo { min-height: 38px; }"
        "#badgeAccent, #badgeOutline, #statusChipIdle { padding: 7px 14px; border-radius: 15px; font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 9pt; }"
        "#badgeAccent { color: #05111a; background-color: #1be0ff; }"
        "#badgeOutline { color: #92efff; border: 1px solid rgba(27,224,255,0.35); background-color: rgba(8,30,43,0.85); }"
        "#statusChipIdle { color: #7fe8ff; border: 1px solid rgba(127,232,255,0.35); background-color: rgba(7,29,41,0.92); }"
        "#cardFrame { border: 1px solid rgba(93,148,183,0.20); border-radius: 20px; background-color: rgba(8,19,28,0.92); }"
        "#cardTitle { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 13pt; color: #f3fbff; }"
        "#cardSubtitle, #fieldLabel { color: rgba(188,209,226,0.66); font-size: 9pt; }"
        "#infoCell { border: 1px solid rgba(95,160,205,0.16); border-radius: 12px; background-color: rgba(10,26,38,0.92); }"
        "#infoCellTitle { color: rgba(170,204,228,0.72); font-size: 8.6pt; }"
        "#infoCellValue { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 11pt; color: #f4fbff; }"
        "#metricTile { border-radius: 20px; }"
        "#metricTitle { color: rgba(170,204,228,0.76); font-size: 9pt; }"
        "#metricValue { font-family: 'Bahnschrift SemiBold', 'Microsoft YaHei UI'; font-size: 17pt; color: #f4fbff; }"
        "#metricNote { color: rgba(188,209,226,0.60); font-size: 8.7pt; }"
        "#workspaceSplitter::handle { background-color: rgba(60,116,153,0.26); border-radius: 4px; margin: 3px 0; }"
        "#workspaceSplitter::handle:hover { background-color: rgba(24,219,255,0.34); }"
        "#infoText { color: rgba(188,215,232,0.92); background-color: rgba(9,29,42,0.75); border: 1px solid rgba(68,120,160,0.18); border-radius: 12px; padding: 10px 12px; }"
        "QLineEdit, QComboBox, QPlainTextEdit { border: 1px solid rgba(82,142,183,0.24); border-radius: 12px; padding: 8px 10px; background-color: rgba(11,25,37,0.94); selection-background-color: #0fb5ff; }"
        "QComboBox { min-height: 36px; }"
        "QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus { border: 1px solid rgba(46,197,255,0.68); }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 28px; border: none; }"
        "QComboBox QAbstractItemView { background-color: rgba(9,21,31,0.98); color: #dbe8f7; border: 1px solid rgba(46,197,255,0.30); selection-background-color: rgba(27,224,255,0.22); }"
        "#telemetryPanel, #terminalPanel { border-radius: 16px; padding: 12px; background-color: rgba(4,14,22,0.98); border: 1px solid rgba(60,116,153,0.22); font-family: 'Consolas', 'Cascadia Mono'; font-size: 10pt; }"
        "QPushButton { min-height: 38px; border-radius: 14px; padding: 0 18px; border: 1px solid rgba(89,136,168,0.20); background-color: rgba(19,39,55,0.96); }"
        "QPushButton:hover { border-color: rgba(84,208,255,0.42); background-color: rgba(24,48,67,0.98); }"
        "QPushButton:pressed { background-color: rgba(16,35,49,0.98); }"
        "#primaryButton { color: #04131e; font-weight: 700; border: none; background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1be0ff, stop:1 #00ffaa); }"
        "#accentButton { color: #0ed8ff; border: 1px solid rgba(14,216,255,0.32); background-color: rgba(6,28,41,0.96); }"
        "#dangerButton { color: #ffd2d4; border: 1px solid rgba(255,105,132,0.34); background-color: rgba(57,18,29,0.94); }"
        "QProgressBar { min-height: 24px; border-radius: 12px; text-align: center; color: #edf9ff; background-color: rgba(8,24,34,0.92); border: 1px solid rgba(83,130,167,0.22); }"
        "QProgressBar::chunk { border-radius: 12px; background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #15d0ff, stop:1 #00ff9c); }"
        "QCheckBox { color: rgba(201,221,236,0.82); }"
        "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px; border: 1px solid rgba(101,158,193,0.32); background-color: rgba(8,24,34,0.96); }"
        "QCheckBox::indicator:checked { background-color: #18dbff; }"
        "QScrollBar:vertical { width: 10px; margin: 2px; border: none; background: transparent; }"
        "QScrollBar::handle:vertical { min-height: 28px; border-radius: 5px; background-color: rgba(76,130,168,0.55); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }";
}

void MainWindow::refreshMetricTileStyles()
{
    const ThemeMode activeTheme = resolveActiveTheme(currentTheme);
    const QString bg = activeTheme == ThemeMode::Light
        ? "rgba(255, 255, 255, 0.88)"
        : "rgba(9, 21, 31, 0.95)";

    auto applyTile = [&bg](QFrame *tile, const QString &accent) {
        if (!tile) {
            return;
        }
        tile->setStyleSheet(
            QString(
                "#metricTile {"
                "  border: 1px solid %1;"
                "  border-radius: 20px;"
                "  background-color: %2;"
                "}"
            ).arg(accent, bg)
        );
    };

    applyTile(tileTransport, "#19c2ff");
    applyTile(tileFirmware, "#58fba8");
    applyTile(tileTarget, "#ffb347");
}

void MainWindow::populatePlaceholders()
{
    comboTheme->setCurrentIndex(0);
    comboBaud->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"});
    comboBaud->setCurrentText("115200");
    comboDataBits->addItems({"8", "7"});
    comboDataBits->setCurrentText("8");
    comboParity->addItems({"无校验", "偶校验", "奇校验"});
    comboParity->setCurrentIndex(0);
    comboStopBits->addItems({"1", "2"});
    comboStopBits->setCurrentText("1");

    labelFileMeta->setText("当前未加载固件文件。");
    labelFirmwareType->setText("--");
    labelImageSize->setText("--");
    labelChunkSize->setText(QString("%1 字节").arg(kDefaultChunkSize));
    labelProtocolState->setText("就绪");

    updateSerialUiState(false);
    updateUpgradeUiState(UpgradeState::Idle);
    appendLog(textOtaLog, "界面已加载完成。");
}

void MainWindow::wireInteractions()
{
    connect(comboTheme, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto mode = static_cast<ThemeMode>(comboTheme->itemData(index).toInt());
        applyTheme(mode);
        if (mode == ThemeMode::System) {
            appendLog(textOtaLog, "已切换到跟随系统主题。");
        } else {
            appendLog(textOtaLog, mode == ThemeMode::Dark ? "已切换到深色模式。" : "已切换到浅色模式。");
        }
    });

    connect(btnRefreshPorts, &QPushButton::clicked, this, &MainWindow::refreshSerialPorts);
    connect(btnOpenPort, &QPushButton::clicked, this, &MainWindow::toggleSerialPort);
    connect(btnSendText, &QPushButton::clicked, this, &MainWindow::sendSerialText);
    connect(btnSwapPanels, &QPushButton::clicked, this, &MainWindow::toggleWorkspaceOrder);
    connect(btnBrowseFile, &QPushButton::clicked, this, [this]() {
        const QString filePath = QFileDialog::getOpenFileName(
            this,
            "选择固件文件",
            QString(),
            "固件文件 (*.bin *.hex);;二进制文件 (*.bin);;Intel HEX 文件 (*.hex)"
        );
        if (!filePath.isEmpty()) {
            loadFirmwareFile(filePath);
        }
    });
    connect(btnStartUpgrade, &QPushButton::clicked, this, &MainWindow::startUpgrade);
    connect(btnStopUpgrade, &QPushButton::clicked, this, &MainWindow::stopUpgrade);
    connect(editTx, &QLineEdit::returnPressed, btnSendText, &QPushButton::click);

    connect(comboPort, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        labelPortDescription->setText(comboPort->itemData(index, Qt::ToolTipRole).toString());
        appendLog(textOtaLog, QString("已选择端口：%1").arg(comboPort->itemText(index)));
    });

    connect(comboBaud, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (!text.isEmpty()) {
            appendLog(textOtaLog, QString("已设置波特率：%1").arg(text));
        }
    });

    connect(btnClearSerial, &QPushButton::clicked, this, [this]() {
        textSerialMonitor->clear();
        appendLog(textSerialMonitor, "[系统] 调试窗口已清空。");
    });

    connect(serialPort, &QSerialPort::readyRead, this, &MainWindow::handleSerialReadyRead);
    connect(serialPort, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error != QSerialPort::NoError) {
            handleSerialError();
        }
    });
}

void MainWindow::refreshSerialPorts()
{
    const QString previousPort = comboPort->currentData().toString();
    const QSignalBlocker blocker(comboPort);
    comboPort->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        comboPort->addItem(info.portName(), info.portName());
        comboPort->setItemData(comboPort->count() - 1, formatPortDescription(info), Qt::ToolTipRole);
    }

    if (comboPort->count() == 0) {
        comboPort->addItem("未检测到串口", QString());
        const QString noPortText =
            "端口名：无\n"
            "描述：未检测到串口\n"
            "厂商：未知\n"
            "序列号：未知\n"
            "VID/PID：--";
        comboPort->setItemData(0, noPortText, Qt::ToolTipRole);
        comboPort->setEnabled(false);
        btnOpenPort->setEnabled(false);
        labelPortDescription->setText(noPortText);
        appendLog(textOtaLog, "未检测到可用串口。");
        updateSerialUiState(false);
        return;
    }

    comboPort->setEnabled(true);
    btnOpenPort->setEnabled(true);

    int index = previousPort.isEmpty() ? -1 : comboPort->findData(previousPort);
    if (index < 0) {
        index = 0;
    }
    comboPort->setCurrentIndex(index);
    labelPortDescription->setText(comboPort->itemData(index, Qt::ToolTipRole).toString());
    appendLog(textOtaLog, QString("串口扫描完成，共检测到 %1 个端口。").arg(comboPort->count()));
}

void MainWindow::toggleSerialPort()
{
    if (currentUpgradeState == UpgradeState::Running || currentUpgradeState == UpgradeState::Cancelling) {
        appendLog(textOtaLog, "升级过程中不允许切换串口连接。");
        return;
    }

    if (serialPort->isOpen()) {
        const QString portName = serialPort->portName();
        serialPort->close();
        serialRxBuffer.clear();
        updateSerialUiState(false);
        appendLog(textOtaLog, QString("串口已关闭：%1").arg(portName));
        appendLog(textSerialMonitor, QString("[系统] 已关闭串口 %1").arg(portName));
        return;
    }

    const QString portName = comboPort->currentData().toString();
    if (portName.isEmpty()) {
        appendLog(textOtaLog, "请先选择一个有效串口。");
        return;
    }

    bool ok = false;
    const int baudRate = comboBaud->currentText().toInt(&ok);
    if (!ok || baudRate <= 0) {
        appendLog(textOtaLog, "波特率无效，请选择或输入正确数值。");
        return;
    }

    serialPort->setPortName(portName);
    serialPort->setBaudRate(baudRate);
    serialPort->setDataBits(comboDataBits->currentText() == "7" ? QSerialPort::Data7 : QSerialPort::Data8);
    serialPort->setParity(comboParity->currentIndex() == 1 ? QSerialPort::EvenParity
                              : comboParity->currentIndex() == 2 ? QSerialPort::OddParity
                                                                 : QSerialPort::NoParity);
    serialPort->setStopBits(comboStopBits->currentText() == "2" ? QSerialPort::TwoStop : QSerialPort::OneStop);
    serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!serialPort->open(QIODevice::ReadWrite)) {
        appendLog(textOtaLog, QString("串口打开失败：%1").arg(serialPort->errorString()));
        return;
    }

    serialPort->clear(QSerialPort::AllDirections);
    serialRxBuffer.clear();
    updateSerialUiState(true);
    appendLog(textOtaLog, QString("已应用端口 %1，波特率 %2，并成功打开串口。").arg(portName).arg(baudRate));
    appendLog(textSerialMonitor, QString("[系统] 已连接 %1 @ %2").arg(portName).arg(baudRate));
}

void MainWindow::sendSerialText()
{
    const QString text = editTx->text();
    if (text.trimmed().isEmpty()) {
        appendLog(textSerialMonitor, "[发送] <空内容>");
        return;
    }

    if (!serialPort->isOpen()) {
        appendLog(textOtaLog, "串口未打开，无法发送数据。");
        return;
    }

    QByteArray payload;
    if (chkHexDisplay->isChecked()) {
        QString normalized = text;
        normalized.remove(QRegularExpression("\\s+"));
        if (normalized.size() % 2 != 0) {
            appendLog(textOtaLog, "HEX 发送内容长度必须为偶数。");
            return;
        }
        payload = QByteArray::fromHex(normalized.toLatin1());
    } else {
        payload = text.toUtf8();
    }

    if (serialPort->write(payload) < 0) {
        appendLog(textOtaLog, QString("发送失败：%1").arg(serialPort->errorString()));
        return;
    }

    serialPort->flush();
    appendLog(textSerialMonitor, QString("[发送] %1").arg(chkHexDisplay->isChecked()
                                                              ? QString::fromLatin1(payload.toHex(' ').toUpper())
                                                              : text));
    editTx->clear();
}

void MainWindow::handleSerialReadyRead()
{
    const QByteArray data = serialPort->readAll();
    if (!data.isEmpty()) {
        serialRxBuffer.append(data);
        appendLog(textSerialMonitor, QString("[接收] %1").arg(decodeReceivedData(data)));
    }
}

void MainWindow::handleSerialError()
{
    appendLog(textOtaLog, QString("串口错误：%1").arg(serialPort->errorString()));
    if (!serialPort->isOpen()) {
        updateSerialUiState(false);
    }
}

void MainWindow::updateSerialUiState(bool isOpen)
{
    const bool hasValidPort = comboPort->count() > 0 && !comboPort->currentData().toString().isEmpty();
    const bool otaBusy = currentUpgradeState == UpgradeState::Running || currentUpgradeState == UpgradeState::Cancelling;

    comboPort->setEnabled(!isOpen && hasValidPort && !otaBusy);
    comboBaud->setEnabled(!isOpen && !otaBusy);
    comboDataBits->setEnabled(!isOpen && !otaBusy);
    comboParity->setEnabled(!isOpen && !otaBusy);
    comboStopBits->setEnabled(!isOpen && !otaBusy);
    btnRefreshPorts->setEnabled(!isOpen && !otaBusy);
    btnOpenPort->setEnabled((isOpen || hasValidPort) && !otaBusy);
    btnOpenPort->setText(isOpen ? "关闭串口" : "应用并打开");
    btnSendText->setEnabled(isOpen && !otaBusy);

    if (isOpen) {
        labelTransportState->setText(QString("当前已应用  |  %1 @ %2").arg(serialPort->portName()).arg(comboBaud->currentText()));
        labelTransportState->setObjectName("badgeAccent");
    } else {
        labelTransportState->setText(hasValidPort ? "当前未应用串口配置" : "当前无可用串口");
        labelTransportState->setObjectName("statusChipIdle");
    }

    labelTransportState->style()->unpolish(labelTransportState);
    labelTransportState->style()->polish(labelTransportState);
}

void MainWindow::updateUpgradeUiState(UpgradeState state)
{
    currentUpgradeState = state;
    const bool running = state == UpgradeState::Running;
    const bool cancelling = state == UpgradeState::Cancelling;

    btnStartUpgrade->setEnabled(!running && !cancelling);
    btnBrowseFile->setEnabled(!running && !cancelling);
    btnStopUpgrade->setEnabled(running || cancelling);
    btnStopUpgrade->setText(cancelling ? "停止中..." : "停止");

    labelProtocolState->setText(
        state == UpgradeState::Idle ? "就绪" :
        state == UpgradeState::Running ? "升级中" :
        state == UpgradeState::Cancelling ? "取消中" :
        state == UpgradeState::Completed ? "完成" : "失败"
    );

    if (state == UpgradeState::Idle) {
        progressUpgrade->setValue(0);
        progressUpgrade->setFormat("就绪  %p%");
    } else if (state == UpgradeState::Completed) {
        progressUpgrade->setValue(100);
        progressUpgrade->setFormat("升级完成  %p%");
    } else if (state == UpgradeState::Failed) {
        progressUpgrade->setFormat("升级失败  %p%");
    } else if (state == UpgradeState::Cancelling) {
        progressUpgrade->setFormat("正在停止  %p%");
    }

    updateSerialUiState(serialPort->isOpen());
}

void MainWindow::toggleWorkspaceOrder()
{
    if (!workspaceSplitter || !cardUpgradeLog || !cardSerialDebug) {
        return;
    }

    const QList<int> sizes = workspaceSplitter->sizes();
    cardUpgradeLog->setParent(nullptr);
    cardSerialDebug->setParent(nullptr);

    if (isLogPanelFirst) {
        workspaceSplitter->addWidget(cardSerialDebug);
        workspaceSplitter->addWidget(cardUpgradeLog);
    } else {
        workspaceSplitter->addWidget(cardUpgradeLog);
        workspaceSplitter->addWidget(cardSerialDebug);
    }

    if (sizes.size() == 2) {
        workspaceSplitter->setSizes(sizes);
    }

    isLogPanelFirst = !isLogPanelFirst;
    appendLog(textOtaLog, isLogPanelFirst ? "工作区已切换为“升级日志在上”。"
                                          : "工作区已切换为“串口调试窗口在上”。");
}

void MainWindow::loadFirmwareFile(const QString &filePath)
{
    try {
        const QByteArray image = loadImageFile(filePath);
        if (image.isEmpty()) {
            throw std::runtime_error("固件文件为空。");
        }

        firmwareImage = image;
        firmwareFilePath = filePath;

        const QFileInfo info(filePath);
        editFilePath->setText(filePath);
        labelFileMeta->setText(
            QString("已加载文件：%1\n最后修改时间：%2")
                .arg(info.fileName(), info.lastModified().toString("yyyy-MM-dd HH:mm:ss"))
        );
        labelFirmwareType->setText(info.suffix().isEmpty() ? "未知" : "." + info.suffix().toLower());
        labelImageSize->setText(QString::number(image.size() / 1024.0, 'f', 1) + " KB");
        labelChunkSize->setText(QString("%1 字节").arg(kDefaultChunkSize));
        labelProtocolState->setText("文件已就绪");
        appendLog(textOtaLog, QString("固件文件已载入：%1，大小 %2 字节。").arg(info.fileName()).arg(image.size()));
    } catch (const std::exception &ex) {
        firmwareImage.clear();
        firmwareFilePath.clear();
        editFilePath->clear();
        labelFileMeta->setText("固件文件加载失败。");
        labelFirmwareType->setText("--");
        labelImageSize->setText("--");
        labelProtocolState->setText("文件异常");
        appendLog(textOtaLog, QString("固件加载失败：%1").arg(QString::fromUtf8(ex.what())));
    }
}

QByteArray MainWindow::loadImageFile(const QString &filePath) const
{
    const QFileInfo info(filePath);
    if (!info.exists()) {
        throw std::runtime_error("固件文件不存在。");
    }

    if (info.suffix().compare("hex", Qt::CaseInsensitive) == 0) {
        return loadIntelHexFile(filePath);
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("无法打开固件文件。");
    }
    return file.readAll();
}

QByteArray MainWindow::loadIntelHexFile(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("无法打开 HEX 文件。");
    }

    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    QList<QPair<quint32, QByteArray>> records;
    quint32 upperAddress = 0;
    bool eofSeen = false;

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (!line.startsWith(':')) {
            throw std::runtime_error(QString("HEX 第 %1 行缺少 ':' 前缀。").arg(i + 1).toUtf8().constData());
        }

        const QString recordText = line.mid(1);
        if (recordText.size() < 10 || recordText.size() % 2 != 0) {
            throw std::runtime_error(QString("HEX 第 %1 行长度非法。").arg(i + 1).toUtf8().constData());
        }

        const QByteArray raw = QByteArray::fromHex(recordText.toLatin1());
        if (raw.isEmpty()) {
            throw std::runtime_error(QString("HEX 第 %1 行不是有效十六进制。").arg(i + 1).toUtf8().constData());
        }

        const quint8 byteCount = static_cast<quint8>(raw.at(0));
        if (raw.size() != byteCount + 5) {
            throw std::runtime_error(QString("HEX 第 %1 行字节计数不匹配。").arg(i + 1).toUtf8().constData());
        }

        quint8 sum = 0;
        for (const char byte : raw) {
            sum = static_cast<quint8>(sum + static_cast<quint8>(byte));
        }
        if (sum != 0) {
            throw std::runtime_error(QString("HEX 第 %1 行校验和错误。").arg(i + 1).toUtf8().constData());
        }

        const quint16 address =
            (static_cast<quint8>(raw.at(1)) << 8) |
            static_cast<quint8>(raw.at(2));
        const quint8 recordType = static_cast<quint8>(raw.at(3));
        const QByteArray data = raw.mid(4, byteCount);

        if (recordType == 0x00) {
            const quint32 absoluteAddress = upperAddress + address;
            if (absoluteAddress < kDefaultAppAddress) {
                throw std::runtime_error(QString("HEX 数据地址 0x%1 低于应用起始地址。")
                                             .arg(absoluteAddress, 8, 16, QLatin1Char('0'))
                                             .toUpper()
                                             .toUtf8()
                                             .constData());
            }
            records.append(qMakePair(absoluteAddress, data));
        } else if (recordType == 0x01) {
            eofSeen = true;
            break;
        } else if (recordType == 0x04) {
            if (data.size() != 2) {
                throw std::runtime_error(QString("HEX 第 %1 行扩展线性地址记录非法。").arg(i + 1).toUtf8().constData());
            }
            upperAddress =
                (static_cast<quint8>(data.at(0)) << 24) |
                (static_cast<quint8>(data.at(1)) << 16);
        } else if (recordType == 0x05) {
            continue;
        } else {
            throw std::runtime_error(QString("HEX 第 %1 行记录类型 0x%2 不支持。")
                                         .arg(i + 1)
                                         .arg(recordType, 2, 16, QLatin1Char('0'))
                                         .toUpper()
                                         .toUtf8()
                                         .constData());
        }
    }

    if (!eofSeen) {
        throw std::runtime_error("HEX 文件缺少 EOF 记录。");
    }
    if (records.isEmpty()) {
        throw std::runtime_error("HEX 文件中没有数据记录。");
    }

    std::sort(records.begin(), records.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });

    if (records.first().first != kDefaultAppAddress) {
        throw std::runtime_error(QString("HEX 起始地址必须为 0x%1。")
                                     .arg(kDefaultAppAddress, 8, 16, QLatin1Char('0'))
                                     .toUpper()
                                     .toUtf8()
                                     .constData());
    }

    QByteArray image;
    quint32 expectedAddress = kDefaultAppAddress;
    for (const auto &record : std::as_const(records)) {
        if (record.first != expectedAddress) {
            throw std::runtime_error(QString("HEX 数据不连续，期望 0x%1，实际 0x%2。")
                                         .arg(expectedAddress, 8, 16, QLatin1Char('0'))
                                         .arg(record.first, 8, 16, QLatin1Char('0'))
                                         .toUpper()
                                         .toUtf8()
                                         .constData());
        }
        image.append(record.second);
        expectedAddress += static_cast<quint32>(record.second.size());
    }

    return image;
}

void MainWindow::startUpgrade()
{
    if (currentUpgradeState == UpgradeState::Running || currentUpgradeState == UpgradeState::Cancelling) {
        return;
    }
    if (!serialPort->isOpen()) {
        appendLog(textOtaLog, "请先应用并打开串口。");
        return;
    }
    if (firmwareImage.isEmpty()) {
        appendLog(textOtaLog, "请先选择有效的固件文件。");
        return;
    }

    cancelUpgradeRequested = false;
    serialRxBuffer.clear();
    updateUpgradeUiState(UpgradeState::Running);
    appendLog(textOtaLog, "开始执行 OTA 升级流程。");

    const bool success = performUpgrade();
    if (cancelUpgradeRequested) {
        appendLog(textOtaLog, "升级已被用户停止。");
        updateUpgradeUiState(UpgradeState::Failed);
        progressUpgrade->setValue(0);
        progressUpgrade->setFormat("已停止  %p%");
        return;
    }

    updateUpgradeUiState(success ? UpgradeState::Completed : UpgradeState::Failed);
}

void MainWindow::stopUpgrade()
{
    if (currentUpgradeState != UpgradeState::Running && currentUpgradeState != UpgradeState::Cancelling) {
        progressUpgrade->setValue(0);
        progressUpgrade->setFormat("就绪  %p%");
        appendLog(textOtaLog, "当前没有正在进行的升级任务。");
        return;
    }

    cancelUpgradeRequested = true;
    updateUpgradeUiState(UpgradeState::Cancelling);
    appendLog(textOtaLog, "已请求停止当前升级任务。");
}

bool MainWindow::performUpgrade()
{
    if (!tryHandshake(kHandshakeRetries)) {
        return false;
    }

    quint8 seq = 1;
    AckInfo ackInfo;

    QByteArray startPayload(4, 0);
    const quint32 imageSize = static_cast<quint32>(firmwareImage.size());
    startPayload[0] = static_cast<char>(imageSize & 0xFF);
    startPayload[1] = static_cast<char>((imageSize >> 8) & 0xFF);
    startPayload[2] = static_cast<char>((imageSize >> 16) & 0xFF);
    startPayload[3] = static_cast<char>((imageSize >> 24) & 0xFF);

    if (!sendCommand(ProtocolCommand::Start, seq, startPayload, &ackInfo)) {
        return false;
    }
    appendLog(textOtaLog, QString("开始帧已确认，设备状态：%1。").arg(describeBootState(ackInfo.deviceState)));
    seq = nextSequence(seq);
    progressUpgrade->setValue(5);
    progressUpgrade->setFormat("已开始传输  %p%");

    const int totalChunks = (firmwareImage.size() + kDefaultChunkSize - 1) / kDefaultChunkSize;
    for (int index = 0; index < totalChunks; ++index) {
        if (cancelUpgradeRequested) {
            return false;
        }

        const int offset = index * kDefaultChunkSize;
        const QByteArray chunk = firmwareImage.mid(offset, kDefaultChunkSize);
        if (!sendCommand(ProtocolCommand::Data, seq, chunk, &ackInfo)) {
            return false;
        }

        const quint8 usedSeq = seq;
        seq = nextSequence(seq);
        const int progress = 5 + ((index + 1) * 85 / std::max(totalChunks, 1));
        progressUpgrade->setValue(progress);
        progressUpgrade->setFormat(QString("发送分包 %1/%2  %p%").arg(index + 1).arg(totalChunks));
        appendLog(textOtaLog, QString("数据分包 %1/%2 已确认，序号=%3，大小=%4 字节。")
                                  .arg(index + 1)
                                  .arg(totalChunks)
                                  .arg(usedSeq)
                                  .arg(chunk.size()));
        QCoreApplication::processEvents();
    }

    const quint16 crc = crc16Ibm(firmwareImage);
    QByteArray endPayload(2, 0);
    endPayload[0] = static_cast<char>(crc & 0xFF);
    endPayload[1] = static_cast<char>((crc >> 8) & 0xFF);
    if (!sendCommand(ProtocolCommand::End, seq, endPayload, &ackInfo)) {
        return false;
    }
    appendLog(textOtaLog, QString("结束帧已确认，镜像 CRC16=0x%1，设备状态：%2。")
                              .arg(crc, 4, 16, QLatin1Char('0'))
                              .toUpper()
                              .arg(describeBootState(ackInfo.deviceState)));
    seq = nextSequence(seq);
    progressUpgrade->setValue(95);
    progressUpgrade->setFormat("校验完成  %p%");

    if (!sendCommand(ProtocolCommand::Jump, seq, QByteArray(), &ackInfo)) {
        appendLog(textOtaLog, "跳转应用指令发送失败，请手动复位验证。");
        return false;
    }

    appendLog(textOtaLog, QString("跳转指令已确认，设备状态：%1。").arg(describeBootState(ackInfo.deviceState)));
    progressUpgrade->setValue(100);
    progressUpgrade->setFormat("升级完成  %p%");
    appendLog(textOtaLog, "OTA 升级流程执行完成。");
    return true;
}

bool MainWindow::tryHandshake(int retries)
{
    for (int attempt = 1; attempt <= retries; ++attempt) {
        if (cancelUpgradeRequested) {
            return false;
        }

        AckInfo ackInfo;
        if (sendCommand(ProtocolCommand::Handshake, 0, QByteArray(), &ackInfo)) {
            appendLog(textOtaLog, QString("握手成功，第 %1 次尝试完成，设备状态：%2。")
                                  .arg(attempt)
                                  .arg(describeBootState(ackInfo.deviceState)));
            return true;
        }

        if (attempt < retries) {
            appendLog(textOtaLog, QString("第 %1 次握手失败，准备重试。").arg(attempt));
            QThread::msleep(200);
            QCoreApplication::processEvents();
        }
    }

    appendLog(textOtaLog, "握手失败，已达到最大重试次数。");
    return false;
}

bool MainWindow::sendCommand(ProtocolCommand command, quint8 sequence, const QByteArray &payload, AckInfo *ackInfo)
{
    if (!serialPort->isOpen()) {
        appendLog(textOtaLog, "串口未打开，无法发送 OTA 命令。");
        return false;
    }

    const QByteArray frame = buildFrame(command, sequence, payload);
    if (serialPort->write(frame) != frame.size()) {
        appendLog(textOtaLog, QString("命令发送失败：%1").arg(serialPort->errorString()));
        return false;
    }
    if (!serialPort->flush()) {
        appendLog(textOtaLog, "串口刷新发送缓冲区失败。");
        return false;
    }

    return waitForAck(command, sequence, ackInfo);
}

QByteArray MainWindow::buildFrame(ProtocolCommand command, quint8 sequence, const QByteArray &payload) const
{
    QByteArray body;
    body.reserve(4 + payload.size());
    body.append(static_cast<char>(command));
    body.append(static_cast<char>(sequence));
    body.append(static_cast<char>(payload.size() & 0xFF));
    body.append(static_cast<char>((payload.size() >> 8) & 0xFF));
    body.append(payload);

    const quint16 crc = crc16Ibm(body);

    QByteArray frame;
    frame.reserve(2 + body.size() + 2);
    frame.append(static_cast<char>(0x55));
    frame.append(static_cast<char>(0xAA));
    frame.append(body);
    frame.append(static_cast<char>(crc & 0xFF));
    frame.append(static_cast<char>((crc >> 8) & 0xFF));
    return frame;
}

bool MainWindow::waitForAck(ProtocolCommand command, quint8 sequence, AckInfo *ackInfo)
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kDefaultTimeoutMs;

    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (cancelUpgradeRequested) {
            return false;
        }

        ParsedFrame frame;
        if (!readFrame(&frame, kReadPollIntervalMs, 3)) {
            QCoreApplication::processEvents();
            continue;
        }

        if (frame.sequence != sequence) {
            appendLog(textOtaLog, QString("收到序号不匹配的响应：期望 %1，实际 %2。")
                                  .arg(sequence)
                                  .arg(frame.sequence));
            continue;
        }

        if (frame.command == static_cast<quint8>(ProtocolCommand::Nack)) {
            bool ok = false;
            const AckInfo nackInfo = parseAckPayload(frame.payload, command, &ok);
            if (!ok) {
                appendLog(textOtaLog, "收到 NACK，但载荷格式非法。");
            } else {
                appendLog(textOtaLog, QString("设备返回 NACK：错误=%1，状态=%2。")
                                      .arg(describeErrorCode(nackInfo.errorCode))
                                      .arg(describeBootState(nackInfo.deviceState)));
            }
            return false;
        }

        if (frame.command != static_cast<quint8>(ProtocolCommand::Ack)) {
            appendLog(textOtaLog, QString("收到未知响应命令：0x%1。")
                                  .arg(frame.command, 2, 16, QLatin1Char('0'))
                                  .toUpper());
            continue;
        }

        bool ok = false;
        const AckInfo parsedAck = parseAckPayload(frame.payload, command, &ok);
        if (!ok) {
            appendLog(textOtaLog, "ACK 载荷格式非法。");
            return false;
        }
        if (parsedAck.errorCode != 0x00) {
            appendLog(textOtaLog, QString("设备返回错误 ACK：错误=%1，状态=%2。")
                                  .arg(describeErrorCode(parsedAck.errorCode))
                                  .arg(describeBootState(parsedAck.deviceState)));
            return false;
        }

        if (ackInfo) {
            *ackInfo = parsedAck;
        }
        return true;
    }

    appendLog(textOtaLog, QString("等待命令 0x%1 的 ACK 超时。")
                          .arg(static_cast<quint8>(command), 2, 16, QLatin1Char('0'))
                          .toUpper());
    return false;
}

bool MainWindow::readFrame(ParsedFrame *frame, int timeoutMs, int maxPayloadLength)
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;

    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        const int headIndex = serialRxBuffer.indexOf(QByteArray::fromHex("55AA"));
        if (headIndex < 0) {
            if (serialRxBuffer.size() > 1) {
                serialRxBuffer = serialRxBuffer.right(1);
            }
        } else if (headIndex > 0) {
            serialRxBuffer.remove(0, headIndex);
        }

        if (serialRxBuffer.size() >= 6 && serialRxBuffer.startsWith(QByteArray::fromHex("55AA"))) {
            const quint16 payloadLength =
                static_cast<quint8>(serialRxBuffer.at(4)) |
                (static_cast<quint8>(serialRxBuffer.at(5)) << 8);
            if (maxPayloadLength >= 0 && payloadLength > maxPayloadLength) {
                serialRxBuffer.remove(0, 1);
                continue;
            }

            const int frameSize = 8 + payloadLength;
            if (serialRxBuffer.size() >= frameSize) {
                const QByteArray body = serialRxBuffer.mid(2, 4 + payloadLength);
                const quint16 recvCrc =
                    static_cast<quint8>(serialRxBuffer.at(6 + payloadLength)) |
                    (static_cast<quint8>(serialRxBuffer.at(7 + payloadLength)) << 8);
                const quint16 calcCrc = crc16Ibm(body);
                if (recvCrc != calcCrc) {
                    serialRxBuffer.remove(0, 1);
                    continue;
                }

                frame->command = static_cast<quint8>(serialRxBuffer.at(2));
                frame->sequence = static_cast<quint8>(serialRxBuffer.at(3));
                frame->payload = serialRxBuffer.mid(6, payloadLength);
                serialRxBuffer.remove(0, frameSize);
                return true;
            }
        }

        if (serialPort->waitForReadyRead(kReadPollIntervalMs)) {
            serialRxBuffer.append(serialPort->readAll());
        } else {
            QCoreApplication::processEvents();
        }
    }

    return false;
}

MainWindow::AckInfo MainWindow::parseAckPayload(const QByteArray &payload, ProtocolCommand expectedCommand, bool *ok) const
{
    AckInfo info;
    bool localOk = payload.size() >= 3;
    if (localOk) {
        info.originalCommand = static_cast<quint8>(payload.at(0));
        info.errorCode = static_cast<quint8>(payload.at(1));
        info.deviceState = static_cast<quint8>(payload.at(2));
        localOk = info.originalCommand == static_cast<quint8>(expectedCommand);
    }

    if (ok) {
        *ok = localOk;
    }
    return info;
}

quint16 MainWindow::crc16Ibm(const QByteArray &data) const
{
    quint16 crc = 0xFFFF;
    for (const char byte : data) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x0001) {
                crc = static_cast<quint16>((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

quint8 MainWindow::nextSequence(quint8 sequence) const
{
    return static_cast<quint8>((sequence + 1) & 0xFF);
}

QString MainWindow::formatPortDescription(const QSerialPortInfo &info) const
{
    const QString description = info.description().isEmpty() ? "未知" : info.description();
    const QString manufacturer = info.manufacturer().isEmpty() ? "未知" : info.manufacturer();
    const QString serialNumber = info.serialNumber().isEmpty() ? "未知" : info.serialNumber();

    QString vidPid = "--";
    if (info.hasVendorIdentifier() || info.hasProductIdentifier()) {
        const QString vid = info.hasVendorIdentifier()
            ? QString("0x%1").arg(info.vendorIdentifier(), 4, 16, QLatin1Char('0')).toUpper()
            : "----";
        const QString pid = info.hasProductIdentifier()
            ? QString("0x%1").arg(info.productIdentifier(), 4, 16, QLatin1Char('0')).toUpper()
            : "----";
        vidPid = vid + " / " + pid;
    }

    return QString(
               "端口名：%1\n"
               "描述：%2\n"
               "厂商：%3\n"
               "序列号：%4\n"
               "VID/PID：%5"
           )
        .arg(info.portName(), description, manufacturer, serialNumber, vidPid);
}

QString MainWindow::decodeReceivedData(const QByteArray &data) const
{
    if (chkHexDisplay->isChecked()) {
        return QString::fromLatin1(data.toHex(' ').toUpper());
    }

    QString text = QString::fromUtf8(data);
    text.replace("\r", "\\r");
    text.replace("\n", "\\n");
    return text;
}

QString MainWindow::describeBootState(quint8 state) const
{
    switch (state) {
    case 0x00: return "IDLE";
    case 0x01: return "READY";
    case 0x02: return "RECEIVING";
    case 0x03: return "FINISHED";
    default:
        return QString("UNKNOWN(0x%1)").arg(state, 2, 16, QLatin1Char('0')).toUpper();
    }
}

QString MainWindow::describeErrorCode(quint8 code) const
{
    switch (code) {
    case 0x00: return "NONE";
    case 0x01: return "BAD_FRAME";
    case 0x02: return "BAD_CRC";
    case 0x03: return "BAD_LENGTH";
    case 0x04: return "BAD_STATE";
    case 0x05: return "BAD_SEQUENCE";
    case 0x06: return "RANGE";
    case 0x07: return "FLASH";
    case 0x08: return "VERIFY";
    case 0x09: return "APP_INVALID";
    case 0x0A: return "NOT_READY";
    default:
        return QString("UNKNOWN(0x%1)").arg(code, 2, 16, QLatin1Char('0')).toUpper();
    }
}

void MainWindow::appendLog(QPlainTextEdit *target, const QString &message)
{
    QString line = message;
    if (!chkTimestamp || chkTimestamp->isChecked()) {
        line = QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), message);
    }
    target->appendPlainText(line);
    target->moveCursor(QTextCursor::End);
    if (auto *bar = target->verticalScrollBar()) {
        bar->setValue(bar->maximum());
    }
}

QFrame *MainWindow::createCard(const QString &title, const QString &subtitle, QVBoxLayout **bodyLayout)
{
    auto *frame = new QFrame(this);
    frame->setObjectName("cardFrame");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto *titleLabel = new QLabel(title, frame);
    titleLabel->setObjectName("cardTitle");
    auto *subtitleLabel = new QLabel(subtitle, frame);
    subtitleLabel->setObjectName("cardSubtitle");
    subtitleLabel->setWordWrap(true);
    layout->addWidget(titleLabel);
    layout->addWidget(subtitleLabel);
    *bodyLayout = layout;
    return frame;
}

QFrame *MainWindow::createMetricTile(
    const QString &title,
    const QString &value,
    const QString &note,
    const QString &accent
)
{
    auto *frame = new QFrame(this);
    frame->setObjectName("metricTile");
    frame->setProperty("accentColor", accent);

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(6);
    auto *titleLabel = new QLabel(title, frame);
    titleLabel->setObjectName("metricTitle");
    auto *valueLabel = new QLabel(value, frame);
    valueLabel->setObjectName("metricValue");
    auto *noteLabel = new QLabel(note, frame);
    noteLabel->setObjectName("metricNote");
    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    layout->addWidget(noteLabel);
    return frame;
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ThemeChange) {
        if (currentTheme == ThemeMode::System && !themeUpdateInProgress) {
            applyTheme(ThemeMode::System);
        }
    }
    return QMainWindow::event(event);
}
