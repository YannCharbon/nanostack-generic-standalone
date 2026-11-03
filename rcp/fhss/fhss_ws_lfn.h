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
 * Long intervals (phase B1): the interval is 64-bit and may reach the Wi-SUN
 * maximum, 2^24 ms (about 4.66 h, FHSS_LFN_INTERVAL_MAX_US). The anchor is a
 * point on the 32-bit clock with the phase of the schedule there
 * (anchor_phase_us: the time since the start of slot anchor_slot), so t only
 * needs to be within 2^31 µs of the anchor, whatever the interval. With an
 * interval up to FHSS_LFN_SHORT_INTERVAL_US (2^30 µs) the anchor is kept at
 * a slot start (phase 0), as before. fhss_lfn_sched_reanchor() moves the
 * anchor forward to keep it recent; a slot start more than 2^31 µs away is
 * given as a 64-bit delay (fhss_lfn_sched_next_delta()).
 */
#ifndef FHSS_WS_LFN_H_
#define FHSS_WS_LFN_H_

#include <stdbool.h>
#include <stdint.h>

/** Longest listen interval, in µs: the Wi-SUN maximum, 2^24 - 1 ms (the
 *  LUS-IE and LTO-IE carry 24 bits of ms). */
#define FHSS_LFN_INTERVAL_MAX_US (0xffffffULL * 1000)
/** Intervals up to this keep the anchor at a slot start (phase 0). */
#define FHSS_LFN_SHORT_INTERVAL_US 0x40000000u

/** LFN: TX queue poll period while frames wait (restart after a failure). */
#define FHSS_LFN_QUEUE_POLL_US 20000u

typedef struct fhss_lfn_sched {
    uint64_t interval_us;   /**< Listen interval; 0: no schedule */
    uint32_t anchor_us;     /**< A time in slot anchor_slot (its start when anchor_phase_us is 0) */
    uint64_t anchor_phase_us; /**< Time from the start of slot anchor_slot to anchor_us (< interval_us) */
    uint16_t anchor_slot;   /**< Slot number at anchor_us */
} fhss_lfn_sched_t;

/** Schedule from a LUS-IE (interval, ms) and a LUTT-IE (slot, offset in ms)
 *  received at rx_timestamp_us. Returns false if the interval is 0 or above
 *  the limit, or if the offset is not inside the interval. */
bool fhss_lfn_sched_from_lutt(fhss_lfn_sched_t *s, uint32_t interval_ms, uint16_t slot_number,
                              uint32_t interval_offset_ms, uint32_t rx_timestamp_us);

/** The same with the offset in µs (a re-anchored reference, exact). */
bool fhss_lfn_sched_from_lutt_us(fhss_lfn_sched_t *s, uint32_t interval_ms, uint16_t slot_number,
                                 uint64_t interval_offset_us, uint32_t rx_timestamp_us);

/** Delay from t to the first slot that starts at or after t (0 if one starts
 *  at t), 64-bit; its slot number to *slot (if not NULL). */
uint64_t fhss_lfn_sched_next_delta(const fhss_lfn_sched_t *s, uint32_t t, uint16_t *slot);

/** The slot whose start is nearest to t; *delta_us (if not NULL): t minus
 *  that start (negative before it). */
uint16_t fhss_lfn_sched_nearest(const fhss_lfn_sched_t *s, uint32_t t, int64_t *delta_us);

/** The schedule moved later by delta_us (earlier if negative). */
void fhss_lfn_sched_shift(fhss_lfn_sched_t *s, int32_t delta_us);

/** First slot that starts at or after t (t may be before the anchor).
 *  Returns its start time and writes its slot number to *slot (if not NULL).
 *  The time wraps if the start is more than 2^31 µs away: see
 *  fhss_lfn_sched_next_delta(). */
uint32_t fhss_lfn_sched_next(const fhss_lfn_sched_t *s, uint32_t t, uint16_t *slot);

/** Slot that contains t (the last slot started at or before t). Writes the
 *  time since its start to *offset_us (if not NULL). */
uint16_t fhss_lfn_sched_slot_at(const fhss_lfn_sched_t *s, uint32_t t, uint64_t *offset_us);

/** Same schedule, anchored at the start of the slot that contains t. */
void fhss_lfn_sched_reanchor(fhss_lfn_sched_t *s, uint32_t t);

/** LTO-IE applied to a schedule (LFN timing offset, FAN 1.1; docs/LFN_MAC.md
 *  §2.1): with n the slot that contains ref_t and s_n its start, the grid
 *  becomes s_n + offset_us + k * interval_us (interval_us 0: unchanged), with
 *  slot n at s_n + offset_us. A grid point still ahead of ref_t, such as
 *  slot n itself when offset_us is larger than the time since s_n, is a slot
 *  like any other (both ends compute the same grid). */
void fhss_lfn_sched_lto(fhss_lfn_sched_t *s, uint32_t ref_t, uint64_t offset_us, uint64_t interval_us);

/** The same from the slot whose start is nearest to ref_t (a frame sent at
 *  a slot start, or received in a window opened before it). */
void fhss_lfn_sched_lto_nearest(fhss_lfn_sched_t *s, uint32_t ref_t, uint64_t offset_us, uint64_t interval_us);

/** Whether [t, t + duration_us) overlaps a broadcast dwell of a schedule whose
 *  intervals start at interval_start_us + k * interval_us and begin with a
 *  dwell of dwell_us. If so, *resume_us is the end of that dwell (the first
 *  time a frame of this duration can start). A frame that cannot fit between
 *  two dwells is never reported as overlapping. */
bool fhss_lfn_dwell_overlap(uint32_t interval_start_us, uint32_t interval_us, uint32_t dwell_us,
                            uint32_t t, uint32_t duration_us, uint32_t *resume_us);

#endif /* FHSS_WS_LFN_H_ */
