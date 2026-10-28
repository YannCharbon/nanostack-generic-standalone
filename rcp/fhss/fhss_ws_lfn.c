/*
 * Copyright (c) 2026, the mesh-emulator-rework authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LFN unicast listen schedule arithmetic: see fhss_ws_lfn.h.
 */
#include <stdbool.h>
#include <stdint.h>
#include "fhss_ws_lfn.h"

bool fhss_lfn_sched_from_lutt(fhss_lfn_sched_t *s, uint32_t interval_ms, uint16_t slot_number,
                              uint32_t interval_offset_ms, uint32_t rx_timestamp_us)
{
    uint64_t interval_us = (uint64_t)interval_ms * 1000;

    if (!interval_us || interval_us > FHSS_LFN_INTERVAL_MAX_US || interval_offset_ms >= interval_ms) {
        return false;
    }
    s->interval_us = (uint32_t)interval_us;
    s->anchor_us = rx_timestamp_us - interval_offset_ms * 1000u;
    s->anchor_slot = slot_number;
    return true;
}

// Slot index of t relative to the anchor (floor, negative before it) and the
// time since that slot's start. The signed 32-bit difference covers times up
// to 2^31 µs either side of the anchor.
static int32_t fhss_lfn_sched_index(const fhss_lfn_sched_t *s, uint32_t t, uint32_t *rem_us)
{
    int32_t d = (int32_t)(t - s->anchor_us);
    int32_t n;

    if (d >= 0) {
        n = (int32_t)((uint32_t)d / s->interval_us);
    } else {
        n = -(int32_t)(((uint32_t)(-(int64_t)d) + s->interval_us - 1) / s->interval_us);
    }
    *rem_us = (uint32_t)((int64_t)d - (int64_t)n * s->interval_us);
    return n;
}

uint32_t fhss_lfn_sched_next(const fhss_lfn_sched_t *s, uint32_t t, uint16_t *slot)
{
    uint32_t rem_us;
    int32_t n = fhss_lfn_sched_index(s, t, &rem_us);

    if (rem_us) {
        n++;
    }
    if (slot) {
        *slot = (uint16_t)(s->anchor_slot + n);
    }
    return s->anchor_us + (uint32_t)((int64_t)n * s->interval_us);
}

uint16_t fhss_lfn_sched_slot_at(const fhss_lfn_sched_t *s, uint32_t t, uint32_t *offset_us)
{
    uint32_t rem_us;
    int32_t n = fhss_lfn_sched_index(s, t, &rem_us);

    if (offset_us) {
        *offset_us = rem_us;
    }
    return (uint16_t)(s->anchor_slot + n);
}

void fhss_lfn_sched_reanchor(fhss_lfn_sched_t *s, uint32_t t)
{
    uint32_t rem_us;
    int32_t n = fhss_lfn_sched_index(s, t, &rem_us);

    s->anchor_us = t - rem_us;
    s->anchor_slot = (uint16_t)(s->anchor_slot + n);
}
