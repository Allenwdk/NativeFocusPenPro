// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "UkePenRadioProtocol.h"

#include <string.h>

// Uke BluetoothExtension's default 120 Hz configuration, corroborated by
// Project-SunUEFI's live Piano FE11/FE12 transcript. Slot IDs come from the pen.
static const struct {
    uint8_t opcode;
    uint8_t replyPayloadBytes;
} kSteps[] = {
    {0x61, 0}, {0x52, 1}, {0x00, 1}, {0x01, 2}, {0x02, 2}, {0x03, 3},
    {0x04, 1}, {0x30, 0}, {0x31, 0}, {0x32, 0},
    {0x04, 1}, {0x30, 0}, {0x31, 0}, {0x32, 0},
    {0x33, 0}, {0x51, 0}, {0x5f, 1},
};

enum {
    kStepSecondSlot = 10,
    kStepCount = sizeof(kSteps) / sizeof(kSteps[0])
};

size_t uke_pen_radio_request(const UkePenRadioProtocol *protocol,
                             const uint8_t address[UKE_P81C_ADDRESS_BYTES],
                             uint32_t unixSeconds, uint8_t out[UKE_P81C_CONFIG_BYTES]) {
    if (protocol->step >= kStepCount) return 0;
    const uint8_t opcode = kSteps[protocol->step].opcode;
    const unsigned cycle = protocol->step >= kStepSecondSlot;
    const uint8_t slot = protocol->slots[cycle];
    out[0] = opcode;
    out[1] = 0;
    switch (opcode) {
        case 0x61:  // Screen on.
        case 0x51:  // Enable the pen after configuring both radio slots.
        case 0x5f:  // Enable body double tap, as Piano does.
            out[1] = 1;
            out[2] = 1;
            break;
        case 0x30: {
            static const uint8_t timing[] = {0xa0, 0x1e, 0x3e, 0x17, 0x1a, 0x04, 0x1a};
            static const uint8_t intervals[] = {
                0x00, 0x00, 0x0d, 0x68, 0x00, 0x00, 0x05, 0x28, 0x00, 0x00, 0x0d, 0x68, 0xf4, 0x01
            };
            out[1] = UKE_P81C_CONFIG_BYTES - 2;
            memcpy(out + 2, address, UKE_P81C_ADDRESS_BYTES);
            out[8] = 3;
            out[9] = slot;
            memcpy(out + 10, timing, sizeof timing);
            out[17] = cycle ? 1 : 3;
            memcpy(out + 18, intervals, sizeof intervals);
            break;
        }
        case 0x31: {
            static const uint8_t frequency[] = {0x03, 0x9b, 0x6f, 0xef, 0x43, 0x28, 0x00};
            out[1] = 8;
            out[2] = slot;
            memcpy(out + 3, frequency, sizeof frequency);
            break;
        }
        case 0x32:
            out[1] = 3;
            out[2] = slot;
            out[3] = 0x28;
            out[4] = 0;
            break;
        case 0x33:
            out[1] = 4;
            for (unsigned i = 0; i < 4; ++i) out[2 + i] = (uint8_t)(unixSeconds >> (24 - 8 * i));
            break;
    }
    return (size_t)out[1] + 2;
}

enum UkePenRadioReply uke_pen_radio_accept(UkePenRadioProtocol *protocol,
                                           const uint8_t *reply, size_t length) {
    if (protocol->step >= kStepCount || !length) return UkePenRadioUnrelated;
    const uint8_t opcode = kSteps[protocol->step].opcode;
    const uint8_t expected = kSteps[protocol->step].replyPayloadBytes;
    if (reply[0] != (opcode | 0x80)) return UkePenRadioUnrelated;
    if (length != (size_t)expected + 2 || reply[1] != expected) return UkePenRadioInvalid;
    if (opcode == 0x5f && reply[2] != 1) return UkePenRadioInvalid;
    if (opcode == 0x04) {
        const unsigned cycle = protocol->step >= kStepSecondSlot;
        if (reply[2] == 0xff || (cycle && protocol->slots[0] == reply[2])) return UkePenRadioNoSlot;
        protocol->slots[cycle] = reply[2];
    }
    ++protocol->step;
    return UkePenRadioAccepted;
}

int uke_pen_radio_skip_double_tap(UkePenRadioProtocol *protocol) {
    if (protocol->step != kStepCount - 1) return 0;
    ++protocol->step;
    return 1;
}
