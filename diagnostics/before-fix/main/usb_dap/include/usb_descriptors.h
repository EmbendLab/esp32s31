/*
 * usb_descriptors.h —— CMSIS-DAP (WinUSB/Bulk + HID) + CDC-ACM UART 描述符声明
 *
 * 复合设备（bDeviceClass=0，4 个接口，USB-IF IAD 关联 CDC 两接口）：
 *   接口 0        Vendor Specific（CMSIS-DAP v2 bulk，WinUSB 自动绑定）
 *   接口 1/2      CDC-ACM 虚拟串口（mbed UART 透传，Windows 自带 usbser.sys）
 *   接口 3        HID（CMSIS-DAP v1 vendor report，usage page 0xFF00/usage 1，
 *                 Windows 自带 HidUsb；Keil/IAR 等经此口发现调试器）
 * Windows 侧通过 BOS 中的 MS OS 2.0 平台能力描述符为接口 0 自动绑定 WinUSB，
 * 无需 INF 文件。HID/CDC 均由系统标准类驱动匹配。描述符实体见 usb_descriptors.c。
 */
#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 供应商请求码（同时写入 BOS MS OS 2.0 平台能力描述符） */
#define DAP_MS_OS_VENDOR_CODE   0x21

/* 接口 0：CMSIS-DAP v2 vendor */
#define DAP_ITF_NUM             0
#define DAP_EP_OUT              0x01
#define DAP_EP_IN               0x81

/* 接口 1/2：CDC-ACM（IAD 绑定通信+数据两个接口） */
#define DAP_CDC_ITF_COMM        1
#define DAP_CDC_ITF_DATA        2
#define DAP_CDC_EP_NOTIF        0x82    /* 通知中断 IN */
#define DAP_CDC_EP_OUT          0x03    /* 批量 OUT（主机 -> 目标 UART） */
#define DAP_CDC_EP_IN           0x83    /* 批量 IN （目标 UART -> 主机） */

/* 接口 3：HID CMSIS-DAP（双向 interrupt，与 bulk 口共用同一 DAP 协议栈） */
#define DAP_HID_ITF_NUM         3
#define DAP_HID_EP_OUT          0x04    /* interrupt OUT（主机 -> 设备） */
#define DAP_HID_EP_IN           0x84    /* interrupt IN （设备 -> 主机） */

/* 一个 CMSIS-DAP HID report 的有效载荷字节数（无 report ID）。
 * 与 bulk 包 / DAP_Info(PacketSize) 保持同为 512，主机侧缓冲模型一致。 */
#define DAP_HID_REPORT_SIZE     CONFIG_DAP_USB_BULK_EP_SIZE

/* 字符串描述符索引 */
#define DAP_STRID_LANGID        0
#define DAP_STRID_MANUFACTURER  1
#define DAP_STRID_PRODUCT       2
#define DAP_STRID_SERIAL        3
#define DAP_STRID_DAP_ITF       4
#define DAP_STRID_CDC_ITF       5
#define DAP_STRID_HID_ITF       6
#define DAP_STRID_COUNT         7

extern const tusb_desc_device_t             g_dap_device_desc;
extern const tusb_desc_device_qualifier_t   g_dap_qualifier_desc;
extern const uint8_t                        g_dap_fs_config_desc[];
extern const uint8_t                        g_dap_hs_config_desc[];
extern const uint8_t                        g_dap_hid_report_desc[];
extern const char *const                    g_dap_string_descs[DAP_STRID_COUNT];

#ifdef __cplusplus
}
#endif
