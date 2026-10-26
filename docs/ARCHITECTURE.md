# Architecture: how the stack was made generic

This repository holds the Wi-SUN Nanostack that the mesh emulator uses. It is
one portable library: the same code runs in the emulator on Linux and on
FreeRTOS on the EFR32FG25. The OS-specific code sits behind a small porting
interface, and each platform provides its own *port*.

The stack code itself was not changed by this work: Nanostack, the event loop,
libservice and the MAC/FHSS/CCM objects are still byte-identical to the
emulator it came from. Only the code around it was reorganised.

How to write a new port: [PORTING.md](PORTING.md). The step-by-step plan and its
verification gates: [GENERICITY_PLAN.md](GENERICITY_PLAN.md).

## Before: inside `wisun-mbed-simulator`

The stack was not a component of its own. It was mixed into the emulator's
source tree:

```
wisun-mbed-simulator/
  nanostack/  nanostack-event-loop/  mbed-client-libservice/   upper stack
  app_wshwsim/mac/  fhss/  ccm/                                 802.15.4 MAC (RCP side)
  common/       Linux OS layer and emulator plumbing, mixed:
                timers (timerfd), scheduler signal (pipe), critical
                section (pthread mutex), random (/dev/urandom), log, lists,
                the UART/Spinel transport, and one global struct for both
  app_wsbrd/  app_wshwsim/  app_wssimserver/                    the programs
```

Three things made it non-portable:

1. **No boundary.** The stack included `common/rand.h` and
   `common/hal_interrupt.h` directly. Behind them were Linux files
   (`/dev/urandom`, a pthread mutex). Another platform had to provide files with
   exactly those names.
2. **Mixed state.** One global, `g_os_ctxt`, held both the OS state (event
   pipe, timer list) and the UART buffers of the emulator's serial link.
3. **No run loop of its own.** `wsbrd`, `wsnode` and `wshwsim` each had a copied
   `pselect()` loop in `main()`, which polled the timers and dispatched the
   stack's events. The FHSS platform timer lived in `wshwsim`.

## Step 0: extraction (tag `baseline-2026-10-23`)

The tree was split 1-to-1, with git history, into this repository (stack,
MAC in `rcp/`, Linux layer in `port/linux/`) and
[`mesh-emulator-orchestrator`](https://github.com/YannCharbon/mesh-emulator-orchestrator)
(the programs, which link the stack as libraries). It was the same code with
the same coupling, but in its own repository. `MIGRATION.md` in the
orchestrator records the mapping and the equivalence checks.

## After: steps S1–S5

```
nanostack-generic-standalone/
  include/ns_port/     the porting API: what any port must provide
      ns_port.h        includes all of them
      critical.h  timer.h  sched.h  random.h  time.h  fhss_timer.h  trace.h
  include/ns_rand.h    the stack's rand_* helpers (API)
  source/ns_rand.c     their implementation, on ns_port_random_bytes()
  nanostack/  nanostack-event-loop/  mbed-client-libservice/  rcp/    unchanged
  port/posix/common/   the POSIX port (Linux), used by the emulator
  tests/cross/         Cortex-M33 build with a stub port (CI)
```

### The porting API (S1)

A port provides about ten functions, declared in seven headers:

| Header | What the port provides | POSIX port | FreeRTOS/EFR32 port |
| --- | --- | --- | --- |
| `critical.h` | enter and exit a critical section | recursive pthread mutex | `taskENTER_CRITICAL()` |
| `timer.h` | 50 µs callback timers (`eventOS_callback_timer_*`) | one timerfd per timer | RAIL multitimers |
| `sched.h` | `ns_port_init()`, `eventOS_scheduler_signal()`, `ns_port_run()`, `ns_port_add_poll_hook()` | pipe and `pselect()` | task notification |
| `random.h` | `ns_port_random_bytes()` | `/dev/urandom` | Secure Engine TRNG |
| `time.h` | `ns_port_time_us()`, the radio's µs timebase | `CLOCK_MONOTONIC` | `RAIL_GetTime()` |
| `fhss_timer.h` | `ns_port_fhss_timer`, Nanostack's `fhss_timer_t` | one timerfd per callback | RAIL multitimers |
| `trace.h` | `ns_port_trace_write()`, one trace line | stdout | UART ring buffer |

The stack includes only these headers. The compatibility headers
`common/rand.h` and `common/hal_interrupt.h`, which forwarded the old includes
during the migration, were removed once nothing used them. Where Arm
Nanostack already had a name
(`platform_enter_critical()`, `eventOS_*`), the name was kept, so the API
matches upstream Nanostack. New functions use the `ns_port_` prefix.

The stack also uses `malloc()`/`free()` directly, stores files through
`ns_file_system_callbacks_set()` and does AES through `arm_hal_aes.h`,
implemented on mbedTLS by `rcp/ccm/`.

### Moving code to the right side (S2)

- **Random numbers.** The maths helpers (`rand_get_random_in_range()`,
  `rand_randomise_base()`, …) moved into the stack (`source/ns_rand.c`). The
  port only supplies raw random bytes.
- **OS context.** The global was split: the port keeps its own state (event
  pipe, timer lists), and the UART fields moved to the emulator
  (`struct uart_ctxt` in its `common/bus_uart.h`).
- **FHSS timer.** It moved from `wshwsim` into the port, as
  `ns_port_fhss_timer`.
- **Rename.** `port/linux` became `port/posix`. The include paths stay
  `"common/xxx.h"` for the programs that use the port's helpers (log, lists).

### Choosing the port at build time (S3)

- `NANOSTACK_PORT` names the CMake target that implements the API. The default
  is `nanostack_port_posix`. The firmware sets it to its own `ns_port_freertos`.
  With another port, Threads, libbacktrace and libdl are not used.
- mbedTLS can come from the consumer, as `MbedTLS::*` targets.
- `NANOSTACK_FILE_NO_STDIO` removes the stdio fallback of `ns_file_system.c`,
  for targets without a file system.
- So the firmware stopped copying the stack's source lists and faking
  `common/*.h`. It calls `add_subdirectory()` on this repository.
- `tests/cross/` builds everything for a Cortex-M33 with a stub port and links
  a border router and a router image. A GitHub workflow runs it, with the host
  build, on every push.

### One run loop, owned by the port (S4)

`ns_port_run()` replaces the three copied loops. Each emulator program
registers its own inputs (D-Bus, TUN, UART, radio socket) with
`ns_port_posix_add_fd()`, in the order its old loop handled them, and places
the event loop among them with `ns_port_posix_add_scheduler()`. The port runs
the stack's events, the callback timers and the FHSS timers. Callback timer
IDs became small indices, as the API specifies, instead of timerfds.

### MAC in the same process (S5)

On the boards the MAC runs in the same image as the upper stack. In the
emulator it ran in a separate process (`wshwsim`) behind a serial link.

- `nanostack_add_rcp_library(<name> INPROCESS)` builds the MAC for an image
  that also holds the upper stack. It leaves out `rcp/`'s duplicate
  `rf_driver_storage.c`.
- The emulator gained `wsnode-inproc` and `wsbrd-inproc`, which run like the
  firmware but on the simulated radio.
- The split mode (stack process + `wshwsim` over Spinel) stays the emulator's
  default.

## The result, as layers

```
   wsbrd, wsnode, wshwsim, *-inproc           wisun_br, wisun_rn
   (mesh-emulator-orchestrator)               (nanostack-efr32fg25-freertos)
            │                                          │
   ┌────────┴──────────── the same stack code ─────────┴────────┐
   │  Nanostack · event loop · libservice · MAC/FHSS/CCM ·       │
   │  ns_rand — includes only include/ns_port/*.h                │
   └────────┬───────────────────────────────────────────┬───────┘
     port/posix (this repo)                  port/freertos (firmware repo)
     timerfd, pipe, pthread, urandom         RAIL timers, FreeRTOS, SE TRNG
```

A new platform (Zephyr, bare metal, another radio) means one new port against
the seven headers, plus a radio driver: see [PORTING.md](PORTING.md). The stub
port in `tests/cross/stub_port.c` shows the minimum that links.

## How it was verified

Each step was either a pure move or a behaviour change, never both:

- **Pure moves:** every Nanostack, event loop, libservice and MAC object was
  compared with the reference build (`cmp_objs.py`). All are still identical.
- **Behaviour changes** (the port and the emulator programs): 5 runs of the
  15-node `dag_simple` scenario per step, against 5 reference runs, compared
  on join milestones, parents and log messages (`analyze.py`).
- **Non-POSIX build:** the cross-build check, and the firmware on two
  EFR32FG25 boards.

The results are in the plan's progress table and in the orchestrator's
`MIGRATION.md`.

## What is not generic yet

- **One instance per process.** About 84 file-scope variables make the stack
  single-instance, so the emulator runs one process per node. A single-process
  simulator would need them grouped into a per-node context (GENERICITY_PLAN.md
  §2.4). This is not planned: the emulator keeps one process per node and gets
  virtual time from a conductor and a simulation port instead (workspace
  decision DI2, `documents/pre-study/investigation/Virtual_Time_Study.md`).
- **Stack issues found while porting** were fixed in workspace step GS6:
  - shorter FHSS critical sections;
  - an endianness bug that broke unicast with a channel mask;
  - `rand_get_random_in_range()` with an empty range;
  - an option to register the port's TRNG as a strong TLS entropy source.

  See [PORTING.md](PORTING.md), "Pitfalls", and the plan's progress table.
- **Licence.** This is the Silicon Labs-derived Nanostack, with MSLA-covered
  changes (README, Licensing). It is an internal tool for the emulator and our
  tests.
