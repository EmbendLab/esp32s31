/*
 * cmsis_dap —— CMSIS-DAP v2 命令解释层
 *
 * 输入/输出均为 v2 Bulk 报文：首字节即 DAP 命令（无 HID 报告 ID），
 * 响应首字节回显命令号。命令编号与 ARM CMSIS-DAP v2 / DAPLink master
 * 及 OpenOCD >= 0.12 一致（Transfer=0x05、SWJ_Pins=0x10 …）。
 *
 * 本层不含任何 USB / FreeRTOS 细节，纯同步、可单测；由 DAP 核任务
 * 从核间通道取请求、调用 cmsis_dap_process()、把响应投递回 USB 核。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 上电初始化内部状态（总线物理层另行 init/connect） */
void cmsis_dap_init(void);

/**
 * @brief 处理一条 CMSIS-DAP v2 Bulk 命令
 *
 * @param req     请求报文（首字节为命令号）
 * @param in_len  请求长度（字节）
 * @param resp    响应缓冲，容量至少为 DAP 包大小
 * @param cap     响应缓冲容量
 * @return        响应长度（0 表示不应答）
 */
size_t cmsis_dap_process(const uint8_t *req, size_t in_len,
                         uint8_t *resp, size_t cap);

#ifdef __cplusplus
}
#endif
