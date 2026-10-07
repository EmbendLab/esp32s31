#pragma once

#include <stdint.h>

#define USBIP_PORT 3240
#define USBIP_BUSID_SIZE 32
#define USBIP_SYSFS_PATH_SIZE 256
#define USBIP_STAGE2_HEADER_SIZE 48

#define USBIP_OP_REQ_DEVLIST 0x8005u
#define USBIP_OP_REP_DEVLIST 0x0005u
#define USBIP_OP_REQ_IMPORT  0x8003u
#define USBIP_OP_REP_IMPORT  0x0003u

#define USBIP_CMD_SUBMIT 0x00000001u
#define USBIP_CMD_UNLINK 0x00000002u
#define USBIP_RET_SUBMIT 0x00000003u
#define USBIP_RET_UNLINK 0x00000004u

#define USBIP_DIR_OUT 0u
#define USBIP_DIR_IN  1u

/* The USB/IP protocol is big-endian except for the eight raw setup bytes. */
typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    union {
        struct __attribute__((packed)) {
            uint32_t transfer_flags;
            int32_t transfer_buffer_length;
            int32_t start_frame;
            int32_t number_of_packets;
            int32_t interval;
            uint8_t setup[8];
        } submit;
        struct __attribute__((packed)) {
            int32_t status;
            int32_t actual_length;
            int32_t start_frame;
            int32_t number_of_packets;
            int32_t error_count;
            uint8_t padding[8];
        } ret_submit;
        struct __attribute__((packed)) {
            uint32_t unlink_seqnum;
            uint8_t padding[24];
        } unlink;
    } u;
} usbip_stage2_header_t;

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t command;
    uint32_t status;
} usbip_stage1_header_t;

typedef struct __attribute__((packed)) {
    char path[USBIP_SYSFS_PATH_SIZE];
    char busid[USBIP_BUSID_SIZE];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_device;
    uint8_t b_device_class;
    uint8_t b_device_subclass;
    uint8_t b_device_protocol;
    uint8_t b_configuration_value;
    uint8_t b_num_configurations;
    uint8_t b_num_interfaces;
} usbip_devlist_device_t;

typedef struct __attribute__((packed)) {
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t padding;
} usbip_devlist_interface_t;

_Static_assert(sizeof(usbip_stage2_header_t) == USBIP_STAGE2_HEADER_SIZE,
               "USB/IP stage2 header must be exactly 48 bytes");
_Static_assert(sizeof(usbip_devlist_device_t) == 312,
               "USB/IP device-list entry must be exactly 312 bytes");
_Static_assert(sizeof(usbip_devlist_interface_t) == 4,
               "USB/IP interface entry must be exactly 4 bytes");
