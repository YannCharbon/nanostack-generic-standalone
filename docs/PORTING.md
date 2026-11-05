# Porting the stack to a new platform

This is the procedure to run the stack on a new OS, MCU or radio. For how the
stack is organised and why, read [ARCHITECTURE.md](ARCHITECTURE.md) first. The
full specification behind this guide is [GENERICITY_PLAN.md](GENERICITY_PLAN.md)
§3 (porting API, radio driver, init sequence) and §5 (the EFR32FG25 port).

Two ports exist and serve as examples:

| Port | Where | Notes |
| --- | --- | --- |
| POSIX (Linux) | `port/posix/common/` here | Used by the emulator. One process per node, timerfds and `pselect()` |
| FreeRTOS on EFR32FG25 | `port/freertos/` and `drivers/rail/` in [`nanostack-efr32fg25-freertos`](https://github.com/YannCharbon/nanostack-efr32fg25-freertos) | RAIL timers and radio, Secure Engine TRNG. Verified on two boards |
| Stub | `tests/cross/stub_port.c` here | Does nothing; shows the minimum that links |

## 1. What you have to write

| Part | What | Reference |
| --- | --- | --- |
| **Port** | The ~10 functions of `include/ns_port/*.h` (section 3) | `port/posix/`, firmware `port/freertos/` |
| **Radio driver** | A Nanostack `phy_device_driver_s` with timed CCA/TX (section 4) | firmware `drivers/rail/rail_phy.c`, emulator `app_wshwsim/sl_rf_driver.c` |
| **Heap** | `malloc()`/`free()` usable from the stack context | firmware `port/freertos/heap_wrap.c` |
| **mbedTLS** | v3.0.0, built by you, with a target configuration (section 6) | firmware `config/mbedtls_config_target.h` |
| **Application** | The init sequence and a tasklet that configures Wi-SUN (section 5) | firmware `apps/common/node.c`, `apps/br/`, `apps/rn/` |
| Optional | File storage (`ns_file_system_callbacks_set()`), a backhaul driver for a border router, AES acceleration (`arm_hal_aes.h`) | GENERICITY_PLAN.md §3.1.9–§3.1.10 |

## 2. Threading model (applies to every port)

- **The stack is single-threaded and run-to-completion.** Every stack API is
  called from one *stack context*: the task or thread that runs
  `ns_port_run()`. That includes the radio driver's callbacks into the MAC and
  all timer callbacks.
- **Interrupts only capture and notify.** A radio or timer ISR stores what it
  needs (a received frame, an expiry flag), then wakes the stack context with
  `eventOS_scheduler_signal()` or your port's own notify. It never calls the
  stack.
- **ISR-safe functions:** `eventOS_scheduler_signal()`,
  `platform_enter_critical()` / `platform_exit_critical()`, `ns_port_time_us()`.
  Nothing else.
- A port may later move the MAC's timers and radio callbacks to a
  higher-priority context ("Phase B", GENERICITY_PLAN.md §2.3). No port does
  this yet.

## 3. The porting API, function by function

Include `"ns_port/ns_port.h"` in your port; it includes all seven headers.

### `critical.h`

```c
void platform_critical_init(void);   /* once, first */
void platform_enter_critical(void);  /* nestable, from task or ISR */
void platform_exit_critical(void);
```

- Mutual exclusion against everything else that touches stack data: ISRs, and
  other tasks if any. At least 8 nesting levels. Never block inside.
- RTOS: mask the interrupts that call into your driver. FreeRTOS:
  `taskENTER_CRITICAL()`, or the `_FROM_ISR` variant with a saved mask when
  called from an ISR. The radio IRQ priority must be numerically at or above
  `configMAX_SYSCALL_INTERRUPT_PRIORITY`, so that it is actually masked.
- **Careful:** the stack holds some critical sections for hundreds of µs (see
  Pitfalls). Your radio must survive its interrupts being masked that long.

### `timer.h`: callback timers

```c
int eventOS_callback_timer_register(void (*cb)(int timer_id, uint16_t slots));
int eventOS_callback_timer_unregister(int timer_id);
int eventOS_callback_timer_start(int timer_id, uint16_t slots);   /* 1 slot = 50 µs */
int eventOS_callback_timer_stop(int timer_id);
```

- IDs are small integers that you allocate, **starting at 0** (0 is a valid
  ID; the stack treats only negative values as errors). Provide at least 12;
  the stack registers about 6, plus your driver's own.
- `start` is one-shot and relative, with 50 µs resolution or better. Starting
  an armed timer re-arms it; a pending expiry of the previous arming must be
  dropped. `stop` on an idle or expired timer is harmless.
- The callback runs in the stack context, never in the ISR. Its second
  argument is 0.
- The event loop's 10 ms system tick is one of these timers, so they must work
  before `eventOS_scheduler_init()`. With idle-tick skipping (below) it runs
  only when an event timer is due.
- Optional, `eventOS_callback_timer_start_usec(id, usec)`: a port whose timers
  have µs resolution implements it and defines `NS_PORT_TIMER_USEC` for its
  consumers (a PUBLIC compile definition of the port target). The POSIX and
  simulation ports do. The tick then lands exactly on its 10 ms grid;
  without it, a tick armed from between two slots fires up to 50 µs late.
- Pattern: the hardware timer ISR sets `expired[id]` and notifies; the run loop
  calls `cb(id, 0)`. A zero delay, or a deadline already in the past, expires
  immediately.

### Idle-tick skipping in the event loop

`NANOSTACK_EVENTLOOP_SKIP_IDLE_TICKS` (CMake, default ON; it defines
`NS_EVENTLOOP_SKIP_IDLE_TICKS` for `nanostack_eventloop` and its consumers, so a port
can test it) changes only `nanostack-event-loop/source/system_timer.c`.
The stack stays tick-driven: timers keep their 10 ms grid and fire at the same
instants as with the periodic tick. Only ticks at which no event timer is due
are skipped, so the CPU is not woken for nothing.


- The event timers keep their 10 ms ticks and their rounding, but the tick
  timer runs only when one is due. It is armed for the grid point at which the
  earliest event timer expires, and not at all when there is none.
- The elapsed ticks are counted from `ns_port_time_us()` at each tick and
  before each timer request (a grid point at exactly now counts), so a request
  is relative to the current tick, as with a periodic tick.
- `ns_port_time_us()` must therefore run on the same clock as the callback
  timers, or close to it: a tick that comes a little early counts nothing and
  re-arms for the remaining µs.
- The tick timer is armed at most 3.28 s ahead (16-bit slots), or 2^30 µs with
  `NS_PORT_TIMER_USEC`; a longer wait wakes once in between and re-arms. While
  no event timer is pending, nothing reads the tick count, so a wait longer
  than the 32-bit clock's 71.6 min is harmless.
- `eventOS_event_timer_shortest_active_timer()` gives the time to the earliest
  event timer (ms, 0 if none), for a port's sleep decision.
- `eventOS_event_timer_ticks()` and `eventOS_event_timer_request_at()` (Arm's
  API, declared in `eventOS_event_timer.h`, implemented since GS11 for the
  LFN's LPAS timer) count the grid points passed first, as the other
  requests do, so an absolute tick means the same with both ticks.

What a port must change: nothing in its API, but **its run loop must not rely
on the tick to wake up**. A timeout on its wait becomes a periodic wake of its
own. The FG25 firmware's safety timeout (`FW_RUNLOOP_SAFETY_MS`) defaults to 0
since idle-tick skipping was validated on the boards.

What it does not remove: the protocol timers (next section) and the FHSS
timers have callback timers of their own. On a Wi-SUN FFN in the emulator the
wakes go from about 115 to about 39 per second once joined (emulator
`docs/POWER_MODEL.md`).

Checks: `tests/eventloop/run.sh` runs one schedule of event timers (one-shot,
periodic, chained from handlers, cancelled, long idle gaps, the 32-bit clock
wrap) with the periodic tick and with idle-tick skipping, on a fake port, and requires the
same deliveries at the same µs. `OFF` gives the periodic tick of the
reference.

### Idle-tick skipping of the protocol timers

`NANOSTACK_PROTOCOL_TIMERS_SKIP_IDLE` (CMake, default ON; it defines
`NS_PROTOCOL_TIMERS_SKIP_IDLE` for `nanostack_eventloop` and its consumers)
applies the same principle to the upper stack's periodic timers (decision
DI6). Without it, these timers wake the CPU 20 times a second whether or not a
module has work:

- the protocol timer (`nwk_interface/protocol_timer.c`): a 50 ms callback
  timer that re-arms itself and posts the protocol event. It drives three
  timers: the core timer, TCP's and MPL's;
- the core timer (`protocol_core.c`, `protocol_core_cb()` and
  `core_timer_event_handle()`): every 100 ms it runs every module's fast
  timer, and every tenth time the modules' 1 s slow timers.

**The grids stay.** The protocol timer keeps its 50 ms grid, which starts at
`protocol_timer_init()` as before, and the core timer its 100 ms and 1 s grid
points. Only the grid points at which nothing is due are skipped.

**Protocol timer.** The callback timer is armed only for the grid point at
which the earliest of the three timers expires, and stopped when none runs.
The grid points passed are counted from `ns_port_time_us()`, as in the event
loop: by the callback timer when it fires (handed to the protocol event as
before), and before a timer is started (`protocol_timer_catch_up()`, applied
to the running timers up to the one before the earliest expiry), so that a new
timer counts from the current grid point. It stays a callback timer of its
own, on its own grid: moving it onto the event-timer service would put it on
the 10 ms tick grid with another phase, so every run would differ from the
periodic one, and its wakes would no longer be told apart in the emulator's
power ledger. The arming limits are those of the event loop's tick: 3.28 s
with 16-bit slots, 2^30 µs with `NS_PORT_TIMER_USEC`; a longer wait wakes once
on the way and re-arms. TCP and MPL re-arm their 50 ms and 200 ms ticks only
while they have work, as before.

**Core timer.** After each run, `core_timer_next()` asks every module when it
next has something to do, and the core timer is restarted for that grid point
(`protocol_timer_start_at()`), at most 250 s ahead. Each module that the core
timer drives has a `..._next()` function next to its timer function, under
`NS_PROTOCOL_TIMERS_SKIP_IDLE`:

- fast timers return the number of 100 ms ticks until the handler would act,
  slow timers the number of seconds; `UINT32_MAX` means nothing is due;
- "act" means anything other than counting down: send, change state, log,
  draw a random number. A countdown that only runs down, or a capped refill
  (`icmp_tokens`), needs no wake;
- a module that cannot tell returns 1 while it is active, which is today's
  behaviour. Skipping therefore works when it is only partly applied: a
  module without a real `..._next()` only keeps the timer at its own period.

The skipped grid points are handed to the modules later, in one call with
`ticksUpdate` > 1, which the handlers already accepted:

- by the next run of the core timer. It runs the grid points before the last
  one in one call, then the last one on its own, so that on a 1 s point the
  slow timers run first and see the fast timers' state as they would with
  the periodic timer;
- **before an event is dispatched** (`protocol_core_timer_catch_up()`, a hook
  of `eventOS_scheduler_dispatch_event()`), unless the protocol or the core
  event is pending. Code that runs in an event therefore never sees a module
  counter that lags behind the grid, and work it gives a module counts from
  the current grid point, as with the periodic timer.

And when the event queue becomes empty after events were dispatched
(`protocol_core_timer_recheck()`, the other hook), `core_timer_next()` is
asked again: an event may have given a module work due before the grid point
the core timer waits for.

For this, two handlers that counted calls now count the seconds they are
given, under the option: the Wi-SUN uptime and the join-state statistics
(`ws_bootstrap_seconds_timer()`). The MAC neighbour table asks its user when
its NUD callback would act (`user_nud_next_cb`, `ws_neighbor_entry_nud_next()`
for Wi-SUN), since that callback runs every second for every neighbour.
`CORE_TIMER_MAX_TICKS` (2500, 250 s) keeps the seconds handed over in one
call below 256: some slow timers take them as `uint8_t`.

**Covered.** Fast: the bootstrap state machine counter (every tick while it
runs), ND router objects (every tick while one exists), the Wi-SUN trickles
(PAS, PCS every tick, PA, PC), Wi-SUN NUD, EDFE, 6LoWPAN contexts, the IPv6
neighbour cache, address state timers, MLD reports, RPL DIO trickle and DAO
timers, router advertisements, the PAE supplicant (every tick while its timer
runs), the PAE authenticator (every tick while its timer runs, and the key
storage's scatter timer). Slow: the Wi-SUN seconds timers (FFN, LFN, uptime
and MDR analysis, EAPOL temporary entries, the blacklist), the MAC neighbour
table and its NUD callback, ETX, address lifetimes, the IPv6 neighbour cache
and its GC, `ReachableTime`, MLD's query timer, MPL seeds and the messages
they hold, RPL (below), routes, the destination cache GC, both reassembly
timers, DNS, the 6LoWPAN high-priority state, the PAE supplicant and the
frame-counter store.

**RPL's slow timer** (`rpl_control_slow_timer_next()`) is due at the earliest
of: a DIO route or a published, unheld DIO prefix whose lifetime ends; the
parent selection timer; the repair DIS timer while there is no preferred
parent; a DAO target that expires or a published target to refresh; every
second while an address registration to the parents is pending or RPL's
memory is above its soft limit (one item purged a second); and the next
second when an own address has no DAO target yet. All are countdowns linear
in the seconds. Two details make a call for several seconds give what the
periodic timer would have:
- the statistics counter of seconds without a next hop counts the seconds
  given, not the calls;
- `rpl_control_publish_own_addresses()` copies each own address's lifetime
  into its DAO target every second, after the address timer has aged it and
  before the target is aged: called for n seconds it copies the lifetime plus
  n - 1, which the target's ageing takes off again. A target whose address
  exists therefore expires when the address's lifetime reaches 1, whatever
  its own value (which may be stale after the address was renewed).

**Deadlines on the core time.** MPL's buffered messages and the 6LoWPAN
high-priority state end at a core time (100 ms ticks), checked by a slow
timer. A call for several seconds runs with the core time of its last grid
point, so a wake for "the second at which the deadline has passed" could act
in the first part of the call, one grid point early. Their `..._next()`
round the whole seconds left down instead (at least 1): the wake comes at or
before the first second that sees the deadline passed, and an early wake does
nothing.

**Not covered** (every tick or second while active, as before): the border
router's own timers (`ws_bbr_seconds_timer()`, the 6LBR and LFN-parent
timers), the PAE authenticator's slow timer (GTK and LGTK lifetimes,
supplicant entries, key storage: every second), and the IPv6 backhaul
interface's ND timers (`IF_IPV6`). The bootstrap state machine counter counts
calls, not ticks: counting it in ticks would let it run during a long-delayed
core event (in the emulator, a node frozen during boot by `run.sh RESTART`),
which changes the timing of the bootstrap. It stays at every tick while it
runs: about 10 wakes a second on an FFN that searches for a network.

**What a port must change: nothing.** No porting API changes and no new
callback timer. Its run loop must dispatch events through
`eventOS_scheduler_dispatch_event()` (or `eventOS_scheduler_run_until_idle()`),
which runs the hooks. A port without `NS_PORT_TIMER_USEC` wakes every 3.28 s
during a long idle period (16-bit slots); implementing
`eventOS_callback_timer_start_usec()` removes these wakes, for the event
loop's tick as well.

**Limit.** A module whose timer state is changed outside an event (from a
callback timer or a poll hook) and that no event follows is re-checked only
at the next event. In the stack, the MAC hands its indications and confirms
to the upper layers from its tasklet, and the paths that run from a callback
timer post events.

Statistics: `-DNS_PROTOCOL_TIMERS_SKIP_STATS` (C flag, for tuning) prints on
stderr at exit which module asked for each core timer wake.

Effect in the emulator (FG25 profile, EU mode 3; emulator
`docs/POWER_MODEL.md` §15): an idle LFN (3 seeds, 3600 s) from 41.6–42.4 µA and
20.6 wakes/s to 16.6–17.3 µA and 0.66 wakes/s; Wi-SUN FFN routers once joined
(`dag_simple`, 10 seeds) from 37.7 to 19.3 wakes/s (protocol timer 20.0 to
1.17). With RPL's slow timer and the authenticator's fast timer covered
(`dag_simple`, 3 seeds, 300–600 s): routers' protocol timer from 1.17 to 0.29
wakes/s (RPL's slow timer from about 0.9 to 0.02), the border router's from
10.0 to 1.8.

Checks: `tests/protocol_timers/run.sh` runs the stack's `protocol_timer.c`,
`protocol_core.c` and event loop on a fake port with fake modules (countdowns
in ticks and in seconds, a module counting calls, a period that keeps its
phase, TCP and MPL style users), one schedule of events that start work and
read the modules' state at arbitrary instants, long idle gaps and the 32-bit
clock wrap. It requires the same actions and observations with the periodic
timers, with skipping on µs timers and with 50 µs slots. It also has a
lifetime copied every second from another module and aged (RPL's own DAO
targets) and a deadline on the core time (MPL's messages). `OFF` gives the
periodic timers of the reference; with `ON`, `dag_simple` and the LFN tests
give the same logs as `OFF` (emulator `docs/POWER_MODEL.md` §15).

### Housekeeping with nothing to do

`NANOSTACK_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING` (CMake, default ON; effective
only with `NANOSTACK_PROTOCOL_TIMERS_SKIP_IDLE`; it defines
`NS_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING` for `nanostack_eventloop` and its
consumers) refines four `..._next()` functions of the previous section. Without
it, these modules are due at every expiry of their periodic timers, even when
the expiry does nothing: an idle LFN then wakes about 4.3 times a minute for
them. With it, a module is due only when its timer would act; what its timer
does in between (ageing, phase) is applied for the whole elapsed time at its
next call, so that the result is the periodic timer's. `OFF` gives the wakes
of the previous section exactly.

| Module | What an expiry does | Due with the option | Catch-up at the next call |
| --- | --- | --- | --- |
| Neighbour cache GC (`ipv6_neighbour_cache_slow_timer()`, every 20 s, `NCACHE_GC_PERIOD`) | Removes garbage-collectible entries, but only above the long-term number of them (8 by default, `ipv6_neighbour_cache_configure()`) | The GC only above that number; a registered or tentative entry's lifetime expiry (it is removed then), as before. A garbage-collectible entry's lifetime only runs down (0 is an input of the GC): no wake | Lifetimes aged by the seconds given, as before; a call that spans several GC periods keeps the GC's phase (`gc_timer`), and one GC run stands for all of them (each would have found too few entries) |
| Destination cache GC (`ipv6_destination_cache_timer()`, every 20 s, `DCACHE_GC_PERIOD`) | Ages each entry's lifetime (in GC periods) and path-MTU lifetime, resets an expired path MTU (with a trace), removes entries only above the long-term number (16) | Every GC above that number; otherwise the GC that resets the first path MTU; none without either | One GC per period passed, in order: the ageing the periodic GCs would have done, and nothing else |
| MLD query timer (`mld_slow_timer()`, 125 s) | Sends a general query, only if we are the querier (`mld_querier`, which nothing in the stack sets) | The reset only if we are the querier | The counter advances modulo 125, as one second per call does |
| Blacklist purge timer (`blacklist_ttl_update()`, 30–90 s, random) | Draws the next timeout; purges only from `entry_max_nbr` − `purge_nbr` entries (7 in Wi-SUN) | **LFN:** the expiry only from that number of entries. **FFN, border router:** every expiry, as before. Entry expiries (with a trace): as before | **LFN:** the expiries passed, in turn, each drawing the next timeout: the purge timer keeps its timing law, but its draws come later in the random sequence. **FFN, border router:** as before |

The number of entries, the querier flag and the path MTUs change only in
events or in the modules' own timer actions, after which `core_timer_next()`
is asked again (previous section), so a module whose expiry would start to act
is due again in time.

**FFNs and the border router.** Their logs do not change: none of these
modules draws a random number for them, and the FFN's blacklist purge stays
as before (an FFN wakes many times a second for its FHSS anyway, and its
random sequence, hence the FAN 1.0 runs, stays the same). An FFN router loses
the cache GC wakes (`dag_simple`: protocol timer 0.29 to 0.25 wakes/s); the
border router, whose own timers run every second, does not change.

**Late timers.** A call can also span several periods when the protocol timer
fires late (in the emulator, an instance started with `--vtime-start` runs its
first timers after the time jump to its start). The caches' and MLD's phases
are then kept instead of being reset, with no other effect; an FFN's purge
timer gives one expiry, as before, so its random draws do not change. (The
slow timers that take their seconds as `uint8_t` get them modulo 256 in that
case, with the option or without.)

**Left as they are:**

- the PAE controller's frame-counter check every 60 s
  (`ws_pae_controller_frame_counter_timer()`, `FRAME_COUNTER_STORE_INTERVAL`).
  It reads the MAC's frame counters, which advance with every secured frame
  the MAC sends, and writes them to NVM when they advanced by a threshold or
  the active key changed. Skipping it would need the MAC's counter changes to
  re-check the core timer and could move NVM writes, so it stays: about 1 wake
  a minute on an idle LFN. Before, it fell in the same wake as the cache GCs
  (both periods start at the stack's start);
- `ReachableTime`'s random re-roll (2–3 wakes in 1500 s on an idle LFN) and
  the core timer's longest wait (250 s).

**Effect.** Idle LFN in the emulator (FG25 LFN profile, EU mode 3, under the
border router or a router, steady state 300–1800 s, the
`NS_PROTOCOL_TIMERS_SKIP_STATS` build): 108–109 core timer wakes in 1500 s
(4.3–4.4 a minute: neighbour cache GC 75, Wi-SUN seconds timer 25, MLD 8–9)
become 26 (1.04 a minute: frame-counter check 22–23, `ReachableTime` 2–3,
destination cache 1); the LFN's protocol timer wakes 0.07 to 0.02 a second.
On the FG25 kits (AEM at 100 kHz, idle LFN under a router, default LFN
settings): the wakes without radio activity go from 4.0 to 1.0 a minute (one
every 60 s), 6.05 µC each: 0.40 to 0.10 µA (emulator `docs/POWER_MODEL.md`
§15.2).

Checks: `tests/housekeeping/run.sh` runs the stack's `ipv6_routing_table.c`,
`mld.c` and `blacklist.c` on stubs, called every second (the reference code)
and as the skipping core timer calls them (the due second, at most 250 s
ahead, and the catch-up before each event), on one schedule of events over
66 000 s that adds, refreshes and removes neighbour cache entries
(garbage-collectible and registered), destinations and path MTUs, blacklist
entries, changes the caches' thresholds and the querier flag, and reads every
module's state; then 6000 s idle. It requires the same traces, removals, MLD
queries, state reads and random draws with the periodic timer, with
`NS_PROTOCOL_TIMERS_SKIP_IDLE` alone, and with the option as an LFN and as an
FFN, and, as an LFN, at most one wake per 250 s once idle. In the emulator,
`dag_simple` gives the permanent reference runs' logs with the option ON and
OFF, and the LFN suite passes.

### Idle-poll skipping of the FHSS TX queue

`NANOSTACK_FHSS_POLL_SKIP_IDLE` (CMake, default ON; it defines
`NS_FHSS_POLL_SKIP_IDLE` for `nanostack_eventloop` and its consumers, the RCP
library among them) removes the FHSS's 10 ms TX queue poll while there is
nothing to send (GS9). Until its schedules start (no unicast or broadcast
schedule timer: a router that searches for a network, on a fixed channel),
the FHSS polls the MAC's TX queue every 10 ms (`DEFAULT_POLL_PERIOD`,
`fhss_event_timer_cb()` in `rcp/fhss/fhss_ws.c`), whether or not a frame
waits: about 100 wakes a second. LFNs never had it.

- A poll that finds the unicast queue empty does nothing but re-arm itself.
  With the option it is skipped: the poll timer is left unarmed and the time
  of that poll is kept (`idle_poll_skipped`, `idle_poll_at_us`).
- Whatever could make that poll act differently first restarts it on its
  10 ms grid (`fhss_ws_idle_poll_resume()`): the first poll at or after now,
  at the µs the periodic poll would have run. That is a frame put in the
  unicast queue (the MAC calls `fhss_api->tx_queued`, a new optional member
  of the FHSS API, from `mcps_sap_pd_req_queue_write()`), and every change
  of the FHSS state, before the change (`synch_state_set`, expedited
  forwarding included, `ns_fhss_ws_set_parent()`,
  `ns_fhss_ws_configuration_set()`, `ns_fhss_ws_lfn_listen_set()`), which may
  then re-arm or stop the timer as it would have the periodic one.
- While the queue holds frames, the poll runs every 10 ms as before; frames
  sent from the MAC's TX confirm are not affected.

What a port must change: nothing. A port without `NS_PORT_TIMER_USEC` arms
the restarted poll in 50 µs slots, rounded up: it may come up to 49 µs after
the periodic one.

Effect in the emulator (FG25 profile, EU mode 3; emulator
`docs/POWER_MODEL.md` §15): two routers searching for a network, 300–600 s,
from 103.7 to 13.8 wakes/s (FHSS 100.0 to 0.13), 8.36 to 8.29 mA (listening
dominates).

Checks: `tests/fhss_poll/run.sh` runs the stack's FHSS on a fake port and a
fake MAC through its states (fixed channel, unicast hopping, synchronized with
a broadcast schedule and a parent, expedited forwarding, unsynchronized) with
frames queued at arbitrary instants, some of which cannot go at the first
poll, and requires the same polls at the same µs with the periodic poll and
with skipping. With `ON`, `dag_simple` and the LFN tests give the same logs as
`OFF`; with `OFF`, the emulator's power ledger is identical to the previous
stack's.

### `sched.h`: scheduler signal and run loop

```c
void ns_port_init(void);                    /* once, before eventOS_scheduler_init() */
void eventOS_scheduler_signal(void);        /* "events pending", ISR-safe */
void ns_port_run(void) __attribute__((noreturn));
void ns_port_add_poll_hook(void (*hook)(void));
```

- `ns_port_init()` sets up what the other functions need: the event pipe on
  POSIX; the stack task handle and RAIL multitimers on FreeRTOS.
- `ns_port_run()` never returns. Each iteration dispatches pending events
  (`eventOS_scheduler_run_until_idle()`), expired callback timers, expired FHSS
  timers and the poll hooks, then sleeps until signalled or until a timer
  expires. The order within an iteration is the port's choice.
- Poll hooks are extra work in the stack context: the radio driver's bottom
  half (deliver received frames, handle TX completion), a console, and so on.
- POSIX extras (`port/posix/common/os_scheduler.h`):
  `ns_port_posix_add_fd()` adds a file descriptor with its callback,
  `ns_port_posix_add_scheduler()` places the event loop among them,
  `ns_port_posix_set_busy()` and `ns_port_posix_set_poll_timeout()` control
  when `pselect()` polls.
- FreeRTOS: signal = `xTaskNotifyGive()` (or the `FromISR` variant);
  the loop sleeps with `ulTaskNotifyTake()`.

### `random.h`

```c
void ns_port_random_bytes(void *buf, size_t n);
```

- **Must be a real TRNG on hardware.** The stack's `rand_*` helpers are built
  on it, and `rand_get_8bit()` is registered as an entropy source of the TLS
  DRBG.
- Must not block for long, and should not be called from an ISR. A small pool
  refilled in task context works; refill it ahead of time, outside critical
  sections, because the stack also draws random numbers inside them (see
  Pitfalls).
- Do not implement the `rand_*` helpers yourself: they are in the stack
  (`source/ns_rand.c`, built into `nanostack_libservice`).

### `time.h`

```c
uint32_t ns_port_time_us(void);     /* monotonic µs, wraps at 2^32 (~71 min), ISR-safe */
uint64_t ns_port_time_wall_s(void); /* optional; 0 if unknown */
```

- This must be **the same clock as the radio driver's timestamps**:
  `PHY_EXTENSION_GET_TIMESTAMP`, `PHY_EXTENSION_READ_RX_TIME`, the CSMA
  `backoff_time` and `fhss_get_timestamp()` are all absolute µs on this one
  32-bit base. All comparisons must be wrap-safe (`(int32_t)(a - b)`).

### `fhss_timer.h`

```c
extern const struct fhss_timer ns_port_fhss_timer;   /* Nanostack's fhss_timer_t */
```

- Fill in `fhss_timer_start(us, cb, api)`, `fhss_timer_stop(cb, api)`,
  `fhss_get_remaining_slots(cb, api)` (in µs), `fhss_get_timestamp(api)`
  (= `ns_port_time_us()`), `fhss_resolution_divider = 1`.
- One pending timeout **per distinct callback**: the FHSS uses up to 4
  (the FFN unicast and broadcast schedules, and the FAN 1.1 LFN's listen
  timer and LFN broadcast-slot timer of `docs/LFN_MAC.md`).
  Starting one that is already armed replaces it. Callbacks run in the stack
  context.
- The application passes it to `ws_management_node_init()`.

### `trace.h`

```c
void ns_port_trace_write(const char *line);   /* one complete line, no newline */
```

- The application installs it with `mbed_trace_print_function_set()`.
- It must not block the stack: buffer it and drain it from a low-priority
  context. The stack traces a lot during joins; drop and count lines rather
  than stall. Set `MBED_TRACE_MAX_LEVEL` to bound flash use.

### Example: the simulation port

`port/sim/` is a second implementation of this API on the host, for the
emulator's virtual time (README, "Simulation port"; contract in
`port/sim/ns_port_sim.h`). It shows how little of the API depends on a real
clock: timers are deadlines in a table, `eventOS_scheduler_signal()` sets a
flag, `ns_port_run()` runs until idle and then asks an external conductor for
the next point in time, and `ns_port_time_us()` returns the virtual time. It
reuses the POSIX port's critical sections, log, trace and lists. It must
never be used on hardware: its random source is a seeded PRNG.

## 4. The radio driver

The MAC talks to the radio through Nanostack's `phy_device_driver_s`
(`nanostack/nanostack/mac/platform/arm_hal_phy.h`). The full contract table is
GENERICITY_PLAN.md §3.2. The essentials:

- **Registration:** `link_type = PHY_LINK_15_4_SUBGHZ_TYPE`, `phy_MTU = 2043`,
  no PHY header or tail. Register with `arm_net_phy_register()`, then set the
  EUI-64 with `arm_net_phy_mac64_set()`.
- **No FCS in the frames.** The MAC neither adds nor checks it: the radio must
  append it on TX and check it on RX, and pass up only frames with a correct
  CRC.
- **Timed TX.** `tx()` only stores the frame. The TX starts at the absolute
  time given by the last `PHY_EXTENSION_SET_CSMA_PARAMETERS` (`backoff_time`;
  0 means abort and go back to RX). Before the CCA, call
  `phy_tx_done_cb(..., PHY_LINK_CCA_PREPARE, ...)`; the MAC may change channel
  or refuse there. Then report `CCA_FAIL`, `CCA_FAIL_RX` or the TX result.
  Enh-ACKs are scheduled only about 1 ms ahead.
- **Every started TX must end with a callback.** If your radio can lose a
  scheduled TX without an event, add a watchdog (see Pitfalls).
- **RX:** call `phy_rx_cb(psdu, len, lqi, rssi, id)` from the stack context. Keep
  the sync-word time for `PHY_EXTENSION_READ_RX_TIME`, on the
  `ns_port_time_us()` base.
- **Channels:** `PHY_EXTENSION_SET_CHANNEL` changes the RX channel when idle,
  receiving or waiting for CSMA. Otherwise store it and return -1.
- **Radio off:** `state_control(PHY_INTERFACE_DOWN)` turns the receiver off
  (a TX in progress ends first); `PHY_INTERFACE_UP` turns it on, on the given
  channel. A FAN 1.0 FFN never asks for off; a node with RX off when idle
  (GS8 stage 1) and an LFN (`docs/LFN_MAC.md`) do.
- **Optional, for an LFN:** `PHY_EXTENSION_READ_RX_ACTIVE` writes one byte, 1
  while a frame is being received (sync word seen, frame not complete). The
  MAC then closes an LFN receive window only after that frame. Without it
  (return -1) the window closes on time and a frame arriving at its end is
  lost.
- **Optional, for an LFN:** `PHY_EXTENSION_SET_BACKOFF_RX_OFF` takes one
  byte, set before each CSMA-CA transmission of a node whose receiver is off
  when idle (an LFN outside its windows): 1 means the receiver need not
  listen during the coming backoff (it is on for the CCA, the TX and the
  ACK), 0 means it listens as usual. A node whose receiver is on when idle
  never sends it. Without it (return -1) the radio listens during the
  backoff: 20–45 ms of RX per MAC retry of an LFN.
- The EFR32 driver also documents the RAIL specifics: scheduled-TX lead time,
  `CCA_OK` reporting, radio config (firmware README, "Design notes").

## 5. Build integration and init sequence

### CMake

```cmake
# Your port: implements include/ns_port/*.h
add_library(my_port STATIC my_port/critical.c my_port/core.c my_port/random.c my_port/trace.c)
target_link_libraries(my_port PUBLIC nanostack_port_api)        # the API headers
target_link_libraries(my_port PRIVATE nanostack_eventloop)      # if it calls the event loop

# Your mbedTLS v3.0.0 build, under the names the stack links
add_library(MbedTLS::mbedtls    ALIAS my_mbedtls)
add_library(MbedTLS::mbedcrypto ALIAS my_mbedtls)
add_library(MbedTLS::mbedx509   ALIAS my_mbedtls)

set(NANOSTACK_PORT my_port CACHE STRING "" FORCE)
set(NANOSTACK_FILE_NO_STDIO ON CACHE BOOL "" FORCE)             # if no file system
add_subdirectory(path/to/nanostack-generic-standalone nanostack)

nanostack_add_library(ns_br CONFIG ws_border_router BORDER_ROUTER)   # and/or
nanostack_add_library(ns_rn CONFIG ws_router)
nanostack_add_rcp_library(ns_mac INPROCESS)                     # MAC in the same image

# The stack's code needs these with GCC >= 14
foreach(t ns_br ns_rn ns_mac nanostack_eventloop nanostack_libservice)
    target_compile_options(${t} PRIVATE -std=gnu17 -fpermissive)
endforeach()

# The libraries reference each other: link them as a group
target_link_libraries(my_app PRIVATE
    -Wl,--start-group ns_rn ns_mac nanostack_eventloop nanostack_libservice
    my_port my_radio my_mbedtls -Wl,--end-group)
```

`tests/cross/CMakeLists.txt` is a complete, working example; the firmware's
`cmake/nanostack.cmake` is the real one.

### Init sequence

```c
platform_critical_init();
mbed_trace_init();
mbed_trace_print_function_set(ns_port_trace_write);
my_radio_hw_init();                    /* if ns_port_init() needs the radio timer */
ns_port_init();
eventOS_scheduler_init();
rf_id = my_radio_register(eui64);      /* arm_net_phy_register + arm_net_phy_mac64_set */
mac = ns_sw_mac_create(rf_id, &(mac_description_storage_size_t){ 32, 4, 1, 3 });
net_init_core();
ws_if = arm_nwk_interface_lowpan_init(mac, "ws0");   /* calls mac_initialize itself */
/* border router: a backhaul interface (bh_if), e.g. a null driver */
eventOS_event_handler_create(app_tasklet, ARM_LIB_TASKLET_INIT_EVENT);
ns_port_run();
```

In `app_tasklet`'s init event, as the emulator's `wsbr_configure_ws()` does:

- `arm_nwk_interface_configure_6lowpan_bootstrap_set()`;
- `ws_management_node_init(ws_if, domain, name, (fhss_timer_t *)&ns_port_fhss_timer)`;
- regulatory domain, channel functions, network size and certificates;
- `arm_nwk_interface_up()`;
- border router: `ws_bbr_start()`.

Do not call `mac->mac_initialize()` yourself.

## 6. mbedTLS configuration

The stack needs mbedTLS v3.0.0. Required features (GENERICITY_PLAN.md D12):

- TLS 1.2 client and server, ECDHE-ECDSA-AES128-CCM8, P-256;
- X.509 with CRL and PEM;
- CTR_DRBG, NIST_KW, MD5, SHA-256, CCM.

On a target:

- **Register a strong entropy source.** By default the stack registers its own
  source (the port's random bytes) as *weak*, so with
  `MBEDTLS_NO_PLATFORM_ENTROPY` and nothing else, every TLS handshake fails
  ("drbg seed fail"). Either define `MBEDTLS_ENTROPY_HARDWARE_ALT` and implement
  `mbedtls_hardware_poll()` on your TRNG (what the EFR32 firmware does), or, if
  `ns_port_random_bytes()` is a real TRNG, build with
  `NANOSTACK_TLS_STRONG_PORT_ENTROPY=ON` so the stack registers it as strong.
- **Undefine `MBEDTLS_HAVE_TIME` and `MBEDTLS_HAVE_TIME_DATE`** if there is no
  real-time clock. Otherwise certificates appear "not yet valid" at 1970.
- Remove `MBEDTLS_NET_C`, `MBEDTLS_TIMING_C`, `MBEDTLS_FS_IO` and
  `MBEDTLS_PSA_ITS_FILE_C`, which need an OS.

## 7. Pitfalls found so far

- **Long critical sections can make the radio lose a TX.** The FHSS handlers
  hold critical sections while they update the hopping state. Before GS6 the
  broadcast-slot handler kept a trace and a random draw inside its section,
  which lasted up to about 600 µs on the EFR32FG25 (Cortex-M33). That masked
  RAIL's interrupts across a scheduled CCA TX start; RAIL then silently
  dropped the TX, and the driver waited for it forever. The trace and the draw
  now run after the section, as does `fhss_ws_set_parent()`'s trace. On the
  board the longest sections are now about 170–250 µs, with one 396 µs
  outlier at MAC start. That is still long, so give your driver a **TX
  watchdog**: if no TX event has come by the start time + air time + a margin,
  abort, report `CCA_FAIL` and go back to RX (firmware `rail_phy.c`).
- **Don't block inside critical sections.** Since GS6 the stack draws no
  random numbers inside its critical sections, but keep your TRNG non-blocking
  anyway: a blocking refill inside any critical section has the same effect.
- **Printf with 64-bit values.** The stack traces with `PRIu64`/`PRIi64`.
  newlib-nano cannot format them, mis-reads the following arguments and
  crashed the border router. Use full newlib, or a printf that supports them.
- **Compiler flags.** With GCC ≥ 14 the inherited code needs
  `-std=gnu17 -fpermissive`.
- **Device table size.** The `device_description_table_size` given to
  `ns_sw_mac_create()` also sizes the Wi-SUN neighbour, ETX and MAC-neighbour
  tables. The emulator uses 255; use 32–64 on an MCU.
- **Heap.** No pool and no upper bound: the stack uses `malloc()` directly. Make
  it thread-safe, route it to one accounted heap, and log the high-water mark.
  The EFR32 BR peaks at about 46 kB in a two-node network.
- **Long CPU stalls.** EAP-TLS elliptic-curve maths blocks the stack context
  for 0.7–0.9 s on the EFR32FG25 (Cortex-M33). Joins still work, but ACK timing
  suffers meanwhile. Use hardware ECC or a higher-priority MAC context if that
  matters.
- **Timing differences from the emulator.**
  - FHSS drift compensation is compiled only when `__linux__` is not defined
    (`rcp/fhss/fhss_ws.c:1176`), so it runs on targets but not in the
    emulator.
  - In-process, the MAC reads exact timestamps and starts with a 127-byte MTU
    until Wi-SUN switches it to 802.15.4g (GENERICITY_PLAN.md §2.2).
- **No persistence by default.** With a NULL storage root path, keys and frame
  counters are not stored, and every reboot is a fresh join. Provide
  `ns_file_system_callbacks_set()` on flash (e.g. NVM3) when that matters.
- **Licence.** The stack contains Silicon Labs MSLA code (README, Licensing).
  Check that your target and use fall within it.

## 8. Checklist and verification

1. **Link:** build your port against the stack, and link a border router and a
   router image. `tests/cross/` does this with a stub port; replace the stub
   with yours. No undefined symbol, and no file or POSIX calls unless you want
   them.
2. **Host twin (recommended):** compile your port, driver and application on
   Linux against fakes of the OS and radio, and run two nodes over a socket
   medium. The firmware's `tests/host_twin/` shows how; it caught the entropy
   problem before hardware.
3. **Timers on the target:** check timer jitter (< 50 µs) and that re-arming
   and zero-delay starts behave.
4. **Two boards:** border router and router node. Check the join (EAP-TLS, GTK,
   RPL parent, DHCPv6 address), ping and UDP both ways, the Enh-ACK turnaround
   (it must be well inside the peer's ACK wait of about 10–21 ms) and heap use.
   Repeat over several cold boots: some problems show in a fraction of boots
   only. The firmware's `tests/hw/run_hw.sh` runs one boot.
5. **Keep the stack untouched.** If the stack needs a change, make it in this
   repository, gated as in GENERICITY_PLAN.md §4.3 (identical objects for the
   emulator's build, `dag_simple` runs when behaviour changes).
