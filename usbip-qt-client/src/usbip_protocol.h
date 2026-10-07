#pragma once

/*
 * USB/IP v1.1.1 网络协议结构（对端为 usbipd / ESP32 上的 usbip_server）。
 * 协议字段一律网络字节序（大端）。结构体按协议打包，禁止编译器自动填充。
 *
 * 参考: tools/usb/usbip/src/usbip_network.h (Linux kernel)
 *       reference/usbip-win2-master/include/usbip/proto_op.h
 */

#include <cstdint>

namespace usbip {

constexpr std::uint16_t USBIP_VERSION = 0x0111;
constexpr int BUS_ID_SIZE = 32;
constexpr int DEV_PATH_MAX = 256;

constexpr std::uint16_t OP_REQUEST = 0x80 << 8;
constexpr std::uint16_t OP_REPLY   = 0x00 << 8;

constexpr std::uint16_t OP_DEVLIST     = 5;
constexpr std::uint16_t OP_REQ_DEVLIST = OP_REQUEST | OP_DEVLIST; // 0x8005
constexpr std::uint16_t OP_REP_DEVLIST = OP_REPLY   | OP_DEVLIST; // 0x0005

#pragma pack(push, 1)

struct usbip_usb_interface
{
    std::uint8_t bInterfaceClass;
    std::uint8_t bInterfaceSubClass;
    std::uint8_t bInterfaceProtocol;
    std::uint8_t padding;
};

struct usbip_usb_device
{
    char path[DEV_PATH_MAX];
    char busid[BUS_ID_SIZE];

    std::uint32_t busnum;
    std::uint32_t devnum;
    std::uint32_t speed; // usb_device_speed

    std::uint16_t idVendor;
    std::uint16_t idProduct;
    std::uint16_t bcdDevice;

    std::uint8_t bDeviceClass;
    std::uint8_t bDeviceSubClass;
    std::uint8_t bDeviceProtocol;

    std::uint8_t bConfigurationValue;
    std::uint8_t bNumConfigurations;
    std::uint8_t bNumInterfaces;
};

struct op_common
{
    std::uint16_t version; // USBIP_VERSION
    std::uint16_t code;
    std::uint32_t status; // op_status_t，请求时为 0
};

struct op_devlist_request
{
    std::uint32_t _reserved;
};

struct op_devlist_reply
{
    std::uint32_t ndev; // 导出的设备数量
};

struct op_devlist_reply_extra
{
    usbip_usb_device udev;
    // 后紧跟 bNumInterfaces 个 usbip_usb_interface
};

#pragma pack(pop)

static_assert(sizeof(op_common) == 8, "op_common must be 8 bytes");
static_assert(sizeof(op_devlist_reply) == 4, "op_devlist_reply must be 4 bytes");

// ---- 大端字节序转换（主机序 <-> 网络序，二者互为逆运算） ----
inline std::uint16_t swap16(std::uint16_t v)
{
    return static_cast<std::uint16_t>((v >> 8) | (v << 8));
}

inline std::uint32_t swap32(std::uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

inline void byteswap(op_common &c)
{
    c.version = swap16(c.version);
    c.code    = swap16(c.code);
    c.status  = swap32(c.status);
}

inline void byteswap(usbip_usb_device &d)
{
    d.busnum    = swap32(d.busnum);
    d.devnum    = swap32(d.devnum);
    d.speed     = swap32(d.speed);
    d.idVendor  = swap16(d.idVendor);
    d.idProduct = swap16(d.idProduct);
    d.bcdDevice = swap16(d.bcdDevice);
}

inline void byteswap(op_devlist_reply &r)
{
    r.ndev = swap32(r.ndev);
}

inline void byteswap(op_devlist_reply_extra &r)
{
    byteswap(r.udev);
}

// usbip_usb_interface 全为单字节，无需转换
inline void byteswap(usbip_usb_interface &)
{
}

} // namespace usbip