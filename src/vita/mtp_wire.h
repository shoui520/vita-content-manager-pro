#ifndef VCM_MTP_WIRE_H
#define VCM_MTP_WIRE_H

#include <stdint.h>
#include "mtp_catalog.h"

/* MTP/PTP data sets are little-endian, including their UTF-16 strings. */
typedef struct {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
    int error;
} VcmMtpWriter;

typedef struct {
    uint32_t storage_id;
    uint16_t format;
    uint32_t size;
    uint32_t parent;
    const char *name;
    const char *created;
    const char *modified;
    uint32_t thumb_size;
    uint16_t thumb_width;
    uint16_t thumb_height;
    uint32_t width;
    uint32_t height;
} VcmMtpObjectInfo;

void vcm_mtp_writer_init(VcmMtpWriter *writer, uint8_t *data, uint32_t capacity);
int vcm_mtp_device_info(VcmMtpWriter *writer);
int vcm_mtp_storage_info(VcmMtpWriter *writer, uint64_t capacity, uint64_t free_bytes);
int vcm_mtp_object_info(VcmMtpWriter *writer, const VcmMtpObjectInfo *object);
int vcm_mtp_u32_array(VcmMtpWriter *writer, const uint32_t *values, uint32_t count);
int vcm_mtp_object_props_supported(VcmMtpWriter *writer, uint16_t format);
int vcm_mtp_object_prop_desc(VcmMtpWriter *writer, uint16_t code, uint16_t format);
int vcm_mtp_object_prop_value(VcmMtpWriter *writer, const VcmMtpEntry *entry, uint16_t code);

#endif
