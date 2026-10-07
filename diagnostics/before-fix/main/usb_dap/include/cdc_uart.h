/*
 * cdc_uart.h —— USB CDC-ACM 虚拟串口与物理 UART 的双向透传
 *
 * 复合设备的接口 1/2 枚举为 CDC-ACM（Windows 自带 usbser.sys，免驱出现 COM 口）。
 * 本模块把该 CDC 通道与一个物理 UART（默认 UART1，GPIO17/18）桥接起来：
 *   主机 -> CDC OUT -> UART TX -> 目标 RXD
 *   目标 TXD -> UART RX -> CDC IN -> 主机
 * 主机通过 CDC SET_LINE_CODING 改波特率/格式时自动同步到物理 UART。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 安装 UART 驱动并创建 CDC<->UART 透传任务（在 usb_dap_start 之后调用） */
esp_err_t cdc_uart_start(void);

#ifdef __cplusplus
}
#endif
