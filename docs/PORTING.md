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

What a port must change: nothing in its API, but **its run loop must not rely
on the tick to wake up**. A timeout on its wait becomes a periodic wake of its
own. The FG25 firmware keeps its 100 ms safety timeout until idle-tick skipping
is validated on the boards; `FW_RUNLOOP_SAFETY_MS=0` in its CMake removes it.

What it does not remove: the protocol timer (50 ms, `protocol_timer.c`) and the
FHSS timers have callback timers of their own. On a Wi-SUN FFN in the emulator
the wakes go from about 115 to about 39 per second once joined (emulator
`docs/POWER_MODEL.md`).

Checks: `tests/eventloop/run.sh` runs one schedule of event timers (one-shot,
periodic, chained from handlers, cancelled, long idle gaps, the 32-bit clock
wrap) with the periodic tick and with idle-tick skipping, on a fake port, and requires the
same deliveries at the same µs. `OFF` gives the periodic tick of the
reference.

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
- One pending timeout **per distinct callback**: the FHSS uses up to 3
  (unicast, broadcast, and the FAN 1.1 LFN timer of `docs/LFN_MAC.md`).
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
