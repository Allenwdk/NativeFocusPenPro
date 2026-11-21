// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef UKE_PEN_FRAMES_H
#define UKE_PEN_FRAMES_H

#include <stddef.h>
#include <stdint.h>

#define UKE_PEN_FRAME_SYSCTL "kern.uke_pen_frame"
#define UKE_PEN_HOVER_SYSCTL "kern.uke_pen_hover"
#define UKE_PEN_FRAME_CAPACITY 5433
#define UKE_PEN_FRAME_ABI 1
#define UKE_PEN_SPI_PREFIX 257

enum {
    UKE_PEN_TIP_FROM_PRESSURE = 0,
    UKE_PEN_TIP_HOVER = 1,
    UKE_PEN_TIP_CONTACT = 2,
};

struct UkePenFrame {
    uint32_t version;
    uint32_t generation;
    uint64_t sequence;
    uint64_t continuous_ns;
    uint32_t length;
    uint8_t bytes[UKE_PEN_FRAME_CAPACITY];
};

// Position, contact pressure and optional panel-axis tilt from the latest frame.
struct UkePenHoverReport {
    uint64_t sequence;
    uint32_t generation;
    uint16_t x, y;
    uint32_t in_range;
    uint16_t pressure, tip_state;
    int16_t tilt_x, tilt_y;
};

#ifdef KERNEL
#ifdef __cplusplus
extern "C" {
#endif
size_t uke_pen_frame_take(struct UkePenFrame *record);
int uke_pen_hover_submit(const struct UkePenHoverReport *report);
#ifdef __cplusplus
}
#endif
#endif

#endif
