// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "UkePenFrames.h"
#include "UkePenBle.h"
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include <kern/kalloc.h>

static int
sysctl_uke_pen_frame SYSCTL_HANDLER_ARGS
{
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    if (req->newptr) return EPERM;
    if (!req->oldptr) return SYSCTL_OUT(req, NULL, sizeof(struct UkePenFrame));
    if (req->oldlen < sizeof(struct UkePenFrame)) return ENOMEM;
    struct UkePenFrame *record = kalloc_data(sizeof(*record), Z_WAITOK);
    if (!record) return ENOMEM;
    const size_t length = uke_pen_frame_take(record);
    const int result = length ? SYSCTL_OUT(req, record, length) : 0;
    kfree_data(record, sizeof(*record));
    return result;
}

SYSCTL_PROC(_kern, OID_AUTO, uke_pen_frame,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, sysctl_uke_pen_frame, "S,UkePenFrame", "Uke pen digitizer frame");

static int
sysctl_uke_pen_hover SYSCTL_HANDLER_ARGS
{
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    struct UkePenHoverReport report;
    if (!req->newptr || req->newlen != sizeof(report)) return EINVAL;
    int error = SYSCTL_IN(req, &report, sizeof(report));
    return error ? error : uke_pen_hover_submit(&report);
}

SYSCTL_PROC(_kern, OID_AUTO, uke_pen_hover,
    CTLTYPE_OPAQUE | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, sysctl_uke_pen_hover, "S,UkePenHoverReport", "Uke pen hover position");

extern int uke_pen_ble_snapshot(struct UkePenBleSnapshot *) __attribute__((weak_import));

static int
sysctl_uke_pen_ble SYSCTL_HANDLER_ARGS
{
    if (!kauth_cred_issuser(kauth_cred_get()) || req->newptr) return EPERM;
    if (!&uke_pen_ble_snapshot) return ENODEV;
    struct UkePenBleSnapshot snapshot = {0};
    if (!req->oldptr) return SYSCTL_OUT(req, NULL, sizeof(snapshot));
    if (req->oldlen < sizeof(snapshot)) return ENOMEM;
    const int result = uke_pen_ble_snapshot(&snapshot);
    return result ? result : SYSCTL_OUT(req, &snapshot, sizeof(snapshot));
}

SYSCTL_PROC(_kern, OID_AUTO, uke_pen_ble,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, sysctl_uke_pen_ble, "S,UkePenBleSnapshot", "Uke P81c BLE reports");

extern bool native_hid_pen_take_haptic(void) __attribute__((weak_import));
extern void native_hid_pen_request_tap_haptic(void) __attribute__((weak_import));

static int
sysctl_uke_pen_haptic SYSCTL_HANDLER_ARGS
{
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    if (req->newptr) {
        uint32_t pulse;
        if (req->newlen != sizeof(pulse) || req->oldptr) return EINVAL;
        const int error = SYSCTL_IN(req, &pulse, sizeof(pulse));
        if (error) return error;
        if (pulse != 1) return EINVAL;
        if (!&native_hid_pen_request_tap_haptic) return ENODEV;
        native_hid_pen_request_tap_haptic();
        return 0;
    }
    if (!req->oldptr) return SYSCTL_OUT(req, NULL, sizeof(uint32_t));
    if (req->oldlen < sizeof(uint32_t)) return ENOMEM;
    const uint32_t pending = &native_hid_pen_take_haptic && native_hid_pen_take_haptic();
    return SYSCTL_OUT(req, &pending, sizeof(pending));
}

SYSCTL_PROC(_kern, OID_AUTO, uke_pen_haptic,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, sysctl_uke_pen_haptic, "IU", "Uke P81c system haptic request");
