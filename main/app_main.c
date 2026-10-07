/*
 * app_main.c —— ESP32-S31 无线 DAPLink 调试桥入口
 *
 * 双核分工：
 *   CPU0 : Wi-Fi SoftAP + TinyUSB 协议栈 + bulk 端点 pump
 *   CPU1 : CMSIS-DAP 命令解释 + SWD/JTAG 物理时序（本文件 dap_proto_task）
 *
 * 启动顺序：
 *   NVS -> Wi-Fi AP -> 核间报文通道 -> 调试总线 -> 协议层状态 ->
 *   USB 设备（内部自建任务）-> DAP 协议任务（绑定核1）
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "wifi_ap.h"
#include "dap_link.h"
#include "dap_bus.h"
#include "cmsis_dap.h"
#include "usb_dap.h"
#include "cdc_uart.h"
#include "usbip_server.h"

static const char *TAG = "app_main";

#define DAP_PROTO_STACK   6144
#define DAP_PKT_CAP       DAP_LINK_PACKET_CAPACITY

/* 核1：从核间通道取请求，解释执行后把响应投递回核0 */
static void dap_proto_task(void *arg)
{
    (void)arg;
    static uint8_t req[DAP_PKT_CAP];
    static uint8_t resp[DAP_PKT_CAP];
    ESP_LOGI(TAG, "DAP execution core=%d", xPortGetCoreID());

    for (;;) {
        uint16_t packet_size = CONFIG_DAP_USB_BULK_EP_SIZE;
        dap_link_route_t route;
        size_t in_len = dap_link_get_routed_request(req, sizeof(req), &packet_size, &route);
        if (in_len == 0) {
            continue;
        }

        int64_t started = esp_timer_get_time();
        size_t out_len = cmsis_dap_process_ex(req, in_len, resp, sizeof(resp),
                                              packet_size);
        uint32_t execute_us = (uint32_t)(esp_timer_get_time() - started);
        if (out_len == 0) {
            continue;
        }

        /* 严格一问一答：响应通道满（核0未取走）时让出等待，不丢响应 */
        dap_link_post_routed_response(resp, out_len, &route, execute_us,
                                     cmsis_dap_last_wait_acks());
    }
}

static void nvs_init_once(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    nvs_init_once();

    /* 核0：Wi-Fi（已关 modem-sleep，射频常在线） */
    ESP_ERROR_CHECK(wifi_ap_start());

    /* 核间报文通道（必须在 USB / DAP 任务启动前就绪） */
    dap_link_init();

    /* 调试引脚与协议层状态 */
    dap_bus_init();
    cmsis_dap_init();

    /* 核0：USB CMSIS-DAP v2 bulk（内部创建 TinyUSB device 任务与 pump） */
    ESP_ERROR_CHECK(usb_dap_start());

    /* 核0：CDC-ACM 虚拟串口 <-> 物理 UART 透传（复合设备第二个功能） */
    ESP_ERROR_CHECK(cdc_uart_start());

    /* Wi-Fi USB/IP server.  It exposes the same CMSIS-DAP v2 interface as
       the local USB device; one remote client owns the debug link at a time. */
    ESP_ERROR_CHECK(usbip_server_start());

    /* 核1：DAP 协议与时序 */
    BaseType_t ok = xTaskCreatePinnedToCore(dap_proto_task, "dap_proto",
                                            DAP_PROTO_STACK, NULL,
                                            CONFIG_DAP_PROTO_TASK_PRIO, NULL,
                                            CONFIG_DAP_PROTO_TASK_CORE);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "wireless DAPLink bridge up: USB on core %d, DAP on core %d",
             CONFIG_DAP_USB_TASK_CORE, CONFIG_DAP_PROTO_TASK_CORE);
}
