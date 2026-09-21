#include "mtp_engine.h"

enum { MTP_OK = 0x2001, MTP_GENERAL_ERROR = 0x2002 };

static void le32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static int send_handles(const VcmMtpServer *server, const VcmMtpCommand *command,
                        const VcmMtpResult *result, const VcmMtpTransport *transport,
                        void *context, uint8_t *scratch, uint32_t capacity) {
    uint32_t expected = (result->data_length - 4) / 4;
    uint32_t offset = 0, used = 4, found = 0;
    le32(scratch, expected);
    for (uint32_t i = 0; i < server->catalog->count; ++i) {
        const VcmMtpEntry *entry = &server->catalog->entries[i];
        if ((result->parent != VCM_MTP_ALL_HANDLES && entry->parent !=
             (result->parent == VCM_MTP_ROOT_HANDLE ? VCM_MTP_NO_PARENT : result->parent)) ||
            (result->format && entry->format != result->format)) continue;
        if (capacity - used < 4) {
            if (transport->send_data(context, command, scratch, used, offset,
                                     result->data_length) < 0) return -1;
            offset += used;
            used = 0;
        }
        le32(scratch + used, entry->handle);
        used += 4;
        ++found;
    }
    if (found != expected) return -1;
    return transport->send_data(context, command, scratch, used, offset,
                                result->data_length);
}

static int send_object(const VcmMtpServer *server, const VcmMtpCommand *command,
                       const VcmMtpResult *result, const VcmMtpTransport *transport,
                       void *context, uint8_t *scratch, uint32_t capacity) {
    const VcmMtpEntry *entry = vcm_mtp_catalog_get(server->catalog, result->object_handle);
    if (!entry || !transport->open_object || !transport->read_object ||
        !transport->close_object || transport->open_object(context, entry) < 0) return -1;
    uint32_t offset = 0;
    int failure = 0;
    while (offset < result->data_length) {
        uint32_t wanted = result->data_length - offset;
        if (wanted > capacity) wanted = capacity;
        int got = transport->read_object(context, scratch, wanted);
        if (got <= 0 || (uint32_t)got > wanted ||
            transport->send_data(context, command, scratch, (uint32_t)got, offset,
                                 result->data_length) < 0) {
            failure = -1;
            break;
        }
        offset += (uint32_t)got;
    }
    transport->close_object(context);
    return failure;
}

int vcm_mtp_engine_run(VcmMtpServer *server, const VcmMtpCommand *command,
                       const VcmMtpTransport *transport, void *context,
                       uint8_t *scratch, uint32_t scratch_capacity,
                       uint8_t *thumbnail, uint32_t thumbnail_capacity) {
    if (!server || !command || !transport || !transport->send_data ||
        !transport->send_response || !scratch || scratch_capacity < 4096) return -1;
    VcmMtpResult result;
    if (vcm_mtp_server_execute(server, command, &result, scratch, scratch_capacity) < 0) {
        result.response = *command;
        result.response.code = MTP_GENERAL_ERROR;
        result.response.param_count = 0;
        result.data_kind = VCM_MTP_DATA_NONE;
    }
    if (result.response.code == MTP_OK) {
        int sent = 0;
        if (result.data_kind == VCM_MTP_DATA_MEMORY)
            sent = transport->send_data(context, command, scratch, result.data_length,
                                        0, result.data_length);
        else if (result.data_kind == VCM_MTP_DATA_HANDLES)
            sent = send_handles(server, command, &result, transport, context,
                                scratch, scratch_capacity);
        else if (result.data_kind == VCM_MTP_DATA_OBJECT)
            sent = send_object(server, command, &result, transport, context,
                               scratch, scratch_capacity);
        else if (result.data_kind == VCM_MTP_DATA_THUMB) {
            const VcmMtpEntry *entry = vcm_mtp_catalog_get(server->catalog,
                                                            result.object_handle);
            if (!entry || !transport->read_thumb || !thumbnail || !thumbnail_capacity)
                sent = -1;
            else {
                int size = transport->read_thumb(context, entry, thumbnail,
                                                 thumbnail_capacity);
                sent = size > 0 ? transport->send_data(context, command, thumbnail,
                    (uint32_t)size, 0, (uint32_t)size) : -1;
            }
        }
        if (sent < 0) result.response.code = MTP_GENERAL_ERROR;
    }
    return transport->send_response(context, &result.response);
}
