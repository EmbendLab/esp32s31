#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

// 一台远端（服务器）可导出的 USB 设备
struct RemoteDevice
{
    QString busid;
    quint16 vendor = 0;
    quint16 product = 0;
    quint16 bcdDevice = 0;
    quint8 deviceClass = 0;
    quint8 deviceSubClass = 0;
    quint8 deviceProtocol = 0;
    quint32 speed = 0;
    QString path;

    QString speedString() const;
};

namespace usbip {

// 通过 USB/IP OP_REQ_DEVLIST 从服务器枚举可导出设备（同步阻塞，局域网内很快）。
// 成功返回设备列表；失败返回空并填充 error。
QVector<RemoteDevice> fetchExportableDevices(const QString &host, quint16 port, QString *error);

} // namespace usbip