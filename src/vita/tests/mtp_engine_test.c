#include "../mtp_engine.h"
#include <assert.h>
#include <string.h>

typedef struct {
    uint32_t sent;
    uint32_t total;
    uint32_t left;
    uint16_t response;
    unsigned data_calls;
    unsigned closed;
} Fake;

static int send_data(void *context, const VcmMtpCommand *command, const void *bytes,
                     uint32_t count, uint32_t offset, uint32_t total) {
    Fake *fake = context;
    assert(command && bytes && count && offset == fake->sent);
    if (!fake->data_calls) fake->total = total;
    assert(total == fake->total);
    fake->sent += count;
    ++fake->data_calls;
    return 0;
}
static int send_response(void *context, const VcmMtpCommand *response) {
    Fake *fake = context;
    fake->response = response->code;
    return 0;
}
static int open_object(void *context, const VcmMtpEntry *entry) {
    Fake *fake = context;
    fake->left = (uint32_t)entry->size;
    return 0;
}
static int read_object(void *context, void *bytes, uint32_t capacity) {
    Fake *fake = context;
    if (capacity > fake->left) capacity = fake->left;
    memset(bytes, 'Q', capacity);
    fake->left -= capacity;
    return (int)capacity;
}
static void close_object(void *context) { ((Fake *)context)->closed++; }
static int read_thumb(void *context, const VcmMtpEntry *entry,
                      void *bytes, uint32_t capacity) {
    (void)context;
    assert(entry->kind == VCM_MTP_PHOTO && capacity >= 54);
    memset(bytes, 0, 54);
    return 54;
}
static VcmMtpCommand command(uint16_t code, uint32_t parameter) {
    VcmMtpCommand result = {0};
    result.code = code;
    if (parameter) { result.param_count = 1; result.params[0] = parameter; }
    return result;
}

int main(void) {
    VcmMtpEntry entries[8];
    VcmMtpCatalog catalog;
    assert(vcm_mtp_catalog_init(&catalog, entries, 8) == 0);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 17, "A.jpg", 12345, 0x3801) == 4);
    entries[3].thumb_size = 54;
    VcmMtpServer server;
    vcm_mtp_server_init(&server, &catalog);
    const VcmMtpTransport transport = {
        send_data, send_response, open_object, read_object, close_object, read_thumb
    };
    Fake fake = {0};
    uint8_t scratch[4096], thumb[64];
    VcmMtpCommand cmd = command(0x1001, 0);
    assert(vcm_mtp_engine_run(&server, &cmd, &transport, &fake, scratch,
                              sizeof(scratch), thumb, sizeof(thumb)) == 0);
    assert(fake.response == 0x2001 && fake.sent == fake.total && fake.sent > 64);

    memset(&fake, 0, sizeof(fake));
    cmd = command(0x1002, 1);
    assert(vcm_mtp_engine_run(&server, &cmd, &transport, &fake, scratch,
                              sizeof(scratch), thumb, sizeof(thumb)) == 0);
    assert(fake.response == 0x2001 && fake.data_calls == 0);

    memset(&fake, 0, sizeof(fake));
    cmd = command(0x1009, 4);
    assert(vcm_mtp_engine_run(&server, &cmd, &transport, &fake, scratch,
                              sizeof(scratch), thumb, sizeof(thumb)) == 0);
    assert(fake.response == 0x2001 && fake.sent == 12345 && fake.data_calls == 4);
    assert(fake.closed == 1 && fake.left == 0);

    memset(&fake, 0, sizeof(fake));
    cmd = command(0x100a, 4);
    assert(vcm_mtp_engine_run(&server, &cmd, &transport, &fake, scratch,
                              sizeof(scratch), thumb, sizeof(thumb)) == 0);
    assert(fake.response == 0x2001 && fake.sent == 54);

    memset(&fake, 0, sizeof(fake));
    cmd = command(0x1007, 0);
    cmd.param_count = 3;
    cmd.params[0] = VCM_MTP_STORAGE_ID;
    cmd.params[1] = 0;
    cmd.params[2] = 1;
    assert(vcm_mtp_engine_run(&server, &cmd, &transport, &fake, scratch,
                              sizeof(scratch), thumb, sizeof(thumb)) == 0);
    assert(fake.response == 0x2001 && fake.sent == 8);
    return 0;
}
