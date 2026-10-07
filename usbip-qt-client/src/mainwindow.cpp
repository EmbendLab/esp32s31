#include "mainwindow.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include <QtConcurrent/QtConcurrentRun>

namespace {

constexpr quint16 kDefaultPort = 3240;

QString deviceClassString(quint8 c)
{
    switch (c) {
    case 0x00: return QStringLiteral("Use interface");
    case 0x02: return QStringLiteral("Communications");
    case 0x03: return QStringLiteral("HID");
    case 0x07: return QStringLiteral("Printer");
    case 0x08: return QStringLiteral("Mass Storage");
    case 0x09: return QStringLiteral("Hub");
    case 0x0A: return QStringLiteral("CDC Data");
    case 0x0B: return QStringLiteral("Smart Card");
    case 0xDC: return QStringLiteral("Diagnostic");
    case 0xE0: return QStringLiteral("Wireless");
    case 0xEF: return QStringLiteral("Misc");
    case 0xFF: return QStringLiteral("Vendor");
    default:   return QStringLiteral("0x%1").arg(c, 2, 16, QLatin1Char('0'));
    }
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();
    applyStyle();
    loadFavorites();

    connect(&m_watcher, &QFutureWatcher<QVector<RemoteDevice>>::finished,
            this, &MainWindow::onRefreshFinished);

    if (!m_vhci.isDriverAvailable()) {
        log(QStringLiteral("警告：未检测到 USB/IP 内核驱动（usbip2_ude）。请先安装驱动并运行 devnode.exe 创建根设备节点。"));
    } else {
        log(QStringLiteral("已检测到 USB/IP 内核驱动。"));
        refreshImported();
    }

    statusBar()->showMessage(QStringLiteral("就绪"));
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // ---- 顶部：主机 + 端口 + 操作按钮 ----
    auto *top = new QHBoxLayout();
    top->setSpacing(8);

    m_hostCombo = new QComboBox(central);
    m_hostCombo->setEditable(true);
    m_hostCombo->setInsertPolicy(QComboBox::NoInsert);
    m_hostCombo->setMinimumWidth(260);
    m_hostCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_portSpin = new QSpinBox(central);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(kDefaultPort);
    m_portSpin->setSuffix(QStringLiteral(""));
    m_portSpin->setMaximumWidth(90);

    m_addFavBtn = new QPushButton(QStringLiteral("收藏"), central);
    m_refreshBtn = new QPushButton(QStringLiteral("刷新设备列表"), central);
    m_refreshBtn->setObjectName(QStringLiteral("primaryButton"));

    auto *hostLabel = new QLabel(QStringLiteral("服务器地址"), central);
    auto *portLabel = new QLabel(QStringLiteral("端口"), central);

    top->addWidget(hostLabel);
    top->addWidget(m_hostCombo, 1);
    top->addWidget(portLabel);
    top->addWidget(m_portSpin);
    top->addWidget(m_addFavBtn);
    top->addWidget(m_refreshBtn);

    // ---- 中部：远端设备表 + 已导入设备表 ----
    m_remoteTable = new QTableWidget(0, 6, central);
    m_remoteTable->setHorizontalHeaderLabels({
        QStringLiteral("Bus ID"), QStringLiteral("VID:PID"), QStringLiteral("类别"),
        QStringLiteral("速度"), QStringLiteral("设备路径"), QStringLiteral("操作")});
    m_remoteTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_remoteTable->verticalHeader()->setVisible(false);
    m_remoteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remoteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteTable->setAlternatingRowColors(true);

    m_importedTable = new QTableWidget(0, 6, central);
    m_importedTable->setHorizontalHeaderLabels({
        QStringLiteral("端口"), QStringLiteral("VID:PID"), QStringLiteral("Bus ID"),
        QStringLiteral("服务器"), QStringLiteral("速度"), QStringLiteral("操作")});
    m_importedTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_importedTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_importedTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_importedTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_importedTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_importedTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_importedTable->verticalHeader()->setVisible(false);
    m_importedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_importedTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_importedTable->setAlternatingRowColors(true);

    auto *remoteGroup = new QGroupBox(QStringLiteral("服务器可导出设备"), central);
    auto *remoteLayout = new QVBoxLayout(remoteGroup);
    remoteLayout->addWidget(m_remoteTable);

    auto *importedGroup = new QGroupBox(QStringLiteral("已导入到本机的设备"), central);
    auto *importedLayout = new QVBoxLayout(importedGroup);
    importedLayout->addWidget(m_importedTable);

    auto *splitter = new QSplitter(Qt::Vertical, central);
    splitter->addWidget(remoteGroup);
    splitter->addWidget(importedGroup);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    // ---- 日志 ----
    m_logView = new QPlainTextEdit(central);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1000);
    m_logView->setFixedHeight(140);

    root->addLayout(top);
    root->addWidget(splitter, 1);
    root->addWidget(m_logView);

    setCentralWidget(central);
    resize(960, 640);
    setWindowTitle(QStringLiteral("USB/IP 客户端"));

    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefresh);
    connect(m_addFavBtn, &QPushButton::clicked, this, &MainWindow::onAddFavorite);
    connect(m_hostCombo->lineEdit(), &QLineEdit::returnPressed, this, &MainWindow::onRefresh);
}

void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow { background: #1e1f24; }
        QWidget { color: #e6e6e6; font-family: "Segoe UI", "Microsoft YaHei"; font-size: 13px; }
        QGroupBox {
            border: 1px solid #3a3b42; border-radius: 8px;
            margin-top: 10px; padding-top: 8px;
            background: #26272e;
            font-weight: 600;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }
        QComboBox, QSpinBox, QLineEdit {
            background: #2b2c33; border: 1px solid #3a3b42; border-radius: 6px;
            padding: 6px 8px; selection-background-color: #3b82f6;
        }
        QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border-color: #3b82f6; }
        QPushButton {
            background: #34353d; border: 1px solid #3a3b42; border-radius: 6px;
            padding: 6px 14px;
        }
        QPushButton:hover { background: #3d3e46; }
        QPushButton:pressed { background: #2b2c33; }
        QPushButton#primaryButton {
            background: #2563eb; border-color: #2563eb; color: white; font-weight: 600;
        }
        QPushButton#primaryButton:hover { background: #3b82f6; }
        QTableWidget {
            background: #26272e; gridline-color: #3a3b42;
            border: 1px solid #3a3b42; border-radius: 6px;
            alternate-background-color: #2b2c33;
        }
        QTableWidget::item { padding: 4px 6px; }
        QHeaderView::section {
            background: #2b2c33; border: none; border-bottom: 1px solid #3a3b42;
            padding: 6px; font-weight: 600;
        }
        QPlainTextEdit {
            background: #141519; border: 1px solid #3a3b42; border-radius: 6px;
            font-family: "Consolas", "Courier New", monospace; font-size: 12px;
        }
        QStatusBar { background: #1e1f24; color: #9a9ba3; }
    )"));
}

void MainWindow::log(const QString &msg)
{
    m_logView->appendPlainText(msg);
}

void MainWindow::onRefresh()
{
    const QString host = currentHost();
    const quint16 port = currentPort();

    if (host.trimmed().isEmpty()) {
        log(QStringLiteral("请输入服务器地址。"));
        return;
    }

    log(QStringLiteral("正在查询 %1:%2 ...").arg(host).arg(port));
    m_refreshBtn->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("查询中..."));

    QFuture<QVector<RemoteDevice>> future =
        QtConcurrent::run(&usbip::fetchExportableDevices, host, port, &m_lastError);
    m_watcher.setFuture(future);
}

void MainWindow::onRefreshFinished()
{
    m_refreshBtn->setEnabled(true);

    const QVector<RemoteDevice> devices = m_watcher.result();

    if (!m_lastError.isEmpty()) {
        log(QStringLiteral("查询失败：%1").arg(m_lastError));
        statusBar()->showMessage(QStringLiteral("查询失败"));
    } else {
        log(QStringLiteral("发现 %1 台可导出设备。").arg(devices.size()));
        statusBar()->showMessage(QStringLiteral("查询完成"));
    }

    m_remoteDevices = devices;

    m_remoteTable->setRowCount(devices.size());
    for (int i = 0; i < devices.size(); ++i) {
        const RemoteDevice &d = devices.at(i);

        m_remoteTable->setItem(i, 0, new QTableWidgetItem(d.busid));
        m_remoteTable->setItem(i, 1, new QTableWidgetItem(
            QStringLiteral("%1:%2").arg(d.vendor, 4, 16, QLatin1Char('0'))
                                   .arg(d.product, 4, 16, QLatin1Char('0'))));
        m_remoteTable->setItem(i, 2, new QTableWidgetItem(deviceClassString(d.deviceClass)));
        m_remoteTable->setItem(i, 3, new QTableWidgetItem(d.speedString()));
        m_remoteTable->setItem(i, 4, new QTableWidgetItem(d.path));

        auto *btn = new QPushButton(QStringLiteral("导入"), m_remoteTable);
        btn->setObjectName(QStringLiteral("primaryButton"));
        connect(btn, &QPushButton::clicked, this, [this, i] { onAttachRow(i); });
        m_remoteTable->setCellWidget(i, 5, btn);
    }
}

void MainWindow::onAttachRow(int row)
{
    if (row < 0 || row >= m_remoteDevices.size()) {
        return;
    }

    const RemoteDevice &d = m_remoteDevices.at(row);
    const QString host = currentHost();
    const quint16 port = currentPort();

    QString err;
    log(QStringLiteral("正在导入 %1 (%2)...").arg(d.busid).arg(host));
    const int hubPort = m_vhci.attach(host, d.busid, port, &err);

    if (hubPort <= 0) {
        log(QStringLiteral("导入失败：%1").arg(err));
    } else {
        log(QStringLiteral("成功导入 %1，端口 %2。").arg(d.busid).arg(hubPort));
        refreshImported();
    }
}

void MainWindow::onDetachRow(int row)
{
    auto *portItem = m_importedTable->item(row, 0);
    if (!portItem) {
        return;
    }

    const int port = portItem->text().toInt();
    QString err;
    if (m_vhci.detach(port, &err)) {
        log(QStringLiteral("已断开端口 %1。").arg(port));
    } else {
        log(QStringLiteral("断开失败：%1").arg(err));
    }
    refreshImported();
}

void MainWindow::refreshImported()
{
    const QVector<ImportedDevice> devices = m_vhci.importedDevices();

    m_importedTable->setRowCount(devices.size());
    for (int i = 0; i < devices.size(); ++i) {
        const ImportedDevice &d = devices.at(i);

        m_importedTable->setItem(i, 0, new QTableWidgetItem(QString::number(d.port)));
        m_importedTable->setItem(i, 1, new QTableWidgetItem(d.vidPidString()));
        m_importedTable->setItem(i, 2, new QTableWidgetItem(d.busid));
        m_importedTable->setItem(i, 3, new QTableWidgetItem(d.host));
        m_importedTable->setItem(i, 4, new QTableWidgetItem(d.speedString()));

        auto *btn = new QPushButton(QStringLiteral("断开"), m_importedTable);
        connect(btn, &QPushButton::clicked, this, [this, i] { onDetachRow(i); });
        m_importedTable->setCellWidget(i, 5, btn);
    }
}

void MainWindow::onAddFavorite()
{
    const QString host = currentHost();
    if (host.trimmed().isEmpty()) {
        return;
    }
    saveFavorite(host);
    log(QStringLiteral("已收藏服务器 %1。").arg(host));
}

QString MainWindow::currentHost() const
{
    return m_hostCombo->currentText().trimmed();
}

quint16 MainWindow::currentPort() const
{
    return static_cast<quint16>(m_portSpin->value());
}

void MainWindow::loadFavorites()
{
    QSettings settings;
    const QStringList hosts = settings.value(QStringLiteral("favorites/hosts")).toStringList();
    m_hostCombo->addItems(hosts);
}

void MainWindow::saveFavorite(const QString &host)
{
    if (m_hostCombo->findText(host) < 0) {
        m_hostCombo->addItem(host);
    }

    QStringList hosts;
    for (int i = 0; i < m_hostCombo->count(); ++i) {
        hosts << m_hostCombo->itemText(i);
    }

    QSettings settings;
    settings.setValue(QStringLiteral("favorites/hosts"), hosts);
}