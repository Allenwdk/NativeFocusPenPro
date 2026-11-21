// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef UKE_PEN_RADIO_PROTOCOL_H
#define UKE_PEN_RADIO_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define UKE_P81C_CONFIG_BYTES 32
#define UKE_P81C_ADDRESS_BYTES 6

enum UkePenRadioReply {
    UkePenRadioUnrelated,
    UkePenRadioAccepted,
    UkePenRadioInvalid,
    UkePenRadioNoSlot
};

typedef struct {
    unsigned step;
    uint8_t slots[2];
} UkePenRadioProtocol;

size_t uke_pen_radio_request(const UkePenRadioProtocol *protocol,
                             const uint8_t address[UKE_P81C_ADDRESS_BYTES],
                             uint32_t unixSeconds, uint8_t out[UKE_P81C_CONFIG_BYTES]);
enum UkePenRadioReply uke_pen_radio_accept(UkePenRadioProtocol *protocol,
                                           const uint8_t *reply, size_t length);
int uke_pen_radio_skip_double_tap(UkePenRadioProtocol *protocol);

#endif
