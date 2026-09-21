#include "../src/vita/id3_tags.h"

#include <assert.h>
#include <string.h>

static size_t add_text(unsigned char *tag, size_t at, const char id[4],
                       const char *value) {
    size_t length = strlen(value) + 1;
    memcpy(tag + at, id, 4);
    tag[at + 4] = (unsigned char)(length >> 24);
    tag[at + 5] = (unsigned char)(length >> 16);
    tag[at + 6] = (unsigned char)(length >> 8);
    tag[at + 7] = (unsigned char)length;
    tag[at + 8] = tag[at + 9] = 0;
    tag[at + 10] = 0;
    memcpy(tag + at + 11, value, length - 1);
    return at + 10 + length;
}

int main(void) {
    unsigned char tag[512] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
    size_t end = 10;
    end = add_text(tag, end, "TIT2", "Example Track");
    end = add_text(tag, end, "TPE1", "Example Artist");
    end = add_text(tag, end, "TALB", "Example Album");
    end = add_text(tag, end, "TCON", "Example Genre");
    end = add_text(tag, end, "TRCK", "2/12");
    size_t size = end - 10;
    tag[6] = (unsigned char)((size >> 21) & 0x7f);
    tag[7] = (unsigned char)((size >> 14) & 0x7f);
    tag[8] = (unsigned char)((size >> 7) & 0x7f);
    tag[9] = (unsigned char)(size & 0x7f);

    VcmId3Tags parsed;
    assert(vcm_parse_id3v23(tag, end, &parsed) == 0);
    assert(!strcmp(parsed.title, "Example Track"));
    assert(!strcmp(parsed.artist, "Example Artist"));
    assert(!strcmp(parsed.album, "Example Album"));
    assert(!strcmp(parsed.album_artist, parsed.artist));
    assert(!strcmp(parsed.genre, "Example Genre"));
    assert(parsed.track == 2);
    return 0;
}
