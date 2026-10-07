/*
 * dap_bus —— 目标调试总线物理层实现（SWD + JTAG）
 *
 * 时序逐位对齐 ARM 官方 DAPLink 参考：
 *   source/daplink/cmsis-dap/SW_DP.c   （SWD 帧 / turnaround / data phase）
 *   source/daplink/cmsis-dap/JTAG_DP.c （TAP 状态机 / DPACC-APACC 扫描）
 *
 * 所有信号位操作走 GPIO 寄存器且置于 IRAM；SPI 硬件移位器仅加速
 * 纯输出的 SWJ 序列（线复位 / JTAG-to-SWD 选择）。
 */
#include "dap_bus.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_cpu.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "rom/gpio.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"

static const char *TAG = "dap_bus";

/* ------------------------------- 引脚定义 ------------------------------- */

#define PIN_SWCLK   CONFIG_DAP_PIN_SWCLK
#define PIN_MOSI    CONFIG_DAP_PIN_SWDIO_MOSI
#define PIN_SENSE   CONFIG_DAP_PIN_SWDIO_MISO
#define PIN_TMS     CONFIG_DAP_PIN_TMS
#define PIN_TDI     CONFIG_DAP_PIN_TDI
#define PIN_TDO     CONFIG_DAP_PIN_TDO
#define PIN_NRESET  CONFIG_DAP_PIN_NRESET
#define PIN_NTRST   CONFIG_DAP_PIN_NTRST

/* ------------------------------- 寄存器 GPIO ---------------------------- */

static inline IRAM_ATTR void gpio_pin_set(int pin, int v)
{
    if (pin < 0) return;
    if (pin < 32) {
        if (v) GPIO.out_w1ts.val = 1u << pin;
        else   GPIO.out_w1tc.val = 1u << pin;
    } else {
        if (v) GPIO.out1_w1ts.val = 1u << (pin - 32);
        else   GPIO.out1_w1tc.val = 1u << (pin - 32);
    }
}

static inline IRAM_ATTR int gpio_pin_get(int pin)
{
    if (pin < 0) return 0;
    if (pin < 32) return (GPIO.in.val  >> pin) & 1u;
    return (GPIO.in1.val >> (pin - 32)) & 1u;
}

static inline IRAM_ATTR void gpio_pin_oe(int pin, int en)
{
    if (pin < 0) return;
    if (pin < 32) {
        if (en) GPIO.enable_w1ts.val  = 1u << pin;
        else    GPIO.enable_w1tc.val  = 1u << pin;
    } else {
        if (en) GPIO.enable1_w1ts.val = 1u << (pin - 32);
        else    GPIO.enable1_w1tc.val = 1u << (pin - 32);
    }
}

/* ------------------------------- 时钟延时 ------------------------------- */

#define CPU_FREQ_HZ 320000000u
#define BIT_OVERHEAD_CYCLES 12u   /* 每半拍的取指/置位/采样开销估算 */

static uint32_t s_half_cycles;
static uint32_t s_clk_hz;

static void calc_bitbang_delay(uint32_t clk_hz)
{
    uint32_t half = CPU_FREQ_HZ / (2u * clk_hz);
    s_half_cycles = (half > BIT_OVERHEAD_CYCLES) ? (half - BIT_OVERHEAD_CYCLES) : 0;
}

static inline IRAM_ATTR void bb_delay(void)
{
    if (s_half_cycles) {
        uint32_t start = esp_cpu_get_cycle_count();
        while ((uint32_t)(esp_cpu_get_cycle_count() - start) < s_half_cycles) {
        }
    }
}

/* ------------------------------- 运行状态 ------------------------------- */

static dap_tport_t s_transport = DAP_TPORT_NONE;
static uint8_t  s_turnaround = 1;   /* SWD turnaround 周期数（1..4） */
static bool     s_data_phase;       /* SWD WAIT/FAULT 仍打满数据相位 */
static uint8_t  s_idle_cycles;      /* 帧末空闲时钟 */

static struct {
    uint8_t  count;
    uint8_t  index;
    uint8_t  ir_len[8];
    uint16_t ir_before[8];
    uint16_t ir_after[8];
    int      cur_ir;                /* 当前 IR，-1 = 未知 */
} s_jtag;

/* ------------------------------- SPI 加速层 ----------------------------- */

static spi_device_handle_t s_spi;
static bool s_spi_ready;

/* SWD 数据面引脚模式：仅实验性全帧 SPI 引擎使用（Kconfig 默认关闭）。
 * SCK 进入 SPI 会话后常驻，仅 MOSI 在"推挽 / 真高阻"间随相位切换。 */
#if CONFIG_DAP_SPI_FRAME
enum { BUS_GPIO = 0, BUS_SPI_DRV, BUS_SPI_HIZ };
static uint8_t s_bus_mode = BUS_GPIO;
static bool s_spi_acquired;
static uint32_t s_dbg_n;                 /* SPI 帧诊断计数，每次 connect 清零 */
#endif

static void spi_ensure(void)
{
    if (s_spi_ready) return;

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_SENSE,
        .sclk_io_num = PIN_SWCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_DAP_SPI_HOST, &buscfg, SPI_DMA_DISABLED));
    s_spi_ready = true;
    ESP_LOGI(TAG, "SPI%d ready (SCK=%d MOSI=%d MISO/SENSE=%d)",
             CONFIG_DAP_SPI_HOST, PIN_SWCLK, PIN_MOSI, PIN_SENSE);
}

static void spi_apply_clock(uint32_t hz)
{
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    spi_device_interface_config_t devcfg = {
        .mode = 0,                    /* 空闲低；上升沿采样，与 SWD 边沿约定一致 */
        .clock_speed_hz = hz,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_TXBIT_LSBFIRST | SPI_DEVICE_RXBIT_LSBFIRST,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_DAP_SPI_HOST, &devcfg, &s_spi));
}

/* 纯输出任意 bit 数移位（LSB-first），接收丢弃；无 DMA，单次 ≤512 bit */
static void spi_emit(const uint8_t *tx, uint32_t nbits)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = nbits;
    t.tx_buffer = tx;
    t.rx_buffer = NULL;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

/* --------------------------- 引脚矩阵/方向切换 -------------------------- */

/* 全部时钟/数据线交回 GPIO 直控（位操作模式） */
static void pins_enter_gpio(void)
{
#if CONFIG_DAP_SPI_FRAME
    /* 若 SWD SPI 会话正占用总线，先释放（set_clock 删建设备、SWJ 序列要位翻转） */
    if (s_spi_acquired) {
        spi_device_release_bus(s_spi);
        s_spi_acquired = false;
    }
    s_bus_mode = BUS_GPIO;
#endif
    rom_gpio_matrix_out(PIN_SWCLK, SIG_GPIO_OUT_IDX, false, false);
    rom_gpio_matrix_out(PIN_MOSI,  SIG_GPIO_OUT_IDX, false, false);
    rom_gpio_matrix_out(PIN_TMS,   SIG_GPIO_OUT_IDX, false, false);
    rom_gpio_matrix_out(PIN_TDI,   SIG_GPIO_OUT_IDX, false, false);
}

#if CONFIG_DAP_SPI_FRAME
/* SWD 纯输出序列临时把 SCK/MOSI 交 SPI；结束后恢复 GPIO。
 * 注意主机 1-bit 模式发送数据走 D(MOSI/D0)=SPI2_D_PAD_OUT_IDX；
 * Q 是 MISO/D1，主机模式下其输出信号未连接，接 Q 会导致 SWDIO 无数据
 * （JTAG->SWD 激活序列发不出去，目标不切 SWD，读 IDCODE 恒 ACK=111）。 */
static void pins_enter_spi(void)
{
    rom_gpio_matrix_out(PIN_SWCLK, SPI2_CK_PAD_OUT_IDX, false, false);
    rom_gpio_matrix_out(PIN_MOSI,  SPI2_D_PAD_OUT_IDX,  false, false);
}

/* ----------------------- SWD 全帧硬件 SPI 引擎 -------------------------- *
 * SCK 全程由 SPI2 移位器产生（mode 0：下降沿更新、上升沿采样，与 SWD 边沿
 * 约定一致）。一笔 46-bit 寄存器帧拆成 2（读）/3（写）个 polling 事务，
 * 在 turnaround 边界通过 GPIO 矩阵把 MOSI 切成"推挽驱动 / 真高阻"：
 *   - 请求 / 写数据相位：MOSI 经 ~100R 推挽；
 *   - turnaround / ACK / 读数据相位：MOSI 切 SIG_GPIO_OUT 且关 OE（高阻），
 *     SENSE(Q) 直接采目标电平（板上架构禁止读相位输出高电平硬对抗）。
 * 总线在 SWD 会话期间常驻 acquired，省掉每事务互斥量；SCK 常驻 SPI。 */

static inline uint32_t rx_bit(const uint8_t *p, uint32_t i)
{
    return ((uint32_t)p[i >> 3] >> (i & 7)) & 1u;
}

static void spi_mosi_drive(void)
{
    if (!s_spi_acquired) {
        spi_device_acquire_bus(s_spi, portMAX_DELAY);
        s_spi_acquired = true;
        pins_enter_spi();                       /* 懒重入：SCK + MOSI 进 SPI */
    } else if (s_bus_mode != BUS_SPI_DRV) {
        rom_gpio_matrix_out(PIN_MOSI, SPI2_D_PAD_OUT_IDX, false, false);
    }
    s_bus_mode = BUS_SPI_DRV;
}

static void spi_mosi_hiz(void)
{
    /* 数据位先置 1，再切软控 OE 关闭；串联电阻上无压降，线交给目标驱动 */
    gpio_pin_set(PIN_MOSI, 1);
    rom_gpio_matrix_out(PIN_MOSI, SIG_GPIO_OUT_IDX, false, false);
    gpio_pin_oe(PIN_MOSI, 0);
    s_bus_mode = BUS_SPI_HIZ;
}

static void spi_xfer(const void *tx, void *rx, uint32_t nbits)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = nbits;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void dbg_dump(uint8_t rw, uint32_t nbits,
                     const uint8_t *rx, uint32_t ack)
{
    if (s_dbg_n++ < 40) {
        ESP_LOGI(TAG, "SPI rw=%u nb=%lu ack=%lu rx=%02x %02x %02x %02x %02x %02x %02x %02x",
                 rw, (unsigned long)nbits, (unsigned long)ack,
                 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
    }
}

static dap_ack_t swd_spi_transfer(uint8_t request, uint32_t *data)
{
    uint8_t tx[16];
    uint8_t rx[16];
    uint8_t trn = s_turnaround;
    uint32_t ack;

    /* 相位 1：8 位请求 —— start=1, APnDP/RnW/A2/A3, parity, stop=0, park=1 */
    uint8_t req = (uint8_t)(0x81u | ((request & 0x0Fu) << 1) |
                            ((uint32_t)__builtin_parity(request & 0x0Fu) << 5));
    spi_mosi_drive();
    spi_xfer(&req, NULL, 8);

    if (request & 0x02u) {
        /* 读帧：trn + ACK(3) + 数据(32) + 奇偶(1) + trn (+idle)，全程高阻 */
        uint32_t nbits = (uint32_t)trn + 3u + 33u + trn + s_idle_cycles;
        memset(tx, 0xFF, sizeof(tx));
        memset(rx, 0,    sizeof(rx));
        spi_mosi_hiz();
        spi_xfer(tx, rx, nbits);

        ack = rx_bit(rx, trn) | (rx_bit(rx, trn + 1) << 1) |
              (rx_bit(rx, trn + 2) << 2);
        dbg_dump(1, nbits, rx, ack);
        if (ack != DAP_ACK_OK) {
            return (ack == DAP_ACK_WAIT || ack == DAP_ACK_FAULT)
                       ? (dap_ack_t)ack : DAP_ACK_ERROR;
        }
        uint32_t val = 0;
        for (uint32_t k = 0; k < 32; k++)
            val |= rx_bit(rx, trn + 3u + k) << k;
        if (__builtin_parity(val) != rx_bit(rx, trn + 35u))
            return DAP_ACK_ERROR;
        if (data) *data = val;
        return DAP_ACK_OK;
    }

    /* 写帧相位 2：trn + ACK(3) + trn，MOSI 高阻；ACK 在第 trn..trn+2 位 */
    uint32_t n2 = (uint32_t)trn + 3u + trn;
    memset(tx, 0xFF, sizeof(tx));
    memset(rx, 0,    sizeof(rx));
    spi_mosi_hiz();
    spi_xfer(tx, rx, n2);
    ack = rx_bit(rx, trn) | (rx_bit(rx, trn + 1) << 1) |
          (rx_bit(rx, trn + 2) << 2);
    dbg_dump(0, n2, rx, ack);

    /* 写帧相位 3：数据(32) + 奇偶(1) + idle(0) + 末尾 park(1)。
     * 即便 ACK=WAIT/FAULT 也打满时钟（等价 always_data_phase，目标忽略）。 */
    uint32_t val = data ? *data : 0u;
    memset(tx, 0, sizeof(tx));
    tx[0] = (uint8_t)(val);
    tx[1] = (uint8_t)(val >> 8);
    tx[2] = (uint8_t)(val >> 16);
    tx[3] = (uint8_t)(val >> 24);
    uint32_t n3 = 33u + s_idle_cycles + 1u;
    if (__builtin_parity(val))
        tx[4] |= 1u << 0;                                   /* 奇偶位于 bit32 */
    tx[(n3 - 1u) >> 3] |= 1u << ((n3 - 1u) & 7u);          /* 末位 park=1 */
    spi_mosi_drive();
    spi_xfer(tx, NULL, n3);

    if (ack == DAP_ACK_OK) return DAP_ACK_OK;
    return (ack == DAP_ACK_WAIT || ack == DAP_ACK_FAULT)
               ? (dap_ack_t)ack : DAP_ACK_ERROR;
}
#endif /* CONFIG_DAP_SPI_FRAME */

/* ------------------------------- 位级时序原语 --------------------------- */

/* SWD：输出 1 bit（下降沿更新，上升沿被目标采样） */
static inline IRAM_ATTR void swd_out_bit(int d)
{
    gpio_pin_set(PIN_MOSI, d);
    gpio_pin_set(PIN_SWCLK, 0);
    bb_delay();
    gpio_pin_set(PIN_SWCLK, 1);
    bb_delay();
}

/* SWD：输入 1 bit（下降沿后目标已驱动，低电平半周中部采样） */
static inline IRAM_ATTR int swd_in_bit(void)
{
    gpio_pin_set(PIN_SWCLK, 0);
    bb_delay();
    int d = gpio_pin_get(PIN_SENSE);
    gpio_pin_set(PIN_SWCLK, 1);
    bb_delay();
    return d;
}

/* SWD：仅一个时钟脉冲（turnaround / idle / 等待相位） */
static inline IRAM_ATTR void swd_clk(void)
{
    gpio_pin_set(PIN_SWCLK, 0);
    bb_delay();
    gpio_pin_set(PIN_SWCLK, 1);
    bb_delay();
}

static inline IRAM_ATTR void swd_drive_oe(int en)
{
    gpio_pin_oe(PIN_MOSI, en);
}

/* JTAG：一个 TCK 周期，可同时给 TMS/TDI 并可选采样 TDO */
static inline IRAM_ATTR void jtag_cycle(int tms, int tdi, int *tdo)
{
    gpio_pin_set(PIN_TMS, tms);
    gpio_pin_set(PIN_TDI, tdi);
    gpio_pin_set(PIN_SWCLK, 0);
    bb_delay();
    if (tdo) *tdo = gpio_pin_get(PIN_TDO);
    gpio_pin_set(PIN_SWCLK, 1);
    bb_delay();
}

/* ------------------------------- SWJ 通用 -------------------------------- */

uint8_t dap_bus_swj_pins(uint8_t value, uint8_t select)
{
    pins_enter_gpio();

    if (select & DAP_PIN_SWCLK_BIT) {
        gpio_pin_oe(PIN_SWCLK, 1);
        gpio_pin_set(PIN_SWCLK, (value >> 0) & 1);
    }
    if (select & DAP_PIN_SWDIO_BIT) {
        if (s_transport == DAP_TPORT_JTAG) {
            gpio_pin_oe(PIN_TMS, 1);
            gpio_pin_set(PIN_TMS, (value >> 1) & 1);
        } else {
            swd_drive_oe(1);
            gpio_pin_set(PIN_MOSI, (value >> 1) & 1);
        }
    }
    if ((select & DAP_PIN_TDI_BIT) && (s_transport == DAP_TPORT_JTAG)) {
        gpio_pin_oe(PIN_TDI, 1);
        gpio_pin_set(PIN_TDI, (value >> 2) & 1);
    }
    if (select & DAP_PIN_NTRST_BIT) {
        if (PIN_NTRST >= 0) gpio_set_level(PIN_NTRST, (value >> 5) & 1);
    }
    if (select & DAP_PIN_NRESET_BIT) {
        if (PIN_NRESET >= 0) {
            gpio_set_level(PIN_NRESET, (value >> 7) & 1);
        }
    }

    uint8_t r = 0;
    r |= (uint8_t)(gpio_pin_get(PIN_SWCLK) ? DAP_PIN_SWCLK_BIT : 0);
    if (s_transport == DAP_TPORT_JTAG) {
        r |= (uint8_t)(gpio_pin_get(PIN_TMS) ? DAP_PIN_SWDIO_BIT : 0);
        r |= (uint8_t)(gpio_pin_get(PIN_TDI) ? DAP_PIN_TDI_BIT   : 0);
        r |= (uint8_t)(gpio_pin_get(PIN_TDO) ? DAP_PIN_TDO_BIT   : 0);
    } else {
        int d = gpio_pin_get(PIN_SENSE);
        r |= d ? DAP_PIN_SWDIO_BIT : 0;
        r |= d ? DAP_PIN_TDO_BIT   : 0;   /* SWD 模式 TDO 位反映同一 SWDIO 节点 */
    }
    if (PIN_NTRST  >= 0 && gpio_get_level(PIN_NTRST))  r |= DAP_PIN_NTRST_BIT;
    if (PIN_NRESET >= 0 && gpio_get_level(PIN_NRESET)) r |= DAP_PIN_NRESET_BIT;
    return r;
}

void dap_bus_swj_sequence(uint32_t count, const uint8_t *data)
{
    if (count == 0) count = 256;
    if (count == 0 || data == NULL) return;

    /* SWD/JTAG 的序列与传输统一走 GPIO 位操作（swd_out_bit/swd_in_bit）。
     * SWD 读写传输本就是 GPIO，序列也走同一已验证路径可保证 SWDIO 输出使能
     * 与电平一致；SPI 仅作时钟/数据的替代时会引入矩阵切换与 OE 的不确定性，
     * 且对实际吞吐无增益（真正的 DAP 读写不经 SPI），故此处不再使用 SPI。 */
    pins_enter_gpio();
    if (s_transport == DAP_TPORT_JTAG) {
        gpio_pin_oe(PIN_TMS, 1);
        for (uint32_t i = 0; i < count; i++) {
            int d = (data[i >> 3] >> (i & 7)) & 1;   /* LSB-first */
            jtag_cycle(d, 1, NULL);                  /* TDI 保持空闲高 */
        }
        gpio_pin_set(PIN_SWCLK, 0);
    } else {
        swd_drive_oe(1);
        for (uint32_t i = 0; i < count; i++) {
            int d = (data[i >> 3] >> (i & 7)) & 1;
            swd_out_bit(d);
        }
        gpio_pin_set(PIN_SWCLK, 0);
    }
}

/* ------------------------------- SWD 物理层 ----------------------------- */

void dap_swd_configure(uint8_t turnaround_cycles, bool always_data_phase)
{
    s_turnaround = (turnaround_cycles == 0) ? 1 : turnaround_cycles;
    if (s_turnaround > 4) s_turnaround = 4;
    s_data_phase = always_data_phase;
}

/* 56 个 1 + JTAG-to-SWD 选择(0xE79E, LSB-first) + 56 个 1，共 128 bit */
static const uint8_t SWD_ACTIVATE[16] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x9E, 0xE7,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

void dap_swd_line_reset(void)
{
    dap_bus_swj_sequence(128, SWD_ACTIVATE);
}

void dap_swd_sequence(uint8_t info, const uint8_t *out, uint8_t *in)
{
    unsigned bits = info & 63u;
    if (!bits) bits = 64;
    pins_enter_gpio();
    bool capture = (info & 0x80u) != 0;
    swd_drive_oe(!capture);
    if (capture) memset(in, 0, (bits + 7) / 8);
    for (unsigned i = 0; i < bits; ++i) {
        if (capture) in[i >> 3] |= (uint8_t)(swd_in_bit() << (i & 7));
        else swd_out_bit((out[i >> 3] >> (i & 7)) & 1);
    }
    gpio_pin_set(PIN_SWCLK, 0);
}

/* GPIO 位翻转版 SWD 单帧（SPI 不可用 / idle 周期超限时的回退路径）。
 * request: bit0 APnDP bit1 RnW bit2 A2 bit3 */
static dap_ack_t gpio_swd_transfer(uint8_t request, uint32_t *data)
{
#if CONFIG_DAP_SPI_FRAME
    /* 实验 SPI 帧失败回退时，把 SCK/MOSI 矩阵从 SPI 拉回软控 */
    pins_enter_gpio();
#endif
    swd_drive_oe(1);
    uint32_t ack, bit, val, parity, n;

    /* 8 位请求：start, APnDP, RnW, A2, A3, parity, stop=0, park=1 */
    parity = 0;
    swd_out_bit(1);                 /* start */
    for (n = 0; n < 4; n++) {
        bit = (request >> n) & 1;
        swd_out_bit((int)bit);
        parity ^= bit;
    }
    swd_out_bit((int)(parity & 1));
    swd_out_bit(0);                 /* stop */
    swd_out_bit(1);                 /* park */

    /* turnaround：释放 SWDIO */
    swd_drive_oe(0);
    for (n = s_turnaround; n; n--) swd_clk();

    /* ACK，LSB-first */
    ack  = (uint32_t)swd_in_bit() << 0;
    ack |= (uint32_t)swd_in_bit() << 1;
    ack |= (uint32_t)swd_in_bit() << 2;

    if (ack == DAP_ACK_OK) {
        if (request & 0x02) {       /* 读 */
            val = 0;
            parity = 0;
            for (n = 32; n; n--) {
                bit = swd_in_bit();
                parity ^= bit;
                val >>= 1;
                val  |= bit << 31;
            }
            bit = swd_in_bit();     /* 数据奇偶 */
            if ((parity ^ bit) & 1) ack = DAP_ACK_ERROR;
            if (data) *data = val;
            for (n = s_turnaround; n; n--) swd_clk();
            swd_drive_oe(1);
        } else {                    /* 写 */
            for (n = s_turnaround; n; n--) swd_clk();
            swd_drive_oe(1);
            val = data ? *data : 0;
            parity = 0;
            for (n = 32; n; n--) {
                swd_out_bit((int)(val & 1));
                parity ^= val & 1;
                val >>= 1;
            }
            swd_out_bit((int)(parity & 1));
        }

        /* idle：主机驱动 0，结束后释放为高电平 */
        gpio_pin_set(PIN_MOSI, 0);
        for (n = s_idle_cycles; n; n--) swd_clk();
        gpio_pin_set(PIN_MOSI, 1);
        gpio_pin_set(PIN_SWCLK, 0);
        return (dap_ack_t)ack;
    }

    if ((ack == DAP_ACK_WAIT) || (ack == DAP_ACK_FAULT)) {
        if (s_data_phase && (request & 0x02)) {
            for (n = 32 + 1; n; n--) swd_clk();   /* dummy 读 32+parity */
        }
        for (n = s_turnaround; n; n--) swd_clk();
        swd_drive_oe(1);
        if (s_data_phase && !(request & 0x02)) {
            gpio_pin_set(PIN_MOSI, 0);
            for (n = 32 + 1; n; n--) swd_clk();   /* dummy 写 */
        }
        gpio_pin_set(PIN_MOSI, 1);
        gpio_pin_set(PIN_SWCLK, 0);
        return (dap_ack_t)ack;
    }

    /* 协议错误：backoff turnaround + 33 周期 */
    for (n = s_turnaround + 32 + 1; n; n--) swd_clk();
    swd_drive_oe(1);
    gpio_pin_set(PIN_MOSI, 1);
    gpio_pin_set(PIN_SWCLK, 0);
    return DAP_ACK_ERROR;
}

dap_ack_t dap_swd_transfer(uint8_t request, uint32_t *data)
{
#if CONFIG_DAP_SPI_FRAME
    /* 实验路径：整帧走 SPI2 移位器（读时序仍在调校，默认 Kconfig 关闭）。 */
    if (s_transport == DAP_TPORT_SWD && s_spi_ready && s_idle_cycles <= 80)
        return swd_spi_transfer(request, data);
#endif
    return gpio_swd_transfer(request, data);
}

void dap_swd_write_abort(uint32_t value)
{
    (void)dap_swd_transfer(0x00, &value);   /* DP ABORT：A[3:2]=0，写 */
}

/* ------------------------------- JTAG 物理层 ---------------------------- */

static void jtag_select_ir(int ir)
{
    if (s_jtag.count == 0 || s_jtag.index >= s_jtag.count) return;
    if (s_jtag.cur_ir == ir) return;
    pins_enter_gpio();
    gpio_pin_oe(PIN_TMS, 1);
    gpio_pin_oe(PIN_TDI, 1);

    int v = ir;
    jtag_cycle(1, 1, NULL);        /* Run-Test -> Select-DR-Scan */
    jtag_cycle(1, 1, NULL);        /* Select-DR-Scan -> Select-IR-Scan */
    jtag_cycle(0, 1, NULL);        /* Capture-IR */
    jtag_cycle(0, 1, NULL);        /* Shift-IR */

    for (uint16_t k = 0; k < s_jtag.ir_before[s_jtag.index]; k++)
        jtag_cycle(0, 1, NULL);

    uint32_t len = s_jtag.ir_len[s_jtag.index];
    for (uint32_t k = 0; k + 1 < len; k++) {
        jtag_cycle(0, v & 1, NULL);
        v >>= 1;
    }
    uint16_t after = s_jtag.ir_after[s_jtag.index];
    if (after) {
        jtag_cycle(0, v & 1, NULL);        /* 最后一个 IR 位 */
        for (uint16_t k = 0; k + 1 < after; k++)
            jtag_cycle(0, 1, NULL);        /* 后续 TAP bypass */
        jtag_cycle(1, 1, NULL);            /* Exit1-IR */
    } else {
        jtag_cycle(1, v & 1, NULL);        /* 末位与 Exit1-IR 同拍 */
    }
    jtag_cycle(1, 1, NULL);                /* Update-IR */
    jtag_cycle(0, 1, NULL);                /* Run-Test/Idle */
    s_jtag.cur_ir = ir;
}

dap_ack_t dap_jtag_transfer(uint8_t request, uint32_t *data)
{
    jtag_select_ir((request & 0x01) ? JTAG_IR_APACC : JTAG_IR_DPACC);
    pins_enter_gpio();
    gpio_pin_oe(PIN_TMS, 1);
    gpio_pin_oe(PIN_TDI, 1);

    uint32_t ack = DAP_ACK_ERROR, val, n;

    jtag_cycle(1, 1, NULL);        /* -> Select-DR-Scan */
    jtag_cycle(0, 1, NULL);        /* Capture-DR */
    jtag_cycle(0, 1, NULL);        /* Shift-DR */

    for (n = s_jtag.index; n; n--)
        jtag_cycle(0, 1, NULL);    /* 前面 TAP bypass */

    int b;
    jtag_cycle(0, (request >> 1) & 1, &b); ack  = (uint32_t)b << 1;  /* RnW / ACK.0 */
    jtag_cycle(0, (request >> 2) & 1, &b); ack |= (uint32_t)b << 0;  /* A2  / ACK.1 */
    jtag_cycle(0, (request >> 3) & 1, &b); ack |= (uint32_t)b << 2;  /* A3  / ACK.2 */

    uint32_t after = (uint32_t)s_jtag.count - s_jtag.index - 1u;

    if (ack == DAP_ACK_OK) {
        if (request & 0x02) {      /* 读 */
            val = 0;
            for (n = 31; n; n--) {
                jtag_cycle(0, 1, &b);
                val  |= (uint32_t)b << 31;
                val >>= 1;
            }
            if (after) {
                jtag_cycle(0, 1, &b);                 /* D31 */
                for (uint32_t k = 0; k + 1 < after; k++)
                    jtag_cycle(0, 1, NULL);
                jtag_cycle(1, 1, NULL);               /* bypass & Exit1-DR */
            } else {
                jtag_cycle(1, 1, &b);                 /* D31 与 Exit1-DR 同拍 */
            }
            val |= (uint32_t)b << 31;
            if (data) *data = val;
        } else {                  /* 写 */
            val = data ? *data : 0;
            for (n = 31; n; n--) {
                jtag_cycle(0, (int)(val & 1), NULL);
                val >>= 1;
            }
            if (after) {
                jtag_cycle(0, (int)(val & 1), NULL);
                for (uint32_t k = 0; k + 1 < after; k++)
                    jtag_cycle(0, 1, NULL);
                jtag_cycle(1, 1, NULL);
            } else {
                jtag_cycle(1, (int)(val & 1), NULL);
            }
        }
    } else {
        jtag_cycle(1, 1, NULL);    /* 非 OK：立即 Exit1-DR */
    }

    jtag_cycle(1, 1, NULL);        /* Update-DR */
    jtag_cycle(0, 1, NULL);        /* Run-Test/Idle */

    for (n = s_idle_cycles; n; n--)
        jtag_cycle(0, 1, NULL);
    gpio_pin_set(PIN_SWCLK, 0);

    return (dap_ack_t)ack;
}

uint32_t dap_jtag_read_idcode(void)
{
    jtag_select_ir(JTAG_IR_IDCODE);
    pins_enter_gpio();
    gpio_pin_oe(PIN_TMS, 1);
    gpio_pin_oe(PIN_TDI, 1);

    jtag_cycle(1, 1, NULL);
    jtag_cycle(0, 1, NULL);
    jtag_cycle(0, 1, NULL);
    for (uint32_t k = 0; k < s_jtag.index; k++)
        jtag_cycle(0, 1, NULL);

    uint32_t val = 0, n;
    int b;
    for (n = 31; n; n--) {
        jtag_cycle(0, 1, &b);
        val  |= (uint32_t)b << 31;
        val >>= 1;
    }
    jtag_cycle(1, 1, &b);          /* D31 与 Exit1-DR 同拍 */
    val |= (uint32_t)b << 31;
    jtag_cycle(1, 1, NULL);        /* Update-DR */
    jtag_cycle(0, 1, NULL);        /* Run-Test/Idle */
    gpio_pin_set(PIN_SWCLK, 0);
    return val;
}

void dap_jtag_write_abort(uint32_t value)
{
    jtag_select_ir(JTAG_IR_ABORT);
    pins_enter_gpio();
    gpio_pin_oe(PIN_TMS, 1);
    gpio_pin_oe(PIN_TDI, 1);

    jtag_cycle(1, 0, NULL);
    jtag_cycle(0, 0, NULL);
    jtag_cycle(0, 0, NULL);
    for (uint32_t k = 0; k < s_jtag.index; k++)
        jtag_cycle(0, 0, NULL);

    jtag_cycle(0, 0, NULL);        /* RnW=0 */
    jtag_cycle(0, 0, NULL);        /* A2=0  */
    jtag_cycle(0, 0, NULL);        /* A3=0  */

    uint32_t after = (uint32_t)s_jtag.count - s_jtag.index - 1u;
    uint32_t n;
    for (n = 31; n; n--) {
        jtag_cycle(0, (int)(value & 1), NULL);
        value >>= 1;
    }
    if (after) {
        jtag_cycle(0, (int)(value & 1), NULL);
        for (uint32_t k = 0; k + 1 < after; k++)
            jtag_cycle(0, 1, NULL);
        jtag_cycle(1, 1, NULL);
    } else {
        jtag_cycle(1, (int)(value & 1), NULL);
    }
    jtag_cycle(1, 1, NULL);        /* Update-DR */
    jtag_cycle(0, 1, NULL);        /* Run-Test/Idle */
    gpio_pin_set(PIN_SWCLK, 0);
}

void dap_jtag_sequence(uint8_t info, const uint8_t *tdi, uint8_t *tdo)
{
    uint32_t n = info & 0x3Fu;
    if (n == 0) n = 64;
    int tms = (info & 0x40u) ? 1 : 0;
    bool capture = (info & 0x80u) != 0;

    pins_enter_gpio();
    gpio_pin_oe(PIN_TMS, 1);
    gpio_pin_oe(PIN_TDI, 1);
    gpio_pin_set(PIN_TMS, tms);

    while (n) {
        uint8_t iv = *tdi++;
        uint8_t ov = 0;
        uint32_t chunk = (n > 8) ? 8 : n;
        for (uint32_t k = 0; k < chunk; k++) {
            int inb;
            jtag_cycle(tms, iv & 1, &inb);
            iv >>= 1;
            ov >>= 1;
            ov |= (uint8_t)(inb << 7);
        }
        ov >>= (8 - chunk);        /* 不足 8 位时低位对齐 */
        if (capture && tdo) *tdo++ = ov;
        n -= chunk;
    }
    gpio_pin_set(PIN_SWCLK, 0);

    /* 主机可能已用该序列改变 TAP 状态，IR 缓存失效 */
    s_jtag.cur_ir = -1;
}

bool dap_jtag_configure(uint8_t count, const uint8_t *ir_lengths)
{
    if (count > 8) return false;
    s_jtag.count = count;
    s_jtag.index = 0;
    uint16_t bits = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (ir_lengths[i] == 0) return false;
        s_jtag.ir_len[i] = ir_lengths[i];
        s_jtag.ir_before[i] = bits;
        bits = (uint16_t)(bits + ir_lengths[i]);
    }
    for (uint8_t i = 0; i < count; i++) {
        bits = (uint16_t)(bits - s_jtag.ir_len[i]);
        s_jtag.ir_after[i] = bits;
    }
    return true;
}

bool dap_jtag_index_is_valid(uint8_t index)
{
    return (s_jtag.count > 0) && (index < s_jtag.count);
}

void dap_jtag_set_index(uint8_t index)
{
    if (index < s_jtag.count) s_jtag.index = index;
}

/* ------------------------------- 生命周期 ------------------------------- */

void dap_bus_init(void)
{
    s_clk_hz = CONFIG_DAP_SWD_DEFAULT_CLOCK_HZ;
    calc_bitbang_delay(s_clk_hz);
    s_jtag.cur_ir = -1;

    uint64_t out_mask = (1ULL << PIN_SWCLK) | (1ULL << PIN_MOSI) |
                        (1ULL << PIN_TMS)   | (1ULL << PIN_TDI);
    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        /* INPUT_OUTPUT：保持推挽输出，同时打开 pad 输入缓冲(FUN_IE)，
         * 使 SWJ_Pins 能回读 SWCLK/MOSI 真实引脚电平（纯 OUTPUT 模式下
         * gpio_get_level 恒为 0，无法用于诊断引脚是否被外部钉低）。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << PIN_SENSE) | (1ULL << PIN_TDO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    pins_enter_gpio();

    /* 上电安全态：时钟低、数据脚高阻，避免误驱动目标 */
    gpio_pin_set(PIN_SWCLK, 0);
    gpio_pin_set(PIN_MOSI, 1);
    gpio_pin_set(PIN_TMS, 1);
    gpio_pin_set(PIN_TDI, 1);
    gpio_pin_oe(PIN_SWCLK, 0);
    gpio_pin_oe(PIN_MOSI, 0);
    gpio_pin_oe(PIN_TMS, 0);
    gpio_pin_oe(PIN_TDI, 0);

    if (PIN_NRESET >= 0) {
        /* INPUT_OUTPUT：推挽驱动 nRESET 的同时打开输入缓冲，使 SWJ_Pins
         * 能回读复位线真实电平（纯 OUTPUT 下 gpio_get_level 恒为 0，
         * 无法判断线是否被外部钉低 / 复位是否真的送达目标）。 */
        gpio_set_direction(PIN_NRESET, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_pull_mode(PIN_NRESET, GPIO_PULLUP_ONLY);
        gpio_set_level(PIN_NRESET, 1);
    }
    if (PIN_NTRST >= 0) {
        gpio_set_direction(PIN_NTRST, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_pull_mode(PIN_NTRST, GPIO_PULLUP_ONLY);
        gpio_set_level(PIN_NTRST, 1);
    }

    ESP_LOGI(TAG, "init SWDIO=%d sense=%d TMS=%d TDI=%d TDO=%d nRST=%d nTRST=%d",
             PIN_MOSI, PIN_SENSE, PIN_TMS, PIN_TDI, PIN_TDO, PIN_NRESET, PIN_NTRST);
}

void dap_bus_connect(dap_tport_t transport)
{
    s_transport = transport;
    pins_enter_gpio();

    if (transport == DAP_TPORT_SWD) {
        spi_ensure();
        spi_apply_clock(s_clk_hz);
        /* MISO/SENSE 接 Q(D1) 输入（MOSI 才是 D）；SWD 读位走 GPIO，
         * 此路由仅让 SPI 输入侧信号正确，避免误用 D 输入。 */
        rom_gpio_matrix_in(PIN_SENSE, SPI2_Q_PAD_IN_IDX, false);

        gpio_pin_oe(PIN_TMS, 0);
        gpio_pin_oe(PIN_TDI, 0);
        if (PIN_NTRST >= 0) gpio_set_level(PIN_NTRST, 1);

        /* SWCLK 低、SWDIO 高，发线复位 + JTAG-to-SWD 选择序列 */
        gpio_pin_oe(PIN_SWCLK, 1);
        gpio_pin_set(PIN_SWCLK, 0);
        gpio_pin_set(PIN_MOSI, 1);
        s_turnaround = 1;
        s_data_phase = false;
#if CONFIG_DAP_SPI_FRAME
        s_dbg_n = 0;
#endif
        dap_swd_line_reset();
        ESP_LOGI(TAG, "SWD connected");
    } else if (transport == DAP_TPORT_JTAG) {
        /* TCK/TMS/TDI 全 GPIO；MOSI 高阻，TDO 直读 */
        gpio_pin_oe(PIN_MOSI, 0);
        gpio_pin_set(PIN_MOSI, 1);
        gpio_pin_oe(PIN_SWCLK, 1);
        gpio_pin_oe(PIN_TMS, 1);
        gpio_pin_oe(PIN_TDI, 1);
        gpio_pin_set(PIN_TDI, 1);

        /* nTRST 脉冲 + TMS=1 至少 5 个 TCK 进 Test-Logic-Reset */
        if (PIN_NTRST >= 0) gpio_set_level(PIN_NTRST, 0);
        gpio_pin_set(PIN_TMS, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        for (int i = 0; i < 6; i++) {
            gpio_pin_set(PIN_SWCLK, 0);
            bb_delay();
            gpio_pin_set(PIN_SWCLK, 1);
            bb_delay();
        }
        if (PIN_NTRST >= 0) {
            gpio_set_level(PIN_NTRST, 1);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        /* TMS=0 进入 Run-Test/Idle；TLR 后所有 TAP 的 IR=IDCODE */
        gpio_pin_set(PIN_TMS, 0);
        gpio_pin_set(PIN_SWCLK, 0);
        bb_delay();
        gpio_pin_set(PIN_SWCLK, 1);
        bb_delay();
        gpio_pin_set(PIN_SWCLK, 0);
        s_jtag.cur_ir = JTAG_IR_IDCODE;
        ESP_LOGI(TAG, "JTAG connected (%u TAP)", s_jtag.count);
    }
}

void dap_bus_disconnect(void)
{
    pins_enter_gpio();
    gpio_pin_oe(PIN_SWCLK, 0);
    gpio_pin_oe(PIN_MOSI, 0);
    gpio_pin_oe(PIN_TMS, 0);
    gpio_pin_oe(PIN_TDI, 0);
    gpio_pin_set(PIN_SWCLK, 0);
    gpio_pin_set(PIN_MOSI, 1);
    if (PIN_NRESET >= 0) gpio_set_level(PIN_NRESET, 1);
    if (PIN_NTRST >= 0) gpio_set_level(PIN_NTRST, 1);
    s_transport = DAP_TPORT_NONE;
}

void dap_bus_set_clock(uint32_t hz)
{
    if (hz == 0) hz = CONFIG_DAP_SWD_DEFAULT_CLOCK_HZ;
    s_clk_hz = hz;
    calc_bitbang_delay(hz);
    if (s_spi_ready) {
#if CONFIG_DAP_SPI_FRAME
        /* spi_apply_clock 会删建设备：先退出常驻 SPI 会话，下一帧懒重入 */
        pins_enter_gpio();
#endif
        spi_apply_clock(hz);
    }
    ESP_LOGI(TAG, "clock %lu Hz", (unsigned long)hz);
}

void dap_bus_set_idle_cycles(uint8_t cycles)
{
    s_idle_cycles = cycles;
}
