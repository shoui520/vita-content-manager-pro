#ifndef VCM_MTP_THUMB_H
#define VCM_MTP_THUMB_H
#include <stdint.h>

/* Converts a native DXT1 AVContent icon (up to 256x256) to 24-bit BMP. */
int vcm_mtp_dxt1_dds_to_bmp(const uint8_t *dds, uint32_t dds_size,
                            uint8_t *bmp, uint32_t bmp_capacity);
#endif
