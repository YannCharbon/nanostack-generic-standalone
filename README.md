# nanostack-generic-standalone

The Nanostack network stack used by the Wi-SUN mesh emulator, extracted from
[`wisun-mbed-simulator`](../wisun-mbed-simulator) (commit `d86eb34`) into a
repository of its own. The emulator that consumes it lives in
[`mesh-emulator-orchestrator`](../mesh-emulator-orchestrator).

The extraction was a pure move: the code was the same as in
`wisun-mbed-simulator`, file for file, and built into the same machine code
(tag `baseline-2026-10-23`). Since then the stack was made portable, step by
step, without changing the stack code itself (see [Verification](#verification)):

- a porting API (`include/ns_port/`) that every port implements;
- the Linux layer became the POSIX port (`port/posix/`), with the run loop
  that the emulator programs used to copy;
- the port is chosen at build time, so a firmware builds the stack with
  `add_subdirectory()` and its own port;
- the emulator can run nodes with the MAC in the same process.

Documentation:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): how the stack was made
  generic, before and after;
- [`docs/PORTING.md`](docs/PORTING.md): how to port it to a new platform;
- [`docs/GENERICITY_PLAN.md`](docs/GENERICITY_PLAN.md): the plan, its
  specification and the progress table. The stack runs on FreeRTOS
on the EFR32FG25, see
[`nanostack-efr32fg25-freertos`](https://github.com/YannCharbon/nanostack-efr32fg25-freertos).

This is the Silicon Labs-derived Wi-SUN stack (see [Licensing](#licensing)). It
is an internal tool for the emulator and our tests. It is not the code base of
the fire-alarm protocol stack, which takes no code from it.

## Content

| Path | Was in `wisun-mbed-simulator` | Content |
| --- | --- | --- |
| `nanostack/` | `nanostack/` | 6LoWPAN, IPv6, RPL, MPL, Wi-SUN, security (host side) |
| `nanostack-event-loop/` | `nanostack-event-loop/` | Event loop and callback timers |
| `mbed-client-libservice/` | `mbed-client-libservice/` | `ns_list`, `ns_trace`, IPv6 string helpers |
| `rcp/mac/`, `rcp/fhss/`, `rcp/ccm/` | `app_wshwsim/mac/`, `fhss/`, `ccm/` | IEEE 802.15.4 MAC, FHSS and CCM, which run on the RCP side |
| `port/posix/common/` | `common/` (OS parts) | POSIX port: timers (timerfd), FHSS timer, scheduler signal and run loop, critical sections, random, logging, lists |
| `port/sim/` | — | Simulation port: virtual time driven by the emulator's conductor (see [Simulation port](#simulation-port)) |
| `include/ns_port/` | — | The porting API: what a port implements |
| `include/ns_rand.h`, `source/ns_rand.c` | `common/rand.*` | The stack's `rand_*` helpers, built on the port's random source |
| `tests/cross/` | — | Cross-build check: Cortex-M33, stub port |
| `tests/port_sim/` | — | Unit tests of the simulation port |

`rcp/` keeps its own `mac/rf_driver_storage.h`, which differs slightly from
`nanostack/source/mac/rf_driver_storage.h`, exactly as before.

`port/posix/common/` keeps the `common/` directory name, so applications keep
`#include "common/log.h"` style includes. The stack itself includes only
`ns_port/*.h` and `ns_rand.h`. The Spinel/UART transport (`bus_uart`,
`spinel_buffer`) is emulator plumbing and stayed in the orchestrator.

## Porting API

`include/ns_port/ns_port.h` includes everything a port provides
(`docs/GENERICITY_PLAN.md` §3.1 has the full contract):

| Header | Functions |
| --- | --- |
| `critical.h` | `platform_critical_init()`, `platform_enter_critical()`, `platform_exit_critical()` |
| `timer.h` | `eventOS_callback_timer_register/unregister/start/stop()`: 50 µs slots, IDs from 0 |
| `sched.h` | `ns_port_init()`, `eventOS_scheduler_signal()`, `ns_port_run()`, `ns_port_add_poll_hook()` |
| `random.h` | `ns_port_random_bytes()`: must be a real TRNG on hardware, it seeds TLS |
| `time.h` | `ns_port_time_us()`, the timebase of the radio timestamps; `ns_port_time_wall_s()` (optional) |
| `fhss_timer.h` | `ns_port_fhss_timer`, the `fhss_timer_t` passed to `ws_management_node_init()` |
| `trace.h` | `ns_port_trace_write()`, for `mbed_trace_print_function_set()` |

The stack calls `malloc()`/`free()` directly, stores files through
`ns_file_system_callbacks_set()` and does AES through `arm_hal_aes.h`.

The POSIX port adds calls for programs with their own file descriptors
(`port/posix/common/os_scheduler.h`): `ns_port_posix_add_fd()`,
`ns_port_posix_add_scheduler()` and `ns_port_posix_set_busy()`. The three
emulator programs use them instead of their own `pselect()` loops.

The git history of these files was carried over with `git filter-repo`
(paths renamed as in the table).

## Using it from CMake

The repository declares libraries only. Executables come from the consumer:

```cmake
add_subdirectory(path/to/nanostack-generic-standalone nanostack-build)

# Upper layers. CONFIG selects nanostack/source/configs/cfg_<config>.h.
nanostack_add_library(nanostack_ws_border_router CONFIG ws_border_router BORDER_ROUTER
                      DEFINITIONS EXTRA_DEBUG_INFO)
nanostack_add_library(nanostack_ws_router CONFIG ws_router)
# 802.15.4 MAC + FHSS + CCM, e.g. DEFINITIONS FHSS_CHANNEL_DEBUG
nanostack_add_rcp_library(nanostack_rcp)

target_link_libraries(my_app PRIVATE nanostack_ws_router)
```

Fixed targets:

- `nanostack_port_api`: the porting API headers (interface target)
- `nanostack_libservice`
- `nanostack_eventloop`
- `nanostack_port_posix` or `nanostack_port_sim`, only when it is the
  selected port

Options:

- `NANOSTACK_PORT` names the target that implements the porting API. The
  default is `nanostack_port_posix`. A firmware sets it to its own target,
  which links `nanostack_port_api`; then Threads, libbacktrace and libdl are
  not used.
- `NANOSTACK_FILE_NO_STDIO=ON` compiles out the stdio fallback of
  `ns_file_system.c`, for targets without files.
- `nanostack_add_rcp_library(<name> INPROCESS)` builds the MAC for an image
  that also holds the upper layers: it leaves out `rcp/`'s copy of
  `rf_driver_storage.c`.
- mbedTLS: if `MbedTLS::mbedtls`, `MbedTLS::mbedcrypto` and `MbedTLS::mbedx509`
  already exist (for example as aliases of the consumer's own build), they are
  used; otherwise `find_package(MbedTLS)` looks for v3.0.0.

The upper layers are built through functions because their compile-time
configuration (`NSCONFIG`, debug defines, border-router-only files) differs
per application. `BORDER_ROUTER` adds the files only the border router used:

- `ws_bbr_api.c`
- `ws_bootstrap_6lbr.c`
- `dhcpv6_server_service.c`
- `libdhcpv6_server.c`

Compile options (`-Wall`, `-funwind-tables`, build type) come from the
consumer's directory, as they did when the stack was part of the emulator's
CMake project.

## Building on its own

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<mbedtls v3.0.0 build dir> \
      -DCMAKE_C_FLAGS="-std=gnu17 -fpermissive"
ninja -C build
```

This builds the default variants listed above. It needs Threads and
MbedTLS v3.0.0, plus libbacktrace if it is installed.

The cross-build check builds the stack for a Cortex-M33 with a stub port, as a
firmware would, and links a border router and a router image:

```bash
tests/cross/run.sh <mbedtls v3.0.0 source tree>
```

It needs `arm-none-eabi-gcc`. The GitHub workflow runs it, the default
build, the build on the simulation port and its unit tests on every push.

`-std=gnu17 -fpermissive` is needed with GCC ≥ 14. GCC 15 defaults to C23 and
turns several legacy warnings into errors, so the inherited code doesn't build
without those flags (the same holds for `wisun-mbed-simulator`).

## Simulation port

`-DNANOSTACK_PORT=nanostack_port_sim` builds the stack on virtual time, for
the emulator's batch mode (`mesh-emulator-orchestrator/docs/VIRTUAL_TIME.md`).
The port (`port/sim/`) replaces the POSIX port's clock, callback timers, FHSS
timer, run loop and random source, and shares its other files and headers:
programs keep including `common/os_scheduler.h` and the other `common/`
headers, and test `NS_PORT_SIM` (a public definition of the target) for the
few things they do differently.

The contract, in `port/sim/ns_port_sim.h`:

- A conductor (`wssimserver --vtime`) runs one node at a time over one
  `SOCK_SEQPACKET` socket. The program connects, sends its hello and calls
  `ns_port_sim_attach(fd, node_id)`, which waits for the welcome and its seed.
  It registers the same fd with `ns_port_posix_add_fd()`: the callback is
  called once per frame the conductor delivers.
- `ns_port_run()` runs the node until idle (no event signalled, no
  `pending()` source, no timer due), sends `"xi"` with its earliest deadline
  and blocks until `"xa"` moves time forward. Then the frames, then the
  timers due. Code runs in zero virtual time. `"xq"` exits with status 0.
- Timers are absolute 64-bit deadlines; a zero-length start disarms, as the
  POSIX port's `timerfd` does, and is counted (logged at exit).
  `ns_port_time_us()` is the low 32 bits of the virtual time plus `NSIM_CLOCK32_OFFSET_US` (it wraps 120 s into every run).
- The random source is splitmix64 seeded from the run's seed and the node
  ID: two runs with the same seed draw the same numbers. Drawing before
  attach is fatal. It is not a TRNG: this port is for the emulator only.
- `stdout` and `stderr` write whole lines, each prefixed with the virtual
  time (`<seconds>.<µs> `, from `NSIM_EPOCH_S`), and `TRACE()` stamps virtual
  time too. File descriptors other than the conductor's are never polled, and
  `ns_port_posix_set_busy()` (split mode) is fatal.

Unit tests (no stack and no mbedTLS needed; a fake conductor over
`socketpair()`):

```bash
tests/port_sim/run.sh [build dir]
```

## Verification

The extraction was checked against `wisun-mbed-simulator` `d86eb34`, with the
same compiler and flags:

- **Object level:** the old build compiled stack sources into 317 objects,
  counted per executable. Every one of them is identical to its counterpart in
  the new build. That covers code with symbolic relocations and every
  non-debug data section. Of the 57 application objects in the orchestrator,
  50 are identical. The other 7 differ only through the absolute source path
  that `__FILE__` embeds.
- **Link level:** each new executable defines a subset of the old one's
  symbols. The static archives leave out objects nothing references, such as
  the authenticator security protocols in the router. Nothing else is missing
  or added.
- **Runtime:** `dag_simple` (15 nodes) runs were compared against the
  reference. See `mesh-emulator-orchestrator/MIGRATION.md`.

The genericity steps S1–S5 kept those gates (`docs/GENERICITY_PLAN.md`,
Progress): every Nanostack, event loop, libservice and MAC/FHSS/CCM object is
still identical to the reference, and the port and emulator changes were
checked with 5 `dag_simple` runs per step, including runs with the MAC
in-process. The cross-build check covers the non-POSIX build.

## Licensing

This code comes from Silicon Labs' `wisun-br-linux`. The top-level `LICENSE`
applies:

- Silicon Labs' modifications are under the Silicon Labs MSLA.
- The original Arm code is under Apache-2.0.

So this repository is **not yet free of MSLA code**. Making it so means
rebasing the stack on Arm's Apache-2.0 Nanostack. See
`documents/pre-study/legal/MSLA_License_Review.md` in `mesh-emulator-rework`.
