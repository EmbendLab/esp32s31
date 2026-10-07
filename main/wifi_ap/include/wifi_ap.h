/*
 * Wi-Fi SoftAP 模块：负责 AP 启动与站点上下线事件。
 * 已强制关闭 modem-sleep，射频全程在线（配合实时调试桥场景）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并启动 SoftAP（内部完成 netif/event loop/wifi 初始化）
 */
esp_err_t wifi_ap_start(void);

#ifdef __cplusplus
}
#endif
