/*
 * Copyright (c) 2026, the mesh-emulator-rework authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LFN unicast listen schedule (Wi-SUN FAN 1.1, GS8 stage 2): slot arithmetic
 * on the stack's 32-bit µs clock, shared by the LFN (its own schedule) and by
 * its parent (the LFN's schedule, learnt from LUS-IE and LUTT-IE). No state,
 * no timers: unit-tested on its own (tests/fhss_lfn/).
 *
 * Model (docs/LFN_MAC.md §2): slot n of a schedule starts at
 *     start(n) = anchor_us + (n - anchor_slot) * interval_us
 * and the LFN listens around the start of every slot. A LUTT-IE received at
 * time rx_ts carries the sender's slot number and the offset into that slot
 * (ms), so anchor_us = rx_ts - offset, anchor_slot = slot. Slot numbers are
 * 16 bits on the air and wrap.
 *
 * Limits: interval_us <= FHSS_LFN_INTERVAL_MAX_US (2^31 µs, about 35.8 min),
 * and t must be within 2^31 µs of the anchor, because the clock is 32 bits
 * (LFN study M11, 64-bit time, is a later item). fhss_lfn_sched_reanchor()
 * moves the anchor forward to keep it recent.
 */
#ifndef FHSS_WS_LFN_H_
#define FHSS_WS_LFN_H_

#include <stdbool.h>
#include <stdint.h>

/** Longest listen interval the 32-bit clock allows, in µs (2^31 - 1). */
#define FHSS_LFN_INTERVAL_MAX_US 0x7fffffffu

/** LFN: TX queue poll period while frames wait (restart after a failure). */
#define FHSS_LFN_QUEUE_POLL_US 20000u

typedef struct fhss_lfn_sched {
    uint32_t interval_us;   /**< Listen interval; 0: no schedule */
    uint32_t anchor_us;     /**< Start time of slot anchor_slot */
    uint16_t anchor_slot;   /**< Slot number that starts at anchor_us */
} fhss_lfn_sched_t;

/** Schedule from a LUS-IE (interval, ms) and a LUTT-IE (slot, offset in ms)
 *  received at rx_timestamp_us. Returns false if the interval is 0 or above
 *  the limit, or if the offset is not inside the interval. */
bool fhss_lfn_sched_from_lutt(fhss_lfn_sched_t *s, uint32_t interval_ms, uint16_t slot_number,
                              uint32_t interval_offset_ms, uint32_t rx_timestamp_us);

/** First slot that starts at or after t (t may be before the anchor).
 *  Returns its start time and writes its slot number to *slot (if not NULL). */
uint32_t fhss_lfn_sched_next(const fhss_lfn_sched_t *s, uint32_t t, uint16_t *slot);

/** Slot that contains t (the last slot started at or before t). Writes the
 *  time since its start to *offset_us (if not NULL). */
uint16_t fhss_lfn_sched_slot_at(const fhss_lfn_sched_t *s, uint32_t t, uint32_t *offset_us);

/** Same schedule, anchored at the start of the slot that contains t. */
void fhss_lfn_sched_reanchor(fhss_lfn_sched_t *s, uint32_t t);

/** LTO-IE applied to a schedule (LFN timing offset, FAN 1.1; docs/LFN_MAC.md
 *  §2.1): with n the slot that contains ref_t and s_n its start, the grid
 *  becomes s_n + offset_us + k * interval_us (interval_us 0: unchanged), with
 *  slot n at s_n + offset_us. A grid point still ahead of ref_t, such as
 *  slot n itself when offset_us is larger than the time since s_n, is a slot
 *  like any other (both ends compute the same grid). */
void fhss_lfn_sched_lto(fhss_lfn_sched_t *s, uint32_t ref_t, uint32_t offset_us, uint32_t interval_us);

/** Whether [t, t + duration_us) overlaps a broadcast dwell of a schedule whose
 *  intervals start at interval_start_us + k * interval_us and begin with a
 *  dwell of dwell_us. If so, *resume_us is the end of that dwell (the first
 *  time a frame of this duration can start). A frame that cannot fit between
 *  two dwells is never reported as overlapping. */
bool fhss_lfn_dwell_overlap(uint32_t interval_start_us, uint32_t interval_us, uint32_t dwell_us,
                            uint32_t t, uint32_t duration_us, uint32_t *resume_us);

#endif /* FHSS_WS_LFN_H_ */
