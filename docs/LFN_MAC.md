# Wi-SUN FAN 1.1 LFN unicast mechanics in the MAC and FHSS

GS8 stages 2, 4 and 5 (root repo: `documents/pre-study/investigation/LFN_FAN11_Study.md` §5, items M3–M6, M8–M10, E3, E4). This note describes what the MAC and the FHSS do for a Limited Function Node (LFN) and for its parent, the interfaces, the settings, the sources each rule comes from, and how it is tested. The upper layers (LLC, LFN state machine, LGTK, 6LoWPAN-ND for LFNs) are stage 3 and later: here the MAC and FHSS are driven through their APIs.

Tags: **[S]** a public source (listed in §7); **[M]** observed on Silicon Labs' LFN or parents (root repo, `HW5_BlackBox_Phase1_Results.md`; the channel and LTO rules were checked against the phase 1 captures in black-box phase 2, root repo `LFN_BlackBox_Phase2_3_Results.md`); **[I]** our inference.

## 1. Summary

- **Both roles are off by default.** With them off, the MAC and FHSS behave exactly as before: `dag_simple` logs are byte-identical to those of the previous commit for 10 seeds (§6).
- **LFN role** (`ns_fhss_ws_lfn_listen_set()`): the FHSS runs the node's unicast listen schedule instead of the FFN unicast hopping and broadcast schedule. Around the start of each slot it opens a receive window on the channel of the node's unicast channel function for that slot; the MAC turns the radio on and, at the end of the window, off (after a frame being received and its ACK). Outside the windows the radio is off except to transmit: TX, wait for the ACK, off. The FHSS writes the LUTT-IE (slot number and offset) of every frame at TX time.
- **Parent role** (`ns_fhss_ws_lfn_parent_set()`): an acknowledged unicast frame to a neighbour that has an LFN schedule is held in the MAC queue and released for the LFN's next slot. It is sent at the slot start plus an offset, on the LFN's channel for that slot, whatever the parent's own TX/RX slots and broadcast dwell. A failed attempt (no ACK, busy channel, late start) waits for the next slot; a frame that waits longer than its TTL is dropped as timed out.
- **One frame per window.** A window is a few ms long, so the parent sends at most one frame per LFN slot: the downlink capacity to one LFN is one frame per listen interval, and the latency up to one interval (Silicon Labs' parent behaves the same way [M]).
- **Every timing is a setting**, with defaults (§4). The 32-bit µs clock limits the listen interval to 2^31 µs (35.8 min); see §5.

## 2. The listen schedule [S, M, I]

A LUS-IE gives the listen interval *I* (24 bits, ms) [S: the codec in `ws_ie_lib.c`; M: the LUS of Silicon Labs' LFN reads 1 250 ms during EAPOL]. A LUTT-IE gives the sender's unicast slot number (16 bits) and the offset into that slot (24 bits) at the frame's TX time [S: `ws_ie_lib.c`]; the offset is in ms, like the interval [M: Silicon Labs' parents send to the LFN at the slot starts this gives, within 1–3 ms, 288 frames of the phase 1 captures]. Slot *n* starts at

    start(n) = anchor + (n - anchor_slot) × I

and the LFN listens around each slot start [M: every downlink frame of Silicon Labs' parent starts at the same phase of the LFN's grid, and the LFN wakes once per interval]. The parent anchors the schedule at `rx_timestamp - offset` with `anchor_slot = slot` (`rcp/fhss/fhss_ws_lfn.c`).

**Channel of a slot [M].** The LFN uses its unicast channel function (DH1CF, TR51 or fixed) with the slot number as the channel-sequence index, as an FFN does with its dwell-interval index; the parent computes the same from the LFN's channel plan (its LCP-IE). Confirmed: every unicast frame of Silicon Labs' parents to their LFN in the phase 1 captures (288 frames, `wsbrd` and the embedded border router) is on DH1CF(slot, LFN EUI-64) over the plan's allowed channels, the slot computed from the LFN's last LUTT-IE (tool: firmware `tests/hw/lfn_blackbox/chan_check.py`).

**Window.** The LFN listens in `[start(n) - rx_guard_us - w, start(n) - rx_guard_us + rx_window_us + w)`, where the widening `w` is `drift_ppm` µs per second since the last sync with the parent (§2.3), at most `widening_max_us`, and 0 before the first sync. The parent starts its frame at `start(n) + tx_offset_us`. Silicon Labs' LFN listens about 4 ms per wake and widens the window by about 0.11 ms per second since the last LTS [M].

**Offset rounding.** The LFN writes the offset rounded to the nearest ms, so the parent's anchor is within ±0.5 ms; with the default guard of 1 ms the frame still starts inside the window.

### 2.1 LTO-IE: timing offset and adjusted interval [S, M]

An LTO-IE carries an offset (24 bits, ms) and an adjusted listening interval (24 bits, ms) [S: `ws_ie_lib.c`, Wireshark's dissector]. `wisun-br-linux` computes the interval as a multiple or divisor of the LFN broadcast interval within the LFN's NR-IE range, and the offset as a random multiple of a 300 ms guard; its RCP converts the offset to one "relative to the destination LFN's unicast schedule" at TX time [S: `ws_neigh.c`, `HIF.md`]. What the LFN does with the offset is not public. Our reading, confirmed on Silicon Labs' LFN [M: 65 of the 71 LTO-IEs of the phase 1 captures, every one sent at a slot start (LPC, EAPOL) among them; the 6 others are LPAs sent far from a slot start, or frames of a group-key handshake]: with *n* the LFN's slot whose start *s_n* is nearest to the LTO frame, the grid becomes

    start(k) = s_n + offset + (k - n) × adjusted_interval

Both ends compute it: the LFN with `ns_fhss_ws_lfn_timing_set(api, offset_ms, interval_ms, rx_timestamp)`, the parent, once the LTO frame is acknowledged, with `ns_fhss_ws_lfn_neighbor_lto(api, eui64, offset_ms, interval_ms, tx_timestamp)`, which rewrites the neighbour's `lfn_uc_timing_info`. A grid point still ahead (slot *n* itself, when the offset is larger than the time since *s_n*) is a slot like any other. An open window stays open; the next one follows the new grid. The same call with offset 0 changes the interval alone, from the current slot (the LFN shortens it during EAPOL [M: Silicon Labs' LFN keeps slot *n*'s start and numbering and applies 1 250 ms from there]). The LFN's next LUTT/LUS re-synchronises the parent in any case. The slot numbering also follows *n*: the slot that contained the LTO frame may become slot *n* − 1 once *s_n* moves later.

### 2.2 The parent's LFN broadcast schedule [S, M, I]

The parent's LFN broadcast slots start every LFN broadcast interval *B* (LBS-IE, 10 s by default) and are numbered from 0, the first one *B* after `ns_fhss_ws_lfn_bc_set()`. Slot *k* is on the channel of the parent's broadcast channel function (its BS-IE) for slot number *k* and the LFN broadcast schedule identifier (LBS-IE) [M: all 57 broadcast LPC and LTS frames of the phase 1 captures are on DH1CF_bc(LBT slot, LBS BSI); the embedded border router uses an LBS BSI different from its BS-IE's, so the LBS-IE's value is the one to use]. An LBT-IE gives the slot number and the offset into it (ms) at the frame's TX time [S: `ws_ie_lib.c`, Wireshark's dissector; M: in the phase 1 captures the LBT-IEs of a parent's successive frames (LPA, EAPOL, LPC) give the same slot start within 1 ms, and an LTS reads offset 1 ms]; the FHSS writes it at TX time on every frame that has one, as it writes the BT-IE.

- **Frames for the slots.** The upper layer marks a broadcast frame `LfnBroadcast` (`mcps_data_req_t`): the MAC keeps it in the unicast queue and asks `lfn_tx_check` with no destination; it is released for the next slot as a frame to an LFN is for the LFN's window (§4), sent at slot start + `tx_offset_us` on the slot's channel (`tx_handle` with frame type `FHSS_LFN_BC_FRAME`), and a busy channel or a late start sends it back to the queue for the next slot.
- **LTO-IE offset [S, I].** `wisun-br-linux`'s RCP converts the LTO offset "relative to the destination LFN's unicast schedule" at TX time [S: `HIF.md`]. Here the upper layer gives the phase it wants for the LFN's slots against the broadcast slots (a multiple of 300 ms), and the FHSS writes the offset that puts the LFN's slot nearest to the frame at that phase, modulo the smaller of the new interval and *B* (§2.1). The acknowledgement applies the offset as written (`ns_fhss_ws_lfn_neighbor_lto()`).
- **32-bit clock.** The schedule is re-anchored near the current time by every use and by the FFN broadcast timer.

### 2.3 Sync and drift on the LFN [S, M, I]

`ns_fhss_ws_lfn_bc_sync()` takes the values of a frame from the parent with an LBT-IE (LPA, LPC, LTS) and the LBS-IE values the LFN keeps: the slot's start on the LFN's clock is the reception time minus the offset.

- **Which frames.** The LBT offset is in ms: a frame sent within a slot (LPA, unicast LPC) gives the slot start to ±0.5 ms, one sent at the slot start (LTS, broadcast LPC) to the TX timing. The first sync may come from any frame; after it, only frames with an offset of at most 2 ms correct the phase and measure the rate [I]. Silicon Labs' parents write 1 ms in their LTS and broadcast LPCs, ours 0 [M: black-box phases 1–2]; with "offset 0 only", as before, our LFN never re-synchronised under a Silicon Labs parent and its windows kept widening.
- **Rate.** The rate of the LFN's clock against the parent's is measured between a reference sync and the current one, at least one sync period apart and less than 2^30 µs (the 32-bit clock); a sync further away becomes the new reference and the rate is kept. Measured values beyond ±500 ppm are ignored. The parent's slots, on the LFN's clock, are *B* × (1 + rate) apart.
- **The LFN's own slots follow the parent's time base [I].** The parent's view of the LFN's unicast slots comes from the LFN's last LUTT-IE and runs on the parent's clock. At every sync the LFN moves its own slots by the error measured on the broadcast slot, and runs its listen interval on its clock at *I* × (1 + rate). Its slots then stay where the parent expects them between uplinks, however long it stays silent. Silicon Labs' LFN widens its unicast windows from its last LTS too, which fits the same model [M].
- **Windows.** The LFN listens in every parent broadcast slot (`bc_rx_window_us` from `rx_guard_us` before the slot start, widened like the unicast windows), or only in the slots that carry sync frames (slot number a multiple of the sync period) with `bc_sync_only` [M: Silicon Labs' LFN wakes in every slot, even with its "LTS only" setting]. When a unicast and a broadcast window meet, the one opened first keeps the radio and the other is skipped (counted); the parent's LTO phase keeps them at least 300 ms apart.
- **Stop.** `ns_fhss_ws_lfn_bc_stop()` (parent lost) stops the broadcast windows and the rate correction.

### 2.4 LND discovery [S, M, I]

After an LPAS sweep the LFN listens continuously during its discovery window, and each parent answers with an LPA in a random slot of it [S: the LND-IE; `wsbrd` `ws_mngt_lpa_schedule`; M: black-box items 4–5].

- **Discovery slot channel [M].** Slot *i* of the discovery window is on the LFN's unicast channel for slot number `discovery_first_slot + i`, the LND-IE's Discovery First Slot, which Silicon Labs' LFN draws anew for each sweep. Found in black-box phase 2: all 22 LPAs of the phase 1 captures (both Silicon Labs parents) are on DH1CF(first slot + *k*, LFN EUI-64) for their slot *k*; the LFN's own unicast slots (our stage 5 inference) match 1 of them.
- **LFN.** `ns_fhss_ws_lfn_discovery_set(api, start, duration, first_slot, slot_us)` opens the window at `start` for `duration` (the upper layer: `lnd_slots` × `lnd_slot_time_ms`) and retunes at each slot start to the channel of slot number `first_slot + i`; the upper layer draws `first_slot` per sweep and writes it in the LND-IE. With `slot_us` 0, the stage 5 behaviour: the channel of the LFN's own unicast slot nearest to the current time. The LFN's unicast windows wait meanwhile. The FHSS writes each frame's LND-IE response delay at TX time, from the frame to `start`, as Silicon Labs' LFN does [M item 4].
- **Parent.** The MAC sends a frame marked `LfnTxTimeSet` (`mcps_data_req_t`) at `LfnTxTime`: `fhss_api->lfn_tx_at` holds it until `tx_lead_us` before that time and drops it once the time has passed by 20 ms. With `LfnTxSlotSet`, `tx_handle` puts it (and its CCA, within 20 ms of the TX time) on the LFN's channel of slot number `LfnTxSlot`; without, on the LFN's channel of its slot nearest to the TX time. The upper layer picks the LND slot *k* and gives `discovery_first_slot + k` (`ws_lfn_parent.c`).

## 3. LFN role

### FHSS (`rcp/fhss/fhss_ws.c`, `fhss_ws_lfn.c`)

- `ns_fhss_ws_lfn_listen_set(api, &cfg)` stores the schedule; it starts at MLME-START (FHSS synchronized), or at once if the MAC already runs. Slot 0 starts `first_slot_delay_us` later. `NULL` stops it and restarts the FFN unicast hopping.
- While it runs: no FFN unicast hop timer, no FFN broadcast schedule (`ns_fhss_ws_set_parent()` is ignored), no periodic TX queue poll (a frame queued behind another goes from the MAC's TX confirm). Broadcast frames go at once on the current channel.
- One FHSS timer callback (`fhss_ws_lfn_timer_handler`) opens and closes the unicast windows, another (`fhss_ws_lfn_bc_timer_handler`) the broadcast windows (§2.3). A port needs at most four distinct FHSS timer callbacks in all (unicast, broadcast, LFN, LFN broadcast).
- A late timer skips the slots whose window has passed (counted in `windows_skipped`); the phase is kept.
- The own schedule is re-anchored at every window, so it runs indefinitely on the 32-bit clock.
- **Uplinks avoid the parent's broadcast dwell.** `ns_fhss_ws_set_parent()` on an LFN stores the parent's broadcast schedule (interval, dwell, BT-IE timing) without following it. A unicast frame whose airtime plus `uplink_guard_us` would meet a dwell is held: `check_tx_conditions` returns false, and at TX time `tx_handle` returns -3 (back to the queue); the FHSS polls the queue at the dwell's end (the event timer). Counted in `uplink_deferred`. A frame that cannot fit between two dwells is never held [I: an FFN does not listen on its unicast channel during its dwell, FAN 1.0].
- `write_synch_info`: if the frame has a LUTT-IE (Wi-SUN WH sub-ID 10), the FHSS writes the slot number and offset of the frame's TX time, as it writes the UFSI of a UTT-IE.

### MAC (`rcp/mac/ieee802154/`)

New FHSS → MAC callback `rx_window(api, event, channel)` (`fhss_api.h`, `mac_mlme_rx_window()`):

| Event | MAC |
| --- | --- |
| `FHSS_RX_WINDOWS_START` / `STOP` | Marks the MAC as running timed windows (`mac_rx_windows`) |
| `FHSS_RX_WINDOW_OPEN` | Channel change (unless a data frame or an ACK is being sent: the FHSS's TX done moves to the window's channel afterwards), radio on |
| `FHSS_RX_WINDOW_CLOSE` | Radio off. Deferred (returns 1, the FHSS asks again every `close_retry_us`) while a frame is being received or an ACK sent. During a data frame of its own, the TX confirm turns the radio off |

Other MAC changes, all inactive unless `mac_rx_windows` (or the window flag) is set:

- the TX confirm's radio-off (`mac_data_poll_radio_disable_check()`) leaves the radio on while a window is open;
- the Thread data-poll receive filter is skipped (an LFN does not poll);
- a frame that waits for a restart after failed retries does so with the radio off;
- before each CSMA-CA transmission outside a window, the MAC sets the optional driver extension `PHY_EXTENSION_SET_BACKOFF_RX_OFF` to 1 (0 inside a window): the receiver need not listen during the backoff, retry periods included; it is on for the CCA, the TX and the ACK. Only an LFN's MAC sends it. A driver without it returns -1 and listens (20–45 ms of listening per MAC retry). The emulated radio implements it (backoff = radio idle; frames arriving then are lost).

The MAC needs `macRxOnWhenIdle` false (`ws_management_rx_on_when_idle_set()`, GS8 stage 1). TX → ACK → off is the stage-1 path: the MAC turns the radio on for the CSMA and TX, waits for the ACK, and the confirm turns it off.

**Radio driver.** New optional extension `PHY_EXTENSION_READ_RX_ACTIVE` (`arm_hal_phy.h`): one byte, 1 while a frame is being received (sync word detected, frame not complete). A driver without it returns -1 and the MAC closes at once, losing a frame that was arriving. RAIL has the state for it (H1 in HW8); the emulated radio implements it.

## 4. Parent role

### FHSS

`fhss_api->lfn_tx_check(api, dst, queued_since_us, &tx_time)` (`fhss_api.h`), called by the MAC for every acknowledged unicast frame with a 64-bit destination before it is sent or taken from the queue:

| Result | When | MAC |
| --- | --- | --- |
| `NOT_LFN` | parent role off, or the neighbour has no LFN schedule (`lfn_uc_timing_info.listen_interval_ms` 0) | the usual FHSS rules (`check_tx_conditions`) |
| `NOW` | the TX time `start(n) + tx_offset_us` of the first slot at least `tx_min_prep_us` ahead is at most `tx_lead_us` ahead | sends; the frame's TX time is that time instead of a random backoff |
| `WAIT` | the TX time is further | keeps it queued; the FHSS arms its timer to poll the queue `tx_lead_us` before the TX time (one timer, for the earliest held frame) |
| `EXPIRED` | queued for more than `tx_ttl_intervals` × *I* | drops it as `MAC_TX_TIMEOUT` (the 10 s FHSS frame timeout does not apply to LFN frames) |

`tx_handle` sends a frame for an LFN on the LFN's channel for the slot nearest to `tx_time - tx_offset_us`, bypassing the parent's TX/RX slots and broadcast dwell (the multi-CCA starts a little before the TX time, so the nearest slot, not the slot containing the instant, is used).

**Neighbour data.** `fhss_ws_neighbor_timing_info_t` has a new `lfn_uc_timing_info` (`fhss_ws_extension.h`): listen interval, slot number, interval offset, LUTT reception time. The upper layer fills it from LUS-IE and LUTT-IE (stage 3); FFN neighbours keep it zero. Once the LUTT is more than 2^30 µs old, the FHSS replaces it in the entry by an equivalent one at a recent slot start, so a parent that keeps sending to a silent LFN keeps the phase.

### MAC

- The queue (`mcps_sap_pd_req_queue_write/read`) asks `lfn_tx_check` first; for `NOT_LFN` the code path is the old one.
- **Retries go to the next window.** No ACK: the MAC counts the retry and returns the frame to the queue (`MAC_RETURN_TO_QUEUE`) instead of a backoff retry; busy channel or a start more than 5 ms late: the same. The retry and restart limits (`macMaxFrameRetries`, `macRequestRestart`) still apply, so a frame gets at most one attempt per window. Silicon Labs' parent retried a frame for an LFN once per listening slot (every 1.25 s during the LGTK outage) [M].

## 5. Settings and limits

All in `fhss_ws_extension.h`; `ns_fhss_ws_lfn_listen_defaults()` and `ns_fhss_ws_lfn_parent_defaults()` fill the defaults. The emulator's test program takes each as an option.

| Setting | Default | Role | Meaning |
| --- | --- | --- | --- |
| `listen_interval_ms` | 1000 | LFN | Unicast listen interval (LUS-IE), 1 .. 2 147 483 ms |
| `rx_window_us` | 5000 | LFN | Window length |
| `rx_guard_us` | 1000 | LFN | Window opening before the slot start |
| `close_retry_us` | 2000 | LFN | Retry period of a deferred close |
| `first_slot_delay_us` | 10000 | LFN | Slot 0 start after the schedule starts |
| `uplink_guard_us` | 5000 | LFN | Time after an uplink's airtime kept clear of the parent's broadcast dwell (ACK) |
| `drift_ppm` | 50 | LFN | Window widening per side, µs per second since the last sync (Silicon Labs' LFN: about 55 [M]) |
| `widening_max_us` | 100000 | LFN | Upper bound of the widening, per side |
| `bc_rx_window_us` | 5000 | LFN | Window length in the parent's broadcast slots |
| `bc_sync_only` | false | LFN | Listen only in the broadcast slots of the sync frames |
| `fhss_ws_lfn_bc_t.interval_ms`, `.bsi` | (upper layer) | parent | LFN broadcast interval and schedule identifier (LBS-IE) |
| `tx_offset_us` | 0 | parent | TX start after the LFN's slot start |
| `tx_lead_us` | 20000 | parent | Release before the TX time (≤ 60 ms: the PHY's scheduled-TX limit is 65 ms) |
| `tx_min_prep_us` | 2000 | parent | Shortest release-to-TX time; a closer slot is skipped |
| `tx_ttl_intervals` | 20 | parent | Frame TTL in listen intervals |

Silicon Labs' profiles use 6 s (Test), 60 s (Balanced) and up to 16 777 s (Eco) listen intervals, and the parent may adjust the interval with an LTO-IE [M] (§2.1). Limits:

- **32-bit clock (LFN study M11):** listen intervals up to 2^31 µs (35.8 min); Eco's longest are not possible until 64-bit time.
- **Split programs (M12):** the Spinel `ns_fhss_ws_update_neighbor` does not carry `lfn_uc_timing_info`; only in-process MACs support the parent role.
- **One node, one role:** a node is an LFN or a parent, not both (`-3`).

## 6. Tests

| Test | What |
| --- | --- |
| `tests/fhss_lfn/run.sh` | The real FHSS on a fake port and MAC (virtual µs clock across the 32-bit wrap): slot arithmetic and re-anchoring; LTO arithmetic and its application in and outside a window; uplinks held during and before the parent's broadcast dwell; windows at the right times and channels for 200 slots; deferred close; late timer; LUTT written at TX time; 50 min of schedule; the parent's TX time inside the LFN's window and on its channel, from a LUTT the LFN wrote; held frames polled before their window; TTL; a silent LFN for 45 min; invalid settings |
| Emulator `test_lfn_join.sh`, `test_lfn_mesh.sh` | LND discovery end to end with one, two and six parents (`LFN_STACK.md` §9) |
| `tests/fhss_lfn/run.sh`, black-box phase 2 | Discovery slot *i* on the LFN's channel of slot number first slot + *i* (40 slots, slot numbers wrapping past 65 535), retuned at each slot start, closed at the window's end; the parent's LPA (and its CCA) on the LFN's channel of the slot number given, and on its nearest slot's channel without one. 1 082 checks in all |
| `tests/fhss_lfn/run.sh`, stage 4 | The parent's broadcast slots (first slot, frames held and released, channel, LBT-IE, 50 min of schedule), the LTO-IE conversion and its application on the ACK; the LFN synced by LBT-IEs from a parent whose clock is 30 ppm off: rate measured within 200 ppb, phase error from 1.5 ms (before the rate is known) to under 50 µs, broadcast windows at the predicted starts with the widening and on the parent's channels, its unicast windows at the parent's interval on its own clock, sync-only listening, stop. 985 checks in all |
| Emulator `sim_utils/vtime/test_lfn_mac.sh` | Two nodes in virtual time with the MAC in-process on the emulated radio (`wslfntest-inproc`, no upper layers): the LFN solicits on every channel with LUTT/LUS; the parent learns its schedule and answers in a window; then traffic both ways. The parent's frames carry a BT-IE, and optionally an LTO-IE in its answer (`--lto-offset-ms`, `--lto-interval-ms`). Checks from the logs and the EM2 power ledger: every frame the LFN receives starts in a window (on the LTO's grid after it); no uplink starts in, or runs into, the parent's broadcast dwell; the radio does not listen during the LFN's backoffs; the parent sends on the LFN's channel of the slot; downlink frames delivered within one interval (one attempt per window on retries); uplink TX → ACK → radio off; the LFN's radio on only in windows and around its own transmissions; MCU in deep sleep most of the time |

Non-regression (both roles off, FAN 1.0): `dag_simple` in virtual time, 10 seeds, logs byte-identical to those of the previous stack commit (emulator `compare_logs.py --mask-pointers`); `test_radio_off.sh`, `tests/eventloop`, `tests/cross` pass.

## 7. Sources

- IEEE 802.15.4-2015/2020: frame formats, header and payload IEs, Enh-ACK.
- Wi-SUN FAN 1.0 (IEEE 2857-2021): UTT/US-IE, channel functions DH1CF and TR51, which the LFN schedule reuses.
- Arm Nanostack (Apache-2.0, this tree): LUTT-IE and LUS-IE codecs (`ws_ie_lib.c`), sub-ID values (`ws_common_defines.h`).
- Wireshark's Wi-SUN dissector (LFN study §7): to check the frames the emulator writes (pcap), not done yet for these IEs.
- Silicon Labs' published LFN parameters and the black-box results [M]: LFN profiles, the parent sending at the LFN's slot phase, one retry per slot, the LFN's wake length and window widening, no extra window after the LFN's own TX; from the phase 1 captures (black-box phase 2 analysis, `tests/hw/lfn_blackbox/chan_check.py` in the firmware): the channels of unicast, LFN broadcast and discovery slots, and the LTO reading.

## 8. Not done here (later stages)

- **Stage 3:** the LLC sends and parses LUTT/LUS/NR/LND/LTO and fills `lfn_uc_timing_info`; the LFN state machine; calling the FHSS APIs from `ws_management_*` and the config files.
- **Stage 4:** done (§2.2, §2.3).
- **HW8:** the FG25 port maps `PHY_EXTENSION_READ_RX_ACTIVE` and `PHY_EXTENSION_SET_BACKOFF_RX_OFF` (RAIL idle during the backoff) and, optionally, opens windows with `RAIL_ScheduleRx`; its FHSS timer table (4 entries) is enough.
