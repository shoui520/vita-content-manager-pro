#ifndef VCM_MTP_ENGINE_H
#define VCM_MTP_ENGINE_H

#include "mtp_server.h"

/* A transport-independent command runner. The USB adapter owns the port and
 * implements these callbacks; this layer never knows about ux0 paths. */
typedef struct {
    int (*send_data)(void *context, const VcmMtpCommand *command,
                     const void *bytes, uint32_t count, uint32_t offset, uint32_t total);
    int (*send_response)(void *context, const VcmMtpCommand *response);
    int (*open_object)(void *context, const VcmMtpEntry *entry);
    int (*read_object)(void *context, void *bytes, uint32_t capacity);
    void (*close_object)(void *context);
    int (*read_thumb)(void *context, const VcmMtpEntry *entry,
                      void *bytes, uint32_t capacity);
} VcmMtpTransport;

/* Scratch needs >=4096 bytes. Thumbnail scratch needs >=196662 bytes. */
int vcm_mtp_engine_run(VcmMtpServer *server, const VcmMtpCommand *command,
                       const VcmMtpTransport *transport, void *context,
                       uint8_t *scratch, uint32_t scratch_capacity,
                       uint8_t *thumbnail, uint32_t thumbnail_capacity);

#endif
