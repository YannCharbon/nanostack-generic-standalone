# Wi-SUN FAN 1.1 LFN in the upper layers: one LFN under the border router

GS8 stage 3 (root repo: `documents/pre-study/investigation/LFN_FAN11_Study.md` §5, items U2–U7, U9–U13, U15). The MAC and FHSS mechanics are in `LFN_MAC.md`. This note describes what the LLC, the LFN bootstrap and the border router do, the settings, the sources each rule comes from, the deviations from Silicon Labs' implementation, and the tests.

Tags as in `LFN_MAC.md`: **[S]** a public source (`LFN_MAC.md` §7: the Arm codecs, Wireshark's dissector, `wisun-br-linux` and its `HIF.md`, RFC 8505); **[M]** observed on Silicon Labs' LFN (root repo, `HW5_BlackBox_Phase1_Results.md`, item numbers); **[I]** our choice or inference, to check in black-box phase 2.

## 1. Summary

- **Run-time only.** An interface is an LFN when the application starts it with `NET_6LOWPAN_HOST` (`arm_nwk_interface_configure_6lowpan_bootstrap_set()`); the router library contains the LFN code (`HAVE_WS_HOST`). A border router parents LFNs after `ws_management_lfn_parent_set(id, true)`, which needs FAN 1.1. With neither, nothing changes: FAN 1.0 `dag_simple` logs are byte-identical (§7).
- **Join** (`ws_bootstrap_lfn.c`): LPAS sweeps → LPA in the LFN's next listen window → EAPOL with the parent as authenticator (shorter listen interval meanwhile) → LPCS → LPC → DHCPv6 through the parent → address registration (NS with ARO) to the parent. No RPL, no MPL forwarding; a default route through the parent.
- **Parent side** (`ws_lfn_parent.c`, border router): LPAS → the LFN child and a unicast LPA with an LTO-IE; LPCS → LPC; frames to an LFN child wait for its listen window (FHSS parent role, `LFN_MAC.md` §4); an LPC to every LFN child every sync period; a MAC-unicast copy of each `ff02::1` multicast to every LFN child.
- **Parent maintenance**: the LFN declares its parent lost after 1.5 × `maintain_parent_time` sync periods with no frame from it [M item 46], then rediscovers and rejoins without EAPOL [M item 47].
- **Checked** in virtual time with `wsbrd-inproc` and `wsnode-inproc` (emulator `sim_utils/vtime/test_lfn_join.sh`, §7).

## 2. Frames and IEs (LLC, `ws_llc_data_service.c`)

| Frame | Sent by | Header IEs | Payload IEs | Security | Source |
| --- | --- | --- | --- | --- | --- |
| LPAS (type 10) | LFN, asynchronous on every channel | LUTT, NR (role LFN, interval range), LUS, LND | US, NETNAME | none | [S] codecs, [M] items 1–4 |
| LPA (9) | parent, unicast | UTT, BT, NR, PANID, LBS, LTO | US, BS, PAN, NETNAME | none | [S] `ws_mngt.c` IE list, [M] item 5 |
| LPCS (12) | LFN, unicast | LUTT, LUS | US | group key | [M] item 13 |
| LPC (11) | parent, unicast | UTT, BT, LBS | LFNVER, LGTKHASH | group key | [M] item 14, §5.2 |
| Data, EAPOL | LFN | LUTT, LUS (instead of UTT, BT) | US | as FFN | [S] |

- **Frame type** is 8 bits in `asynch_request_t` and 4 bits in an LLC message (types up to 12).
- **Receiving**: a frame with a LUTT-IE and no UTT-IE is accepted (FAN 1.1 only); its LUTT/LUS fill the sender's `lfn_uc_timing_info` (`ws_llc_lfn_timing_update()`), instead of the UTT update. LPA, LPC, LPAS and LPCS go to the bootstrap's asynchronous handler.
- **Unicast management frames** go through `ws_llc_asynch_request()` with `unicast` set: acknowledged, no channel list, so the MAC holds a frame to an LFN for its window. An LTO-IE that the LFN acknowledges is applied to the parent's view of its schedule on the ACK (`ns_fhss_ws_lfn_neighbor_lto()`).
- **LBS-IE** now has its sync-period byte (7 bytes, as Wireshark's dissector and `wisun-br-linux`; the Arm codec had 6). Nothing called the old one.
- **Deviations [I]**: the LPA has US and BS IEs (the LFN computes its parent's unicast channel and broadcast dwell from them) instead of FLUS and an LCP; the LPAS and LPCS carry the LFN's US-IE instead of an LCP; the LND-IE response delay is 0 and the parent answers in the LFN's next listen window, not in an LND slot (Silicon Labs' LFN listens 2.4 s for the LPA [M item 4], ours one window); no Silicon Labs vendor IE. These matter for interoperability (black-box phases 2–3), not inside our network.

## 3. The LFN (`ws_bootstrap_lfn.c`)

| State | What it does | Source |
| --- | --- | --- |
| Discovery (`ER_ACTIVE_SCAN`) | FHSS: own unicast channel function, no broadcast schedule; listen schedule at the proposed interval (started without a PAN: `ns_fhss_ws_lfn_listen_start()`); receiver off outside windows. An LPAS sweep every `lpas_interval_s` until an LPA comes | [M] items 1, 8 |
| LPA | Network name checked; PAN ID from PANID-IE; the parent becomes a neighbour (UTT, US, BS, BT); its broadcast schedule is given to the FHSS (uplinks avoid its dwell); LTO-IE applied (offset, adjusted interval within the NR range); LBS gives the sync period | [M] items 5–6 |
| Authentication (`ER_PANA_AUTH`) | Listen interval `eapol_listen_interval_ms` (1 250 ms), EAPOL with the parent as target (the existing supplicant); skipped on a rejoin with valid keys | [M] items 10, 36, 47 |
| Configuration (`ER_SCAN`) | Agreed interval back; LPCS, repeated every `lpcs_retry_intervals` listen intervals until an LPC | [M] items 13–15 |
| Operation | Default route through the parent; DHCPv6 Solicit to the parent's link-local address (no prefix hint); on the reply, NS with ARO (lifetime `registration_lifetime_min`) to the parent; re-registration at 92 % of the lifetime | [M] items 16–18, 52; RFC 8505 |
| Maintenance | Any secured frame from the parent keeps it; lost after 1.5 × `maintain_parent_time` × sync period without one → discovery, keys kept | [M] items 34, 46, 47 |

- **Group key before the first LPC**: the LFN sends its LPCS with the lowest group key installed by the 4-way handshake; every LPC's key index then sets the active one (as an FFN learns it from a PAN configuration).
- **Neighbours**: the LFN keeps only its parent; its entry is refreshed by every frame from it.

## 4. The border router as parent (`ws_lfn_parent.c`)

- **LPAS**: needs NETNAME, NR (role LFN), LUS, US and LUTT. The LFN becomes a neighbour (an "LFN child": `lfn_uc_timing_info` set, `ffd_device` false, lifetime 2 × the registration lifetime, refreshed by its frames; no NUD or ETX probing). One LPA per sweep: a second LPAS within 2 proposed intervals is ignored (otherwise a second LTO-IE would move the LFN's schedule in our view only).
- **LTO-IE** (`lfn_lto`, default on) [S: the rule described in `wisun-br-linux` `ws_neigh.c`, our arithmetic]: the proposed interval snapped to a multiple (above) or a divisor (below) of the LFN broadcast interval within the LFN's NR range (6 000 ms → 5 000 ms with 10 s, as observed [M item 6]); the offset a random multiple of 300 ms in [300, min(interval, broadcast interval) − 300] ms.
- **LPCS**: from a known LFN, secured → LPC.
- **Sync frames [I]**: until the LFN broadcast schedule exists (stage 4), the border router sends a unicast LPC to every LFN child every sync period (`bc_interval_ms` × `bc_sync_period`, default 50 s). Silicon Labs' parents send broadcast LTS/LPC frames in the LFN broadcast slots [M items 24, 32, §5.2]; our LFN wakes for its unicast slots only, so the sync costs one unicast reception per sync period.
- **Registration**: the LFN's NS(ARO) is handled by the existing 6LoWPAN-ND code: a registered neighbour-cache entry, through which the border router routes to the LFN; a non-FFD child is published to RPL (`rpl_control_publish_host_address`), which for the root is local.
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
| `lgtk_max_mismatch_s` | 120 | LFN | Key request after an LGTK hash mismatch [M item 49] |
| `registration_lifetime_min` | 2160 | both | ARO lifetime (36 h [M item 19]) |
| `bc_interval_ms`, `bc_sync_period` | 10000, 5 | parent | LBS-IE; sync period = their product [M §3.1] |
| `lto` | true | parent | Send an LTO-IE in the LPA |
| `tx_offset_us`, `tx_lead_us`, `tx_min_prep_us`, `tx_ttl_intervals` | 0, 20000, 2000, 20 | parent | `LFN_MAC.md` §5 |

Limits: one LFN parent per LFN, and only the border router parents LFNs (FFN routers as parents: stage 5). The split (Spinel) programs refuse the LFN roles (LFN study M12).

## 6. Not done here

- LFN broadcast schedule, LTS frames, window widening with drift (stage 4); FFN routers as parents (stage 5).
- RFC 9685 multicast subscriptions; LCP/FLUS and LND-slot answers (interoperability).
- LGTK (§8).

## 7. Tests and results

| Test | What |
| --- | --- |
| Emulator `sim_utils/vtime/test_lfn_join.sh` | Border router + LFN in virtual time with test traffic (`app_wsbrd/test_traffic.c`): join; UDP both ways; `ff02::1` received as MAC-unicast copies; every frame the LFN receives in a window (ACKs to its own frames aside); parent kept; the LFN's average current over a steady-state window, by part (EM2 ledger) |
| `tests/fhss_lfn/run.sh`, emulator `test_lfn_mac.sh` | The MAC and FHSS mechanics (`LFN_MAC.md` §6) |

Test hook: `ws_test_lfn_parent_silence()` (`net_ws_test.h`) makes the border router ignore its LFN children and send them no LPC, as a parent that is gone (emulator `SILENT_AT`, `SILENT_FOR`).

**Results** (emulator `docs/POWER_MODEL.md` §14; FG25 profile, EU mode 3, +10 dBm, −50 dBm link):
- 10 seeds with test traffic, 1800 s: all checks pass; operational 37–67 s after start; LFN 74.6–79.5 µA.
- Idle, 3 seeds, 3600 s: 41.6–42.4 µA, of which about 24 µA come from the 50 ms protocol timer (20 wakes/s), 5.8 µA from listening. With idle-tick skipping of the protocol timers (`PORTING.md`, DI6): 16.6–17.3 µA, 0.66 wakes/s, of which 0.07/s from the protocol timers; with test traffic 49.6–54.5 µA (emulator `docs/POWER_MODEL.md` §15).
- Parent loss, 3 seeds: declared 375.2–375.7 s after the last frame from the parent, rejoin without EAPOL in 21–22 s (Silicon Labs' LFN: 375 s, 21.6–21.9 s [M items 45–47]).

Non-regression (FAN 1.0, no LFN): `dag_simple` in virtual time, 10 seeds, logs byte-identical to the previous stack commit (`compare_logs.py --mask-pointers --mask-source-lines`; the second option masks the programs' own `function():line` trace prefixes, which move when their source is edited).

## 8. LGTK: not done yet

The LFN and its parent use the network's GTKs (MAC key indexes 1–4). What remains, with its sources:
- KDEs (Wi-SUN OUI): NR (type 3, node role), LGTKL (4), LGTK (5, same layout as the GTK KDE) [S: `wisun-br-linux` `common/specs/ws.h`, `kde.c`]; `kde_helper.c` has none of them.
- 4-way handshake: the LFN adds an NR KDE (role LFN) to message 2; the authenticator answers with LGTK and LGTKL KDEs instead of GTKs in message 3; the same in the group-key handshake [S: `authenticator.c` picks the LGTK group by the supplicant's node role; M items 48–50].
- Keys: three LGTKs on the border router (generation, LFN version, LGTK hash for the LPC's LGTKHASH-IE; `ws_info->lfngtk` exists), MAC key indexes 5–7 (the emulated MAC's key table has 4 entries: `sl_rf_inproc.c`, `sl_wsrcp.h`); frames to and from LFNs secured with the active LGTK.
- An LGTK event (installed, activated, expired) increments the LFN version and sends an LPC to every child (`lpc_pending` exists for it); the LFN, on an LGTK hash mismatch, requests the new key `lgtk_max_mismatch_s` later (group-key handshake); the parent must not activate a new LGTK before its LFNs can have fetched it [M item 51].
