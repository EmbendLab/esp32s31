/*
 * usb_descriptors.c —— CMSIS-DAP v2 WinUSB/Bulk 描述符
 *
 * 帧布局与 Microsoft OS 2.0 描述符集严格对照 TinyUSB 官方
 * webusb_serial 例程（examples/device/webusb_serial）：
 *   - BOS 仅携带一个 MS OS 2.0 平台能力描述符（无 WebUSB landing page）
 *   - 控制传输 vendor code 0x21 / wIndex 7 返回完整 descriptor set：
 *     WINUSB 兼容 ID + DeviceInterfaceGUIDs 注册表属性
 *   - GUID 使用 CMSIS-DAP v2 在 Windows 上的标准接口 GUID
 */
#include "usb_descriptors.h"

#include <string.h>

#include "sdkconfig.h"

/* ----------------------------- 设备描述符 ------------------------------ */

const tusb_desc_device_t g_dap_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,        /* BOS 要求 USB >= 2.1 */

    /* 设备类=0：类定义在接口层；4 个接口（vendor + CDC IAD 对 + HID）使 Windows
       加载通用父驱动 usbccgp 并为每个功能生成子 PDO（对照本机 303A:1001）。 */
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = CONFIG_DAP_USB_VID,
    .idProduct          = CONFIG_DAP_USB_PID,
    /* Keep the physical composite identity used by the already-installed
       CMSIS-DAP WinUSB binding.  USB/IP has its own PID and descriptor set. */
    .bcdDevice          = 0x0100,

    .iManufacturer      = DAP_STRID_MANUFACTURER,
    .iProduct           = DAP_STRID_PRODUCT,
    .iSerialNumber      = DAP_STRID_SERIAL,

    .bNumConfigurations = 0x01,
};

/* USB/IP follows the reference WinUSB mode.  USB 2.1 advertises the BOS platform
 * capability so Windows requests the MS OS 2.0 WinUSB compatible ID. */
const tusb_desc_device_t g_dap_usbip_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_DAP_USB_VID,
    .idProduct          = DAP_USBIP_PID,
    .bcdDevice          = 0x0106,
    .iManufacturer      = DAP_STRID_MANUFACTURER,
    .iProduct           = DAP_STRID_PRODUCT,
    .iSerialNumber      = DAP_STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/* The USB/IP record advertises a high-speed capable device.  Return a
 * matching qualifier instead of the physical device's USB 2.1/BOS qualifier. */
const tusb_desc_device_qualifier_t g_dap_usbip_qualifier_desc = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0x00,
};

/* 高速设备在其它速度下被查询时返回 */
const tusb_desc_device_qualifier_t g_dap_qualifier_desc = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0x00,
};

/* ----------------------------- HID report 描述符 ------------------------ */

/* CMSIS-DAP vendor HID：usage page 0xFF00 / usage 1，单个 IN+OUT report，
 * 有效载荷 512B、不使用 report ID（report 首字节即 DAP 命令号）。
 * 对照真实 DAPLink（0D28:0204）的 HID 口识别要素，Keil/IAR 据此发现探头。
 *
 * 注意：不能用 TUD_HID_REPORT_DESC_GENERIC_INOUT(512)——其 HID_REPORT_COUNT
 * 固定为 8 位短项，512 会被截断为 0（-Werror=overflow）。512 字段必须用 16 位
 * Report Count（0x96, 0x00, 0x02），此处按 ARM CMSIS-DAP 标准报告描述符手写。 */
const uint8_t g_dap_hid_report_desc[DAP_HID_REPORT_DESC_LEN] = {
    0x06, 0x00, 0xFF,   /* Usage Page (Vendor Defined 0xFF00)          */
    0x09, 0x01,         /* Usage (0x01)                                */
    0xA1, 0x01,         /* Collection (Application)                    */
    0x09, 0x02,         /*   Usage (0x02) — OUT                        */
    0x15, 0x00,         /*   Logical Minimum (0)                       */
    0x26, 0xFF, 0x00,   /*   Logical Maximum (255), 16-bit             */
    0x75, 0x08,         /*   Report Size (8 bits)                      */
    0x96, 0x00, 0x02,   /*   Report Count (512), 16-bit short item     */
    0x09, 0x02,         /*   Usage (0x02)                              */
    0x91, 0x02,         /*   Output (Data | Variable | Absolute)       */
    0x09, 0x03,         /*   Usage (0x03) — IN                         */
    0x81, 0x02,         /*   Input  (Data | Variable | Absolute)       */
    0xC0,               /* End Collection                              */
};

/* USB/IP/HIDClass 报告描述符。这个是 reference 使用的标准 64B CMSIS-DAP
 * v1 形式：输入、输出各 64B，并带一个 1B Feature 报告。虚拟 USB 总线
 * 对 512B HID report 的启动兼容性很差，远端端点因此固定为 64B。 */
const uint8_t g_dap_usbip_hid_report_desc[DAP_USBIP_HID_REPORT_DESC_LEN] = {
    0x06, 0x00, 0xFF,   /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,         /* Usage (0x01)                       */
    0xA1, 0x01,         /* Collection (Application)           */
    0x15, 0x00,         /*   Logical Minimum (0)              */
    0x26, 0xFF, 0x00,   /*   Logical Maximum (255)            */
    0x75, 0x08,         /*   Report Size (8)                  */
    0x95, 0x40,         /*   Report Count (64)                 */
    0x09, 0x01,         /*   Usage (0x01)                      */
    0x81, 0x02,         /*   Input (Data, Variable, Absolute)  */
    0x95, 0x40,         /*   Report Count (64)                 */
    0x09, 0x01,         /*   Usage (0x01)                      */
    0x91, 0x02,         /*   Output (Data, Variable, Absolute) */
    0x95, 0x01,         /*   Report Count (1)                  */
    0x09, 0x01,         /*   Usage (0x01)                      */
    0xB1, 0x02,         /*   Feature (Data, Variable, Absolute) */
    0xC0,               /* End Collection                      */
};

/* --------------------------- 配置描述符 FS/HS --------------------------- */

/* 9(config) + 23(vendor itf0) + 66(CDC IAD+itf1/2) + 32(HID INOUT itf3) = 130 */
#define DAP_CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + \
                                 TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

const uint8_t g_dap_fs_config_desc[] = {
    /* 配置数 1，接口数 4，总线供电，最大 100mA */
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, DAP_CONFIG_TOTAL_LEN,
                          TU_BIT(7), 100),

    /* 接口 0：CMSIS-DAP v2 vendor，EP OUT 0x01 / EP IN 0x81，全速包长 64 */
    TUD_VENDOR_DESCRIPTOR(DAP_ITF_NUM, DAP_STRID_DAP_ITF,
                          DAP_EP_OUT, DAP_EP_IN, 64),

    /* 接口 1/2：CDC-ACM 虚拟串口（宏自带 IAD），通知 EP 0x82，
       数据 EP OUT 0x03 / IN 0x83，全速 bulk 包长 64、通知包长 8 */
    TUD_CDC_DESCRIPTOR(DAP_CDC_ITF_COMM, DAP_STRID_CDC_ITF,
                       DAP_CDC_EP_NOTIF, 8,
                       DAP_CDC_EP_OUT, DAP_CDC_EP_IN, 64),

    /* 接口 3：CMSIS-DAP HID，interrupt EP OUT 0x04 / IN 0x84，
       全速端点包长 64（512B report 由主机按 HID 重组），bInterval=1 */
    TUD_HID_INOUT_DESCRIPTOR(DAP_HID_ITF_NUM, DAP_STRID_HID_ITF,
                             HID_ITF_PROTOCOL_NONE,
                             sizeof(g_dap_hid_report_desc),
                             DAP_HID_EP_OUT, DAP_HID_EP_IN, 64, 1),
};

const uint8_t g_dap_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, DAP_CONFIG_TOTAL_LEN,
                          TU_BIT(7), 100),

    /* 接口 0：CMSIS-DAP v2 vendor，高速包长 512，bulk bInterval 填 0 */
    TUD_VENDOR_DESCRIPTOR(DAP_ITF_NUM, DAP_STRID_DAP_ITF,
                          DAP_EP_OUT, DAP_EP_IN,
                          CONFIG_DAP_USB_BULK_EP_SIZE),

    /* 接口 1/2：CDC-ACM，高速 bulk 包长必须为 512，通知包长 16 */
    TUD_CDC_DESCRIPTOR(DAP_CDC_ITF_COMM, DAP_STRID_CDC_ITF,
                       DAP_CDC_EP_NOTIF, 16,
                       DAP_CDC_EP_OUT, DAP_CDC_EP_IN,
                       CONFIG_DAP_USB_BULK_EP_SIZE),

    /* 接口 3：CMSIS-DAP HID，高速 interrupt 端点包长 512，bInterval=1
       （2^(1-1)=1 个微帧，即 125us 轮询） */
    TUD_HID_INOUT_DESCRIPTOR(DAP_HID_ITF_NUM, DAP_STRID_HID_ITF,
                             HID_ITF_PROTOCOL_NONE,
                             sizeof(g_dap_hid_report_desc),
                             DAP_HID_EP_OUT, DAP_HID_EP_IN,
                             CONFIG_DAP_USB_BULK_EP_SIZE, 1),
};

/* Reference-style single WinUSB CMSIS-DAP interface. */
#define DAP_USBIP_CONFIG_TOTAL_LEN (9 + 9 + 7 + 7 + 7)
const uint8_t g_dap_usbip_config_desc[] = {
    0x09, TUSB_DESC_CONFIGURATION,
    U16_TO_U8S_LE(DAP_USBIP_CONFIG_TOTAL_LEN),
    0x01, 0x01, 0x00, 0x80, 250,
    0x09, TUSB_DESC_INTERFACE, 0x00, 0x00, 0x03, 0xFF, 0x00, 0x00, 0x00,
    0x07, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_BULK,
    U16_TO_U8S_LE(CONFIG_DAP_USB_BULK_EP_SIZE), 0x00,
    0x07, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_BULK,
    U16_TO_U8S_LE(CONFIG_DAP_USB_BULK_EP_SIZE), 0x00,
    0x07, TUSB_DESC_ENDPOINT, 0x82, TUSB_XFER_BULK,
    U16_TO_U8S_LE(CONFIG_DAP_USB_BULK_EP_SIZE), 0x00,
};

/* The reference's device-level WINUSB compatible ID for a single interface. */
#define DAP_USBIP_MS_OS_20_LEN 0xA2
static const uint8_t s_usbip_bos_desc[] = {
    0x05, 0x0F, 0x21, 0x00, 0x01,
    0x1C, 0x10, 0x05, 0x00,
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,
    0xA2, 0x00, DAP_USBIP_MS_OS_VENDOR_CODE, 0x00,
};

static const uint8_t s_usbip_ms_os_20_desc[] = {
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0xA2, 0x00,
    0x14, 0x00, 0x03, 0x00,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x84, 0x00, 0x04, 0x00, 0x07, 0x00, 0x2A, 0x00,
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0,
    'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0,
    'D', 0, 's', 0, 0, 0,
    0x50, 0x00,
    '{', 0, 'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0, '5', 0, 'A', 0, 'D', 0, '-', 0,
    '2', 0, '9', 0, '3', 0, 'B', 0, '-', 0, '4', 0, '6', 0, '6', 0, '3', 0, '-', 0,
    'A', 0, 'A', 0, '3', 0, '6', 0, '-', 0, '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0,
    '6', 0, '4', 0, '6', 0, '3', 0, '7', 0, '7', 0, '6', 0, '}', 0,
    0, 0, 0, 0,
};

TU_VERIFY_STATIC(sizeof(s_usbip_bos_desc) == 0x21,
                 "USB/IP BOS descriptor size mismatch");
TU_VERIFY_STATIC(sizeof(s_usbip_ms_os_20_desc) == DAP_USBIP_MS_OS_20_LEN,
                 "USB/IP MS OS 2.0 descriptor size mismatch");

/* --------------------------------- BOS --------------------------------- */

#define DAP_BOS_TOTAL_LEN   (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
/* set header(10) + config subset(8) + 1 × WINUSB function block(160) = 178 */
#define DAP_MS_OS_20_LEN    0xB2

static const uint8_t s_bos_desc[] = {
    TUD_BOS_DESCRIPTOR(DAP_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(DAP_MS_OS_20_LEN, DAP_MS_OS_VENDOR_CODE),
};

const uint8_t *dap_bos_descriptor(size_t *len)
{
    if (len) *len = sizeof(s_bos_desc);
    return s_bos_desc;
}

/* esp_tinyusb 占用了 device/config/string 回调，BOS 回调仍由应用提供 */
uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos_desc;
}

/* --------------------------- MS OS 2.0 描述符集 ------------------------- */

/* UTF-16LE 单字符；末尾双 0 同时结束字符串与 REG_MULTI_SZ 列表 */
#define WCH(c)      (c), 0x00

/* 每个功能块 160B：function subset(8) + compat WINUSB(20) + reg property(132) */
#define DAP_MSOS_FUNC_LEN    0xA0

/* 属性名 "DeviceInterfaceGUIDs" + 单 0，共 42B（21 个 UTF-16 单元） */
#define DAP_MSOS_NAME \
    WCH('D'), WCH('e'), WCH('v'), WCH('i'), WCH('c'), WCH('e'), \
    WCH('I'), WCH('n'), WCH('t'), WCH('e'), WCH('r'), WCH('f'), \
    WCH('a'), WCH('c'), WCH('e'), WCH('G'), WCH('U'), WCH('I'), \
    WCH('D'), WCH('s'), 0x00, 0x00

/* The physical composite device historically used this GUID.  Keep it stable
 * so Windows reuses the working WinUSB association for PID 4041. */
#define DAP_MSOS_GUID_PHYSICAL \
    WCH('{'), \
    WCH('C'), WCH('D'), WCH('B'), WCH('3'), WCH('F'), WCH('5'), WCH('2'), WCH('0'), WCH('-'), \
    WCH('5'), WCH('A'), WCH('1'), WCH('2'), WCH('-'), \
    WCH('4'), WCH('6'), WCH('D'), WCH('2'), WCH('-'), \
    WCH('A'), WCH('B'), WCH('3'), WCH('7'), WCH('-'), \
    WCH('2'), WCH('9'), WCH('0'), WCH('C'), WCH('A'), WCH('E'), \
    WCH('0'), WCH('3'), WCH('6'), WCH('8'), WCH('8'), WCH('D'), WCH('}'), \
    0x00, 0x00, 0x00, 0x00

/* USB/IP uses the reference single-interface CMSIS-DAP v2 GUID. */
#define DAP_MSOS_GUID_USBIP \
    WCH('{'), \
    WCH('C'), WCH('D'), WCH('B'), WCH('3'), WCH('B'), WCH('5'), WCH('A'), WCH('D'), WCH('-'), \
    WCH('2'), WCH('9'), WCH('3'), WCH('B'), WCH('-'), \
    WCH('4'), WCH('6'), WCH('6'), WCH('3'), WCH('-'), \
    WCH('A'), WCH('A'), WCH('3'), WCH('6'), WCH('-'), \
    WCH('1'), WCH('A'), WCH('A'), WCH('E'), WCH('4'), WCH('6'), WCH('4'), WCH('6'), WCH('3'), WCH('7'), WCH('7'), WCH('6'), WCH('}'), \
    0x00, 0x00, 0x00, 0x00

/* 一个 WINUSB 功能块：function subset + compat ID + DeviceInterfaceGUIDs */
#define DAP_MSOS_FUNC(first_itf, guid) \
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), \
    (first_itf), 0x00, U16_TO_U8S_LE(DAP_MSOS_FUNC_LEN), \
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), \
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY), \
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), \
    DAP_MSOS_NAME, U16_TO_U8S_LE(0x0050), guid

static const uint8_t s_ms_os_20_desc[DAP_MS_OS_20_LEN] = {
    /* Set header: wLength, wType, dwWindowsVersion(6.3), wTotalLength */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(DAP_MS_OS_20_LEN),

    /* Preserve the physical DAPLink descriptor that Windows already accepts. */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(DAP_MS_OS_20_LEN - 0x0A),

    /* 接口 0：CMSIS-DAP v2（WinUSB + DAP GUID）。
       接口 1/2 是 CDC-ACM（内置 usbser.sys）、接口 3 是 HID（内置 HidUsb），
       均按标准类匹配，无需 MS OS 功能块。 */
    DAP_MSOS_FUNC(DAP_ITF_NUM, DAP_MSOS_GUID_PHYSICAL),
};

const uint8_t *dap_ms_os_20_descriptor(size_t *len)
{
    if (len) *len = sizeof(s_ms_os_20_desc);
    return s_ms_os_20_desc;
}

const uint8_t *dap_usbip_bos_descriptor(size_t *len)
{
    if (len) *len = sizeof(s_usbip_bos_desc);
    return s_usbip_bos_desc;
}

const uint8_t *dap_usbip_ms_os_20_descriptor(size_t *len)
{
    if (len) *len = sizeof(s_usbip_ms_os_20_desc);
    return s_usbip_ms_os_20_desc;
}

/* USB/IP virtual host controllers may use the legacy OS descriptor query
 * instead of BOS. Both paths advertise the same WinUSB ID and DAP GUID. */
static const uint8_t s_usbip_ms_os_10_string[] = {
    18, TUSB_DESC_STRING,
    WCH('M'), WCH('S'), WCH('F'), WCH('T'), WCH('1'), WCH('0'), WCH('0'),
    DAP_USBIP_MS_OS_VENDOR_CODE, 0,
};
static const uint8_t s_usbip_ms_os_10_compat[] = {
    U32_TO_U8S_LE(40), U16_TO_U8S_LE(0x0100), U16_TO_U8S_LE(4),
    1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, /* interface zero, reserved byte must be one */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
};
static const uint8_t s_usbip_ms_os_10_properties[] = {
    U32_TO_U8S_LE(146), U16_TO_U8S_LE(0x0100), U16_TO_U8S_LE(5),
    U16_TO_U8S_LE(1),
    U32_TO_U8S_LE(136), U32_TO_U8S_LE(7), U16_TO_U8S_LE(42),
    DAP_MSOS_NAME, U32_TO_U8S_LE(80), DAP_MSOS_GUID_USBIP,
};
TU_VERIFY_STATIC(sizeof(s_usbip_ms_os_10_string) == 18, "MS OS string size");
TU_VERIFY_STATIC(sizeof(s_usbip_ms_os_10_compat) == 40, "MS OS compatible ID size");
TU_VERIFY_STATIC(sizeof(s_usbip_ms_os_10_properties) == 146, "MS OS properties size");

const uint8_t *dap_usbip_ms_os_10_string(size_t *len)
{
    if (len) *len = sizeof(s_usbip_ms_os_10_string);
    return s_usbip_ms_os_10_string;
}

const uint8_t *dap_usbip_ms_os_10_descriptor(uint16_t index, size_t *len)
{
    if (index == 4) {
        if (len) *len = sizeof(s_usbip_ms_os_10_compat);
        return s_usbip_ms_os_10_compat;
    }
    if (index == 5) {
        if (len) *len = sizeof(s_usbip_ms_os_10_properties);
        return s_usbip_ms_os_10_properties;
    }
    if (len) *len = 0;
    return NULL;
}

TU_VERIFY_STATIC(sizeof(s_ms_os_20_desc) == DAP_MS_OS_20_LEN,
                 "MS OS 2.0 descriptor size mismatch");

/* ----------------------------- 字符串描述符 ----------------------------- */

/* index 0 的前两个字节为 LANGID（0x0409 美式英语），由 esp_tinyusb 拷贝 */
const char *const g_dap_string_descs[DAP_STRID_COUNT] = {
    "\x09\x04",
    "Trae",
    "ESP32-S31 CMSIS-DAP",
    "S31DAP0001",
    "CMSIS-DAP v2",
    "CMSIS-DAP UART",
    "CMSIS-DAP v1",
};

const uint8_t *dap_string_descriptor(uint8_t index, size_t *len)
{
    static uint8_t descriptor[2 + 2 * 63];
    static const uint8_t lang_descriptor[] = { 4, TUSB_DESC_STRING, 0x09, 0x04 };
    if (index >= DAP_STRID_COUNT) {
        if (len) *len = 0;
        return NULL;
    }
    if (index == DAP_STRID_LANGID) {
        if (len) *len = sizeof(lang_descriptor);
        return lang_descriptor;
    }
    const char *text = g_dap_string_descs[index];
    size_t text_len = strlen(text);
    if (text_len > 63) text_len = 63;
    descriptor[0] = (uint8_t)(2 + 2 * text_len);
    descriptor[1] = TUSB_DESC_STRING;
    for (size_t i = 0; i < text_len; ++i) {
        descriptor[2 + 2 * i] = (uint8_t)text[i];
        descriptor[3 + 2 * i] = 0;
    }
    if (len) *len = 2 + 2 * text_len;
    return descriptor;
}

/* ----------------------- 供应商控制请求（WinUSB 枚举） ---------------------- */

/*
 * TinyUSB usbd 对所有 bmRequestType=vendor 的请求（含 device recipient，
 * 即 MS OS 2.0 的 GET_MS_DESCRIPTOR）直接路由到本回调。
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }
    if (request->bRequest == DAP_MS_OS_VENDOR_CODE && request->wIndex == 7) {
        /* MS OS 2.0 描述符 178 字节，是枚举中唯一跨多个 EP0 包的控制 IN。
           DWC2 DMA 模式把描述符直接放在 Flash/.rodata 指针上、分多包逐次
           重填 DMA 地址；跨缓存行的多包取数在部分端口上会让主机收到损坏内容
           而静默丢弃整个描述符。这里搬到内部 SRAM 中 64B 对齐的 DMA 安全缓冲。 */
        static __attribute__((aligned(64))) uint8_t s_ms_os_ram[DAP_MS_OS_20_LEN];
        static bool s_ms_os_copied = false;
        if (!s_ms_os_copied) {
            memcpy(s_ms_os_ram, s_ms_os_20_desc, DAP_MS_OS_20_LEN);
            s_ms_os_copied = true;
        }
        uint16_t total_len;
        memcpy(&total_len, s_ms_os_ram + 8, 2);
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)s_ms_os_ram, total_len);
    }
    return false;   /* 其它 vendor 请求：STALL */
}
