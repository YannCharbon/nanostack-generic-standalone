/*
 * Copyright (c) 2026, the mesh-emulator-rework authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LFN unicast listen schedule arithmetic: see fhss_ws_lfn.h.
 */
#include <stdbool.h>
#include <stdint.h>
#include "fhss_ws_lfn.h"

// A short interval keeps the anchor at a slot start (phase 0)
static void fhss_lfn_sched_normalize(fhss_lfn_sched_t *s)
{
    if (s->anchor_phase_us && s->interval_us <= FHSS_LFN_SHORT_INTERVAL_US) {
        s->anchor_us -= (uint32_t)s->anchor_phase_us;
        s->anchor_phase_us = 0;
    }
}

bool fhss_lfn_sched_from_lutt_us(fhss_lfn_sched_t *s, uint32_t interval_ms, uint16_t slot_number,
                                 uint64_t interval_offset_us, uint32_t rx_timestamp_us)
{
    uint64_t interval_us = (uint64_t)interval_ms * 1000;

    if (!interval_us || interval_us > FHSS_LFN_INTERVAL_MAX_US || interval_offset_us >= interval_us) {
        return false;
    }
    s->interval_us = interval_us;
    s->anchor_us = rx_timestamp_us;
    s->anchor_phase_us = interval_offset_us;
    s->anchor_slot = slot_number;
    fhss_lfn_sched_normalize(s);
    return true;
}

bool fhss_lfn_sched_from_lutt(fhss_lfn_sched_t *s, uint32_t interval_ms, uint16_t slot_number,
                              uint32_t interval_offset_ms, uint32_t rx_timestamp_us)
{
    if (interval_offset_ms >= interval_ms) {
        return false;
    }
    return fhss_lfn_sched_from_lutt_us(s, interval_ms, slot_number, (uint64_t)interval_offset_ms * 1000, rx_timestamp_us);
}

// Slot index of t relative to the anchor's slot (floor, negative before it)
// and the time since that slot's start. t within 2^31 µs of the anchor.
static int64_t fhss_lfn_sched_index(const fhss_lfn_sched_t *s, uint32_t t, uint64_t *rem_us)
{
    int64_t pos = (int64_t)s->anchor_phase_us + (int32_t)(t - s->anchor_us);
    int64_t iv = (int64_t)s->interval_us;
    int64_t n = pos >= 0 ? pos / iv : -((-pos + iv - 1) / iv);

    *rem_us = (uint64_t)(pos - n * iv);
    return n;
}

uint64_t fhss_lfn_sched_next_delta(const fhss_lfn_sched_t *s, uint32_t t, uint16_t *slot)
{
    uint64_t rem_us;
    int64_t n = fhss_lfn_sched_index(s, t, &rem_us);
    uint64_t d = 0;

    if (rem_us) {
        n++;
        d = s->interval_us - rem_us;
    }
    if (slot) {
        *slot = (uint16_t)(s->anchor_slot + n);
    }
    return d;
}

uint32_t fhss_lfn_sched_next(const fhss_lfn_sched_t *s, uint32_t t, uint16_t *slot)
{
    return t + (uint32_t)fhss_lfn_sched_next_delta(s, t, slot);
}

uint16_t fhss_lfn_sched_slot_at(const fhss_lfn_sched_t *s, uint32_t t, uint64_t *offset_us)
{
    uint64_t rem_us;
    int64_t n = fhss_lfn_sched_index(s, t, &rem_us);

    if (offset_us) {
        *offset_us = rem_us;
    }
    return (uint16_t)(s->anchor_slot + n);
}

uint16_t fhss_lfn_sched_nearest(const fhss_lfn_sched_t *s, uint32_t t, int64_t *delta_us)
{
    uint64_t rem_us;
    int64_t n = fhss_lfn_sched_index(s, t, &rem_us);
    int64_t d = (int64_t)rem_us;

    if (rem_us > s->interval_us / 2) {
        n++;
        d -= (int64_t)s->interval_us;
    }
    if (delta_us) {
        *delta_us = d;
    }
    return (uint16_t)(s->anchor_slot + n);
}

void fhss_lfn_sched_reanchor(fhss_lfn_sched_t *s, uint32_t t)
{
    uint64_t rem_us;
    int64_t n = fhss_lfn_sched_index(s, t, &rem_us);

    s->anchor_us = t;
    s->anchor_phase_us = rem_us;
    s->anchor_slot = (uint16_t)(s->anchor_slot + n);
    fhss_lfn_sched_normalize(s);
}

void fhss_lfn_sched_shift(fhss_lfn_sched_t *s, int32_t delta_us)
{
    // Later by delta: at the anchor time, the phase is delta less
    int64_t pos = (int64_t)s->anchor_phase_us - delta_us;
    int64_t iv = (int64_t)s->interval_us;
    while (pos < 0) {
        pos += iv;
        s->anchor_slot--;
    }
    while (pos >= iv) {
        pos -= iv;
        s->anchor_slot++;
    }
    s->anchor_phase_us = (uint64_t)pos;
    fhss_lfn_sched_normalize(s);
}

static void fhss_lfn_sched_lto_from(fhss_lfn_sched_t *s, uint32_t ref_t, int64_t n, int64_t rem, uint64_t offset_us, uint64_t interval_us);

void fhss_lfn_sched_lto_nearest(fhss_lfn_sched_t *s, uint32_t ref_t, uint64_t offset_us, uint64_t interval_us)
{
    uint64_t rem_us;
    int64_t n = fhss_lfn_sched_index(s, ref_t, &rem_us);
    int64_t rem = (int64_t)rem_us;

    if (rem_us > s->interval_us / 2) {
        n++;
        rem -= (int64_t)s->interval_us;
    }
    fhss_lfn_sched_lto_from(s, ref_t, n, rem, offset_us, interval_us);
}

void fhss_lfn_sched_lto(fhss_lfn_sched_t *s, uint32_t ref_t, uint64_t offset_us, uint64_t interval_us)
{
    uint64_t rem_us;
    int64_t n = fhss_lfn_sched_index(s, ref_t, &rem_us);

    fhss_lfn_sched_lto_from(s, ref_t, n, (int64_t)rem_us, offset_us, interval_us);
}

// Slot n (rem from its start to ref_t, possibly negative) moved to start
// offset_us later, the interval changed
static void fhss_lfn_sched_lto_from(fhss_lfn_sched_t *s, uint32_t ref_t, int64_t n, int64_t rem, uint64_t offset_us, uint64_t interval_us)
{

    // Slot n now starts at s_n + offset (s_n = ref_t - rem): at ref_t its
    // phase is rem - offset, possibly before its start (slot n - 1 there)
    if (interval_us) {
        s->interval_us = interval_us;
    }
    int64_t pos = rem - (int64_t)offset_us;
    int64_t iv = (int64_t)s->interval_us;
    while (pos < 0) {
        pos += iv;
        n--;
    }
    while (pos >= iv) {
        pos -= iv;
        n++;
    }
    s->anchor_us = ref_t;
    s->anchor_phase_us = (uint64_t)pos;
    s->anchor_slot = (uint16_t)(s->anchor_slot + n);
    fhss_lfn_sched_normalize(s);
}

bool fhss_lfn_dwell_overlap(uint32_t interval_start_us, uint32_t interval_us, uint32_t dwell_us,
                            uint32_t t, uint32_t duration_us, uint32_t *resume_us)
{
    uint32_t phase;

    if (!interval_us || !dwell_us || dwell_us >= interval_us || duration_us > interval_us - dwell_us) {
        return false;
    }
    // Phase of t in the broadcast interval (interval_start_us may be after t)
    phase = (uint32_t)(((int64_t)(int32_t)(t - interval_start_us) % (int64_t)interval_us + interval_us) % interval_us);
    if (phase < dwell_us) {
        *resume_us = t + (dwell_us - phase);
        return true;
    }
    if (phase + duration_us > interval_us) {
        *resume_us = t + (interval_us - phase) + dwell_us;
        return true;
    }
    return false;
}
