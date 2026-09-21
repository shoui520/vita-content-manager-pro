#include "../mtp_wire.h"
#include <assert.h>
#include <stdint.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }

int main(void) {
    uint8_t data[1024];
    VcmMtpWriter w;
    vcm_mtp_writer_init(&w, data, sizeof(data));
    assert(vcm_mtp_device_info(&w) > 0);
    assert(le16(data) == 100);
    assert(le32(data + 2) == 6);
    assert(data[8] == 21); /* microsoft.com: 1.0;  plus UTF-16 NUL */

    vcm_mtp_writer_init(&w, data, sizeof(data));
    assert(vcm_mtp_storage_info(&w, 1000, 500) > 0);
    assert(le16(data) == 3);
    assert(le16(data + 4) == 1); /* file-manager view is read-only */

    const VcmMtpObjectInfo japanese = {
        .storage_id = 0x10001,
        .format = 0x3801,
        .size = 345,
        .parent = 1,
        .name = "写真.jpg",
    };
    vcm_mtp_writer_init(&w, data, sizeof(data));
    assert(vcm_mtp_object_info(&w, &japanese) > 0);
    assert(le32(data) == 0x10001);
    assert(le16(data + 4) == 0x3801);
    assert(le32(data + 8) == 345);
    assert(le32(data + 38) == 1);
    assert(data[52] == 7); /* two Kanji, period, three ASCII, NUL */
    assert(le16(data + 53) == 0x5199);
    assert(le16(data + 55) == 0x771f);

    uint32_t handles[] = {1, 2, 0xdeadbeef};
    vcm_mtp_writer_init(&w, data, sizeof(data));
    assert(vcm_mtp_u32_array(&w, handles, 3) == 16);
    assert(le32(data) == 3 && le32(data + 12) == 0xdeadbeef);

    vcm_mtp_writer_init(&w, data, 8);
    assert(vcm_mtp_device_info(&w) < 0);
    assert(w.length <= 8);
    return 0;
}
