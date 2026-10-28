# Wi-SUN FAN 1.1 LFN unicast mechanics in the MAC and FHSS

GS8 stage 2 (root repo: `documents/pre-study/investigation/LFN_FAN11_Study.md` §5, items M3, M4, M6, M8, M9, E3). This note describes what the MAC and the FHSS do for a Limited Function Node (LFN) and for its parent, the interfaces, the settings, the sources each rule comes from, and how it is tested. The upper layers (LLC, LFN state machine, LGTK, 6LoWPAN-ND for LFNs) are stage 3 and later: here the MAC and FHSS are driven through their APIs.

Tags: **[S]** a public source (listed in §7); **[M]** observed on Silicon Labs' LFN (root repo, `HW5_BlackBox_Phase1_Results.md`); **[I]** our inference, to check against a capture of a Silicon Labs LFN (black-box phase 2).

## 1. Summary

- **Both roles are off by default.** With them off, the MAC and FHSS behave exactly as before: `dag_simple` logs are byte-identical to those of the previous commit for 10 seeds (§6).
- **LFN role** (`ns_fhss_ws_lfn_listen_set()`): the FHSS runs the node's unicast listen schedule instead of the FFN unicast hopping and broadcast schedule. Around the start of each slot it opens a receive window on the channel of the node's unicast channel function for that slot; the MAC turns the radio on and, at the end of the window, off (after a frame being received and its ACK). Outside the windows the radio is off except to transmit: TX, wait for the ACK, off. The FHSS writes the LUTT-IE (slot number and offset) of every frame at TX time.
- **Parent role** (`ns_fhss_ws_lfn_parent_set()`): an acknowledged unicast frame to a neighbour that has an LFN schedule is held in the MAC queue and released for the LFN's next slot. It is sent at the slot start plus an offset, on the LFN's channel for that slot, whatever the parent's own TX/RX slots and broadcast dwell. A failed attempt (no ACK, busy channel, late start) waits for the next slot; a frame that waits longer than its TTL is dropped as timed out.
- **One frame per window.** A window is a few ms long, so the parent sends at most one frame per LFN slot: the downlink capacity to one LFN is one frame per listen interval, and the latency up to one interval (Silicon Labs' parent behaves the same way [M]).
- **Every timing is a setting**, with defaults (§4). The 32-bit µs clock limits the listen interval to 2^31 µs (35.8 min); see §5.

## 2. The listen schedule [S, M, I]

A LUS-IE gives the listen interval *I* (24 bits, ms) [S: the codec in `ws_ie_lib.c`; M: the LUS of Silicon Labs' LFN reads 1 250 ms during EAPOL]. A LUTT-IE gives the sender's unicast slot number (16 bits) and the offset into that slot (24 bits) at the frame's TX time [S: `ws_ie_lib.c`]; the offset is taken in ms, like the interval [I]. Slot *n* starts at

    start(n) = anchor + (n - anchor_slot) × I

and the LFN listens around each slot start [M: every downlink frame of Silicon Labs' parent starts at the same phase of the LFN's grid, and the LFN wakes once per interval]. The parent anchors the schedule at `rx_timestamp - offset` with `anchor_slot = slot` (`rcp/fhss/fhss_ws_lfn.c`).

**Channel of a slot [I].** The LFN uses its unicast channel function (DH1CF, TR51 or fixed) with the slot number as the channel-sequence index, as an FFN does with its dwell-interval index; the parent computes the same from the LFN's channel plan (its US-IE / LCP-IE in stage 3). No public source states it; it is the natural reading of the 16-bit slot number in LUTT. To check against a capture (black-box phase 2).

**Window.** The LFN listens in `[start(n) - rx_guard_us, start(n) - rx_guard_us + rx_window_us)`. The parent starts its frame at `start(n) + tx_offset_us`. Silicon Labs' LFN listens about 4 ms per wake and widens the window with the time since its last sync (2.9 to 5.5 ms over 50 s) [M]; drift and widening are stage 4 (LFN broadcast and sync, EM3 step 5). Here the clocks are the emulator's, without drift.

**Offset rounding.** The LFN writes the offset rounded to the nearest ms, so the parent's anchor is within ±0.5 ms; with the default guard of 1 ms the frame still starts inside the window.

## 3. LFN role

### FHSS (`rcp/fhss/fhss_ws.c`, `fhss_ws_lfn.c`)

- `ns_fhss_ws_lfn_listen_set(api, &cfg)` stores the schedule; it starts at MLME-START (FHSS synchronized), or at once if the MAC already runs. Slot 0 starts `first_slot_delay_us` later. `NULL` stops it and restarts the FFN unicast hopping.
- While it runs: no FFN unicast hop timer, no FFN broadcast schedule (`ns_fhss_ws_set_parent()` is ignored), no periodic TX queue poll (a frame queued behind another goes from the MAC's TX confirm). Broadcast frames go at once on the current channel.
- One FHSS timer callback (`fhss_ws_lfn_timer_handler`) opens and closes the windows. A port needs at most three distinct FHSS timer callbacks in all (unicast, broadcast, LFN).
- A late timer skips the slots whose window has passed (counted in `windows_skipped`); the phase is kept.
- The own schedule is re-anchored at every window, so it runs indefinitely on the 32-bit clock.
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
- a frame that waits for a restart after failed retries does so with the radio off.

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
| `tx_offset_us` | 0 | parent | TX start after the LFN's slot start |
| `tx_lead_us` | 20000 | parent | Release before the TX time (≤ 60 ms: the PHY's scheduled-TX limit is 65 ms) |
| `tx_min_prep_us` | 2000 | parent | Shortest release-to-TX time; a closer slot is skipped |
| `tx_ttl_intervals` | 20 | parent | Frame TTL in listen intervals |

Silicon Labs' profiles use 6 s (Test), 60 s (Balanced) and up to 16 777 s (Eco) listen intervals, and the parent may adjust the interval with an LTO-IE [M]; LTO and the negotiation are stage 3. Limits:

- **32-bit clock (LFN study M11):** listen intervals up to 2^31 µs (35.8 min); Eco's longest are not possible until 64-bit time.
- **Split programs (M12):** the Spinel `ns_fhss_ws_update_neighbor` does not carry `lfn_uc_timing_info`; only in-process MACs support the parent role.
- **One node, one role:** a node is an LFN or a parent, not both (`-3`).

## 6. Tests

| Test | What |
| --- | --- |
| `tests/fhss_lfn/run.sh` | The real FHSS on a fake port and MAC (virtual µs clock across the 32-bit wrap): slot arithmetic and re-anchoring; windows at the right times and channels for 200 slots; deferred close; late timer; LUTT written at TX time; 50 min of schedule; the parent's TX time inside the LFN's window and on its channel, from a LUTT the LFN wrote; held frames polled before their window; TTL; a silent LFN for 45 min; invalid settings |
| Emulator `sim_utils/vtime/test_lfn_mac.sh` | Two nodes in virtual time with the MAC in-process on the emulated radio (`wslfntest-inproc`, no upper layers): the LFN solicits on every channel with LUTT/LUS; the parent learns its schedule and answers in a window; then traffic both ways. Checks from the logs and the EM2 power ledger: every frame the LFN receives starts in a window; the parent sends on the LFN's channel of the slot; downlink frames delivered within one interval (one attempt per window on retries); uplink TX → ACK → radio off; the LFN's radio on only in windows and around its own transmissions; MCU in deep sleep most of the time |

Non-regression (both roles off, FAN 1.0): `dag_simple` in virtual time, 10 seeds, logs byte-identical to those of the previous stack commit (emulator `compare_logs.py --mask-pointers`); `test_radio_off.sh`, `tests/eventloop`, `tests/cross` pass.

## 7. Sources

- IEEE 802.15.4-2015/2020: frame formats, header and payload IEs, Enh-ACK.
- Wi-SUN FAN 1.0 (IEEE 2857-2021): UTT/US-IE, channel functions DH1CF and TR51, which the LFN schedule reuses.
- Arm Nanostack (Apache-2.0, this tree): LUTT-IE and LUS-IE codecs (`ws_ie_lib.c`), sub-ID values (`ws_common_defines.h`).
- Wireshark's Wi-SUN dissector (LFN study §7): to check the frames the emulator writes (pcap), not done yet for these IEs.
- Silicon Labs' published LFN parameters and the black-box results [M]: LFN profiles, the parent sending at the LFN's slot phase, one retry per slot, the LFN's wake length and window widening, no extra window after the LFN's own TX.

## 8. Not done here (later stages)

- **Stage 3:** the LLC sends and parses LUTT/LUS/NR/LND/LTO and fills `lfn_uc_timing_info`; the LFN state machine; LTO-IE (slot offset and adjusted interval); calling the FHSS APIs from `ws_management_*` and the config files.
- **Stage 4:** the LFN broadcast schedule (LBS/LBT), the LFN's sync and window widening with drift (EM3 step 5), and the LFN's uplink avoiding its parent's broadcast dwell (it needs the parent's BS/BT; today a uplink sent during the parent's broadcast dwell is retried or restarted).
- **Power:** the emulated radio listens during a CSMA backoff, so an LFN's MAC retry (20-45 ms of retry period) costs listening time; a radio that sleeps during backoff, or an LFN-specific retry period, would save it.
- **HW8:** the FG25 port maps `PHY_EXTENSION_READ_RX_ACTIVE` and, optionally, opens windows with `RAIL_ScheduleRx`; its FHSS timer table (4 entries) is enough.
