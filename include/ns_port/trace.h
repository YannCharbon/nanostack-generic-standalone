/*
 * Porting API: trace output (docs/GENERICITY_PLAN.md §3.1.8).
 *
 * The stack traces through mbed-client-libservice ns_trace. The application
 * passes this function to mbed_trace_print_function_set().
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NS_PORT_TRACE_H
#define NS_PORT_TRACE_H

/* Write one complete trace line, without newline. */
void ns_port_trace_write(const char *line);

#endif
