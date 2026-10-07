#include "dap_link.h"
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "sdkconfig.h"

#define PKT_SIZE DAP_LINK_PACKET_CAPACITY
#define DEPTH CONFIG_DAP_USB_PACKET_COUNT
typedef struct {
    uint16_t len, packet_size;
    dap_link_route_t route;
    uint8_t data[PKT_SIZE];
} request_t;
typedef struct {
    uint16_t len;
    uint32_t session;
    dap_link_timing_t timing;
    uint8_t data[PKT_SIZE];
} response_t;
static QueueHandle_t s_req, s_local_resp, s_remote_resp;
static atomic_uint s_active_session;
static uint32_t s_next_session;
/* Lifetime descriptor: never closed while core 1 can signal it. */
static int s_response_fd = -1;

void dap_link_init(void)
{
    s_req = xQueueCreate(DEPTH, sizeof(request_t));
    s_local_resp = xQueueCreate(DEPTH, sizeof(response_t));
    s_remote_resp = xQueueCreate(DEPTH, sizeof(response_t));
    configASSERT(s_req && s_local_resp && s_remote_resp);
    esp_vfs_eventfd_config_t cfg = { .max_fds = 1 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&cfg));
    s_response_fd = eventfd(0, 0);
    configASSERT(s_response_fd >= 0);
}
int dap_link_response_event_fd(void) { return s_response_fd; }
uint32_t dap_link_wireless_begin(void)
{
    if (++s_next_session == 0) ++s_next_session;
    atomic_store(&s_active_session, s_next_session);
    return s_next_session;
}
void dap_link_wireless_end(void) { atomic_store(&s_active_session, 0); }

static BaseType_t post_request(uint32_t session, const uint8_t *pkt,
                               size_t len, uint16_t packet_size)
{
    if (!pkt || !len || len > packet_size || !packet_size || packet_size > PKT_SIZE) return 0;
    /* A wireless import owns the shared SWD protocol state until detach. */
    if (atomic_load(&s_active_session) != session) return 0;
    request_t req = { .len = len, .packet_size = packet_size,
                     .route = { .session = session, .queued_us = esp_timer_get_time() } };
    memcpy(req.data, pkt, len);
    return xQueueSend(s_req, &req, 0);
}
BaseType_t dap_link_post_request(const uint8_t *pkt, size_t len)
{
    return post_request(0, pkt, len, CONFIG_DAP_USB_BULK_EP_SIZE);
}
BaseType_t dap_link_post_request_ex(const uint8_t *pkt, size_t len, uint16_t packet_size)
{
    return post_request(0, pkt, len, packet_size);
}
BaseType_t dap_link_wireless_request(uint32_t session, const uint8_t *pkt, size_t len)
{
    return session ? post_request(session, pkt, len, CONFIG_DAP_USBIP_PACKET_SIZE) : 0;
}
size_t dap_link_get_routed_request(uint8_t *buf, size_t cap, uint16_t *packet_size,
                                  dap_link_route_t *route)
{
    request_t req;
    for (;;) {
        if (xQueueReceive(s_req, &req, portMAX_DELAY) != pdTRUE) continue;
        if (req.route.session && req.route.session != atomic_load(&s_active_session)) continue;
        configASSERT(req.len <= cap);
        memcpy(buf, req.data, req.len);
        *packet_size = req.packet_size;
        *route = req.route;
        route->queue_us = (uint32_t)(esp_timer_get_time() - req.route.queued_us);
        return req.len;
    }
}
BaseType_t dap_link_post_routed_response(const uint8_t *pkt, size_t len,
                                        const dap_link_route_t *route,
                                        uint32_t execute_us, uint32_t wait_acks)
{
    configASSERT(len <= PKT_SIZE);
    response_t response = { .len = len, .session = route->session,
        .timing = { .queue_us = route->queue_us, .execute_us = execute_us,
                    .wait_acks = wait_acks, .ready_us = esp_timer_get_time() } };
    memcpy(response.data, pkt, len);
    if (!route->session) return xQueueSend(s_local_resp, &response, portMAX_DELAY);
    while (route->session == atomic_load(&s_active_session)) {
        if (xQueueSend(s_remote_resp, &response, pdMS_TO_TICKS(10)) == pdTRUE) {
            uint64_t signal = 1;
            configASSERT(write(s_response_fd, &signal, sizeof(signal)) == sizeof(signal));
            return pdTRUE;
        }
    }
    return pdTRUE; /* Detached session: discard its response. */
}
size_t dap_link_get_response(uint8_t *buf, size_t cap, TickType_t wait)
{
    response_t response;
    if (xQueueReceive(s_local_resp, &response, wait) != pdTRUE) return 0;
    configASSERT(response.len <= cap);
    memcpy(buf, response.data, response.len);
    return response.len;
}
size_t dap_link_wireless_response(uint32_t session, uint8_t *buf,
                                  size_t cap, dap_link_timing_t *timing)
{
    response_t response;
    while (xQueueReceive(s_remote_resp, &response, 0) == pdTRUE) {
        if (response.session != session) continue;
        configASSERT(response.len <= cap);
        memcpy(buf, response.data, response.len);
        *timing = response.timing;
        return response.len;
    }
    return 0;
}
