# Wi-SUN FAN 1.1 LFN in the upper layers: one LFN under the border router

GS8 stages 3–5 (root repo: `documents/pre-study/investigation/LFN_FAN11_Study.md` §5, items U2–U13, U15). The MAC and FHSS mechanics are in `LFN_MAC.md`. This note describes what the LLC, the LFN bootstrap and the border router do, the settings, the sources each rule comes from, the deviations from Silicon Labs' implementation, and the tests.

Tags as in `LFN_MAC.md`: **[S]** a public source (`LFN_MAC.md` §7: the Arm codecs, Wireshark's dissector, `wisun-br-linux` and its `HIF.md`, RFC 8505; for LGTK, §8); **[M]** observed on Silicon Labs' LFN (root repo, `HW5_BlackBox_Phase1_Results.md`, item numbers); **[I]** our choice or inference, to check in black-box phase 2.

## 1. Summary

- **FFN routers as parents** (stage 5, §9): a FAN 1.1 router with `ws_management_lfn_parent_set()` parents LFNs once it is operational and holds an LGTK, which it gets from the border router by EAPOL; LFNs choose among the parents that answer their discovery (LND slots); the router relays their EAPOL and DHCPv6 and registers their addresses with the border router (RPL DAO).
- **Run-time only.** An interface is an LFN when the application starts it with `NET_6LOWPAN_HOST` (`arm_nwk_interface_configure_6lowpan_bootstrap_set()`); the router library contains the LFN code (`HAVE_WS_HOST`). A border router parents LFNs after `ws_management_lfn_parent_set(id, true)`, which needs FAN 1.1. With neither, nothing changes: FAN 1.0 `dag_simple` logs are byte-identical (§7).
- **Join** (`ws_bootstrap_lfn.c`): LPAS sweeps → LPA in the LFN's next listen window → EAPOL with the parent as authenticator (shorter listen interval meanwhile) → LPCS → LPC → DHCPv6 through the parent → address registration (NS with ARO) to the parent. No RPL, no MPL forwarding; a default route through the parent.
- **Parent side** (`ws_lfn_parent.c`, border router): LPAS → the LFN child and a unicast LPA with an LTO-IE; LPCS → LPC; frames to an LFN child wait for its listen window (FHSS parent role, `LFN_MAC.md` §4); an LPC to every LFN child every sync period; a MAC-unicast copy of each `ff02::1` multicast to every LFN child.
- **LGTK** (§8): the LFN authenticates with a node role KDE and gets LFN group keys (LGTKs, MAC key indexes 5–7) instead of GTKs; the border router keeps three LGTKs with their own lifetime, rotates them, announces each change with the LFN version and LGTK hash in an LPC to every LFN child, and activates a new LGTK only once its LFNs can have fetched it; the LFN fetches a new LGTK `lgtk_max_mismatch_s` after it sees its hash. FFNs keep their GTKs.
- **LFN broadcast schedule and sync** (stage 4, §4.1 and `LFN_MAC.md` §2.2–2.3): while it has an LFN child, the border router sends a sync frame in the LFN broadcast slot of every sync period (an LTS, or a broadcast LPC after the schedule starts and after each LGTK event); the LFN listens in the parent's broadcast slots, syncs from every frame with an LBT-IE, keeps its own unicast slots in the parent's time base and widens its windows for clock drift.
- **Parent maintenance**: the LFN declares its parent lost after 1.5 × `maintain_parent_time` sync periods with no sync frame from it (LTS, LPC) [M items 34, 46], then rediscovers and rejoins without EAPOL [M item 47].
- **Checked** in virtual time with `wsbrd-inproc` and `wsnode-inproc` (emulator `sim_utils/vtime/test_lfn_join.sh`, §7).

## 2. Frames and IEs (LLC, `ws_llc_data_service.c`)

| Frame | Sent by | Header IEs | Payload IEs | Security | Source |
| --- | --- | --- | --- | --- | --- |
| LPAS (type 10) | LFN, asynchronous on every channel | LUTT, NR (role LFN, interval range), LUS (tag 1), LND (with a random Discovery First Slot) | LCP (tag 1: the LFN's unicast schedule), NETNAME | none | [S] codecs, `wsbrd` `ws_mngt.c` (needs the LCP-IE of the LUS-IE's tag); [M] items 1–4 |
| LPA (9) | parent, unicast | UTT, BT, NR, FLUS (dwell, tag 0), PANID, LBS (tag 0), LTO, LBT | LCP (tag 0: the parent's unicast schedule), BS, PAN, NETNAME | none | [S] `ws_mngt.c` IE list, [M] item 5 |
| LPCS (12) | LFN, unicast | LUTT, LUS (tag 255) | NETNAME | active LGTK (ours); none (Silicon Labs' LFN) | [S] `wsbrd` needs NETNAME; [M] item 13 |
| LPC (11) | parent, unicast (answer to an LPCS) or broadcast in an LFN broadcast slot | UTT, BT, LBS, LBT | LFNVER, LGTKHASH | active LGTK | [M] items 14, 48, §5.2 |
| LTS (13) | parent, broadcast in an LFN broadcast slot | UTT, BT, LBT | none | active LGTK | [M] items 24, §5.2 [I: IE set] |
| Data, EAPOL | LFN | LUTT, LUS (tag 255: "current", instead of UTT, BT) | — | data: active LGTK; EAPOL: none | [S], [M] |
| Enh-ACK | LFN | LUTT (frame type 5), RSL | — | as the frame acknowledged | [M] (Silicon Labs' LFN) |

- **Frame type** is 8 bits in `asynch_request_t` and 4 bits in an LLC message (types up to 13).
- **Broadcast sync frames** go through `ws_llc_asynch_request()` with `lfn_broadcast` set: the MAC holds them for the next LFN broadcast slot (`LfnBroadcast`, `LFN_MAC.md` §2.2). An LTS has header IEs only; the MAC now accepts such frames (it dropped a frame with IEs and no payload) and the LLC passes it to the bootstrap without payload IEs.
- **Receiving**: a frame with a LUTT-IE and no UTT-IE is accepted (FAN 1.1 only); its LUTT/LUS fill the sender's `lfn_uc_timing_info` (`ws_llc_lfn_timing_update()`), instead of the UTT update. LPA, LPC, LPAS and LPCS go to the bootstrap's asynchronous handler.
- **Unicast management frames** go through `ws_llc_asynch_request()` with `unicast` set: acknowledged, no channel list, so the MAC holds a frame to an LFN for its window. An LTO-IE that the LFN acknowledges is applied to the parent's view of its schedule on the ACK (`ns_fhss_ws_lfn_neighbor_lto()`).
- **LBS-IE** now has its sync-period byte (7 bytes, as Wireshark's dissector and `wisun-br-linux`; the Arm codec had 6). Nothing called the old one.
- **Channel plans (black-box phases 2–3).** An LFN gives its unicast schedule once, in an LCP-IE of its LPAS (tag 1, the LUS-IE's tag), and "current" (tag 255) in its LUS-IEs afterwards; a parent gives its unicast schedule as FLUS-IE (dwell interval, tag 0) + LCP-IE (tag 0) in its LPA, no US-IE [S: `wsbrd` `ws_mngt.c`, `ws_llc.c`; M: Silicon Labs' LFN and both parents]. Readers take the LCP-IE of the tag given, or a US-IE (our nodes before this change). With a FAN 1.1 channel plan ID (`ws_management_domain_configuration_set()`, PHY mode ID + channel plan ID) the US, BS and LCP IEs use channel plan 2 (regulatory domain and channel plan ID), which `wsbrd` requires in an LCP-IE; FAN 1.0 configurations (no plan ID) are unchanged. `wisun-br-linux` reads the LCP-IE of a non-255 LUS tag in data, EAPOL and LPCS frames from the header IEs (where it never is), so an LFN must send tag 255 there.
- **Kept deviations [I]**: no Silicon Labs vendor IE (LFN session ID, `VH 1a 02 xx`) in our LPC and LTS; our LPA, LPC and LPCS request an acknowledgement (Silicon Labs' are unacknowledged, with the sequence number suppressed); our LPCS is secured; no JM-IE in our LPA. Results with Silicon Labs' stack: root repo, `LFN_BlackBox_Phase2_3_Results.md`.

## 3. The LFN (`ws_bootstrap_lfn.c`)

| State | What it does | Source |
| --- | --- | --- |
| Discovery (`ER_ACTIVE_SCAN`) | FHSS: own unicast channel function, no broadcast schedule; listen schedule at the proposed interval (started without a PAN: `ns_fhss_ws_lfn_listen_start()`); receiver off outside windows. An LPAS sweep every `lpas_interval_s` until an LPA comes | [M] items 1, 8 |
| LPA | Network name checked; PAN ID from PANID-IE; the parent becomes a neighbour (UTT, FLUS + LCP or US, BS, BT); its broadcast schedule is given to the FHSS (uplinks avoid its dwell); LTO-IE applied (offset, adjusted interval within the NR range); LBS gives the sync period | [M] items 5–6 |
| Authentication (`ER_PANA_AUTH`) | Listen interval `eapol_listen_interval_ms` (1 250 ms), EAPOL with the parent as target (the existing supplicant); skipped on a rejoin with valid keys | [M] items 10, 36, 47 |
| Configuration (`ER_SCAN`) | Agreed interval back; LPCS, repeated every `lpcs_retry_intervals` listen intervals until an LPC | [M] items 13–15 |
| Operation | Default route through the parent; DHCPv6 Solicit to the parent's link-local address (no prefix hint); on the reply, NS with ARO (lifetime `registration_lifetime_min`) to the parent; re-registration at 92 % of the lifetime | [M] items 16–18, 52; RFC 8505 |
| Sync | Every frame from the parent with an LBT-IE (LPA, LPC, LTS) syncs the FHSS to its LFN broadcast schedule (`ns_fhss_ws_lfn_bc_sync()`, with the LBS-IE values kept from the LPA or an LPC and the parent's broadcast channel function) | [M] items 21–25, §5.2 |
| Maintenance | A sync frame from the parent keeps it; lost after 1.5 × `maintain_parent_time` × sync period without one → broadcast windows stopped, discovery, keys kept. Other secured frames keep the parent's neighbour entry only | [M] items 34, 46, 47 |

- **Group key before the first LPC**: the LFN sends its LPCS with the lowest LGTK installed by the 4-way handshake (key index 5–7); every LPC's key index then sets the active one (as an FFN learns its GTK from a PAN configuration), after the LPC's LGTK hash has been applied (§8).
- **Neighbours**: the LFN keeps only its parent; its entry is refreshed by every frame from it.

## 4. The border router as parent (`ws_lfn_parent.c`)

- **LPAS**: needs NETNAME, NR (role LFN), LUS, LUTT and the LCP-IE of the LUS-IE's tag (or a US-IE). The LFN becomes a neighbour (an "LFN child": `lfn_uc_timing_info` set, `ffd_device` false, lifetime 2 × the registration lifetime, refreshed by its frames; no NUD or ETX probing). One LPA per sweep: a second LPAS within 2 proposed intervals is ignored, and no new LPA is queued while an earlier one to the same LFN still waits for a window (`ws_llc_lfn_lpa_pending()`). Otherwise a second LTO-IE would move the LFN's schedule in our view only: the LFN ignores an LPA once it has a parent, but the border router applies the LTO-IE of each acknowledged LPA. (Found during the LGTK work: an LPA from the first sweep, still queued, was sent after the one the LFN took, and the border router then missed the LFN's windows until its next uplink, about 340 s.)
- **LTO-IE** (`lfn_lto`, default on) [S: the rule described in `wisun-br-linux` `ws_neigh.c`, our arithmetic]: the proposed interval snapped to a multiple (above) or a divisor (below) of the LFN broadcast interval within the LFN's NR range (6 000 ms → 5 000 ms with 10 s, as observed [M item 6]); the offset a random multiple of 300 ms in [300, min(interval, broadcast interval) − 300] ms.
- **LPCS**: from a known LFN → LPC. Secured (ours) or unsecured with the network name (Silicon Labs' LFN); an unsecured one does not make the LFN a trusted neighbour (its secured frames do).
- **LPA in an LND slot** (`LFN_MAC.md` §2.4): a random slot *k* of the LFN's discovery window, a quarter into it, on the LFN's channel of slot number `discovery_first_slot + k` [M: black-box phase 2 analysis].
- **LFN broadcast schedule** (stage 4): the FHSS runs it from the FHSS activation (`ns_fhss_ws_lfn_bc_set()`: `bc_interval_ms`, the BSI of the border router's hopping schedule, as in its LBS-IE); every LPA and LPC carries an LBT-IE.
- **Sync frames** [M items 24, 32, 48, §5.2; S `wsbrd` `ws_mngt.c`]: only while the border router has an LFN child. In the slot of every sync period (slot number a multiple of `bc_sync_period`) an LTS, or a broadcast LPC during the first `maintain_parent_time` sync periods after the first child appears and after each LGTK event; an LGTK event's LPC goes in the next slot, whatever its number. The seconds timer queues the frame when its slot is less than 2 s away; the MAC holds it until the slot.
- **LTO-IE**: the offset the upper layer gives is the phase of the LFN's slots against the LFN broadcast slots; the FHSS converts it at TX time (`LFN_MAC.md` §2.2), so the LFN's unicast slots stay at least 300 ms from the broadcast slots.
- **Security**: frames to an LFN child (data through `ws_llc_lowpan_mpx_data_request()`, LPC) and the broadcast sync frames use the active LGTK (key index 5–7); everything else uses the GTKs as before (§8).
- **Registration**: the LFN's NS(ARO) is handled by the existing 6LoWPAN-ND code: a registered neighbour-cache entry, through which the border router routes to the LFN; a non-FFD child is published to RPL (`rpl_control_publish_host_address`), which for the root is local.
- **EARO (RFC 8505)** [S: RFC 8505, `wisun-br-linux` `nd_router_object.c`; M: item 19]: Silicon Labs' LFN registers with an NS from its link-local address, the address to register as Target, and an EARO with T and R set and a TID; then each RFC 9685 multicast group with P = 1. On a FAN 1.1 interface, such an NS registers the Target (the same checks as an ARO) and is always answered by an NA with an EARO echoing the flags and TID (the Wi-SUN omission of a successful NA does not apply); a multicast registration is accepted (status 0) but only `ff02::1` is copied to LFN children. Our LFN keeps the RFC 6775 ARO (source = registered address), which both `wsbrd` and our parents accept.
- **Multicast** [M items 41–42]: a multicast to `ff02::1` leaving the border router is also sent as a MAC-unicast copy to every LFN child (IPv6 destination unchanged), in its window (`lowpan_down()` → `ws_lfn_parent_multicast_copy()`). Other groups need the LFN's RFC 9685 subscription (EARO P flag), not implemented.

## 5. Settings

`ws_lfn_settings_t` (`ws_management_api.h`), set with `ws_management_lfn_settings_set()`; defaults from `ws_management_lfn_settings_defaults()`. The emulator's programs take each field as `lfn_<field> = <value>` in their configuration (`examples/wsnode-lfn.conf`, `examples/wsbrd-lfn.conf`).

| Field | Default | Role | Meaning |
| --- | --- | --- | --- |
| `listen_interval_ms` | 6000 | LFN | Proposed in the LUS-IE (Silicon Labs Test profile [M item 3]) |
| `listen_interval_min_ms`, `_max_ms` | 1000, 300000 | LFN | NR-IE range [M item 2] |
| `eapol_listen_interval_ms` | 1250 | LFN | During EAPOL [M item 10]; 0: unchanged |
| `rx_window_us`, `rx_guard_us`, `close_retry_us`, `uplink_guard_us` | 5000, 1000, 2000, 5000 | LFN | `LFN_MAC.md` §5 |
| `lpas_interval_s` | 10 | LFN | Between LPAS sweeps [M item 8] |
| `lnd_slot_time_ms`, `lnd_slots` | 60, 40 | LFN | LND-IE values [M item 4] |
| `lpcs_retry_intervals` | 2 | LFN | LPCS again after this many intervals [I] |
| `maintain_parent_time` | 5 | LFN | Sync periods; loss after 1.5 × [M item 46] |
| `lgtk_max_mismatch_s` | 120 | LFN | Key request this long after the first LPC whose LGTK hash lists an LGTK it lacks (Silicon Labs Test profile `lgtk_max_mismatch_m` = 2 min [M item 49]) |
| `registration_lifetime_min` | 2160 | both | ARO lifetime (36 h [M item 19]) |
| `drift_ppm` | 50 | LFN | Window widening per side, µs per second since the last sync (Silicon Labs' LFN: about 55 [M item 23]) |
| `widening_max_us` | 100000 | LFN | Upper bound of the widening, per side |
| `bc_rx_window_us` | 5000 | LFN | Window in the parent's LFN broadcast slots |
| `bc_sync_only` | false | LFN | Listen only in the broadcast slots of the sync frames (Silicon Labs' LFN listens in every slot [M items 21, 55]) |
| `bc_interval_ms`, `bc_sync_period` | 10000, 5 | parent | LBS-IE; sync period = their product [M §3.1] |
| `lto` | true | parent | Send an LTO-IE in the LPA |
| `tx_offset_us`, `tx_lead_us`, `tx_min_prep_us`, `tx_ttl_intervals` | 0, 20000, 2000, 20 | parent | `LFN_MAC.md` §5 |
| `lgtk_lifetime_s` | 7 776 000 (90 days) | parent | LGTK lifetime (LGTK_EXPIRE_OFFSET; `wsbrd` default 129 600 min [S]) |
| `lgtk_new_install_required` | 90 | parent | The next LGTK is installed when the active one has used this % of its lifetime [S: `wsbrd` default] |
| `lgtk_new_activation_time` | 180 | parent | The next LGTK is activated `lgtk_lifetime_s` / this before the active one expires (12 h with the defaults) [S: `wsbrd` default] |
| `lgtk_activation_guard_s` | 300 | parent | The next LGTK is activated no sooner than this after its installation [I] (§8) |

| `lnd_response_delay_ms` | 1550 | LFN | The discovery window opens this long after the sweep starts, and never before 20 ms per channel plus 300 ms [M item 4] |

Limits: one LFN parent per LFN. The split (Spinel) programs refuse the LFN roles (LFN study M12).

## 6. Not done here

- RFC 9685 multicast subscriptions: accepted (EARO with P = 1) but not used to copy groups other than `ff02::1`; our LFN does not subscribe.
- LGTK leftovers (§8): LGTKs and the node role are not stored in NVM; no LGTK revocation; no frame-counter-based LGTK lifetime check; the role-based PMK/PTK lifetimes of FAN 1.1.

## 7. Tests and results

| Test | What |
| --- | --- |
| Emulator `sim_utils/vtime/test_lfn_join.sh` | Border router + LFN in virtual time with test traffic (`app_wsbrd/test_traffic.c`): join; UDP both ways; `ff02::1` received as MAC-unicast copies; every frame the LFN receives in a window (ACKs to its own frames aside); parent kept; the LFN's average current over a steady-state window, by part (EM2 ledger) |
| `tests/fhss_lfn/run.sh`, emulator `test_lfn_mac.sh` | The MAC and FHSS mechanics (`LFN_MAC.md` §6) |
| `test_lfn_join.sh`, LGTK checks | The join with an LGTK (NR KDE in the key request and in 4-way handshake message 2, LPCS under key index 5–7); for every LGTK installed after the join: the LPC with the new hash, the key request `lgtk_max_mismatch_s` after it, the LGTK received before its activation, the LFN taking the new active index from the next LPC. Rotations come from `LGTK_ROTATE_AT` (`ws_management_lfn_lgtk_rotate()`) or from short lifetimes (`BR_EXTRA="-o lfn_lgtk_lifetime_s=900 ..."`) |

Stage 4 runs use the emulator's clock drift (`SIM_OPTS="--drift-random=20"`: each node's clock off by up to ±20 ppm, docs `VIRTUAL_TIME.md` §4.1); the window check takes each window's logged length, widening included, and the broadcast windows.

Test hook: `ws_test_lfn_parent_silence()` (`net_ws_test.h`) makes the border router ignore its LFN children and send them no LPC, as a parent that is gone (emulator `SILENT_AT`, `SILENT_FOR`).

**Results** (emulator `docs/POWER_MODEL.md` §14; FG25 profile, EU mode 3, +10 dBm, −50 dBm link):
- 10 seeds with test traffic, 1800 s: all checks pass; operational 37–67 s after start; LFN 74.6–79.5 µA.
- Idle, 3 seeds, 3600 s: 41.6–42.4 µA, of which about 24 µA come from the 50 ms protocol timer (20 wakes/s), 5.8 µA from listening. With idle-tick skipping of the protocol timers (`PORTING.md`, DI6): 16.6–17.3 µA, 0.66 wakes/s, of which 0.07/s from the protocol timers; with test traffic 49.6–54.5 µA (emulator `docs/POWER_MODEL.md` §15).
- Parent loss, 3 seeds: declared 375.2–375.7 s after the last frame from the parent, rejoin without EAPOL in 21–22 s (Silicon Labs' LFN: 375 s, 21.6–21.9 s [M items 45–47]).
- With LGTKs (§8): the same tests pass (traffic 10 seeds, idle 3 seeds, parent loss 3 seeds); idle 16.5 µA in each of the 3 seeds, 0.6 wakes/s; with test traffic 49.5–57.6 µA (seed 1, the highest, had 28 uplink retries instead of 15, during the border router's PAN configuration sweeps, whose timing moved with the border router's random draws); parent lost 375.2–376.0 s after its last frame, operational again without EAPOL; operational 27–49 s after start. LGTK rotations and their timing: §8.

- **Stage 4** (LFN broadcast schedule and sync; every run with each node's clock drawn in ±20 ppm unless stated):
  - with test traffic, 10 seeds × 1800 s: all checks pass (every frame the LFN received inside a unicast or broadcast window); operational 27.2–48.9 s after start; LFN 56.3–64.5 µA;
  - idle, 3 seeds × 3600 s: 24.0–24.4 µA without drift, 24.0–24.6 µA with drift, 0.7 wakes/s; of 24.4 µA (seed 1), listening 13.5 µA, MCU sleep 4.9 µA, deep-sleep floor 4.6 µA. The stage 3 figure (16.5 µA) had no broadcast windows. With `bc_sync_only`, 19.5–19.8 µA (0.6 wakes/s);
  - sync: the LFN measured its clock rate against the parent's to the drift set (seed 1 of the idle runs: 7.2 ppm measured, 7.22 ppm set; a 40 ppm pair: −40 000 ppb), and the sync error stayed at 0–1 µs once the rate was known;
  - parent loss, 3 seeds: declared 376.0 s after the last sync frame, rejoin without EAPOL;
  - LGTK rotation at 600 s (3 seeds) and short lifetimes (3 seeds): all checks pass; hash at the LFN 10 s after the installation (next LFN broadcast slot), key request 120 s after it.

Non-regression (FAN 1.0, no LFN): `dag_simple` in virtual time, 10 seeds, logs byte-identical to the previous stack commit (`compare_logs.py --mask-pointers --mask-source-lines`; the second option masks the programs' own `function():line` trace prefixes, which move when their source is edited).

## 8. LGTK

FAN 1.1 gives LFNs their own group keys, LGTKs, distinct from the GTKs of the FFNs. Our code is written from the public sources listed below, not copied: **[S]** `wisun-br-linux` v2.10.3 `common/specs/ws.h` (KDE types), `common/kde.c` (KDE layouts), `common/authenticator/authenticator_key.c` (which KDEs go to which role, quoting Wi-SUN FAN 1.1 6.5.2.2), and the Apache-2.0 stack files of `wisun-br-linux` v1.9.1 (`key_sec_prot.c`, `auth_fwh_sec_prot.c`, `auth_gkh_sec_prot.c`: the node role read from the key request); `examples/wsbrd.conf` (LGTK timer names and defaults); **[M]** the black-box results, items 48–51.

**KDEs** (`kde_helper.c`, Wi-SUN OUI `0c:5a:9e`): node role (type 3, one byte), LGTKL (type 4, one byte, bit i: LGTK i live), LGTK (type 5, the GTK KDE's layout: key index 0–2 in bits 0–1, a reserved byte, the key).

**Handshakes.** The supplicant's key request (initial EAPOL-Key) carries, for an LFN, its LGTKL and an NR KDE with role LFN, instead of its GTKL [S: `wsrd` writes GTKL, LGTKL and NR; the authenticator reads the NR KDE from the key request]. The LFN also puts its NR KDE in 4-way handshake message 2, in clear under the MIC; the authenticator only traces it, since it takes the role from the key request. The authenticator, on an NR KDE with role LFN, switches that supplicant's group keys to the LGTKs (`sec_prot_keys_lfn_set()`): the key to insert is chosen among the LGTKs by its LGTKL, and message 3 and group-key handshake message 1 carry an LGTK, its lifetime and the LGTKL instead of a GTK and the GTKL. A key request without an NR KDE (FAN 1.0, our FFNs) is handled as before; one with role LFN while the border router has no LGTKs is dropped [S]. The LFN reads LGTK and LGTKL KDEs where an FFN reads GTK and GTKL.

**Keys.** The border router keeps three LGTKs next to its GTKs (`pae_controller_t.lgtks`), in MAC key slots 4–6 with key indexes 5–7 (the emulated MAC's key table has 7 entries; a smaller table gives an error and no LGTK on the MAC). Their GAKs are derived from the network name like those of the GTKs. An LFN keeps its LGTKs in its group-key slots 0–2 with key indexes 5–7 (`ws_bootstrap_nw_key_set()`), so a key table of 4 is enough for it.

**Lifecycle on the border router** (`ws_pae_auth.c`, in the authenticator's second timer, once the interface parents LFNs):
- the first LGTK is generated and active at once;
- the next one is installed when the active one has used `lgtk_new_install_required` % of `lgtk_lifetime_s`, and activated `lgtk_lifetime_s` / `lgtk_new_activation_time` before the active one expires [S: LGTK_NEW_INSTALL_REQUIRED, LGTK_NEW_ACTIVATION_TIME];
- **[I] activation guard:** it is activated no sooner than `lgtk_activation_guard_s` after its installation. An LFN fetches a new LGTK about `lgtk_max_mismatch_s` after the LPC that shows its hash; a parent that switches earlier sends frames the LFN cannot read and LPCs it cannot decrypt (black-box item 51: `wsbrd`'s NA under key 7 was not acknowledged for 17 s). The default 300 s covers 120 s of mismatch wait, an LPC that missed its window and the next sync period (50 s), and the handshake. If the active LGTK expires first, the next one is activated then, whatever the guard;
- an LGTK whose lifetime ends is removed;
- `ws_management_lfn_lgtk_rotate()` installs the next LGTK now and shortens the active one's lifetime so that the activation falls when the guard has passed; the old LGTK then expires `lgtk_lifetime_s` / `lgtk_new_activation_time` later.

**Each LGTK event** (installed, activated, expired) increments the LFN version, updates the LGTK hash and active index for the LGTKHASH-IE (`ws_info->lfngtk`), and sets `lpc_pending`: a broadcast LPC goes in the next LFN broadcast slot, and the sync frames of the next `maintain_parent_time` sync periods are LPCs [M item 48: one event, one version, an extra LPC]. The LPC is secured with the active LGTK (`ws_lfn_lgtk_security_set()`), as are data frames to LFN children (`ws_lfn_parent_tx_key_index()`, from the LLC). FAN 1.1 PAN configurations carry the same LFN version and LGTK hash, as before.

**On the LFN** (`ws_lfn_parent.c`, `ws_pae_supp.c`): each LPC's LGTK hash is applied before its key index (`ws_pae_controller_lgtk_hash_update()`): LGTKs it no longer lists, or whose hash differs, are removed; listed ones become fresh; a listed LGTK the LFN lacks starts a timer of `lgtk_max_mismatch_s`. When it ends, the LFN sends a key request to its parent (`ws_pae_supp_lgtk_key_request()`), and the parent answers with a group-key handshake; retries are an FFN's key-update retries. A later LPC whose hash matches stops the timer [M items 49–50; I: a fixed delay from the first LPC that shows the new hash; Silicon Labs' two samples, 113 and 117 s, cannot tell a fixed delay from a random one]. The LPC key index then sets the active LGTK. The LFN's EAPOL target after the join is its LFN parent, not an RPL parent.

**Deviations [I].** The LFN keeps its 5 s interval during the group-key handshake (Silicon Labs' LFN listens every 1.25 s then, item 50); the handshake's first message then waits for the next window. The key request is sent once the timer ends, without the random spread an FFN uses after a GTK mismatch.

**Limits.** LGTKs and the supplicants' node roles are not stored: after a border router restart the LGTKs are new, and LFNs that rejoin without EAPOL cannot read the new LPCs until they authenticate again. No LGTK revocation (`ws_pae_auth_node_access_revoke_start()` acts on GTKs only), no frame-counter-based lifetime shortening, no role-based PMK/PTK lifetimes. With a guard longer than the time between installation and expiry, activation happens at the old LGTK's expiry; the LFN's uplinks under the old key then fail until it hears the next LPC (seen with 6-minute lifetimes: 3 losses in 62 request-echo exchanges).

**Results** (emulator `test_lfn_join.sh`, same conditions as §7):
- Join with an LGTK: in every run (37), NR KDE in the key request and message 2, LPCS under key index 5.
- Rotation at 600 s (`LGTK_ROTATE_AT=600`), 10 seeds × 1800 s with test traffic, all checks pass (no request or echo lost across the rotation): the new hash reached the LFN 5.2–10.1 s after the installation (the LPC waits for the LFN's window); key request 119.0–120.0 s after that LPC (the timer has 1 s steps; Silicon Labs' LFN: 113 and 117 s); LGTK received 0.2–10.0 s after the request (group-key handshake, one or two listen intervals); activation 300 s after installation (guard); the LFN took the new key as active 1.8–8.2 s after the activation. LFN 50.7–53.9 µA.
- Short lifetimes (`lgtk_lifetime_s` 900, install at 50 %, activation 300 s before expiry), 3 seeds × 1800 s: three installations, three activations held 150 s by the guard and two expiries per run, every check passes.
- `lgtk_max_mismatch_s` = 30 on the LFN, 3 seeds: key request 29.2–30.0 s after the LPC with the new hash.
- Without the guard (`lgtk_activation_guard_s` 0) and `wsbrd`'s fast test timers (lifetime 360 s, install at 50 %, activation 120 s before expiry; 1 run): the border router activates 60 s after installation, before the LFN has the key; the LFN then took 142 and 147 s to follow, missed 12 of 23 unicast requests and 16 of 23 `ff02::1` requests, and stopped seeing new hashes. With the guard (300 s, so activation at expiry) the same run lost 1 of 23 unicast echoes, 1 of 23 `ff02::1` requests and 1 of 16 uplink echoes, around the expiries.
- FAN 1.0 `dag_simple`, 10 seeds: logs byte-identical to the previous stack commit.

## 9. FFN routers as LFN parents (stage 5)

**Parents.** `ws_management_lfn_parent_set(id, true)` is accepted on a FAN 1.1 router as on the border router (emulator: `enable_lfn = true` in `wsnode`'s configuration). A router parents LFNs once it is operational (`ER_BOOTSTRAP_DONE`) and holds an LGTK: until then it ignores LPAS and LPCS and sends no sync frames. It then runs the same parent code as the border router (`ws_lfn_parent.c`): its own LFN broadcast schedule (the network's BSI, which the FHSS takes from its configuration once the router has learned it), LTS and broadcast LPCs while it has an LFN child, LPA and LPC answers, unicast frames held for the LFN's windows. Its seconds timer runs every second while it parents LFNs (`ws_bootstrap_ffn_seconds_timer_next()`).

**LGTKs for a router** [S: `wsrd` sends GTKL, LGTKL and an NR KDE; `authenticator_key.c`: an FFN gets GTKs and LGTKs]. A router that parents LFNs sends a key request with its GTKL, its LGTKL and an NR KDE with the router role (`sec_prot_keys_t.ffn_lgtk`). The border router answers as before for the GTKs (EAP-TLS, 4-way handshake, group-key handshakes), then sends each LGTK the router lacks in a group-key handshake of its own [I: FAN 1.1 allows both in one message]. One handshake carries one set: `sec_prot_keys_lgtk_mode_set()` selects the set for each, and the KDE code reads and writes LGTK and LGTKL KDEs for it as for an LFN. The router puts its LGTKs on the MAC at key indexes 5–7 (`ws_pae_controller_ffn_lgtk_install()`), and follows the LFN version and LGTK hash of the border router's PAN configurations: a hash it does not match starts a key request at once [I: the border router activates a new LGTK `lgtk_activation_guard_s` after installing it, and the router's children fetch it through the router, so the GTK update trickle, minutes long, is too slow], and each new LFN version gives its children a broadcast LPC. It secures its LFN frames with the active LGTK once it holds it, until then with the one it used last.

**Discovery and choice** (`LFN_MAC.md` §2.4). The LPAS sweep covers the channels of the plan (as an FFN's PAN advertisement solicit), and its LND-IE gives the discovery window: `lnd_response_delay_ms` after the sweep starts (and after the sweep's end, at about 20 ms per channel), `lnd_slots` × `lnd_slot_time_ms` long (40 × 60 ms, [M item 4]). Each parent sends its LPA in a random slot of it, a quarter into the slot [S: `wsbrd`]. The LFN keeps up to four LPAs and, after the window, chooses the parent with the lowest routing cost (PAN-IE) among those heard `CAND_PARENT_THRESHOLD` (10 dB) above its minimum sensitivity, the strongest on a tie; without one, the same among those above the minimum [I: an FFN's candidate-parent margin]. An LPA heard outside a discovery window is taken at once, as before. A parent counts an LFN that sent it an LPAS as a candidate child for 600 s; the LFN's LPCS and its secured frames make it a child for twice its registration lifetime [I].

**Through the router.** EAPOL: the LFN's authenticator target is its parent, and the router relays as for an FFN joiner. DHCPv6: through the router's relay. Registration: the LFN's NS with ARO to the router; the router publishes the address to RPL (DAO target) and the border router routes to the LFN through it. The route exists once RPL's DAO delay has passed (1–7 s in the runs below), so a downlink in the first seconds after the LFN's registration can be lost. Multicast: a router copies the `ff02::1` multicasts it sends to its LFN children; the border router's `ff02::1` does not reach LFNs under a router (link-local), and other groups need RFC 9685 subscriptions (not implemented).

**Tests** (emulator, virtual time, each node's clock drawn in ±20 ppm):

| Test | What |
| --- | --- |
| `test_lfn_join.sh` with `MESH=1` | Border router – router – LFN: the router gets an LGTK, answers the LPAS, the LFN chooses it and joins through it; unicast both ways; LGTK rotation through the router (`LGTK_ROTATE_AT`) |
| `MESH=2`, `MESH=3` | The LFN also hears the border router: at −97 dBm (below its minimum sensitivity: it must choose the router) or at −70 dBm (it must choose the border router, routing cost 0) |
| `test_lfn_mesh.sh` | `dag_simple`'s 14 routers, all LFN parents, and six LFNs at the edge, each hearing two or three parents: every LFN joins, keeps a parent among its neighbours, exchanges unicast both ways with the border router, and receives only in its windows |

**Results.**
- `MESH=1`: 3 seeds × 1800 s, all checks pass; LFN operational 119–130 s after start (the router joins first); LFN 46.2–47.1 µA with test traffic. LGTK rotation at 600 s, 3 seeds: the router fetched the new LGTK at once, the LFN fetched it through the router 119–120 s after the LPC showing its hash, and took it as active 15–35 s after the border router activated it; no parent loss.
- `MESH=2`: 3 seeds, the router chosen, operational 109–119 s. `MESH=3`: 3 seeds, the border router chosen, operational 29–44 s.
- `test_lfn_mesh.sh`, 10 seeds × 1800 s: all 60 LFN joins pass; operational 29–396 s after start (an LFN behind a router waits until that router has joined); 1 573 downlink and 1 086 uplink requests, all received and echoed; every frame inside a window; LFNs 42.4–48.7 µA with test traffic (a request each way every 60–90 s).
- One LFN under the border router (the stage 4 set, rerun): traffic 10/10, idle 6/6 (24.0–24.7 µA), parent loss 3/3 (376.0 s), LGTK rotation 3/3, short lifetimes 3/3. With the sweep over the plan's channels only (59 frames, not 240), an LFN whose parent is gone draws 842 µA instead of 2 002 µA while it rediscovers.
- FAN 1.0 `dag_simple`, 10 seeds: logs byte-identical to `b1b9cc5`.

**Fixed with stage 5.** The FHSS read the LTO-IE at sub-ID 14 (FLUS) instead of 17, so the stage 4 conversion of the LTO offset never ran: both sides applied the phase as an offset, which kept them consistent but did not place the LFN's slots against the broadcast slots. The unit test used the same wrong number; both are fixed.

**Not done.** Router-to-router LFN handover (an LFN keeps its parent while the parent keeps it; it changes parent only after a loss); LGTKs and node roles in NVM; RFC 9685 subscriptions beyond accepting them; the parents' LFN children on D-Bus.

## 10. On the FG25 kits (stage 6)

The firmware (`nanostack-efr32fg25-freertos`) builds the router program as an LFN (`wisun_lfn`) and the border router with LFN parenting (`FW_LFN_PARENT`); its README has the build, the board-specific parts and the measurements.

**Memory errors found on the way.** The first LFN image faulted during EAPOL on the kit (an unaligned access in SHA-1, then a bus fault in the MAC's IE copy). Both were heap corruption, which the emulator did not show; the in-process programs built with AddressSanitizer (`-fsanitize=address`) found two errors:
- `supp_fwh_sec_prot.c` (stage 3 code): the LFN's NR KDE in 4-way handshake message 2 was given both to `eapol_pdu_key_frame_init()` and to `sec_prot_lib_message_build()`, which copied it a second time, 7 bytes past the frame buffer. The frame sent was correct; the heap after it was not.
- `dhcp_service_api.c` (inherited from Nanostack): the DHCPv6 relay's I/O vector was declared inside a block and used by `socket_sendmsg()` after it. FFN routers relay DHCPv6 too; with `-Os` on a target the stack slot can be reused.

With both fixed, the LFN tests (one LFN with an LGTK rotation through a router, the six-LFN mesh, one LFN under the border router) and FAN 1.0 `dag_simple` (600 s) run clean under AddressSanitizer, and FAN 1.0 `dag_simple` logs are byte-identical to `b1b9cc5`.
