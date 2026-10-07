/*
 * cmsis_dap.c —— CMSIS-DAP v2 命令解释层实现
 *
 * 处理逻辑对照 ARM 官方 DAPLink DAP.c（CMSIS-DAP v2 命令编号），
 * SWD/JTAG DAP_Transfer 的 posted-read 流水线、WAIT 重试、match
 * 重试与 DAPLink 完全同构；底层时序由 dap_bus 的物理原语完成。
 */
#include "cmsis_dap.h"

#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dap_bus.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

/* ------------------------------- 协议常量 ------------------------------- */

#define DAP_OK              0x00
#define DAP_ERROR           0xFF

/* DAP_Transfer request byte 位定义（与物理帧低 4 位相同） */
#define F_APnDP             (1u << 0)
#define F_RnW               (1u << 1)
#define F_A2                (1u << 2)
#define F_A3                (1u << 3)
#define F_MATCH_VALUE       (1u << 4)
#define F_MATCH_MASK        (1u << 5)

/* DAP_Transfer response flags */
#define RSP_OK              (1u << 0)
#define RSP_WAIT            (1u << 1)
#define RSP_FAULT           (1u << 2)
#define RSP_PROTO_ERROR     (1u << 3)
#define RSP_MISMATCH        (1u << 4)

#define DP_RDBUFF_READ      (F_RnW | F_A2 | F_A3)   /* 0x0E */

/* v2 命令号 */
#define CMD_INFO                  0x00
#define CMD_HOST_STATUS           0x01
#define CMD_CONNECT               0x02
#define CMD_DISCONNECT            0x03
#define CMD_TRANSFER_CONFIGURE    0x04
#define CMD_TRANSFER              0x05
#define CMD_TRANSFER_BLOCK        0x06
#define CMD_TRANSFER_ABORT        0x07
#define CMD_WRITE_ABORT           0x08
#define CMD_DELAY                 0x09
#define CMD_RESET_TARGET          0x0A
#define CMD_SWJ_PINS              0x10
#define CMD_SWJ_CLOCK             0x11
#define CMD_SWJ_SEQUENCE          0x12
#define CMD_SWD_CONFIGURE         0x13
#define CMD_JTAG_SEQUENCE         0x14
#define CMD_JTAG_CONFIGURE        0x15
#define CMD_JTAG_IDCODE           0x16
#define CMD_SWD_SEQUENCE          0x1D
#define CMD_QUEUE_COMMANDS        0x7E
#define CMD_EXECUTE_COMMANDS      0x7F

/* ------------------------------- 内部状态 ------------------------------- */

static struct {
    uint8_t  port;                 /* 0=none 1=SWD 2=JTAG */
    uint8_t  idle_cycles;
    uint16_t retry_count;
    uint16_t match_retry;
    uint32_t match_mask;
    uint8_t  led_connect;
    uint8_t  led_running;
} s_dap;

typedef dap_ack_t (*xfer_fn_t)(uint8_t request, uint32_t *data);
static atomic_bool s_abort;

void cmsis_dap_abort(void)
{
    atomic_store(&s_abort, true);
}

/* ------------------------------- 小工具 --------------------------------- */

static inline uint32_t rd_u32_le(const uint8_t *p)
{
    return  (uint32_t)p[0]        |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t ack_to_flag(dap_ack_t ack)
{
    switch (ack) {
    case DAP_ACK_OK:    return RSP_OK;
    case DAP_ACK_WAIT:  return RSP_WAIT;
    case DAP_ACK_FAULT: return RSP_FAULT;
    default:            return RSP_PROTO_ERROR;
    }
}

/* 单次物理传输，WAIT 按 TransferConfigure 重试 */
static uint32_t s_wait_acks;
uint32_t cmsis_dap_last_wait_acks(void) { return s_wait_acks; }
static uint8_t xfer_retry(xfer_fn_t xfer, uint8_t request, uint32_t *data)
{
    uint16_t retries = s_dap.retry_count;
    dap_ack_t ack;
    do {
        ack = xfer(request, data);
        if (ack == DAP_ACK_WAIT) ++s_wait_acks;
    } while ((ack == DAP_ACK_WAIT) && (retries-- > 0) && !atomic_load(&s_abort));
    return ack_to_flag(ack);
}

/* ------------------------------- DAP_Info ------------------------------- */

static const char INFO_VENDOR[]  = "Trae";
static const char INFO_PRODUCT[] = "ESP32-S31 CMSIS-DAP";
static const char INFO_SERIAL[]  = "S31DAP0001";
static const char INFO_PROTO[]   = "2.0.0";
static const char INFO_FW[]      = "1.2.0";
static const char INFO_BOARD_V[] = "Trae";
static const char INFO_BOARD_N[] = "softAP Wireless DAP";

static uint8_t info_string(uint8_t *dst, const char *s)
{
    uint8_t n = (uint8_t)(strlen(s) + 1); /* CMSIS-DAP strings include NUL. */
    memcpy(dst, s, n);
    return n;
}

static size_t handle_info(uint8_t id, uint8_t *resp, uint16_t packet_size)
{
    /* resp[0] 已放命令回显，这里写 resp[1]=长度，resp[2..]=数据 */
    uint8_t *p = resp + 2;
    uint8_t len = 0;

    switch (id) {
    case 0x01: len = info_string(p, INFO_VENDOR);  break;
    case 0x02: len = info_string(p, INFO_PRODUCT); break;
    case 0x03: len = info_string(p, INFO_SERIAL);  break;
    case 0x04: len = info_string(p, INFO_PROTO);   break;
    case 0x05: len = 0; break;                     /* 目标器件厂商（未指定） */
    case 0x06: len = 0; break;                     /* 目标器件名 */
    case 0x07: len = info_string(p, INFO_BOARD_V); break;
    case 0x08: len = info_string(p, INFO_BOARD_N); break;
    case 0x09: len = info_string(p, INFO_FW);      break;
    case 0xF0:                                     /* Capabilities */
        *p = 0x01                                  /* bit0 SWD */
           | (CONFIG_DAP_JTAG_ENABLE ? 0x02 : 0); /* bit1 JTAG */
        len = 1;
        break;
    case 0xFE: *p = CONFIG_DAP_USB_PACKET_COUNT; len = 1; break;
    case 0xFF:
        p[0] = (uint8_t)(packet_size & 0xFF);
        p[1] = (uint8_t)(packet_size >> 8);
        len = 2;
        break;
    default:
        len = 0;                                   /* 不支持：长度 0 */
        break;
    }
    resp[1] = len;
    return (size_t)len + 2;
}

/* --------------------------- DAP_Transfer 核心 -------------------------- */

/*
 * SWD 与 JTAG 的命令/响应布局完全一致（JTAG 仅多一个前置 device index，
 * 由调用者消费），posted-read 流水线也同构，因此共用一套循环；物理
 * 差异通过 xfer 函数指针注入（dap_swd_transfer / dap_jtag_transfer）。
 */
static size_t handle_transfer(xfer_fn_t xfer,
                              const uint8_t *req, size_t in_len,
                              uint8_t *resp, size_t cap)
{
    const uint8_t *pin  = req + 3;
    const uint8_t *pend = req + in_len;
    /* DAP_Transfer 标准响应（与 OpenOCD/DAPLink 一致，见 OpenOCD
     * cmsis_dap_swd_read_process：count=resp[1]、flags=resp[2]、数据@3）：
     * [0x05, Transfer Count, Response/flags, 读数据从偏移 3 起]。
     * 注意：响应【不】回显 DAP Index（Index 只在请求 byte1）。 */
    uint8_t *pout = resp + 3;
    const uint8_t *oend = resp + cap;

    uint8_t count = req[2];
    uint8_t done = 0;
    uint8_t flags = 0;
    int post_read = 0;
    int check_write = 0;
    uint8_t posted_request = 0;
    const bool jtag = (s_dap.port == DAP_TPORT_JTAG);
    atomic_store(&s_abort, false);

    /* Validate the entire packet before touching the target. In particular,
     * reserve space for the final posted read, not just intermediate data. */
    const uint8_t *scan = pin;
    size_t read_bytes = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (scan >= pend) goto malformed;
        uint8_t r = *scan++;
        if (r & 0xC0u) goto malformed; /* timestamps are not advertised */
        if (!(r & F_RnW) || (r & F_MATCH_VALUE)) {
            if ((size_t)(pend - scan) < 4) goto malformed;
            scan += 4;
        } else {
            read_bytes += 4;
        }
    }
    if (read_bytes > cap - 3) goto malformed;

    for (uint8_t i = 0; i < count; i++) {
        if (pin >= pend) { flags = RSP_PROTO_ERROR; break; }
        uint8_t rv = *pin++;

        if (rv & F_RnW) {
            /* ---- 读 ---- */
            if (post_read) {
                uint32_t d;
                if (!(rv & F_MATCH_VALUE) &&
                    (jtag ? !((rv ^ posted_request) & F_APnDP) : (rv & F_APnDP))) {
                    /* 连续 AP 读：发本次 AP 请求即收回上次数据 */
                    flags = xfer_retry(xfer, rv, &d);
                } else {
                    flags = xfer_retry(xfer, DP_RDBUFF_READ, &d);
                    post_read = 0;
                }
                if (flags != RSP_OK) break;
                if (pout + 4 > oend) { flags = RSP_PROTO_ERROR; break; }
                wr_u32_le(pout, d);
                pout += 4;
            }

            if (rv & F_MATCH_VALUE) {
                if (pin + 4 > pend) { flags = RSP_PROTO_ERROR; break; }
                uint32_t match_value = rd_u32_le(pin);
                pin += 4;
                uint32_t d;
                if (jtag || (rv & F_APnDP)) {
                    flags = xfer_retry(xfer, rv, NULL);   /* post AP 读 */
                    if (flags != RSP_OK) break;
                }
                uint16_t mr = s_dap.match_retry;
                do {
                    flags = xfer_retry(xfer, rv, &d);
                    if (flags != RSP_OK) break;
                } while (((d & s_dap.match_mask) != match_value) && (mr-- > 0) &&
                         !atomic_load(&s_abort));
                if (flags != RSP_OK) break;
                if ((d & s_dap.match_mask) != match_value) {
                    flags |= RSP_MISMATCH;
                    break;
                }
            } else {
                /* 普通读 */
                if (jtag || (rv & F_APnDP)) {
                    if (!post_read) {
                        flags = xfer_retry(xfer, rv, NULL);   /* post AP 读 */
                        if (flags != RSP_OK) break;
                        post_read = 1;
                        posted_request = rv;
                    }
                } else {
                    uint32_t d;
                    flags = xfer_retry(xfer, rv, &d);
                    if (flags != RSP_OK) break;
                    if (pout + 4 > oend) { flags = RSP_PROTO_ERROR; break; }
                    wr_u32_le(pout, d);
                    pout += 4;
                }
            }
            check_write = 0;
        } else {
            /* ---- 写 ---- */
            if (post_read) {
                uint32_t d;
                flags = xfer_retry(xfer, DP_RDBUFF_READ, &d);
                if (flags != RSP_OK) break;
                if (pout + 4 > oend) { flags = RSP_PROTO_ERROR; break; }
                wr_u32_le(pout, d);
                pout += 4;
                post_read = 0;
            }
            if (pin + 4 > pend) { flags = RSP_PROTO_ERROR; break; }
            uint32_t d = rd_u32_le(pin);
            pin += 4;
            if (rv & F_MATCH_MASK) {
                s_dap.match_mask = d;       /* 不上总线 */
                flags = RSP_OK;
            } else {
                flags = xfer_retry(xfer, rv, &d);
                if (flags != RSP_OK) break;
                check_write = 1;
            }
        }
        done++;
        if (atomic_load(&s_abort)) break;
    }

    /* 收尾：取最后一个 post 读，或推进最后一次写检查错误 */
    if (flags == RSP_OK) {
        uint32_t d;
        if (post_read) {
            flags = xfer_retry(xfer, DP_RDBUFF_READ, &d);
            if (flags == RSP_OK) {
                if (pout + 4 <= oend) { wr_u32_le(pout, d); pout += 4; }
            }
        } else if (check_write) {
            flags = xfer_retry(xfer, DP_RDBUFF_READ, NULL);
        }
    }

    resp[1] = done;               /* Transfer Count（成功执行的条数） */
    resp[2] = flags;              /* Response / ACK 标志 */
    return (size_t)(pout - resp);

malformed:
    resp[1] = 0;
    resp[2] = RSP_PROTO_ERROR;
    return 3;
}

/* DAP_TransferBlock has a 16-bit count and a four-byte response header.
 * AP reads (and all JTAG reads) are posted: discard the first scan, then
 * obtain the final word with DP RDBUFF. Never return the stale first word. */
static size_t handle_transfer_block(xfer_fn_t xfer, const uint8_t *req,
                                    size_t len, uint8_t *resp, size_t cap)
{
    uint16_t done = 0;
    uint8_t flags = 0;
    bool read = false;
    uint8_t *out = resp + 4;
    atomic_store(&s_abort, false);
    if (len < 5) { flags = RSP_PROTO_ERROR; goto end; }
    uint16_t count = (uint16_t)(req[2] | ((uint16_t)req[3] << 8));
    uint8_t r = req[4];
    read = (r & F_RnW) != 0;
    bool posted = (r & F_APnDP) || s_dap.port == DAP_TPORT_JTAG;
    if ((r & 0xF0u) || (read ? (size_t)count > (cap - 4) / 4
                                  : (size_t)count > (len - 5) / 4)) {
        flags = RSP_PROTO_ERROR;
        goto end;
    }
    if (!count) goto end;
    if (read && posted) {
        flags = xfer_retry(xfer, r, NULL);
        if (flags != RSP_OK) goto end;
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t data = read ? 0 : rd_u32_le(req + 5 + 4 * i);
        uint8_t request = (read && posted && i == count - 1) ? DP_RDBUFF_READ : r;
        flags = xfer_retry(xfer, request, &data);
        if (flags != RSP_OK) break;
        if (read) { wr_u32_le(out, data); out += 4; }
        ++done;
        if (atomic_load(&s_abort)) break;
    }
    if (!read && flags == RSP_OK) flags = xfer_retry(xfer, DP_RDBUFF_READ, NULL);
end:
    resp[1] = (uint8_t)done;
    resp[2] = (uint8_t)(done >> 8);
    resp[3] = flags;
    return (size_t)(out - resp);
}

static size_t handle_swd_sequence(const uint8_t *req, size_t len,
                                uint8_t *resp, size_t cap)
{
    size_t in = 2, out = 2;
    resp[1] = DAP_ERROR;
    if (len < 2) return 2;
    /* Validate first so a truncated packet cannot leave SWDIO half-switched. */
    for (unsigned i = 0; i < req[1]; ++i) {
        if (in >= len) return 2;
        uint8_t info = req[in++];
        size_t bytes = (((info & 63u) ? (info & 63u) : 64u) + 7u) / 8u;
        if (info & 0x80u) out += bytes; else in += bytes;
        if (in > len || out > cap) return 2;
    }
    in = out = 2;
    for (unsigned i = 0; i < req[1]; ++i) {
        uint8_t info = req[in++];
        size_t bytes = (((info & 63u) ? (info & 63u) : 64u) + 7u) / 8u;
        dap_swd_sequence(info, req + in, resp + out);
        if (info & 0x80u) out += bytes; else in += bytes;
    }
    resp[1] = DAP_OK;
    return out;
}

/* ------------------------------- JTAG 序列 ------------------------------ */

static size_t handle_jtag_sequence(const uint8_t *req, size_t in_len,
                                   uint8_t *resp, size_t cap)
{
    if (in_len < 2) { resp[1] = DAP_ERROR; return 2; }
    uint8_t segments = req[1];
    const uint8_t *pin = req + 2;
    const uint8_t *pend = req + in_len;
    uint8_t *pout = resp + 2;
    resp[1] = DAP_OK;

    for (uint8_t s = 0; s < segments; s++) {
        if (pin >= pend) { resp[1] = DAP_ERROR; break; }
        uint8_t info = *pin++;
        uint32_t bits = info & 0x3Fu;
        if (bits == 0) bits = 64;
        uint32_t nbytes = (bits + 7) / 8;
        if ((uint32_t)(pend - pin) < nbytes) { resp[1] = DAP_ERROR; break; }

        uint8_t tmp[8];
        memcpy(tmp, pin, nbytes);
        pin += nbytes;

        if (info & 0x80u) {                 /* 捕获 TDO */
            if ((size_t)(pout - resp) + nbytes > cap) { resp[1] = DAP_ERROR; break; }
            memcpy(pout, tmp, nbytes);
            dap_jtag_sequence(info, pout, pout);
            pout += nbytes;
        } else {
            dap_jtag_sequence(info, tmp, NULL);
        }
    }
    return (size_t)(pout - resp);
}

/* ------------------------------- 入口 ----------------------------------- */

void cmsis_dap_init(void)
{
    memset(&s_dap, 0, sizeof(s_dap));
    s_dap.retry_count = 100;                /* OpenOCD 通常会再下发配置 */
    s_dap.match_retry = 0;
    s_dap.match_mask  = 0;
}

size_t cmsis_dap_process(const uint8_t *req, size_t in_len,
                         uint8_t *resp, size_t cap)
{
    return cmsis_dap_process_ex(req, in_len, resp, cap,
                                CONFIG_DAP_USB_BULK_EP_SIZE);
}

size_t cmsis_dap_process_ex(const uint8_t *req, size_t in_len,
                            uint8_t *resp, size_t cap,
                            uint16_t packet_size)
{
    s_wait_acks = 0;
    if (in_len < 1 || cap < 4) return 0;

    if (packet_size == 0 || packet_size > cap) {
        packet_size = CONFIG_DAP_USB_BULK_EP_SIZE;
    }
    /* The shared core has larger buffers than a physical USB/HID request.
       Bound replies to the capacity of the originating transport. */
    if (cap > packet_size) cap = packet_size;

    uint8_t cmd = req[0];
    resp[0] = cmd;

    switch (cmd) {
    case CMD_INFO:
        if (in_len < 2) { resp[1] = 0; return 2; }
        return handle_info(req[1], resp, packet_size);

    case CMD_HOST_STATUS: {
        if (in_len < 3) { resp[1] = DAP_ERROR; return 2; }
        if (req[1] == 0)      s_dap.led_connect = req[2];
        else if (req[1] == 1) s_dap.led_running = req[2];
        else { resp[1] = DAP_ERROR; return 2; }
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_CONNECT: {
        if (in_len < 2) { resp[1] = 0; return 2; }
        uint8_t port = req[1];
        if (port == 0) port = DAP_TPORT_SWD;    /* autodetect -> SWD */
        if (port == DAP_TPORT_SWD) {
            dap_bus_connect(DAP_TPORT_SWD);
            s_dap.port = DAP_TPORT_SWD;
            resp[1] = DAP_TPORT_SWD;
        } else if (port == DAP_TPORT_JTAG && CONFIG_DAP_JTAG_ENABLE) {
            dap_bus_connect(DAP_TPORT_JTAG);
            s_dap.port = DAP_TPORT_JTAG;
            resp[1] = DAP_TPORT_JTAG;
        } else {
            s_dap.port = 0;
            resp[1] = 0;                        /* disabled / 不支持 */
        }
        return 2;
    }

    case CMD_DISCONNECT:
        dap_bus_disconnect();
        s_dap.port = 0;
        resp[1] = DAP_OK;
        return 2;

    case CMD_TRANSFER_CONFIGURE:
        if (in_len < 6) { resp[1] = DAP_ERROR; return 2; }
        s_dap.idle_cycles = req[1];
        s_dap.retry_count = (uint16_t)(req[2] | ((uint16_t)req[3] << 8));
        s_dap.match_retry = (uint16_t)(req[4] | ((uint16_t)req[5] << 8));
        dap_bus_set_idle_cycles(req[1]);
        resp[1] = DAP_OK;
        return 2;

    case CMD_TRANSFER: {
        if (in_len < 3) {
            resp[1] = 0; resp[2] = RSP_PROTO_ERROR; return 3;
        }
        size_t tr;
        if (s_dap.port == DAP_TPORT_SWD) {
            tr = handle_transfer(dap_swd_transfer, req, in_len, resp, cap);
        } else if (s_dap.port == DAP_TPORT_JTAG) {
            /* JTAG：index 越界时响应 count=0/flags=0（同 DAPLink） */
            if (!CONFIG_DAP_JTAG_ENABLE || !dap_jtag_index_is_valid(req[1])) {
                resp[1] = 0; resp[2] = 0; return 3;
            }
            dap_jtag_set_index(req[1]);
            tr = handle_transfer(dap_jtag_transfer, req, in_len, resp, cap);
        } else {
            resp[1] = 0;
            resp[2] = RSP_PROTO_ERROR;
            return 3;
        }
        return tr;
    }

    case CMD_TRANSFER_ABORT:
        cmsis_dap_abort();
        return 0; /* Abort is handled out-of-band, with no USB response. */

    case CMD_TRANSFER_BLOCK: {
        xfer_fn_t xfer = NULL;
        if (s_dap.port == DAP_TPORT_SWD) xfer = dap_swd_transfer;
        else if (s_dap.port == DAP_TPORT_JTAG && in_len >= 2 &&
                 CONFIG_DAP_JTAG_ENABLE && dap_jtag_index_is_valid(req[1])) {
            dap_jtag_set_index(req[1]);
            xfer = dap_jtag_transfer;
        }
        if (xfer) return handle_transfer_block(xfer, req, in_len, resp, cap);
        resp[1] = resp[2] = resp[3] = 0;
        return 4;
    }

    case CMD_WRITE_ABORT: {
        if (in_len < 6) { resp[1] = DAP_ERROR; return 2; }
        uint32_t value = rd_u32_le(req + 2);
        if (s_dap.port == DAP_TPORT_SWD) {
            resp[1] = xfer_retry(dap_swd_transfer, 0, &value) == RSP_OK ? DAP_OK : DAP_ERROR;
        } else if (s_dap.port == DAP_TPORT_JTAG && CONFIG_DAP_JTAG_ENABLE) {
            if (!dap_jtag_index_is_valid(req[1])) { resp[1] = DAP_ERROR; return 2; }
            dap_jtag_set_index(req[1]);
            dap_jtag_write_abort(value);
            resp[1] = DAP_OK;
        } else {
            resp[1] = DAP_ERROR;
        }
        return 2;
    }

    case CMD_DELAY: {
        if (in_len < 3) { resp[1] = DAP_ERROR; return 2; }
        uint16_t us = (uint16_t)(req[1] | ((uint16_t)req[2] << 8));
        if (us) esp_rom_delay_us(us);
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_RESET_TARGET:
        resp[1] = DAP_OK;
        resp[2] = 0;                    /* No device-specific reset sequence. */
        return 3;

    case CMD_SWJ_PINS: {
        if (in_len < 7) { resp[1] = DAP_ERROR; return 2; }
        uint32_t wait = rd_u32_le(req + 3);
        if (wait > 3000000) wait = 3000000;
        int64_t deadline = esp_timer_get_time() + wait;
        resp[1] = dap_bus_swj_pins(req[1], req[2]);
        while (((resp[1] ^ req[1]) & req[2]) && esp_timer_get_time() < deadline) {
            esp_rom_delay_us(1);
            resp[1] = dap_bus_swj_pins(0, 0);
        }
        return 2;
    }

    case CMD_SWJ_CLOCK: {
        if (in_len < 5) { resp[1] = DAP_ERROR; return 2; }
        uint32_t hz = rd_u32_le(req + 1);
        if (hz == 0) { resp[1] = DAP_ERROR; return 2; }
        dap_bus_set_clock(hz);
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_SWJ_SEQUENCE: {
        if (in_len < 2) { resp[1] = DAP_ERROR; return 2; }
        uint32_t bits = req[1];
        if (bits == 0) bits = 256;
        uint32_t nbytes = (bits + 7) / 8;
        if (in_len < (size_t)(2 + nbytes)) { resp[1] = DAP_ERROR; return 2; }
        dap_bus_swj_sequence(bits, req + 2);
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_SWD_CONFIGURE: {
        if (in_len < 2 || s_dap.port != DAP_TPORT_SWD) {
            resp[1] = DAP_ERROR; return 2;
        }
        uint8_t v = req[1];
        dap_swd_configure((uint8_t)((v & 0x03u) + 1u), (v & 0x04u) != 0);
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_JTAG_SEQUENCE:
        if (!CONFIG_DAP_JTAG_ENABLE || s_dap.port != DAP_TPORT_JTAG) {
            resp[1] = DAP_ERROR; return 2;
        }
        return handle_jtag_sequence(req, in_len, resp, cap);

    case CMD_JTAG_CONFIGURE: {
        if (!CONFIG_DAP_JTAG_ENABLE) { resp[1] = DAP_ERROR; return 2; }
        if (in_len < 2) { resp[1] = DAP_ERROR; return 2; }
        uint8_t count = req[1];
        if (in_len < (size_t)(2 + count)) { resp[1] = DAP_ERROR; return 2; }
        if (!dap_jtag_configure(count, req + 2)) {
            resp[1] = DAP_ERROR; return 2;
        }
        resp[1] = DAP_OK;
        return 2;
    }

    case CMD_JTAG_IDCODE: {
        if (!CONFIG_DAP_JTAG_ENABLE || s_dap.port != DAP_TPORT_JTAG ||
            in_len < 2 || !dap_jtag_index_is_valid(req[1])) {
            resp[1] = DAP_ERROR; return 2;
        }
        dap_jtag_set_index(req[1]);
        uint32_t id = dap_jtag_read_idcode();
        resp[1] = DAP_OK;
        wr_u32_le(resp + 2, id);
        return 6;
    }

    case CMD_SWD_SEQUENCE:
        if (s_dap.port != DAP_TPORT_SWD) { resp[1] = DAP_ERROR; return 2; }
        return handle_swd_sequence(req, in_len, resp, cap);

    case CMD_QUEUE_COMMANDS:
    case CMD_EXECUTE_COMMANDS:
        resp[0] = DAP_ERROR; /* Atomic commands are not advertised. */
        return 1;

    default:
        resp[0] = DAP_ERROR; /* Standard ID_DAP_Invalid response. */
        return 1;
    }
}
