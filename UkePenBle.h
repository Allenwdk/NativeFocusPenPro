// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef UKE_PEN_BLE_H
#define UKE_PEN_BLE_H

#include <stdint.h>

#define UKE_PEN_BLE_SYSCTL "kern.uke_pen_ble"
#define UKE_PEN_BLE_ABI 3
#define UKE_PEN_BLE_REPORT_COUNT 3
#define UKE_PEN_BLE_REPORT_BYTES 21

struct UkePenBleReport {
    uint64_t absolute_ticks;
    uint64_t continuous_ns;
    uint8_t length;
    uint8_t bytes[UKE_PEN_BLE_REPORT_BYTES];
};

struct UkePenBleSnapshot {
    uint32_t version;
    uint32_t generation;
    uint32_t connected;
    uint32_t reserved;
    uint64_t sender_id;
    uint64_t device_id;
    struct UkePenBleReport reports[UKE_PEN_BLE_REPORT_COUNT];
};

// Shared by replay and the driver. Reports may include their ID depending on
// the origin of the data, but all bridge reports are emitted with the ID.
static inline unsigned
uke_pen_ble_body_size(unsigned id)
{
    switch (id) {
        case 2: return 1;
        case 5: return 14;
        case 6: return 20;
        default: return 0;
    }
}

#ifdef KERNEL
#ifdef __cplusplus
extern "C" {
#endif
int uke_pen_ble_snapshot(struct UkePenBleSnapshot *result);
#ifdef __cplusplus
}
#endif
#endif
#endif
