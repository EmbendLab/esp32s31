/*
 * usb_dap.c —— TinyUSB 安装与 CMSIS-DAP 端点 pump（WinUSB bulk + HID 双口）
 *
 * 核 0（默认，CONFIG_DAP_USB_TASK_CORE）上两个执行体：
 *   1. TinyUSB 内部 device 任务（tinyusb_driver_install 自动创建）
 *   2. 本模块的 pump 任务：
 *        host -> [vendor bulk OUT / HID OUT report] -> 核1 DAP 任务
 *        host <- [vendor bulk IN  / HID IN report ] <- 核1 DAP 任务
 *
 * bulk（CMSIS-DAP v2）没有长度前缀，包边界即 USB 传输边界；vendor 类内部
 * RX FIFO 会拼接连续 OUT 事务，因此用 tud_vendor_rx_cb 记录每笔传输长度，
 * pump 再按记录长度从 FIFO 精确取出整条命令。
 *
 * HID（CMSIS-DAP v1 兼容口，供 Keil/IAR 发现）为单个 512B vendor IN/OUT
 * report、无 report ID；OUT report 在 TinyUSB device 任务上下文回调中拷入
 * 队列。bulk 与 HID 共用同一套核间 DAP 通道，CMSIS-DAP 严格一问一答，pump
 * 串行处理并按“最后一条命令的来源接口”回送响应（HID 固定填满 512B）。
 */
#include "usb_dap.h"
#include "usb_descriptors.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include "dap_link.h"

static const char *TAG = "usb_dap";

#define PUMP_TASK_STACK       4096
#define PUMP_TASK_PRIO        6
#define PKT_CAP               CONFIG_DAP_USB_BULK_EP_SIZE
#define RX_LEN_QUEUE_DEPTH    CONFIG_DAP_USB_PACKET_COUNT
#define HID_QUEUE_DEPTH       2

/* 每个 OUT 传输（即一条 CMSIS-DAP 命令）的字节数 */
static QueueHandle_t s_rx_len_q;

/* HID OUT report：命令来源与一帧 report 一起入队 */
typedef struct {
    uint16_t len;
    uint8_t  data[DAP_HID_REPORT_SIZE];
} hid_pkt_t;
static QueueHandle_t s_hid_q;

/* 当前等待响应的命令来自哪个主机接口 */
typedef enum { SRC_BULK = 0, SRC_HID = 1 } dap_src_t;

/* ------------------------ TinyUSB vendor 类回调 ------------------------- */

/* 运行在 TinyUSB device 任务上下文（非 ISR），可使用普通队列 API */
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    if (idx != 0) {
        return;
    }
    /* vendor 类工作在 buffered/FIFO 模式时，回调的 bufsize 恒为 0（数据已进
     * 内部 RX FIFO）。CMSIS-DAP 严格一问一答：主机发完一条命令必等响应，
     * 因此回调时刻 FIFO 中的可读字节就是一条完整命令；据此通知 pump 取数。 */
    uint32_t len = tud_vendor_n_available(0);
    if (len == 0) {
        return;
    }
    if (len > PKT_CAP) {
        ESP_LOGW(TAG, "rx %lu > packet cap %d, clamping",
                 (unsigned long)len, PKT_CAP);
        len = PKT_CAP;
    }
    (void)xQueueSend(s_rx_len_q, &len, 0);
}

/* ------------------------- TinyUSB HID 类回调 --------------------------- */
/* interrupt OUT 与 EP0 SET_REPORT 两种输出路径最终都汇到 set_report_cb。 */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return g_dap_hid_report_desc;
}

/* 主机不会用 GET_REPORT 主动取 DAP 响应（响应走 IN report），返回 0 即可 */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

/* 运行在 TinyUSB device 任务上下文：缓冲归 HID 驱动所有且会被下一帧复用，
 * 必须立即拷出。CMSIS-DAP 一问一答，队列深度 2 足够。 */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    (void)report_type;
    if (instance != 0 || buffer == NULL || bufsize == 0) {
        return;
    }
    hid_pkt_t pkt;
    uint16_t n = (bufsize > DAP_HID_REPORT_SIZE) ? DAP_HID_REPORT_SIZE : bufsize;
    pkt.len = n;
    memcpy(pkt.data, buffer, n);
    if (xQueueSend(s_hid_q, &pkt, 0) != pdPASS) {
        ESP_LOGW(TAG, "hid report queue full, dropping %u bytes", n);
    }
}

/* ------------------------------ pump 任务 ------------------------------- */

static void pump_task(void *arg)
{
    (void)arg;
    uint8_t *rx_buf = pvPortMalloc(PKT_CAP);
    uint8_t *tx_buf = pvPortMalloc(PKT_CAP);
    configASSERT(rx_buf && tx_buf);

    bool busy = false;              /* 已投递请求、正在等待核1 响应 */
    dap_src_t src = SRC_BULK;       /* 在途请求来自哪个主机接口 */

    for (;;) {
        bool worked = false;

        /* bulk 口未配置时清掉残留长度记录，避免把陈旧 FIFO 误当新命令 */
        if (!tud_vendor_n_mounted(0)) {
            xQueueReset(s_rx_len_q);
        }

        /* ---- 无在途请求：从 bulk / HID 取一条新命令投给核1（严格一问一答） ---- */
        if (!busy) {
            const uint8_t *req = NULL;
            size_t req_len = 0;

            uint32_t len;
            if (tud_vendor_n_mounted(0) &&
                xQueueReceive(s_rx_len_q, &len, 0) == pdPASS) {
                if (len > PKT_CAP) {
                    ESP_LOGW(TAG, "oversized packet %lu, dropping",
                             (unsigned long)len);
                    tud_vendor_n_read_flush(0);
                } else {
                    uint32_t got = tud_vendor_n_read(0, rx_buf, len);
                    if (got != len) {
                        ESP_LOGE(TAG, "FIFO desync: want %lu got %lu",
                                 (unsigned long)len, (unsigned long)got);
                    }
                    req = rx_buf;
                    req_len = (size_t)got;
                    src = SRC_BULK;
                }
            } else {
                hid_pkt_t pkt;
                if (xQueueReceive(s_hid_q, &pkt, 0) == pdPASS) {
                    memcpy(rx_buf, pkt.data, pkt.len);
                    req = rx_buf;
                    req_len = pkt.len;
                    src = SRC_HID;
                }
            }

            if (req && req_len > 0) {
                /* 队列满说明核1异常，自旋让出并加超时保护 */
                uint32_t guard = 0;
                while (dap_link_post_request(req, req_len) == 0) {
                    if (++guard > 1000000) {
                        ESP_LOGE(TAG, "request queue stuck, dropping packet");
                        break;
                    }
                    taskYIELD();
                }
                busy = true;
                worked = true;
            }
        }

        /* ---- 核1 -> 主机：取响应并按来源接口回送 ---- */
        if (busy) {
            memset(tx_buf, 0, PKT_CAP);   /* HID report 需填满，尾部补 0 */
            size_t out_len = dap_link_get_response(tx_buf, PKT_CAP, 0);
            if (out_len > 0) {
                if (src == SRC_HID) {
                    /* IN report 长度必须严格等于描述符声明的 512B */
                    uint32_t guard = 0;
                    while (!tud_hid_n_ready(0)) {
                        if (++guard > 1000000) {
                            break;
                        }
                        taskYIELD();
                    }
                    if (!tud_hid_n_report(0, 0, tx_buf, DAP_HID_REPORT_SIZE)) {
                        ESP_LOGW(TAG, "hid IN report submit failed");
                    }
                } else {
                    size_t off = 0;
                    while (off < out_len) {
                        uint32_t w = tud_vendor_n_write(0, tx_buf + off,
                                                        (uint32_t)(out_len - off));
                        if (w == 0) {
                            taskYIELD();       /* IN FIFO 暂满，等其排空 */
                            continue;
                        }
                        off += (size_t)w;
                    }
                    tud_vendor_n_write_flush(0);
                }
                busy = false;
                worked = true;
            }
        }

        if (!worked) {
            vTaskDelay(1);
        }
    }
}

/* ------------------------------ 公共入口 -------------------------------- */

esp_err_t usb_dap_start(void)
{
    s_rx_len_q = xQueueCreate(RX_LEN_QUEUE_DEPTH, sizeof(uint32_t));
    if (!s_rx_len_q) {
        return ESP_ERR_NO_MEM;
    }

    /* 必须在 tinyusb_driver_install 前就绪：枚举后主机可能立刻发 HID report */
    s_hid_q = xQueueCreate(HID_QUEUE_DEPTH, sizeof(hid_pkt_t));
    if (!s_hid_q) {
        return ESP_ERR_NO_MEM;
    }

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.task = (tinyusb_task_config_t) {
        .size     = PUMP_TASK_STACK,
        .priority = TINYUSB_DEFAULT_TASK_PRIO,
        .xCoreID  = CONFIG_DAP_USB_TASK_CORE,
    };
    cfg.descriptor.device            = &g_dap_device_desc;
    cfg.descriptor.qualifier         = &g_dap_qualifier_desc;
    cfg.descriptor.full_speed_config = g_dap_fs_config_desc;
    cfg.descriptor.high_speed_config = g_dap_hs_config_desc;
    cfg.descriptor.string            = (const char **)g_dap_string_descs;
    cfg.descriptor.string_count      = DAP_STRID_COUNT;

    esp_err_t ret = tinyusb_driver_install(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(pump_task, "dap_pump",
                                            PUMP_TASK_STACK, NULL,
                                            PUMP_TASK_PRIO, NULL,
                                            CONFIG_DAP_USB_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create pump task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CMSIS-DAP started on core %d (bulk %dB + HID report %dB)",
             CONFIG_DAP_USB_TASK_CORE, CONFIG_DAP_USB_BULK_EP_SIZE,
             DAP_HID_REPORT_SIZE);
    return ESP_OK;
}
