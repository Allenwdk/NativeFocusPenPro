// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../UkePenRadioProtocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t host[] = {0x12, 0x23, 0x34, 0x45, 0x56, 0x67};
static const uint8_t firstConfig[] = {
    0x30, 0x1e, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x03, 0x02,
    0xa0, 0x1e, 0x3e, 0x17, 0x1a, 0x04, 0x1a, 0x03,
    0x00, 0x00, 0x0d, 0x68, 0x00, 0x00, 0x05, 0x28,
    0x00, 0x00, 0x0d, 0x68, 0xf4, 0x01
};

static void advance(UkePenRadioProtocol *protocol, unsigned limit) {
    static const uint8_t opcodes[] = {
        0x61, 0x52, 0x00, 0x01, 0x02, 0x03, 0x04, 0x30, 0x31, 0x32,
        0x04, 0x30, 0x31, 0x32, 0x33, 0x51, 0x5f
    };
    static const uint8_t payloadSizes[] = {
        0, 1, 1, 2, 2, 3, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1
    };
    for (unsigned i = protocol->step; i < limit; ++i) {
        uint8_t packet[UKE_P81C_CONFIG_BYTES];
        const size_t len = uke_pen_radio_request(protocol, host, 0x69abcdef, packet);
        assert(len >= 2 && len == (size_t)packet[1] + 2 && packet[0] == opcodes[i]);
        if (i == 7 || i == 11) {
            uint8_t expected[sizeof firstConfig];
            memcpy(expected, firstConfig, sizeof expected);
            if (i == 11) {
                expected[9] = 5;
                expected[17] = 1;
            }
            assert(len == sizeof expected && !memcmp(packet, expected, len));
        }
        if (i == 8 || i == 12) {
            const uint8_t expected[] = {0x31, 8, i == 8 ? 2 : 5, 3, 0x9b, 0x6f, 0xef, 0x43, 0x28, 0};
            assert(len == sizeof expected && !memcmp(packet, expected, len));
        }
        if (i == 9 || i == 13) {
            const uint8_t expected[] = {0x32, 3, i == 9 ? 2 : 5, 0x28, 0};
            assert(len == sizeof expected && !memcmp(packet, expected, len));
        }
        if (i == 14) {
            const uint8_t expected[] = {0x33, 4, 0x69, 0xab, 0xcd, 0xef};
            assert(len == sizeof expected && !memcmp(packet, expected, len));
        }
        if (i == 0 || i == 15 || i == 16) assert(len == 3 && packet[2] == 1);
        const uint8_t ack[] = {opcodes[i] | 0x80, payloadSizes[i], i == 6 ? 2 : i == 16 ? 1 : 5, 0, 0};
        assert(uke_pen_radio_accept(protocol, ack, (size_t)ack[1] + 2) == UkePenRadioAccepted);
    }
}

int main(void) {
    UkePenRadioProtocol protocol = {0};
    uint8_t packet[UKE_P81C_CONFIG_BYTES];
    advance(&protocol, 17);
    assert(protocol.slots[0] == 2 && protocol.slots[1] == 5);
    assert(uke_pen_radio_request(&protocol, host, 0, packet) == 0);

    protocol = (UkePenRadioProtocol){0};
    advance(&protocol, 6);
    const uint8_t unrelated[] = {0x53, 1, 1};
    const uint8_t truncated[] = {0x84, 1};
    const uint8_t exhausted[] = {0x84, 1, 0xff};
    const uint8_t duplicate[] = {0x84, 1, 2};
    assert(uke_pen_radio_accept(&protocol, unrelated, sizeof unrelated) == UkePenRadioUnrelated);
    assert(uke_pen_radio_accept(&protocol, truncated, sizeof truncated) == UkePenRadioInvalid);
    assert(uke_pen_radio_accept(&protocol, exhausted, sizeof exhausted) == UkePenRadioNoSlot);
    assert(protocol.step == 6);
    advance(&protocol, 10);
    assert(uke_pen_radio_accept(&protocol, duplicate, sizeof duplicate) == UkePenRadioNoSlot);
    assert(protocol.step == 10);
    assert(!uke_pen_radio_skip_double_tap(&protocol));
    advance(&protocol, 16);
    const uint8_t disabled[] = {0xdf, 1, 0};
    assert(uke_pen_radio_accept(&protocol, disabled, sizeof disabled) == UkePenRadioInvalid);
    assert(uke_pen_radio_skip_double_tap(&protocol));
    assert(uke_pen_radio_request(&protocol, host, 0, packet) == 0);
    puts("PASS: Uke P81c FE11/FE12, slot configuration and optional double tap");
    return 0;
}
