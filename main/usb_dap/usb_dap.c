/* CMSIS-DAP bulk + HID: preserve OUT boundaries and response ownership. */
#include "usb_dap.h"
#include "usb_descriptors.h"
#include "cmsis_dap.h"
#include "dap_link.h"
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static const char *TAG = "usb_dap";
#define PUMP_TASK_STACK 4096
#define PUMP_TASK_PRIO 6
#define PKT_CAP CONFIG_DAP_USB_BULK_EP_SIZE

typedef enum { SRC_BULK, SRC_HID } dap_src_t;
typedef struct {
    dap_src_t src;
    unsigned session;
    uint16_t len;
    uint8_t data[PKT_CAP];
} dap_packet_t;

/* Each enumerated interface gets its full advertised packet window. */
static QueueHandle_t s_packets;
static atomic_uint s_session;
static atomic_bool s_bulk_tx_done = true;
static TaskHandle_t s_pump_task;

/* TinyUSB 的收发回调运行在 tinyusb 任务上下文，直接用普通通知即可。
 * pump 靠它在响应发送完成的瞬间被唤醒，不做周期性轮询。 */
static void pump_kick(void)
{
    if (s_pump_task) xTaskNotifyGive(s_pump_task);
}

static void enqueue(dap_packet_t *pkt)
{
    if (!pkt->len) return;
    if (pkt->data[0] == 0x07) {
        cmsis_dap_abort(); /* DAP_TransferAbort has no response. */
        return;
    }
    pkt->session = atomic_load(&s_session);
    if (xQueueSend(s_packets, pkt, 0) != pdPASS) {
        ESP_LOGE(TAG, "host exceeded DAP packet window");
    }
}

static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id != TINYUSB_EVENT_ATTACHED && event->id != TINYUSB_EVENT_DETACHED) return;
    atomic_fetch_add(&s_session, 1);
    if (event->id == TINYUSB_EVENT_DETACHED) cmsis_dap_abort();
    xQueueReset(s_packets);
    atomic_store(&s_bulk_tx_done, true);
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "USB %s: DAP packet %d B",
                 tud_speed_get() == TUSB_SPEED_HIGH ? "HIGH 480Mbps" : "FULL 12Mbps",
                 PKT_CAP);
    }
}

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    if (idx != 0) return;
    dap_packet_t pkt = { .src = SRC_BULK };
    /* Only this callback consumes RX FIFO, before the next completion.
     * Thus it contains exactly this transfer, never accumulated requests. */
    pkt.len = (uint16_t)tud_vendor_n_read(0, pkt.data, sizeof(pkt.data));
    enqueue(&pkt);
}

void tud_vendor_tx_cb(uint8_t idx, uint32_t sent_bytes)
{
    /* Ignore the optional terminating ZLP of a preceding full-size reply. */
    if (idx == 0 && sent_bytes) {
        atomic_store(&s_bulk_tx_done, true);
        pump_kick();
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return g_dap_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                             hid_report_type_t report_type,
                             uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                          hid_report_type_t report_type,
                          const uint8_t *buffer, uint16_t bufsize)
{
    (void)report_type;
    if (instance || report_id || !buffer || !bufsize || bufsize > PKT_CAP) return;
    dap_packet_t pkt = { .src = SRC_HID, .len = bufsize };
    memcpy(pkt.data, buffer, bufsize);
    enqueue(&pkt);
}

/* HID IN 报告被主机取走后触发，pump 借此立刻发下一条 */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report,
                               uint16_t len)
{
    (void)report; (void)len;
    if (instance == 0) pump_kick();
}

/* 阻塞等待当前响应真正发完：bulk 等 IN 传输完成，HID 等主机取走报告。
 * 唤醒由 USB 回调的任务通知给出，不做任何 tick 粒度轮询。 */
static void pump_wait_send_done(dap_src_t src)
{
    while (src == SRC_HID ? !tud_hid_n_ready(0)
                          : !atomic_load(&s_bulk_tx_done)) {
        xTaskNotifyWait(0, 0xFFFFFFFFu, NULL, portMAX_DELAY);
    }
}

static void pump_task(void *arg)
{
    (void)arg;
    s_pump_task = xTaskGetCurrentTaskHandle();
    dap_packet_t *pkt = pvPortMalloc(sizeof(*pkt));
    uint8_t *response = pvPortMalloc(PKT_CAP);
    configASSERT(pkt && response);

    for (;;) {
        /* 1. 等待主机请求：发送方是 USB 回调，阻塞接收可即时唤醒 */
        xQueueReceive(s_packets, pkt, portMAX_DELAY);
        if (pkt->session != atomic_load(&s_session) || !tud_mounted()) {
            continue;   /* 重枚举前的陈旧包，直接丢弃 */
        }

        /* 2. 投递给核1，随即阻塞等响应。无线占用时须阻塞让出 CPU0。 */
        bool posted = false;
        while (!(posted = dap_link_post_request(pkt->data, pkt->len) != 0)) {
            if (pkt->session != atomic_load(&s_session) || !tud_mounted()) break;
            vTaskDelay(1);
        }
        if (!posted) continue;
        size_t response_len = 0;
        memset(response, 0, PKT_CAP);   /* HID 始终发满 512B，短响应补零 */
        while (response_len == 0) {
            response_len = dap_link_get_response(response, PKT_CAP, portMAX_DELAY);
        }
        if (pkt->session != atomic_load(&s_session) || !tud_mounted()) {
            continue;   /* 等待期间发生过拔插：丢弃陈旧响应 */
        }

        /* 3. 上一条响应发完后再填 TX FIFO，防止两条短 bulk 响应粘连 */
        pump_wait_send_done(pkt->src);

        if (pkt->src == SRC_HID) {
            while (!tud_hid_n_ready(0) ||
                   !tud_hid_n_report(0, 0, response, DAP_HID_REPORT_SIZE)) {
                xTaskNotifyWait(0, 0xFFFFFFFFu, NULL, portMAX_DELAY);
            }
        } else {
            atomic_store(&s_bulk_tx_done, false);
            uint32_t n = tud_vendor_n_write(0, response, response_len);
            configASSERT(n == response_len);
            tud_vendor_n_write_flush(0);
        }

        /* 4. 本条发完再进入下一轮（response 缓冲才可安全复用） */
        pump_wait_send_done(pkt->src);
    }
}

esp_err_t usb_dap_start(void)
{
    s_packets = xQueueCreate(2 * CONFIG_DAP_USB_PACKET_COUNT, sizeof(dap_packet_t));
    if (!s_packets) return ESP_ERR_NO_MEM;
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.event_cb = usb_event;
    cfg.task = (tinyusb_task_config_t) {
        .size = PUMP_TASK_STACK,
        .priority = TINYUSB_DEFAULT_TASK_PRIO,
        .xCoreID = CONFIG_DAP_USB_TASK_CORE,
    };
    cfg.descriptor.device = &g_dap_device_desc;
    cfg.descriptor.qualifier = &g_dap_qualifier_desc;
    cfg.descriptor.full_speed_config = g_dap_fs_config_desc;
    cfg.descriptor.high_speed_config = g_dap_hs_config_desc;
    cfg.descriptor.string = (const char **)g_dap_string_descs;
    cfg.descriptor.string_count = DAP_STRID_COUNT;
    esp_err_t ret = tinyusb_driver_install(&cfg);
    if (ret != ESP_OK) return ret;
    if (xTaskCreatePinnedToCore(pump_task, "dap_pump", PUMP_TASK_STACK, NULL,
                               PUMP_TASK_PRIO, NULL, CONFIG_DAP_USB_TASK_CORE) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CMSIS-DAP ready: %d-byte packets, %d queued per interface",
             PKT_CAP, CONFIG_DAP_USB_PACKET_COUNT);
    return ESP_OK;
}
