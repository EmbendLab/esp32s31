#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <windows.h>

// 已导入（attach 到本机）的 USB 设备
struct ImportedDevice
{
    int port = 0;
    QString busid;
    QString host;
    QString service;
    quint32 devid = 0;
    quint32 speed = 0;
    quint16 vendor = 0;
    quint16 product = 0;
    QString serial;

    QString speedString() const;
    QString vidPidString() const;
};

// 与 usbip2_ude.sys 交互：打开设备接口，执行 attach / detach / 枚举已导入设备。
// 依赖内核驱动已安装并且 root 设备节点已被创建（等价 devnode.exe）。
class VhciClient : public QObject
{
    Q_OBJECT

public:
    explicit VhciClient(QObject *parent = nullptr);

    bool isDriverAvailable() const;

    // attach 远端设备，成功返回 hub 端口号（>=1），失败返回 0 并填充 err
    int attach(const QString &host, const QString &busid, quint16 port, QString *err);

    bool detach(int port, QString *err);
    bool detachAll(QString *err);

    QVector<ImportedDevice> importedDevices();

private:
    HANDLE openDevice(); // 打开设备接口句柄，失败返回 INVALID_HANDLE_VALUE
};