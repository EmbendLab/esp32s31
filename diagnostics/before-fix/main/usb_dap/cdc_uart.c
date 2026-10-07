/*
 * cdc_uart.c —— USB CDC-ACM 虚拟串口与物理 UART 双向透传实现
 *
 * 桥接任务运行在核 0（与 TinyUSB 同核），轮询两侧可读数据并搬运：
 *   - UART RX（目标 printf/bootloader 日志）-> tud_cdc_write + flush
 *   - CDC OUT（主机下发字节）             -> uart_write_bytes
 * 主机 SET_LINE_CODING（波特率/停止位/校验/数据位）由 TinyUSB 任务上下文
 * 回调，直接热更新 UART 参数，无需重开端口。
 */
#include "cdc_uart.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/uart.h"

#include "tusb.h"

#include "sdkconfig.h"

static const char *TAG = "cdc_uart";

#define BRIDGE_TASK_STACK     3072
#define BRIDGE_TASK_PRIO      5
#define UART_RX_BUF           1024
#define UART_TX_BUF           512
#define CHUNK                 256

/* ------------------------- CDC 线编码 -> UART 参数 ------------------------ */

static uart_stop_bits_t map_stop(uint8_t v)
{
    switch (v) {
    case 0:  return UART_STOP_BITS_1;
    case 1:  return UART_STOP_BITS_1_5;
    default: return UART_STOP_BITS_2;
    }
}

static uart_parity_t map_parity(uint8_t v)
{
    switch (v) {
    case 1:  return UART_PARITY_ODD;
    case 2:  return UART_PARITY_EVEN;
    default: return UART_PARITY_DISABLE;   /* 0/3(Mark)/4(Space) 按无校验处理 */
    }
}

static uart_word_length_t map_data_bits(uint8_t v)
{
    switch (v) {
    case 5:  return UART_DATA_5_BITS;
    case 6:  return UART_DATA_6_BITS;
    case 7:  return UART_DATA_7_BITS;
    default: return UART_DATA_8_BITS;
    }
}

/* ----------------------------- TinyUSB 回调 ------------------------------ */

/* 主机设置波特率/帧格式（运行在 TinyUSB device 任务上下文） */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    if (itf != 0 || coding == NULL) {
        return;
    }
    const uart_port_t port = CONFIG_DAP_UART_PORT;

    uart_set_baudrate(port, coding->bit_rate);
    uart_set_stop_bits(port, map_stop(coding->stop_bits));
    uart_set_parity(port, map_parity(coding->parity));
    uart_set_word_length(port, map_data_bits(coding->data_bits));

    ESP_LOGI(TAG, "line coding: %lu bps, data=%u stop=%u parity=%u",
             (unsigned long)coding->bit_rate, coding->data_bits,
             coding->stop_bits, coding->parity);
}

/* ------------------------------ 透传任务 --------------------------------- */

static void bridge_task(void *arg)
{
    (void)arg;
    static uint8_t buf[CHUNK];
    const uart_port_t port = CONFIG_DAP_UART_PORT;

    for (;;) {
        /* connected 在主机打开 COM 口（DTR 置位）后为真 */
        if (!tud_cdc_connected()) {
            uart_flush_input(port);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        bool worked = false;

        /* 目标 UART -> 主机：最多等 20ms 聚合一批，减少 USB 事务数 */
        int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            uint32_t off = 0;
            while ((int)off < n) {
                uint32_t w = tud_cdc_write(buf + off, (uint32_t)(n - off));
                if (w == 0) {
                    /* USB TX FIFO 满：等主机取走上一传输 */
                    tud_cdc_write_flush();
                    taskYIELD();
                    continue;
                }
                off += w;
            }
            tud_cdc_write_flush();
            worked = true;
        }

        /* 主机 -> 目标 UART：把 CDC RX FIFO 全部搬出 */
        if (tud_cdc_available() > 0) {
            uint32_t m = tud_cdc_read(buf, sizeof(buf));
            if (m > 0) {
                uart_write_bytes(port, buf, m);
                worked = true;
            }
        }

        if (!worked) {
            vTaskDelay(1);
        }
    }
}

/* ------------------------------ 公共入口 --------------------------------- */

esp_err_t cdc_uart_start(void)
{
    const uart_port_t port = CONFIG_DAP_UART_PORT;

    uart_config_t cfg = {
        .baud_rate  = CONFIG_DAP_UART_DEFAULT_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(port, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = uart_set_pin(port, CONFIG_DAP_UART_TX_PIN,
                       CONFIG_DAP_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = uart_driver_install(port, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(bridge_task, "cdc_uart",
                                            BRIDGE_TASK_STACK, NULL,
                                            BRIDGE_TASK_PRIO, NULL,
                                            CONFIG_DAP_USB_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create bridge task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CDC<->UART%d bridge up (TX=%d RX=%d @ %d bps)",
             port, CONFIG_DAP_UART_TX_PIN, CONFIG_DAP_UART_RX_PIN,
             CONFIG_DAP_UART_DEFAULT_BAUD);
    return ESP_OK;
}
