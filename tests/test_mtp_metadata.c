#include "../src/vita/mtp_catalog.h"
#include "../src/vita/mtp_wire.h"
#include "../src/vita/mtp_server.h"
#include <assert.h>
#include <string.h>

static unsigned le32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 |
           (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

int main(void) {
    VcmMtpEntry entries[4];
    VcmMtpCatalog catalog;
    assert(vcm_mtp_catalog_init(&catalog, entries, 4) == 0);
    assert(entries[0].created[0] == 0 && !strcmp(entries[0].display_name, "Photo"));
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_MUSIC, 7, "raw.mp3", 101, 0x3009) == 4);
    VcmMtpEntry *entry = &entries[3];
    strcpy(entry->display_name, "Beautiful title.mp3");
    strcpy(entry->title, "Beautiful title");
    strcpy(entry->artist, "Artist");
    strcpy(entry->album, "Album");
    strcpy(entry->created, "20260921T020304");
    strcpy(entry->modified, "20260921T030405");

    unsigned char data[4096];
    VcmMtpWriter writer;
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_device_info(&writer) > 0);
    assert(memmem(data, writer.length, "\xf0\x9f", 2)); /* private queue publish */
    assert(memmem(data, writer.length, "\xf5\x9f", 2)); /* private cancel */
    assert(!memmem(data, writer.length, "\x0c\x10", 2)); /* standard SendObjectInfo */
    assert(!memmem(data, writer.length, "\x0d\x10", 2)); /* standard SendObject */
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    VcmMtpObjectInfo info = {VCM_MTP_STORAGE_ID, entry->format, 101, 2,
                             entry->display_name, entry->created, entry->modified, 0, 0, 0, 0, 0};
    assert(vcm_mtp_object_info(&writer, &info) > 0);
    assert(le32(data + 8) == 101);
    assert(memmem(data, writer.length, "B\0e\0a\0u\0t\0i\0f\0u\0l\0", 18));

    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_props_supported(&writer, entry->format) > 0);
    assert(le32(data) == 13);
    assert(memmem(data, writer.length, "\x41\xdc", 2));
    assert(memmem(data, writer.length, "\x4f\xdc", 2));
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_prop_desc(&writer, 0xdc41, entry->format) > 0);
    assert(data[2] == 0x0a && data[3] == 0x00);
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_prop_value(&writer, entry, 0xdc41) == 16);
    assert(le32(data) == 7 && le32(data + 8) == VCM_MTP_MUSIC);
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_prop_value(&writer, entry, 0xdc4f) == 1 && data[0] == 0);
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_prop_desc(&writer, 0xdc46, entry->format) > 0);
    assert(data[0] == 0x46 && data[1] == 0xdc && data[2] == 0xff && data[3] == 0xff);
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_prop_value(&writer, entry, 0xdc46) > 0);
    assert(data[0] == 7 && data[1] == 'A' && data[2] == 0);
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_prop_value(&writer, entry, 0xdc9a) > 0);
    assert(data[0] == 6 && data[1] == 'A' && data[2] == 0);

    VcmMtpServer server;
    vcm_mtp_server_init(&server, &catalog);
    server.session = 1;
    VcmMtpCommand command = {0};
    command.code = 0x9803;
    command.param_count = 2;
    command.params[0] = 4;
    command.params[1] = 0xdc44;
    VcmMtpResult result;
    assert(vcm_mtp_server_execute(&server, &command, &result, data, sizeof(data)) == 0);
    assert(result.response.code == 0x2001 && result.data_length > 0);

    entry->format = 0x3801;
    entry->kind = VCM_MTP_PHOTO;
    entry->width = 6000;
    entry->height = 4000;
    entry->thumb_width = entry->thumb_height = 128;
    entry->thumb_size = 54 + 128 * 128 * 3;
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_prop_value(&writer, entry, 0xdc87) == 4);
    assert(le32(data) == 6000);
    info.format = 0x3801;
    info.thumb_size = entry->thumb_size;
    info.thumb_width = info.thumb_height = 128;
    info.width = 6000;
    info.height = 4000;
    vcm_mtp_writer_init(&writer, data, sizeof(data));
    assert(vcm_mtp_object_info(&writer, &info) > 0);
    assert(data[12] == 0x04 && data[13] == 0x38); /* BMP thumbnail format */
    assert(le32(data + 14) == entry->thumb_size);
    return 0;
}
