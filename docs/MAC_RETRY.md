# MAC retries after a missing ACK (GS12)

How the MAC times a retry when a unicast frame got no acknowledgement, with
FHSS, and the change made for GS12: routers that collide at a common parent
no longer retry in step. Code: `rcp/mac/ieee802154/mac_pd_sap.c`
(`mac_csma_backoff_get()`), `rcp/mac/ieee802154/mac_retry_backoff.h`; FHSS
hold: `rcp/fhss/fhss_ws.c` (`fhss_ws_get_retry_period_callback()`). CMake
option `NANOSTACK_MAC_RETRY_SUN_BACKOFF` (default ON; OFF gives the upstream
backoff exactly). Unit test `tests/mac_retry`.

## 1. The upstream retry

After a missing ACK (`mac_sap_no_ack_cb()`), the MAC raises the backoff
exponent BE once per retry (from `macMinBE` 3 to `macMaxBE` 5, Wi-SUN's
values) and rebuilds the frame with a new TX time, `mac_csma_backoff_get()`:

1. a random draw of 0 … 2^BE − 1 units of `aUnitBackoffPeriod` = 20 symbols
   (120 µs at 150 kb/s, 400 µs at 50 kb/s);
2. 5 ms added when the result is below 5 ms (`MIN_FHSS_CSMA_PERIOD_US`);
3. the FHSS hold, `get_retry_period()`: an FFN may send unicast frames only
   in its TX slots (Arm's TX/RX slots between broadcast dwells: 54 ms each
   with the emulator's schedule, the same slots for every node of the same
   hop count), so the hold runs to the start of the next one, past the
   broadcast dwell. No hold inside a TX slot; a random 20–45 ms without the
   broadcast schedule;
4. the MAC cuts any wait at 65 ms (`mcps_calculate_tx_time()`). A cut wait
   whose CCA falls in an RX slot is refused by the FHSS, and the MAC draws
   again (`mac_sap_cca_fail_cb()`).

The first CCA comes 2 ms before the TX time (two CCA periods,
`WS_CSMA_MULTI_CCA_INTERVAL`).

## 2. The defect

The random part is at most 31 × 120 µs = 3.7 ms at 150 kb/s, less than one
data frame (7.6 ms for 126 bytes). Two routers that do not hear each other
and send to the same parent:

- collide there, so neither gets an ACK; their ACK timeouts end within one
  frame of each other;
- inside a TX slot, both retry 5 ms plus at most 3.7 ms later; otherwise
  both are held to the same TX slot start and send 5 ms plus at most 3.7 ms
  after it;
- CCA cannot help (they do not hear each other), so every retry collides
  again, whatever the draws, until the retries (3) and the request restarts
  run out.

Found with the emulator's central medium (EM4, 2026-11-05). With two such
routers and an uplink from each every 5 s at the same time (emulator, 6
seeds × 900 s), 74 of 1608 uplinks were lost, 74 % of the collisions at the
border router were repeats of a collision less than 200 ms before, and a
chain reached 12 collisions.

## 3. The change

With `NS_MAC_RETRY_SUN_BACKOFF`, a retry of an FFN (not an LFN) waits:

    hold + 5 ms + draw × (1 ms + 8 symbols)

- **The backoff unit is IEEE 802.15.4's for SUN PHYs**
  [S: IEEE 802.15.4 aUnitBackoffPeriod = aTurnaroundTime + aCcaTime, with
  aTurnaroundTime 1 ms for SUN PHYs and aCcaTime 8 symbols]: 1053 µs at
  150 kb/s, 1160 µs at 50 kb/s. Upstream's 20 symbols is the value for
  12-symbol turnarounds (O-QPSK). The draw is unchanged (0 … 2^BE − 1, BE
  raised once per retry): 0 … 15.8 ms after the first retry, 0 … 32.6 ms
  after the next ones, at 150 kb/s.
- **The draw comes after the hold**, and 5 ms is always added.
- **A total past 65 ms is not cut** (a cut wait drops the random part: both
  routers would then send at a time set by their ACK timeouts, less than a
  frame apart). The wait ends 5 ms before the hold does instead: the CCA
  falls in the RX slot, the FHSS refuses it (or returns the frame to the
  queue on the broadcast dwell), and the MAC draws again from a hold of
  about 5 ms.

Two such routers now retry less than one frame apart with a probability of
72 % after the first retry and 41 % after the next ones, 12 % for all three
retries in a row (every time before). `tests/mac_retry` computes these
shares over every pair of draws, with and without a hold, for both
variants.

**Unchanged:**

- **first attempts** (the FHSS spreads them over 0.5–10.8 ms after the TX
  slot start, when it polls its queue);
- **an LFN's retries** to its parent (`mac_rx_windows`): its FHSS hold is
  already a random 20–45 ms, which spreads them;
- **frames to an LFN's windows**: their retries go to the LFN's next window
  at a time the FHSS sets ([LFN_MAC.md](LFN_MAC.md) §4, "Retries go to the
  next window"), not through this backoff. Another frame that collided with
  one of them has no reason to come back at that window, so they are not in
  step;
- asynchronous frames (minimum CSMA period), ACKs and EDFE responses.

The cost is the longer wait of a retry: about 8 ms on average after the
first retry, 16 ms after the next ones, at 150 kb/s, against 1–2 ms. A retry
pushed out of its TX slot waits for the next one (108 ms later).

## 4. Effect in the emulator

Central medium, virtual time; uplinks are UDP test requests to the border
router, delay from the request to its reception at the border router.

| Scenario | Upstream | GS12 |
| --- | --- | --- |
| 2 routers hidden from each other, uplinks every 5 s at the same instant, 6 seeds × 900 s | 74 of 1608 lost (4.6 %); delay median 432 ms, 95th percentile 2.06 s; 3552 repeated collisions, longest chain 12 | none lost; 218 ms, 0.78 s; 389, 9 |
| 40 routers in 4 groups that do not hear each other (emulator `medium_gate_star.sh`, hidden), 3 seeds × 900 s | 200 of 2280 lost (8.8 %); 609 ms, 2.85 s; collisions 15.5 % of the frames heard | 4 lost (0.18 %); 331 ms, 0.96 s; 9.6 % |
| `dag_simple` with 50 LFNs (emulator `test_lfn_scale.sh`, `SCALE=mesh`), 3 seeds × 3000 s | 4 requests or echoes lost on the FFN backbone (MAC retries exhausted on a router hop: 0, 2, 2) | none |

Join times are unchanged in the first two (routers ready, median and last ready time
in the range of the upstream runs). FAN 1.0 logs change wherever a retry
happens, so the emulator's permanent reference runs were made again
(emulator `sim_utils/reference-runs.md`).
