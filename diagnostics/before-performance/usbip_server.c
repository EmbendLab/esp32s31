#include "usbip_server.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "cmsis_dap.h"
#include "dap_link.h"
#include "usb_descriptors.h"
#include "usbip_defs.h"

static const char *TAG = "usbip";

#define USBIP_TASK_STACK 8192
#define USBIP_TASK_PRIO  5
#define USBIP_MAX_TRANSFER 4096
#define USBIP_DAP_TIMEOUT_MS 2000
#define USBIP_DAP_WINDOW CONFIG_DAP_USB_PACKET_COUNT

typedef struct {
    uint8_t data[CONFIG_DAP_USB_BULK_EP_SIZE];
    size_t len;
} usbip_dap_response_t;

typedef struct {
    int fd;
    uint32_t devid;
    usbip_dap_response_t responses[USBIP_DAP_WINDOW];
    size_t response_head;
    size_t response_count;
    size_t outstanding_count;
    usbip_stage2_header_t pending_in[USBIP_DAP_WINDOW];
    size_t pending_head;
    size_t pending_count;
} usbip_client_t;

static int recv_all(int fd, void *dst, size_t len)
{
    uint8_t *p = (uint8_t *)dst;
    while (len) {
        int n = recv(fd, p, len, 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int send_all(int fd, const void *src, size_t len)
{
    const uint8_t *p = (const uint8_t *)src;
    while (len) {
        int n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void make_submit_response(uint8_t out[USBIP_STAGE2_HEADER_SIZE],
                                 const usbip_stage2_header_t *req,
                                 int32_t status, int32_t actual_length)
{
    memset(out, 0, USBIP_STAGE2_HEADER_SIZE);
    put_be32(out + 0, USBIP_RET_SUBMIT);
    put_be32(out + 4, req->seqnum);
    /* RET_SUBMIT basic devid/direction/ep are unused and zero on Linux.
       The client associates this completion with the original seqnum. */
    put_be32(out + 20, (uint32_t)status);
    put_be32(out + 24, (uint32_t)actual_length);
}

static int send_submit_response(usbip_client_t *client,
                                const usbip_stage2_header_t *req,
                                int32_t status, const uint8_t *data,
                                int32_t data_len)
{
    uint8_t response[USBIP_STAGE2_HEADER_SIZE];
    make_submit_response(response, req, status, data_len);
    if (req->ep == 0 || status) {
        ESP_LOGI(TAG, "complete seq=%lu ep=%lu status=%ld length=%ld",
                 (unsigned long)req->seqnum, (unsigned long)req->ep,
                 (long)status, (long)data_len);
    }
    if (send_all(client->fd, response, sizeof(response)) < 0) return -1;
    if (req->direction == USBIP_DIR_IN && data_len > 0 && data &&
        send_all(client->fd, data, (size_t)data_len) < 0) return -1;
    return 0;
}

static int send_unlink_response(usbip_client_t *client,
                                const usbip_stage2_header_t *req)
{
    uint8_t response[USBIP_STAGE2_HEADER_SIZE] = {0};
    put_be32(response + 0, USBIP_RET_UNLINK);
    put_be32(response + 4, req->seqnum);
    int32_t status = 0;
    for (size_t i = 0; i < client->pending_count; ++i) {
        size_t slot = (client->pending_head + i) % USBIP_DAP_WINDOW;
        if (client->pending_in[slot].seqnum != req->u.unlink.unlink_seqnum) continue;
        for (size_t j = i; j + 1 < client->pending_count; ++j) {
            size_t dst = (client->pending_head + j) % USBIP_DAP_WINDOW;
            size_t src = (client->pending_head + j + 1) % USBIP_DAP_WINDOW;
            client->pending_in[dst] = client->pending_in[src];
        }
        --client->pending_count;
        status = -ECONNRESET;
        break;
    }
    put_be32(response + 20, (uint32_t)status);
    return send_all(client->fd, response, sizeof(response));
}

static void fill_device_info(usbip_devlist_device_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    strncpy(dev->path, "/sys/devices/virtual/usbip/1-1", sizeof(dev->path) - 1);
    strncpy(dev->busid, "1-1", sizeof(dev->busid) - 1);
    dev->busnum = htonl(1);
    dev->devnum = htonl(1);
    dev->speed = htonl(3); /* USB_SPEED_HIGH */
    dev->id_vendor = htons(CONFIG_DAP_USB_VID);
    dev->id_product = htons(g_dap_usbip_device_desc.idProduct);
    dev->bcd_device = htons(g_dap_usbip_device_desc.bcdDevice);
    dev->b_device_class = g_dap_usbip_device_desc.bDeviceClass;
    dev->b_device_subclass = g_dap_usbip_device_desc.bDeviceSubClass;
    dev->b_device_protocol = g_dap_usbip_device_desc.bDeviceProtocol;
    dev->b_configuration_value = 1;
    dev->b_num_configurations = 1;
    dev->b_num_interfaces = 1;
}

static int send_devlist(usbip_client_t *client)
{
    uint8_t header[8];
    uint8_t count[4];
    usbip_devlist_device_t dev;
    usbip_devlist_interface_t interfaces[1] = {
        { .interface_class = 0xFF, .interface_subclass = 0, .interface_protocol = 0 },
    };

    fill_device_info(&dev);

    put_be16(header + 0, 0x0111); /* USB/IP protocol version 1.1 */
    put_be16(header + 2, USBIP_OP_REP_DEVLIST);
    put_be32(header + 4, 0);
    put_be32(count, 1);
    if (send_all(client->fd, header, sizeof(header)) < 0 ||
        send_all(client->fd, count, sizeof(count)) < 0 ||
        send_all(client->fd, &dev, sizeof(dev)) < 0 ||
        send_all(client->fd, interfaces, sizeof(interfaces)) < 0) return -1;
    return 0;
}

static int send_import_response(usbip_client_t *client, bool found)
{
    uint8_t response[8] = {0};
    put_be16(response + 0, 0x0111);
    put_be16(response + 2, USBIP_OP_REP_IMPORT);
    put_be32(response + 4, found ? 0 : 4); /* ST_NODEV */
    if (send_all(client->fd, response, sizeof(response)) < 0) return -1;
    if (!found) return 0;

    /* OP_REP_IMPORT must include the device record, without interface records.
       The client waits for these 312 bytes before it can submit any URBs. */
    usbip_devlist_device_t dev;
    fill_device_info(&dev);
    return send_all(client->fd, &dev, sizeof(dev));
}

static const uint8_t *descriptor_for_request(const uint8_t setup[8], size_t *len)
{
    /* USB setup is bmRequestType, bRequest, wValue LE, wIndex LE, wLength LE. */
    const uint8_t type = setup[3];
    const uint8_t index = setup[2];
    const uint8_t *data = NULL;
    size_t size = 0;
    /* A high-speed capable device can be queried for its other-speed
       configuration during enumeration.  USB/IP clients vary in whether
       they issue this request; return the same topology with descriptor type
       0x07 instead of stalling it. */
    static uint8_t other_speed_config[39];

    switch (type) {
    case 0x01:
        data = (const uint8_t *)&g_dap_usbip_device_desc;
        size = sizeof(g_dap_usbip_device_desc);
        break;
    case 0x02:
        /* Reference-style single WinUSB interface. */
        data = g_dap_usbip_config_desc;
        size = get_le16(data + 2);
        break;
    case 0x07:
        data = g_dap_usbip_config_desc;
        size = get_le16(data + 2);
        if (size > sizeof(other_speed_config)) size = sizeof(other_speed_config);
        memcpy(other_speed_config, data, size);
        other_speed_config[1] = 0x07; /* USB_DT_OTHER_SPEED_CONFIGURATION */
        /* Full-speed bulk endpoints have a maximum packet size of 64. */
        other_speed_config[22] = other_speed_config[29] = 64;
        other_speed_config[23] = other_speed_config[30] = 0;
        data = other_speed_config;
        break;
    case 0x03:
        if (index == 0xEE) {
            data = dap_usbip_ms_os_10_string(&size);
        } else if (index < DAP_STRID_COUNT) {
            data = dap_string_descriptor(index, &size);
        }
        break;
    case 0x06:
        data = (const uint8_t *)&g_dap_usbip_qualifier_desc;
        size = sizeof(g_dap_usbip_qualifier_desc);
        break;
    case 0x0F:
        data = dap_usbip_bos_descriptor(&size);
        break;
    default:
        break;
    }
    if (len) *len = size;
    return data;
}

static int control_transfer(usbip_client_t *client,
                            const usbip_stage2_header_t *req,
                            const uint8_t *payload, size_t payload_len)
{
    const uint8_t *setup = req->u.submit.setup;
    const uint8_t bm_request_type = setup[0];
    const uint8_t request = setup[1];
    const uint16_t value = get_le16(setup + 2);
    const uint16_t index = get_le16(setup + 4);
    const uint16_t w_length = get_le16(setup + 6);
    ESP_LOGI(TAG, "control seq=%lu %02x/%02x value=%04x index=%04x length=%u urb=%ld",
             (unsigned long)req->seqnum, bm_request_type, request,
             value, index, w_length,
             (long)req->u.submit.transfer_buffer_length);
    uint8_t small[8] = {0};
    const uint8_t *data = NULL;
    size_t data_len = 0;

    (void)payload;
    (void)payload_len;

    if ((bm_request_type & 0x60) == 0x00 && request == 0x06) {
        data = descriptor_for_request(setup, &data_len);
        if (!data) return send_submit_response(client, req, -EPIPE, NULL, 0);
    } else if (bm_request_type == 0xC0 && value == 0 &&
               request == DAP_USBIP_MS_OS_VENDOR_CODE && index == 7) {
        /* Reference-compatible MS OS 2.0 request: bRequest=0x01, wIndex=7. */
        data = dap_usbip_ms_os_20_descriptor(&data_len);
    } else if ((bm_request_type == 0xC0 || bm_request_type == 0xC1) && value == 0 &&
               request == DAP_USBIP_MS_OS_VENDOR_CODE && (index == 4 || index == 5)) {
        data = dap_usbip_ms_os_10_descriptor(index, &data_len);
    } else if ((bm_request_type & 0x60) != 0) {
        /* No class requests exist on the wireless vendor-only interface. */
        return send_submit_response(client, req, -EPIPE, NULL, 0);
    } else if ((bm_request_type & 0x80) && request == 0x00) {
        data = small;
        data_len = 2;
    } else if ((bm_request_type & 0x80) && request == 0x08) {
        small[0] = 1;
        data = small;
        data_len = 1;
    } else if ((bm_request_type & 0x80) && request == 0x0A) {
        data = small;
        data_len = 1;
    } else if (!(bm_request_type & 0x80)) {
        /* Standard SET requests have no response payload. */
        data = NULL;
        data_len = 0;
    } else {
        return send_submit_response(client, req, -EPIPE, NULL, 0);
    }

    if (data_len > w_length) data_len = w_length;
    if (data_len > (size_t)req->u.submit.transfer_buffer_length) {
        data_len = (size_t)req->u.submit.transfer_buffer_length;
    }
    if (req->direction == USBIP_DIR_OUT) data_len = payload_len;
    return send_submit_response(client, req, 0, data, (int32_t)data_len);
}

static bool request_is_abort(const uint8_t *data, size_t len)
{
    return len > 0 && data[0] == 0x07;
}

static void collect_dap_responses(usbip_client_t *client)
{
    while (client->response_count < USBIP_DAP_WINDOW && client->outstanding_count) {
        size_t slot = (client->response_head + client->response_count) % USBIP_DAP_WINDOW;
        usbip_dap_response_t *response = &client->responses[slot];
        response->len = dap_link_get_response(response->data, sizeof(response->data), 0);
        if (!response->len) break;
        --client->outstanding_count;
        ++client->response_count;
    }
}

static int process_dap_out(usbip_client_t *client,
                           const uint8_t *data, size_t len)
{
    const uint16_t packet_size = CONFIG_DAP_USB_BULK_EP_SIZE;
    if (!len || len > packet_size) return -EINVAL;
    if (request_is_abort(data, len)) {
        cmsis_dap_abort();
        return 0;
    }

    collect_dap_responses(client);
    if (client->response_count + client->outstanding_count >= USBIP_DAP_WINDOW) {
        return -ENOSPC;
    }

    TickType_t start = xTaskGetTickCount();
    while (dap_link_post_request_ex(data, len, packet_size) == 0) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(USBIP_DAP_TIMEOUT_MS)) {
            return -ETIMEDOUT;
        }
        vTaskDelay(1);
    }
    ++client->outstanding_count;
    return 0;
}

static int send_dap_response(usbip_client_t *client,
                             const usbip_stage2_header_t *req)
{
    usbip_dap_response_t *response = &client->responses[client->response_head];
    const uint8_t *data = response->data;
    size_t n = response->len;
    size_t requested = (size_t)req->u.submit.transfer_buffer_length;
    uint8_t hid_report[DAP_USBIP_HID_REPORT_SIZE] = {0};
    if (req->ep == 2) {
        if (n > sizeof(hid_report)) n = sizeof(hid_report);
        memcpy(hid_report, data, n);
        data = hid_report;
        n = sizeof(hid_report);
    }
    if (n > requested) n = requested;
    if (send_submit_response(client, req, 0, data, (int32_t)n) < 0) return -1;
    client->response_head = (client->response_head + 1) % USBIP_DAP_WINDOW;
    --client->response_count;
    return 0;
}

static int service_pending_in(usbip_client_t *client)
{
    collect_dap_responses(client);
    while (client->pending_count && client->response_count) {
        usbip_stage2_header_t *req = &client->pending_in[client->pending_head];
        if (send_dap_response(client, req) < 0) return -1;
        client->pending_head = (client->pending_head + 1) % USBIP_DAP_WINDOW;
        --client->pending_count;
    }
    return 0;
}

static int process_submit(usbip_client_t *client,
                          const usbip_stage2_header_t *req,
                          const uint8_t *payload, size_t payload_len)
{
    if (req->devid != client->devid) return send_submit_response(client, req, -ENODEV, NULL, 0);

    if (req->ep == 0) {
        return control_transfer(client, req, payload, payload_len);
    }

    const bool dap_endpoint = req->ep == 1 || req->ep == 2 || req->ep == 0x81;
    if (!dap_endpoint) {
        return send_submit_response(client, req, -EPIPE, NULL, 0);
    }

    if (req->direction == USBIP_DIR_OUT) {
        int status = payload_len ? process_dap_out(client, payload, payload_len) : 0;
        /* The reference acknowledges the OUT URB immediately and lets the DAP
           task produce the response for a later IN poll. */
        /* WinUSB treats a short OUT completion as a failed write.  This
           length is bytes accepted, not the length of the later DAP reply. */
        if (send_submit_response(client, req, status, NULL,
                                 status ? 0 : (int32_t)payload_len) < 0) return -1;
        return service_pending_in(client);
    }

    if (req->u.submit.transfer_buffer_length == 0) {
        return send_submit_response(client, req, 0, NULL, 0);
    }
    if (service_pending_in(client) < 0) return -1;
    if (client->response_count == 0) {
        /* Emulate a USB endpoint that NAKs until a DAP response is ready.
           Returning a successful ZLP makes clients consume an empty reply. */
        if (client->pending_count == USBIP_DAP_WINDOW) {
            return send_submit_response(client, req, -ENOSPC, NULL, 0);
        }
        size_t slot = (client->pending_head + client->pending_count) % USBIP_DAP_WINDOW;
        client->pending_in[slot] = *req;
        ++client->pending_count;
        return 0;
    }
    return send_dap_response(client, req);
}

static int process_client(usbip_client_t *client)
{
    uint8_t first[8];
    int result = recv_all(client->fd, first, sizeof(first));
    if (result <= 0) return result;

    uint16_t version = (uint16_t)((first[0] << 8) | first[1]);
    uint16_t command = (uint16_t)((first[2] << 8) | first[3]);
    if (version != 0x0111) return -1;
    if (command == USBIP_OP_REQ_DEVLIST) {
        if (send_devlist(client) < 0) return -1;
        return 1;
    }
    if (command != USBIP_OP_REQ_IMPORT) return -1;

    char busid[USBIP_BUSID_SIZE];
    if (recv_all(client->fd, busid, sizeof(busid)) <= 0) return 0;
    bool found = memcmp(busid, "1-1\0", 4) == 0;
    if (send_import_response(client, found) < 0) return -1;
    if (!found) return 1;

    client->devid = (1u << 16) | 1u;
    ESP_LOGI(TAG, "USB/IP import complete: busid=1-1, device info=312 bytes; waiting for URBs");

    for (;;) {
        if (service_pending_in(client) < 0) return -1;
        if (client->pending_count) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(client->fd, &readable);
            struct timeval timeout = { .tv_sec = 0, .tv_usec = 10000 };
            int ready = select(client->fd + 1, &readable, NULL, NULL, &timeout);
            if (ready == 0) continue;
            if (ready < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
        }
        uint8_t wire[USBIP_STAGE2_HEADER_SIZE];
        result = recv_all(client->fd, wire, sizeof(wire));
        if (result <= 0) return result;

        usbip_stage2_header_t req;
        memset(&req, 0, sizeof(req));
        req.command = get_be32(wire + 0);
        req.seqnum = get_be32(wire + 4);
        req.devid = get_be32(wire + 8);
        req.direction = get_be32(wire + 12);
        req.ep = get_be32(wire + 16);
        ESP_LOGD(TAG, "URB seq=%lu cmd=%lu devid=%08lx dir=%lu ep=%lu",
                 (unsigned long)req.seqnum, (unsigned long)req.command,
                 (unsigned long)req.devid, (unsigned long)req.direction,
                 (unsigned long)req.ep);
        if (req.direction > USBIP_DIR_IN) return -1;
        if (req.command == USBIP_CMD_SUBMIT) {
            req.u.submit.transfer_flags = get_be32(wire + 20);
            req.u.submit.transfer_buffer_length = (int32_t)get_be32(wire + 24);
            req.u.submit.start_frame = (int32_t)get_be32(wire + 28);
            req.u.submit.number_of_packets = (int32_t)get_be32(wire + 32);
            req.u.submit.interval = (int32_t)get_be32(wire + 36);
            memcpy(req.u.submit.setup, wire + 40, 8);
            if (req.u.submit.transfer_buffer_length < 0 ||
                req.u.submit.transfer_buffer_length > USBIP_MAX_TRANSFER) return -1;

            size_t payload_len = req.direction == USBIP_DIR_OUT ?
                (size_t)req.u.submit.transfer_buffer_length : 0;
            uint8_t *payload = NULL;
            if (payload_len) {
                payload = pvPortMalloc(payload_len);
                if (!payload) return -1;
                result = recv_all(client->fd, payload, payload_len);
                if (result <= 0) {
                    vPortFree(payload);
                    return result;
                }
            }
            result = process_submit(client, &req, payload, payload_len);
            if (payload) vPortFree(payload);
            if (result < 0) return result;
        } else if (req.command == USBIP_CMD_UNLINK) {
            req.u.unlink.unlink_seqnum = get_be32(wire + 20);
            cmsis_dap_abort();
            if (send_unlink_response(client, &req) < 0) return -1;
        } else {
            return -1;
        }
    }
}

static void usbip_task(void *arg)
{
    (void)arg;
    for (;;) {
        int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_fd < 0) {
            ESP_LOGE(TAG, "socket failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int on = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(USBIP_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(listen_fd, 1) < 0) {
            ESP_LOGE(TAG, "bind/listen failed: errno=%d", errno);
            close(listen_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "USB/IP server listening on TCP %d", USBIP_PORT);

        for (;;) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
            if (fd < 0) continue;
            int nodelay = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            usbip_client_t *client = pvPortMalloc(sizeof(*client));
            int result = -ENOMEM;
            if (client) {
                memset(client, 0, sizeof(*client));
                client->fd = fd;
                result = process_client(client);
                if (client->devid) cmsis_dap_abort();
                vPortFree(client);
            }
            shutdown(fd, SHUT_RDWR);
            close(fd);
            ESP_LOGI(TAG, "USB/IP client detached (%d)", result);
        }
    }
}

esp_err_t usbip_server_start(void)
{
    ESP_LOGI(TAG, "USB/IP CMSIS-DAP: single WinUSB, MS OS 1.0/2.0, VID=0x%04x PID=0x%04x, bcdDevice=0x0106",
             (unsigned)g_dap_usbip_device_desc.idVendor,
             (unsigned)g_dap_usbip_device_desc.idProduct);
    BaseType_t ok = xTaskCreatePinnedToCore(usbip_task, "usbip_server",
                                            USBIP_TASK_STACK, NULL,
                                            USBIP_TASK_PRIO, NULL,
                                            CONFIG_DAP_USB_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
