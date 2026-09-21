#include "mtp_server.h"
#include "mtp_wire.h"

enum {
    MTP_OK=0x2001,
    MTP_GENERAL_ERROR=0x2002,
    MTP_SESSION_NOT_OPEN=0x2003,
    MTP_OPERATION_NOT_SUPPORTED=0x2005,
    MTP_INVALID_STORAGE_ID=0x2008,
    MTP_INVALID_OBJECT_HANDLE=0x2009,
    MTP_INVALID_PARAMETER=0x201d,
    MTP_SESSION_ALREADY_OPEN=0x201e
};

static int storage_valid(uint32_t id) {
    return id == VCM_MTP_STORAGE_ID || id == 0xffffffffu;
}

static void set_response(VcmMtpResult *result, const VcmMtpCommand *command, uint16_t code) {
    result->response.code = code;
    result->response.reserved = 0;
    result->response.session = command->session;
    result->response.transaction = command->transaction;
    result->response.param_count = 0;
    for (unsigned i = 0; i < 5; ++i) result->response.params[i] = 0;
    result->data_kind = VCM_MTP_DATA_NONE;
    result->data_length = 0;
    result->object_handle = 0;
    result->parent = 0;
    result->format = 0;
}

void vcm_mtp_server_init(VcmMtpServer *server, const VcmMtpCatalog *catalog) {
    server->session = 0;
    server->catalog = catalog;
    server->storage_capacity = 0;
    server->storage_free = 0;
}

void vcm_mtp_server_set_storage(VcmMtpServer *server, uint64_t capacity, uint64_t free_bytes) {
    if (!server) return;
    server->storage_capacity = capacity;
    server->storage_free = free_bytes <= capacity ? free_bytes : 0;
}

int vcm_mtp_server_execute(VcmMtpServer *server, const VcmMtpCommand *command,
                           VcmMtpResult *result, uint8_t *payload, uint32_t capacity) {
    if (!server || !server->catalog || !command || !result) return -1;
    set_response(result, command, MTP_OK);
    if (command->param_count > 5) { result->response.code = MTP_INVALID_PARAMETER; return 0; }
    VcmMtpWriter writer;
    vcm_mtp_writer_init(&writer, payload, capacity);
    if (command->code == 0x1001) { /* GetDeviceInfo: legal before OpenSession. */
        if (vcm_mtp_device_info(&writer) < 0) return -1;
        result->data_kind = VCM_MTP_DATA_MEMORY;
        result->data_length = writer.length;
        return 0;
    }
    if (command->code == 0x1002) { /* OpenSession */
        if (command->param_count != 1 || !command->params[0]) result->response.code = MTP_INVALID_PARAMETER;
        else if (server->session) result->response.code = MTP_SESSION_ALREADY_OPEN;
        else server->session = command->params[0];
        return 0;
    }
    if (!server->session) { result->response.code = MTP_SESSION_NOT_OPEN; return 0; }
    if (command->code == 0x1003) { /* CloseSession */
        server->session = 0;
        return 0;
    }
    if (command->code == 0x1004) { /* GetStorageIDs */
        const uint32_t id = VCM_MTP_STORAGE_ID;
        if (vcm_mtp_u32_array(&writer, &id, 1) < 0) return -1;
        result->data_kind = VCM_MTP_DATA_MEMORY;
        result->data_length = writer.length;
        return 0;
    }
    if (command->code == 0x1005) { /* GetStorageInfo */
        if (command->param_count < 1 || command->params[0] != VCM_MTP_STORAGE_ID) {
            result->response.code = MTP_INVALID_STORAGE_ID; return 0;
        }
        if (vcm_mtp_storage_info(&writer, server->storage_capacity,
                                 server->storage_free) < 0) return -1;
        result->data_kind = VCM_MTP_DATA_MEMORY;
        result->data_length = writer.length;
        return 0;
    }
    if (command->code == 0x9801) { /* GetObjectPropsSupported(format) */
        if (command->param_count < 1 ||
            vcm_mtp_object_props_supported(&writer, (uint16_t)command->params[0]) < 0)
            return -1;
        result->data_kind = VCM_MTP_DATA_MEMORY;
        result->data_length = writer.length;
        return 0;
    }
    if (command->code == 0x9802) { /* GetObjectPropDesc(code, format) */
        if (command->param_count < 2 ||
            vcm_mtp_object_prop_desc(&writer, (uint16_t)command->params[0],
                                     (uint16_t)command->params[1]) < 0) {
            result->response.code = MTP_OPERATION_NOT_SUPPORTED;
            return 0;
        }
        result->data_kind = VCM_MTP_DATA_MEMORY;
        result->data_length = writer.length;
        return 0;
    }
    if (command->code == 0x9803) { /* GetObjectPropValue(handle, code) */
        if (command->param_count < 2) { result->response.code = MTP_INVALID_PARAMETER; return 0; }
        const VcmMtpEntry *entry = vcm_mtp_catalog_get(server->catalog, command->params[0]);
        if (!entry) { result->response.code = MTP_INVALID_OBJECT_HANDLE; return 0; }
        if (vcm_mtp_object_prop_value(&writer, entry, (uint16_t)command->params[1]) < 0) {
            result->response.code = MTP_OPERATION_NOT_SUPPORTED;
            return 0;
        }
        result->data_kind = VCM_MTP_DATA_MEMORY;
        result->data_length = writer.length;
        return 0;
    }
    if (command->code == 0x1006 || command->code == 0x1007) { /* NumObjects/Handles */
        if (command->param_count < 3 || !storage_valid(command->params[0])) {
            result->response.code = MTP_INVALID_STORAGE_ID; return 0;
        }
        uint16_t format = (uint16_t)command->params[1];
        uint32_t parent = command->params[2];
        if (parent != VCM_MTP_ALL_HANDLES && parent != VCM_MTP_ROOT_HANDLE &&
            (!vcm_mtp_catalog_get(server->catalog, parent) || parent > 3)) {
            result->response.code = MTP_INVALID_OBJECT_HANDLE; return 0;
        }
        uint32_t count = vcm_mtp_catalog_children(server->catalog, parent, format, 0, 0);
        if (command->code == 0x1006) {
            result->response.params[0] = count;
            result->response.param_count = 1;
        } else if (count <= (0xffffffffu - 4) / 4) {
            result->data_kind = VCM_MTP_DATA_HANDLES;
            result->data_length = 4 + 4 * count;
            result->parent = parent;
            result->format = format;
        } else result->response.code = MTP_GENERAL_ERROR;
        return 0;
    }
    if (command->code == 0x1008 || command->code == 0x1009 || command->code == 0x100a) {
        if (command->param_count < 1) { result->response.code = MTP_INVALID_PARAMETER; return 0; }
        const VcmMtpEntry *entry = vcm_mtp_catalog_get(server->catalog, command->params[0]);
        if (!entry) { result->response.code = MTP_INVALID_OBJECT_HANDLE; return 0; }
        /* Standard MTP GetObject uses a 32-bit USB container length. Never
         * advertise a truncated size for a file this responder cannot send. */
        if (entry->size > UINT32_MAX - 12u) {
            result->response.code = MTP_GENERAL_ERROR;
            return 0;
        }
        if (command->code == 0x1008) { /* GetObjectInfo */
            VcmMtpObjectInfo info = {VCM_MTP_STORAGE_ID, entry->format,
                                     (uint32_t)entry->size, entry->parent,
                                     entry->display_name, entry->created,
                                     entry->modified, entry->thumb_size,
                                     entry->thumb_width, entry->thumb_height,
                                     entry->width, entry->height};
            if (vcm_mtp_object_info(&writer, &info) < 0) return -1;
            result->data_kind = VCM_MTP_DATA_MEMORY;
            result->data_length = writer.length;
            return 0;
        }
        if (entry->kind == VCM_MTP_FOLDER) {
            result->response.code = MTP_INVALID_OBJECT_HANDLE; return 0;
        }
        if (command->code == 0x100a && !entry->thumb_size) {
            result->response.code = MTP_OPERATION_NOT_SUPPORTED; return 0;
        }
        result->data_kind = command->code == 0x100a ? VCM_MTP_DATA_THUMB : VCM_MTP_DATA_OBJECT;
        result->data_length = command->code == 0x100a ? 0 : (uint32_t)entry->size;
        result->object_handle = entry->handle;
        return 0;
    }
    result->response.code = MTP_OPERATION_NOT_SUPPORTED;
    return 0;
}
