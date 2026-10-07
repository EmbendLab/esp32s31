#include "remote_client.h"

#include "usbip_protocol.h"
#include "vhci.h"

#include <QNetworkProxy>
#include <QTcpSocket>

namespace {

constexpr int kTimeoutMs = 3000;

bool readExact(QTcpSocket *sock, void *buf, qint64 len)
{
    auto *p = static_cast<char *>(buf);
    while (len > 0) {
        if (!sock->waitForReadyRead(kTimeoutMs)) {
            return false;
        }
        const qint64 n = sock->read(p, len);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

bool writeAll(QTcpSocket *sock, const void *buf, qint64 len)
{
    const auto *p = static_cast<const char *>(buf);
    qint64 written = 0;
    while (written < len) {
        const qint64 n = sock->write(p + written, len - written);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            // 非阻塞缓冲暂时写不下，等待可写后重试
            if (!sock->waitForBytesWritten(kTimeoutMs)) {
                return false;
            }
            continue;
        }
        written += n;
    }
    // waitForBytesWritten 会驱动 QTcpSocket 把内部缓冲真正发到网络（阻塞模式写法）
    return sock->waitForBytesWritten(kTimeoutMs);
}

} // namespace

QString RemoteDevice::speedString() const
{
    switch (speed) {
    case usbip::vhci::USB_SPEED_LOW:        return QStringLiteral("Low (1.5 Mbps)");
    case usbip::vhci::USB_SPEED_FULL:       return QStringLiteral("Full (12 Mbps)");
    case usbip::vhci::USB_SPEED_HIGH:       return QStringLiteral("High (480 Mbps)");
    case usbip::vhci::USB_SPEED_WIRELESS:   return QStringLiteral("Wireless");
    case usbip::vhci::USB_SPEED_SUPER:      return QStringLiteral("Super (5 Gbps)");
    case usbip::vhci::USB_SPEED_SUPER_PLUS: return QStringLiteral("Super+ (10 Gbps)");
    default: return QStringLiteral("Unknown");
    }
}

namespace usbip {

QVector<RemoteDevice> fetchExportableDevices(const QString &host, quint16 port, QString *error)
{
    QVector<RemoteDevice> devices;

    QTcpSocket sock;
    sock.setProxy(QNetworkProxy::NoProxy); // 目标为局域网私有 IP，禁止走系统代理
    sock.connectToHost(host, port);
    if (!sock.waitForConnected(kTimeoutMs)) {
        if (error) {
            *error = QStringLiteral("无法连接 %1:%2 — %3")
                         .arg(host).arg(port).arg(sock.errorString());
        }
        return devices;
    }

    // 1. 发送 OP_REQ_DEVLIST
    usbip::op_common req{usbip::USBIP_VERSION, usbip::OP_REQ_DEVLIST, 0};
    usbip::byteswap(req);
    if (!writeAll(&sock, &req, sizeof(req))) {
        if (error) {
            *error = QStringLiteral("发送请求失败 (socket error %1: %2)")
                         .arg(sock.error()).arg(sock.errorString());
        }
        return devices;
    }

    // 2. 接收 OP_REP_DEVLIST 的 op_common 头
    usbip::op_common rep{};
    if (!readExact(&sock, &rep, sizeof(rep))) {
        if (error) {
            *error = QStringLiteral("读取应答失败");
        }
        return devices;
    }
    usbip::byteswap(rep);

    if (rep.version != usbip::USBIP_VERSION) {
        if (error) {
            *error = QStringLiteral("协议版本不匹配：0x%1").arg(rep.version, 0, 16);
        }
        return devices;
    }
    if (rep.code != usbip::OP_REP_DEVLIST) {
        if (error) {
            *error = QStringLiteral("应答码错误：0x%1").arg(rep.code, 0, 16);
        }
        return devices;
    }
    if (rep.status != 0) {
        if (error) {
            *error = QStringLiteral("服务器返回状态 %1").arg(rep.status);
        }
        return devices;
    }

    // 3. 接收设备数量
    usbip::op_devlist_reply reply{};
    if (!readExact(&sock, &reply, sizeof(reply))) {
        if (error) {
            *error = QStringLiteral("读取设备列表失败");
        }
        return devices;
    }
    usbip::byteswap(reply);

    // 4. 逐个读设备及其接口
    devices.reserve(static_cast<int>(reply.ndev));
    for (quint32 i = 0; i < reply.ndev; ++i) {
        usbip::op_devlist_reply_extra extra{};
        if (!readExact(&sock, &extra, sizeof(extra))) {
            if (error) {
                *error = QStringLiteral("读取设备信息失败");
            }
            devices.clear();
            return devices;
        }
        usbip::byteswap(extra);

        const auto &d = extra.udev;
        RemoteDevice dev;
        dev.busid = QString::fromLatin1(d.busid);
        dev.vendor = d.idVendor;
        dev.product = d.idProduct;
        dev.bcdDevice = d.bcdDevice;
        dev.deviceClass = d.bDeviceClass;
        dev.deviceSubClass = d.bDeviceSubClass;
        dev.deviceProtocol = d.bDeviceProtocol;
        dev.speed = d.speed;
        dev.path = QString::fromLatin1(d.path);
        devices.append(std::move(dev));

        // 跳过接口描述符（本版界面不展示接口）
        for (quint8 j = 0; j < d.bNumInterfaces; ++j) {
            usbip::usbip_usb_interface intf{};
            if (!readExact(&sock, &intf, sizeof(intf))) {
                if (error) {
                    *error = QStringLiteral("读取接口信息失败");
                }
                devices.clear();
                return devices;
            }
        }
    }

    return devices;
}

} // namespace usbip