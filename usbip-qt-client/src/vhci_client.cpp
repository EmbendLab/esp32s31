#include "vhci_client.h"

#include "vhci.h"

#include <setupapi.h>
#include <devguid.h>

#include <QStringList>

namespace {

QString winError(DWORD code)
{
    wchar_t *buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
    if (n && buf) {
        QString s = QString::fromWCharArray(buf, static_cast<int>(n)).trimmed();
        LocalFree(buf);
        return s;
    }
    return QStringLiteral("错误码 %1").arg(code);
}

} // namespace

QString ImportedDevice::speedString() const
{
    switch (speed) {
    case usbip::vhci::USB_SPEED_LOW:        return QStringLiteral("Low");
    case usbip::vhci::USB_SPEED_FULL:       return QStringLiteral("Full");
    case usbip::vhci::USB_SPEED_HIGH:       return QStringLiteral("High");
    case usbip::vhci::USB_SPEED_WIRELESS:   return QStringLiteral("Wireless");
    case usbip::vhci::USB_SPEED_SUPER:      return QStringLiteral("Super");
    case usbip::vhci::USB_SPEED_SUPER_PLUS: return QStringLiteral("Super+");
    default: return QStringLiteral("?");
    }
}

QString ImportedDevice::vidPidString() const
{
    return QStringLiteral("%1:%2")
        .arg(vendor, 4, 16, QLatin1Char('0'))
        .arg(product, 4, 16, QLatin1Char('0'));
}

VhciClient::VhciClient(QObject *parent)
    : QObject(parent)
{
}

HANDLE VhciClient::openDevice()
{
    using namespace usbip::vhci;

    HANDLE result = INVALID_HANDLE_VALUE;

    const HDEVINFO devInfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_USBIP_VHCI, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return result;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVINTERFACE_USBIP_VHCI, 0, &ifData)) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return result;
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, nullptr, 0, &required, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return result;
    }

    auto *detail = static_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(malloc(required));
    detail->cbSize = sizeof(*detail);
    if (SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detail, required, nullptr, nullptr)) {
        result = CreateFileW(detail->DevicePath,
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    free(detail);
    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

bool VhciClient::isDriverAvailable() const
{
    VhciClient *self = const_cast<VhciClient *>(this);
    const HANDLE h = self->openDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

int VhciClient::attach(const QString &host, const QString &busid, quint16 port, QString *err)
{
    using namespace usbip::vhci;

    const HANDLE h = openDevice();
    if (h == INVALID_HANDLE_VALUE) {
        if (err) {
            *err = QStringLiteral("无法打开 USB/IP 驱动设备接口（请确认驱动已安装：%1）")
                       .arg(winError(GetLastError()));
        }
        return 0;
    }

    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray busidUtf8 = busid.toUtf8();
    const QByteArray serviceUtf8 = QString::number(port).toLatin1();

    ioctl::plugin_hardware r{};
    r.size = sizeof(r);
    r.port = 0;
    r.location_hash = 0;

    strncpy_s(r.busid, sizeof(r.busid), busidUtf8.constData(), busidUtf8.size());
    strncpy_s(r.service, sizeof(r.service), serviceUtf8.constData(), serviceUtf8.size());
    strncpy_s(r.host, sizeof(r.host), hostUtf8.constData(), hostUtf8.size());
    r.serial[0] = '\0';
    r.wsk_events = false; // zero-copy，适合大数据量设备

    const DWORD outlen = offsetof(ioctl::plugin_hardware, port) + sizeof(r.port);
    DWORD returned = 0;

    if (!DeviceIoControl(h, ioctl::PLUGIN_HARDWARE, &r, sizeof(r), &r, outlen, &returned, nullptr)) {
        if (err) {
            *err = QStringLiteral("attach 失败：%1").arg(winError(GetLastError()));
        }
        CloseHandle(h);
        return 0;
    }

    CloseHandle(h);

    if (returned != outlen || r.port <= 0) {
        if (err) {
            *err = QStringLiteral("驱动返回异常数据");
        }
        return 0;
    }

    return r.port;
}

bool VhciClient::detach(int port, QString *err)
{
    using namespace usbip::vhci;

    const HANDLE h = openDevice();
    if (h == INVALID_HANDLE_VALUE) {
        if (err) {
            *err = QStringLiteral("无法打开 USB/IP 驱动设备接口");
        }
        return false;
    }

    ioctl::plugout_hardware r{};
    r.size = sizeof(r);
    r.port = port;

    DWORD returned = 0;
    const bool ok = DeviceIoControl(h, ioctl::PLUGOUT_HARDWARE, &r, sizeof(r),
                                    nullptr, 0, &returned, nullptr);
    if (!ok && err) {
        *err = QStringLiteral("detach 失败：%1").arg(winError(GetLastError()));
    }
    CloseHandle(h);
    return ok;
}

bool VhciClient::detachAll(QString *err)
{
    using namespace usbip::vhci;

    for (const ImportedDevice &d : importedDevices()) {
        if (!detach(d.port, err)) {
            return false;
        }
    }
    return true;
}

QVector<ImportedDevice> VhciClient::importedDevices()
{
    using namespace usbip::vhci;

    QVector<ImportedDevice> result;

    const HANDLE h = openDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return result;
    }

    constexpr ULONG devicesOffset = offsetof(ioctl::get_imported_devices, devices);

    std::vector<char> buf;
    bool ok = false;

    for (int cnt = 4; cnt <= 1024; cnt <<= 1) {
        buf.resize(devicesOffset + static_cast<size_t>(cnt) * sizeof(imported_device));

        auto *r = reinterpret_cast<ioctl::get_imported_devices *>(buf.data());
        r->size = sizeof(*r);

        DWORD returned = 0;
        if (DeviceIoControl(h, ioctl::GET_IMPORTED_DEVICES, r, sizeof(r->size),
                            buf.data(), static_cast<DWORD>(buf.size()), &returned, nullptr)) {
            buf.resize(returned);
            ok = true;
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
    }

    if (!ok) {
        CloseHandle(h);
        return result;
    }

    if (buf.size() >= devicesOffset) {
        const size_t bytes = buf.size() - devicesOffset;
        if (bytes % sizeof(imported_device) == 0) {
            const auto *devs = reinterpret_cast<const imported_device *>(buf.data() + devicesOffset);
            const size_t n = bytes / sizeof(imported_device);
            result.reserve(static_cast<int>(n));
            for (size_t i = 0; i < n; ++i) {
                const auto &d = devs[i];
                ImportedDevice dev;
                dev.port = d.port;
                dev.busid = QString::fromLatin1(d.busid);
                dev.host = QString::fromLatin1(d.host);
                dev.service = QString::fromLatin1(d.service);
                dev.devid = d.devid;
                dev.speed = static_cast<quint32>(d.speed);
                dev.vendor = d.vendor;
                dev.product = d.product;
                dev.serial = QString::fromLatin1(d.serial);
                result.append(std::move(dev));
            }
        }
    }

    CloseHandle(h);
    return result;
}