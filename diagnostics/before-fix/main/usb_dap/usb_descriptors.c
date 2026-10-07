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
    .bcdDevice          = 0x0100,

    .iManufacturer      = DAP_STRID_MANUFACTURER,
    .iProduct           = DAP_STRID_PRODUCT,
    .iSerialNumber      = DAP_STRID_SERIAL,

    .bNumConfigurations = 0x01,
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
const uint8_t g_dap_hid_report_desc[] = {
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

/* --------------------------------- BOS --------------------------------- */

#define DAP_BOS_TOTAL_LEN   (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
/* set header(10) + config subset(8) + 1 × WINUSB function block(160) = 178 */
#define DAP_MS_OS_20_LEN    0xB2

static const uint8_t s_bos_desc[] = {
    TUD_BOS_DESCRIPTOR(DAP_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(DAP_MS_OS_20_LEN, DAP_MS_OS_VENDOR_CODE),
};

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

/* 接口 0：CMSIS-DAP v2 标准 GUID {CDB3F520-5A12-46D2-AB37-290CAE03688D} */
#define DAP_MSOS_GUID_DAP \
    WCH('{'), \
    WCH('C'), WCH('D'), WCH('B'), WCH('3'), WCH('F'), WCH('5'), WCH('2'), WCH('0'), WCH('-'), \
    WCH('5'), WCH('A'), WCH('1'), WCH('2'), WCH('-'), \
    WCH('4'), WCH('6'), WCH('D'), WCH('2'), WCH('-'), \
    WCH('A'), WCH('B'), WCH('3'), WCH('7'), WCH('-'), \
    WCH('2'), WCH('9'), WCH('0'), WCH('C'), WCH('A'), WCH('E'), \
    WCH('0'), WCH('3'), WCH('6'), WCH('8'), WCH('8'), WCH('D'), WCH('}'), \
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

    /* Configuration subset header：bConfigurationValue=0（同官方 webusb 例程） */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(DAP_MS_OS_20_LEN - 0x0A),

    /* 接口 0：CMSIS-DAP v2（WinUSB + DAP GUID）。
       接口 1/2 是 CDC-ACM（内置 usbser.sys）、接口 3 是 HID（内置 HidUsb），
       均按标准类匹配，无需 MS OS 功能块。 */
    DAP_MSOS_FUNC(DAP_ITF_NUM, DAP_MSOS_GUID_DAP),
};

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
