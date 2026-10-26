/* SPDX-License-Identifier: Apache-2.0
 * POSIX port: trace output (ns_port/trace.h), one line on stdout. The
 * emulator programs install their own print function instead, which also
 * writes a log file.
 */
#include <stdio.h>
#include "ns_port/trace.h"

void ns_port_trace_write(const char *line)
{
    puts(line);
}
