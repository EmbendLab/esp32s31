/*
 * dap_bus —— 目标调试总线物理层（SWD + JTAG）
 *
 * 硬件模型（DAPLink 经典串联电阻架构，引脚见 Kconfig "DAPLink Configuration"）：
 *
 *   SWD 模式：
 *     SPI2 SCK   ───────────────► SWCLK            (调试口 pin4)
 *     GPIO MOSI ──[ ~100R ]─────► SWDIO            (调试口 pin2)
 *     GPIO SENSE◄─────────────── SWDIO（电阻目标侧采样点）
 *     TMS / TDI 脚高阻。
 *
 *     双向 SWDIO 的方向切换：写相位 MOSI 推挽驱动；读相位（turnaround /
 *     ACK / 数据读）MOSI 切高阻输入，串联电阻上无压降，SENSE 直接采到
 *     目标驱动的真实电平。切勿在读相位"输出高电平"——100R 对上 3.3V
 *     是数十 mA 的硬对抗。
 *
 *   JTAG 模式：
 *     GPIO ──► TCK(pin4)  TMS(pin2)  TDI(pin7)
 *     GPIO ◄── TDO(pin13)；nTRST / nRESET 独立。
 *     MOSI 脚高阻（TMS 直驱同一 pin2 节点）。
 *
 * 性能策略：
 *   - SWD/JTAG 寄存器帧为半双工、ACK 可能提前终止、turnaround 可变，
 *     全部走 IRAM 位级操作，2MHz 下每帧约 23us（320MHz CPU 余量充足）；
 *   - 纯输出的 SWJ 序列（线复位 / JTAG-to-SWD，无方向翻转、无 ACK）
 *     在 CONFIG_DAP_SPI_ACCEL=y 时走 SPI 硬件移位器；
 *   - CONFIG_DAP_SPI_ACCEL=n 时全部 bit-bang。
 *
 * 时序与帧格式逐位对齐 ARM 官方 DAPLink 参考（SW_DP.c / JTAG_DP.c）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAP_TPORT_NONE = 0,
    DAP_TPORT_SWD  = 1,
    DAP_TPORT_JTAG = 2,
} dap_tport_t;

/* SWD/JTAG-DP ACK（数值与 CMSIS-DAP DAP_Transfer 响应标志一致） */
typedef enum {
    DAP_ACK_OK      = 1,
    DAP_ACK_WAIT    = 2,
    DAP_ACK_FAULT   = 4,
    DAP_ACK_ERROR   = 7,   /* SWD 奇偶错 / JTAG 协议错 */
} dap_ack_t;

/* CMSIS-DAP SWJ 引脚位定义（DAP_SWJ_*） */
#define DAP_PIN_SWCLK_BIT   (1u << 0)
#define DAP_PIN_SWDIO_BIT   (1u << 1)
#define DAP_PIN_TDI_BIT     (1u << 2)
#define DAP_PIN_TDO_BIT     (1u << 3)
#define DAP_PIN_NTRST_BIT   (1u << 5)
#define DAP_PIN_NRESET_BIT  (1u << 7)

/* JTAG-DP IR 指令（ARM ADIv5） */
#define JTAG_IR_ABORT       0x08
#define JTAG_IR_DPACC       0x0A
#define JTAG_IR_APACC       0x0B
#define JTAG_IR_IDCODE      0x0E
#define JTAG_IR_BYPASS      0x0F

void dap_bus_init(void);
void dap_bus_connect(dap_tport_t transport);
void dap_bus_disconnect(void);
void dap_bus_set_clock(uint32_t hz);

/* ---- DAP_SWJ_Pins -------------------------------------------------------
 * 仅 value 中由 select 选中的位被驱动，立即返回引脚回读值（不等 wait）。
 * 返回字节按 DAP_SWJ_* 位定义组装，bit3 为 TDO/SWDIO 采样。 */
uint8_t dap_bus_swj_pins(uint8_t value, uint8_t select);

/* ---- DAP_SWJ_Sequence ---------------------------------------------------
 * count 1..256（0 视为 256），data 为 LSB-first（byte0 的 bit0 先发）。
 * SWD 模式打到 SWDIO，JTAG 模式打到 TMS（TDI 保持静态电平）。 */
void dap_bus_swj_sequence(uint32_t count, const uint8_t *data);

/* ---- SWD ----------------------------------------------------------------
 * request 物理编码（同 CMSIS-DAP transfer byte 的低 4 位）：
 *   bit0 APnDP  bit1 RnW  bit2 A2  bit3 A3
 * 写操作 *data 必须有效；post 读（丢弃返回值）时 data 可为 NULL。 */
void  dap_swd_configure(uint8_t turnaround_cycles, bool always_data_phase);
void  dap_swd_line_reset(void);
dap_ack_t dap_swd_transfer(uint8_t request, uint32_t *data);
void  dap_swd_write_abort(uint32_t data);

/* ---- JTAG ---------------------------------------------------------------
 * 链配置：count 个 TAP，ir_lengths 为各 TAP 的 IR 位宽（LSB 设备在前）。 */
bool  dap_jtag_configure(uint8_t count, const uint8_t *ir_lengths);
bool  dap_jtag_index_is_valid(uint8_t index);
void  dap_jtag_set_index(uint8_t index);

/* DAP_JTAG_Sequence 单段：info 低 6 位为 TCK 数（0=64），bit6 为整段固定
 * TMS 电平，bit7 置位时捕获 TDO；tdi 恒为 LSB-first，tdo 可为 NULL。 */
void  dap_jtag_sequence(uint8_t info, const uint8_t *tdi, uint8_t *tdo);

/* DPACC/APACC 单次扫描（IR 自动缓存切换），返回 ACK；读时回填 *data。 */
dap_ack_t dap_jtag_transfer(uint8_t request, uint32_t *data);
uint32_t  dap_jtag_read_idcode(void);
void  dap_jtag_write_abort(uint32_t data);

/* 每帧末尾插入的空闲时钟数（DAP_TransferConfigure idle_cycles） */
void  dap_bus_set_idle_cycles(uint8_t cycles);

#ifdef __cplusplus
}
#endif
