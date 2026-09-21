#include "../mtp_thumb.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    uint8_t dds[8320] = {0};
    uint8_t bmp[49206];
    memcpy(dds, "DDS ", 4);
    dds[4] = 124; dds[12] = 128; dds[16] = 128; dds[76] = 32;
    memcpy(dds + 84, "DXT1", 4);
    for (unsigned i = 128; i < sizeof(dds); i += 8) {
        dds[i] = 0x00; dds[i+1] = 0xf8; /* red */
        dds[i+2] = 0x1f; dds[i+3] = 0x00; /* blue */
    }
    assert(vcm_mtp_dxt1_dds_to_bmp(dds, sizeof(dds), bmp, sizeof(bmp)) == 49206);
    assert(bmp[0] == 'B' && bmp[1] == 'M');
    assert(bmp[54] == 0 && bmp[55] == 0 && bmp[56] == 255); /* BMP BGR red */
    assert(vcm_mtp_dxt1_dds_to_bmp(dds, sizeof(dds), bmp, 49205) == -1);
    dds[84] = 'B';
    assert(vcm_mtp_dxt1_dds_to_bmp(dds, sizeof(dds), bmp, sizeof(bmp)) == -1);
    return 0;
}
