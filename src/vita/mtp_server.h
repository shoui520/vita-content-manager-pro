#ifndef VCM_MTP_SERVER_H
#define VCM_MTP_SERVER_H

#include "mtp_catalog.h"
#include <stddef.h>
#include <stdint.h>

/* Firmware SceMtpIf RecvCommand/SendResponse use 0x24-byte records. */
typedef struct {
    uint16_t code;
    uint16_t reserved;
    uint32_t session;
    uint32_t transaction;
    uint32_t params[5];
    uint32_t param_count;
} VcmMtpCommand;

_Static_assert(sizeof(VcmMtpCommand) == 0x24, "SceMtpIf command/response ABI");
_Static_assert(offsetof(VcmMtpCommand, transaction) == 8, "MTP transaction offset");
_Static_assert(offsetof(VcmMtpCommand, param_count) == 0x20, "MTP parameter count offset");

enum {
    VCM_MTP_DATA_NONE,
    VCM_MTP_DATA_MEMORY,
    VCM_MTP_DATA_HANDLES,
    VCM_MTP_DATA_OBJECT,
    VCM_MTP_DATA_THUMB
};

typedef struct {
    VcmMtpCommand response;
    int data_kind;
    uint32_t data_length;
    uint32_t object_handle;
    uint32_t parent;
    uint16_t format;
} VcmMtpResult;

typedef struct {
    uint32_t session;
    const VcmMtpCatalog *catalog;
    uint64_t storage_capacity;
    uint64_t storage_free;
} VcmMtpServer;

void vcm_mtp_server_init(VcmMtpServer *server, const VcmMtpCatalog *catalog);
void vcm_mtp_server_set_storage(VcmMtpServer *server, uint64_t capacity, uint64_t free_bytes);
/* Reply data, if any, is written into payload or described by data_kind. */
int vcm_mtp_server_execute(VcmMtpServer *server, const VcmMtpCommand *command,
                           VcmMtpResult *result, uint8_t *payload, uint32_t capacity);

#endif
