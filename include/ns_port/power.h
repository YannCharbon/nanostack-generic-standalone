/* SPDX-License-Identifier: Apache-2.0
 *
 * Sleep depth: the rule a port applies when the run loop is idle, so that the
 * emulator and a board sleep in the same places (mesh-emulator-orchestrator
 * docs/POWER_MODEL.md §4). Header only: no port has to implement anything.
 *
 * - Light sleep (EM1-like): clocks kept, wakes in a few cycles.
 * - Deep sleep (EM2-like): only the low-frequency clock runs; waking costs
 *   time and charge (crystal start, PLL lock).
 *
 * Deep sleep is chosen only when nothing holds the fast clock (the radio is
 * not off, a UART is sending: the firmware's EM1 requirements) and the idle
 * time until the next deadline is at least deep_min_us, which covers the
 * transitions and the break-even point (ns_port_deep_min_us()).
 */
#ifndef NS_PORT_POWER_H
#define NS_PORT_POWER_H

#include <stdint.h>

enum ns_port_sleep {
    NS_PORT_SLEEP_LIGHT = 1,
    NS_PORT_SLEEP_DEEP  = 2,
};

// Reasons to stay in light sleep, as a bit set (the firmware's EM1
// requirements).
#define NS_PORT_HOLD_RADIO   0x0001 // the radio is not off (RX, TX, or an idle that needs the fast clock)
#define NS_PORT_HOLD_PERIPH  0x0002 // a peripheral needs the fast clock (UART, SPI to a transceiver)

struct ns_port_sleep_rule {
    uint64_t deep_min_us;           // shortest idle worth a deep sleep
};

// idle_us: from the end of the current work to the next deadline
// (UINT64_MAX when no timer is armed).
static inline enum ns_port_sleep ns_port_sleep_depth(unsigned int holds, uint64_t idle_us,
                                                     const struct ns_port_sleep_rule *rule)
{
    if (holds || idle_us < rule->deep_min_us)
        return NS_PORT_SLEEP_LIGHT;
    return NS_PORT_SLEEP_DEEP;
}

// The shortest idle for which a deep sleep costs less charge than a light
// one, and that leaves time for its transitions. Currents in mA, times in µs:
//   light: i_light × T
//   deep:  i_enter × t_enter + i_wake × t_wake + i_deep × (T − t_enter − t_wake)
static inline uint64_t ns_port_deep_min_us(double i_light, double i_deep,
                                           double t_enter, double i_enter,
                                           double t_wake, double i_wake)
{
    double t_trans = t_enter + t_wake;
    double extra = i_enter * t_enter + i_wake * t_wake - i_deep * t_trans;
    double t_even;

    if (i_light <= i_deep)
        return UINT64_MAX;          // deep sleep never pays
    t_even = extra / (i_light - i_deep);
    if (t_even < t_trans)
        t_even = t_trans;
    return (uint64_t)(t_even + 0.999999);
}

#endif
