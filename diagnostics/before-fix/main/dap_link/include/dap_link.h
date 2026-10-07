/*
 * dap_link —— 核间 DAP 报文通道
 *
 * CPU0(USB/Wi-Fi 核) 与 CPU1(DAP 核) 之间通过一对 FreeRTOS message buffer
 * 传递变长 CMSIS-DAP 报文。单生产者单消费者，无锁、零拷贝拷贝次数最少：
 *
 *   host -> EP OUT -> [request buffer] -> DAP task -> SWD/JTAG
 *                                                  |
 *   host <- EP IN  <- [response buffer] <----------+
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建两个核间 message buffer（静态内存，开机调用一次）
 */
void dap_link_init(void);

/* ---------------- USB 核 (producer / consumer) ---------------- */

/** 投递一个来自主机的 DAP 请求包（满则丢弃最旧策略由上层保证不超队列深度） */
BaseType_t dap_link_post_request(const uint8_t *pkt, size_t len);

/** 等待并取出一个 DAP 响应包，返回实际长度；超时返回 0 */
size_t dap_link_get_response(uint8_t *buf, size_t buf_size, TickType_t wait);

/* ---------------- DAP 核 (consumer / producer) ---------------- */

/** 阻塞等待一个请求包 */
size_t dap_link_get_request(uint8_t *buf, size_t buf_size, TickType_t wait);

/** 投递响应包 */
BaseType_t dap_link_post_response(const uint8_t *pkt, size_t len);

#ifdef __cplusplus
}
#endif
