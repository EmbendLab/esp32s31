/*
 * dap_link —— 核间 DAP 报文通道
 *
 * CPU0(USB/Wi-Fi 核) 与 CPU1(DAP 核) 之间通过 FreeRTOS 队列传递报文。
 * 物理 USB 与无线响应各有独立队列，无线响应通过 eventfd 唤醒网络任务。
 *
 *   host -> EP OUT -> [request buffer] -> DAP task -> SWD/JTAG
 *                                                  |
 *   host <- EP IN  <- [response buffer] <----------+
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define DAP_LINK_PACKET_CAPACITY ((CONFIG_DAP_USBIP_PACKET_SIZE > CONFIG_DAP_USB_BULK_EP_SIZE) ? \
                                 CONFIG_DAP_USBIP_PACKET_SIZE : CONFIG_DAP_USB_BULK_EP_SIZE)

typedef struct {
    uint32_t session;
    int64_t queued_us;
    uint32_t queue_us;
} dap_link_route_t;
typedef struct {
    uint32_t queue_us, execute_us, wait_acks;
    int64_t ready_us;
} dap_link_timing_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建核间队列及响应事件（开机调用一次）
 */
void dap_link_init(void);
int dap_link_response_event_fd(void);
uint32_t dap_link_wireless_begin(void);
void dap_link_wireless_end(void);
BaseType_t dap_link_wireless_request(uint32_t session, const uint8_t *pkt, size_t len);
size_t dap_link_wireless_response(uint32_t session, uint8_t *buf,
                                  size_t cap, dap_link_timing_t *timing);
size_t dap_link_get_routed_request(uint8_t *buf, size_t cap, uint16_t *packet_size,
                                  dap_link_route_t *route);
BaseType_t dap_link_post_routed_response(const uint8_t *pkt, size_t len,
                                        const dap_link_route_t *route,
                                        uint32_t execute_us, uint32_t wait_acks);

/* ---------------- USB 核 (producer / consumer) ---------------- */

/** 投递物理 USB 请求；队列满或无线占用时返回 0，不丢弃已有报文。 */
BaseType_t dap_link_post_request(const uint8_t *pkt, size_t len);

/* Same queue operation with the packet size of the originating transport. */
BaseType_t dap_link_post_request_ex(const uint8_t *pkt, size_t len,
                                    uint16_t packet_size);

/** 等待并取出一个 DAP 响应包，返回实际长度；超时返回 0 */
size_t dap_link_get_response(uint8_t *buf, size_t buf_size, TickType_t wait);

#ifdef __cplusplus
}
#endif
