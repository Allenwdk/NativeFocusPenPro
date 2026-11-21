// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "piano-pen-frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { SpiPrefix = 257, FrameBytes = 5433, PenBytes = 3960, MatrixWords = 480 };

int main(int argc, char **argv)
{
    assert(argc > 1);
    const struct piano_pen_geometry geometry = {12, 40, 60, 8, 2400};
    struct piano_pen_frame full, compact;
    assert(!piano_pen_frame_init(&full, &geometry));
    assert(!piano_pen_frame_init(&compact, &geometry));
    unsigned accepted = 0, salvaged = 0, rejected = 0;
    for (int i = 1; i < argc; ++i) {
        unsigned char bytes[FrameBytes];
        FILE *source = fopen(argv[i], "rb");
        assert(source);
        const size_t length = fread(bytes, 1, sizeof bytes, source);
        assert(length == FrameBytes || length == SpiPrefix + PenBytes);
        assert(fgetc(source) == EOF && !ferror(source));
        fclose(source);
        unsigned char *payload = bytes + SpiPrefix;
        const int old_record = piano_pen_frame_decode(&full, payload, length - SpiPrefix, NULL);
        const int short_read = piano_pen_frame_decode(&compact, payload, PenBytes, NULL);
        assert((old_record == 0) == (short_read == 0));
        if (short_read) {
            ++rejected;
            continue;
        }
        ++accepted;
        if (length == FrameBytes && !full.outer_end) ++salvaged;
        assert(!compact.outer_end && !compact.part_mask && !compact.frame[0x165]);
        for (unsigned j = 0; j < 4; ++j)
            assert(!memcmp(full.matrices[j], compact.matrices[j], MatrixWords * sizeof(int16_t)));
        assert(piano_pen_frame_decode(&compact, payload, PenBytes - 1, NULL));
        assert(piano_pen_frame_decode(&compact, payload, PenBytes + 1, NULL));
        payload[100] ^= 1;
        assert(piano_pen_frame_decode(&compact, payload, PenBytes, NULL));
        payload[100] ^= 1;
        payload[12] ^= 1;
        // The compact SPI read excludes the hand quarter, so its outer
        // checksum is intentionally unavailable. The inner pen checksum
        // remains authoritative.
        assert(!piano_pen_frame_decode(&compact, payload, PenBytes, NULL));
    }
    piano_pen_frame_destroy(&compact);
    piano_pen_frame_destroy(&full);
    assert(accepted && rejected);
    printf("PASS same-board pen replay: %u accepted, %u recovered from hand corruption, %u rejected\n",
           accepted, salvaged, rejected);
    return 0;
}
