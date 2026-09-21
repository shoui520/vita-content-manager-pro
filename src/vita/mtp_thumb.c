#include "mtp_thumb.h"

static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value); put16(p + 2, (uint16_t)(value >> 16));
}
static void unpack565(uint16_t color, uint8_t rgb[3]) {
    unsigned r = (color >> 11) & 31, g = (color >> 5) & 63, b = color & 31;
    rgb[0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[1] = (uint8_t)((g << 2) | (g >> 4));
    rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

int vcm_mtp_dxt1_dds_to_bmp(const uint8_t *dds, uint32_t dds_size,
                            uint8_t *bmp, uint32_t bmp_capacity) {
    if (!dds || !bmp || dds_size < 128 || read32(dds) != 0x20534444 ||
        read32(dds + 4) != 124 || read32(dds + 76) != 32 ||
        read32(dds + 84) != 0x31545844) return -1;
    uint32_t width = read32(dds + 16), height = read32(dds + 12);
    if (!width || !height || width > 256 || height > 256 ||
        (width & 3) || (height & 3) || dds_size != 128 + width * height / 2)
        return -1;
    uint32_t stride = (width * 3 + 3) & ~3u;
    uint32_t size = 54 + stride * height;
    if (bmp_capacity < size) return -1;
    for (uint32_t i = 0; i < size; ++i) bmp[i] = 0;
    bmp[0] = 'B'; bmp[1] = 'M';
    put32(bmp + 2, size); put32(bmp + 10, 54);
    put32(bmp + 14, 40); put32(bmp + 18, width); put32(bmp + 22, height);
    put16(bmp + 26, 1); put16(bmp + 28, 24); put32(bmp + 34, stride * height);
    const uint8_t *blocks = dds + 128;
    for (uint32_t by = 0; by < height / 4; ++by) {
        for (uint32_t bx = 0; bx < width / 4; ++bx) {
            const uint8_t *block = blocks + (by * (width / 4) + bx) * 8;
            uint16_t c0 = read16(block), c1 = read16(block + 2);
            uint32_t selectors = read32(block + 4);
            uint8_t colors[4][3] = {{0}};
            unpack565(c0, colors[0]); unpack565(c1, colors[1]);
            for (unsigned channel = 0; channel < 3; ++channel) {
                if (c0 > c1) {
                    colors[2][channel] = (uint8_t)((2 * colors[0][channel] + colors[1][channel]) / 3);
                    colors[3][channel] = (uint8_t)((colors[0][channel] + 2 * colors[1][channel]) / 3);
                } else colors[2][channel] = (uint8_t)((colors[0][channel] + colors[1][channel]) / 2);
            }
            for (unsigned y = 0; y < 4; ++y) {
                for (unsigned x = 0; x < 4; ++x) {
                    unsigned selector = (selectors >> (2 * (y * 4 + x))) & 3;
                    unsigned py = by * 4 + y, px = bx * 4 + x;
                    uint8_t *out = bmp + 54 + (height - 1 - py) * stride + px * 3;
                    out[0] = colors[selector][2];
                    out[1] = colors[selector][1];
                    out[2] = colors[selector][0];
                }
            }
        }
    }
    return (int)size;
}
