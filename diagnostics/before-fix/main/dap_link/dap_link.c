#include "dap_link.h"

#include "freertos/message_buffer.h"

/* 每个方向排队 CONFIG_DAP_USB_PACKET_COUNT 个最大包；
   message buffer 每条消息额外 4B 长度前缀。 */
#define DAP_PKT_SIZE        CONFIG_DAP_USB_BULK_EP_SIZE
#define DAP_PKT_DEPTH       CONFIG_DAP_USB_PACKET_COUNT
#define DAP_BUF_BYTES       ((DAP_PKT_SIZE + 4) * DAP_PKT_DEPTH)

static StaticMessageBuffer_t s_req_ctrl;
static StaticMessageBuffer_t s_resp_ctrl;
static uint8_t s_req_storage[DAP_BUF_BYTES];
static uint8_t s_resp_storage[DAP_BUF_BYTES];
static MessageBufferHandle_t s_req;
static MessageBufferHandle_t s_resp;

void dap_link_init(void)
{
    s_req = xMessageBufferCreateStatic(sizeof(s_req_storage), s_req_storage, &s_req_ctrl);
    s_resp = xMessageBufferCreateStatic(sizeof(s_resp_storage), s_resp_storage, &s_resp_ctrl);
}

BaseType_t dap_link_post_request(const uint8_t *pkt, size_t len)
{
    return xMessageBufferSend(s_req, pkt, len, 0);
}

size_t dap_link_get_response(uint8_t *buf, size_t buf_size, TickType_t wait)
{
    return xMessageBufferReceive(s_resp, buf, buf_size, wait);
}

size_t dap_link_get_request(uint8_t *buf, size_t buf_size, TickType_t wait)
{
    return xMessageBufferReceive(s_req, buf, buf_size, wait);
}

BaseType_t dap_link_post_response(const uint8_t *pkt, size_t len)
{
    return xMessageBufferSend(s_resp, pkt, len, 0);
}
