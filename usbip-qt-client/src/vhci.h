#pragma once

/*
 * 与内核驱动 usbip2_ude.sys 交互所需的结构体与 IOCTL 定义（Win32 用户态版本）。
 * 类型与成员顺序须与 reference/usbip-win2-master/include/usbip/vhci.h 完全一致，
 * 因为它们是跨驱动边界的 ABI（METHOD_BUFFERED）。
 *
 * 注意：这些结构体使用“自然对齐”（驱动端未加 #pragma pack），
 * 不要手动打包。x64 下 MinGW-w64 与 MSVC 的 POD 布局一致。
 */

#include <windows.h>
#include <winioctl.h>

namespace usbip::vhci {

// {B4030C06-DC5F-4FCC-87EB-E5515A0935C0}
inline constexpr GUID GUID_DEVINTERFACE_USBIP_VHCI =
    { 0xB4030C06, 0xDC5F, 0x4FCC, { 0x87, 0xEB, 0xE5, 0x51, 0x5A, 0x09, 0x35, 0xC0 } };

constexpr int BUS_ID_SIZE = 32;
constexpr int SERIAL_BUFSZ = 16;

enum usb_device_speed
{
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,
    USB_SPEED_FULL,
    USB_SPEED_HIGH,
    USB_SPEED_WIRELESS,
    USB_SPEED_SUPER,
    USB_SPEED_SUPER_PLUS,
};

struct base
{
    ULONG size; // IN，本结构体自身大小
};

struct imported_device_location
{
    int port;          // OUT，>=1，出错为 0
    ULONG location_hash; // OUT，hash(host, service, busid)

    char busid[BUS_ID_SIZE];
    char service[32];    // NI_MAXSERV，通常是 "3240"
    char host[1025];     // NI_MAXHOST，服务器 IP/主机名
};

struct imported_device_properties
{
    UINT32 devid;
    usb_device_speed speed;
    UINT16 vendor;
    UINT16 product;

    char serial[SERIAL_BUFSZ];
    UCHAR iserial;
    bool wsk_events;
};

struct imported_device : imported_device_location, imported_device_properties {};

// 设备状态，与驱动导出的六态一致
enum class state
{
    unplugged = 0,
    connecting,
    connected,
    plugged,
    disconnected,
    unplugging,
};

struct device_state : base, imported_device
{
    state state;
    ULONG source_id;
};

namespace ioctl {

enum class function
{
    plugin_hardware = 0x800, // 小于 0x800 的值保留给微软
    plugout_hardware,
    get_imported_devices,
    set_persistent,
    get_persistent,
    stop_attach_attempts,
    plugin_hardware_once,
    plugout_hardware_and_reattach,
};

constexpr ULONG make(function id)
{
    return CTL_CODE(FILE_DEVICE_UNKNOWN, static_cast<int>(id),
                    METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
}

enum
{
    PLUGIN_HARDWARE      = make(function::plugin_hardware),
    PLUGOUT_HARDWARE     = make(function::plugout_hardware),
    GET_IMPORTED_DEVICES = make(function::get_imported_devices),
};

struct plugin_hardware : base, imported_device_location
{
    char serial[SERIAL_BUFSZ];
    bool wsk_events;
};

struct plugout_hardware : base
{
    int port;
};

struct get_imported_devices : base
{
    imported_device devices[ANYSIZE_ARRAY];
};

} // namespace ioctl

// ---- 编译期校验关键 ABI 布局（x64 自然对齐） ----
static_assert(sizeof(base) == 4);
static_assert(sizeof(imported_device_location) == 1100);
static_assert(sizeof(imported_device_properties) == 32);
static_assert(sizeof(imported_device) == 1132);
static_assert(offsetof(ioctl::get_imported_devices, devices) == 4);
static_assert(sizeof(ioctl::plugin_hardware) == 1124);
static_assert(sizeof(ioctl::plugout_hardware) == 8);

} // namespace usbip::vhci