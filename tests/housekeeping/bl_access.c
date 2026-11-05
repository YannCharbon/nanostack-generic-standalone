/* SPDX-License-Identifier: Apache-2.0
 *
 * The stack's blacklist.c, compiled here with an accessor to its private
 * state, for test_housekeeping.c. */
#include "service_libs/blacklist/blacklist.c"

void test_blacklist_dump(void (*print)(const char *fmt, ...))
{
    if (!blacklist_data) {
        return;
    }
    ns_list_foreach(blacklist_entry_t, e, &blacklist_data->blacklist) {
        print(" bl %02x ttl %u int %u", e->eui64[7], e->ttl, e->interval);
    }
}

unsigned test_blacklist_count(void)
{
    return blacklist_data ? ns_list_count(&blacklist_data->blacklist) : 0;
}
