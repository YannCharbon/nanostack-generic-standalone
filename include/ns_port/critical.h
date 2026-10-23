/*
 * Porting API: critical sections (docs/GENERICITY_PLAN.md §3.1.1).
 *
 * A short mutual exclusion against every other context that may touch stack
 * data (ISRs, or a MAC context on ports that have one). Nestable, at least 8
 * levels. Never block or sleep inside. ISR- and task-safe.
 * Names kept from Arm Nanostack (arm_hal_interrupt.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_CRITICAL_H
#define NS_PORT_CRITICAL_H

/* Once, before anything else uses the stack. */
void platform_critical_init(void);
void platform_enter_critical(void);
void platform_exit_critical(void);

#endif
