// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef UKE_PEN_POWER_H
#define UKE_PEN_POWER_H

#include <stdint.h>

#define UKE_PEN_POWER_PROPERTY "UkePenPowerState"

// One PMIC GLINK snapshot. A field is -1 when firmware did not provide it.
struct UkePenPowerState {
    int32_t docked;
    int32_t percent;
    int32_t charging;
    uint8_t address[6];
    uint8_t address_valid;
    uint8_t reserved;
};

#endif
