/*
 * usb_dap.h —— USB CMSIS-DAP v2 数据通道
 *
 * 在 CONFIG_DAP_USB_TASK_CORE 指定的核上：
 *   - 安装 TinyUSB（内部 USB device 任务）
 *   - 运行端点 pump 任务：EP OUT 报文投入核间请求通道，
 *     响应通道中的报文从 EP IN 发回主机
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 安装 USB 协议栈并启动 bulk 端点 pump
 *
 * @return ESP_OK / TinyUSB 安装错误码
 */
esp_err_t usb_dap_start(void);

#ifdef __cplusplus
}
#endif
