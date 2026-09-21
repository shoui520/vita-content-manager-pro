#include "../mtp_server.h"
#include <assert.h>
#include <string.h>

static VcmMtpCommand request(uint16_t code, unsigned count, uint32_t a, uint32_t b, uint32_t c) {
    VcmMtpCommand command = {0};
    command.code = code;
    command.param_count = count;
    command.params[0] = a;
    command.params[1] = b;
    command.params[2] = c;
    return command;
}

int main(void) {
    _Static_assert(sizeof(VcmMtpCommand) == 0x24, "SceMtpIf command ABI size");
    VcmMtpEntry entries[8];
    VcmMtpCatalog catalog;
    assert(vcm_mtp_catalog_init(&catalog, entries, 8) == 0);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 144115192370823591ull,
                               "写真.jpg", 12345, 0x3801) == 4);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_VIDEO, 8, "Episode.mp4", 99, 0xb982) == 5);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_VIDEO, 9, "Huge.mp4", UINT64_C(0x100000000), 0xb982) == 0);
    VcmMtpServer server;
    vcm_mtp_server_init(&server, &catalog);
    vcm_mtp_server_set_storage(&server, UINT64_C(256) * 1024 * 1024 * 1024,
                               UINT64_C(123) * 1024 * 1024 * 1024);
    uint8_t payload[1024];
    VcmMtpResult reply;

    VcmMtpCommand cmd = request(0x1001, 0, 0, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.response.code == 0x2001 && reply.data_kind == VCM_MTP_DATA_MEMORY);
    cmd = request(0x1004, 0, 0, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.response.code == 0x2003);

    cmd = request(0x1002, 1, 7, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.response.code == 0x2001 && server.session == 7);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.response.code == 0x201e);

    cmd = request(0x1004, 0, 0, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.data_kind == VCM_MTP_DATA_MEMORY && reply.data_length == 8);
    assert(payload[4] == 1 && payload[6] == 1);
    cmd = request(0x1005, 1, VCM_MTP_STORAGE_ID, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.data_kind == VCM_MTP_DATA_MEMORY);
    assert(payload[4] == 1 && payload[5] == 0); /* still read-only */
    assert(payload[6] == 0 && payload[7] == 0 && payload[8] == 0 && payload[9] == 0 &&
           payload[10] == 64 && payload[11] == 0 && payload[12] == 0 && payload[13] == 0);

    cmd = request(0x1006, 3, VCM_MTP_STORAGE_ID, 0, VCM_MTP_ROOT_HANDLE);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.response.params[0] == 3 && reply.response.param_count == 1);
    cmd = request(0x1006, 3, VCM_MTP_STORAGE_ID, 0, VCM_MTP_ALL_HANDLES);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.response.params[0] == 5 && reply.response.param_count == 1);
    cmd = request(0x1007, 3, VCM_MTP_STORAGE_ID, 0, 3);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.data_kind == VCM_MTP_DATA_HANDLES && reply.data_length == 8);

    cmd = request(0x1008, 1, 4, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.data_kind == VCM_MTP_DATA_MEMORY && reply.data_length > 52);
    assert(memcmp(payload + 52, "\007", 1) == 0);
    cmd = request(0x1009, 1, 4, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.data_kind == VCM_MTP_DATA_OBJECT && reply.data_length == 12345);
    assert(reply.object_handle == 4);
    cmd = request(0x1009, 1, 1, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(reply.response.code == 0x2009);
    cmd = request(0x1003, 0, 0, 0, 0);
    assert(vcm_mtp_server_execute(&server, &cmd, &reply, payload, sizeof(payload)) == 0);
    assert(server.session == 0);
    return 0;
}
