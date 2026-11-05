/* SPDX-License-Identifier: Apache-2.0
 *
 * Stubs for mld.c in test_housekeeping.c, apart from the stack's headers
 * (their types are not needed). */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

extern uint32_t now;

// mld.c: queries are built into no buffer; protocol_push() is where they go
void *buffer_get(uint16_t size)
{
    return NULL;
}
void buffer_free(void *buf) { }
void *buffer_corrupt_check(void *buf)
{
    return buf;
}
void *ipv6_buffer_route_to(void *buf, const uint8_t *next_hop, void *cur)
{
    return NULL;
}
void *addr_get_group_entry(void *interface, const uint8_t group[16])
{
    return NULL;
}
int8_t addr_interface_get_ll_address(void *cur, uint8_t *address_ptr, uint8_t type)
{
    return -1;
}
bool addr_is_assigned_to_interface(void *interface, const uint8_t addr[16])
{
    return false;
}
void ws_bootstrap_lfn_group_change(void *cur, const uint8_t group[16], bool add) { }

void protocol_push(void *buf)
{
    printf("%7" PRIu32 " MLD query\n", now);
}

