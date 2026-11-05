/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit test of the housekeeping timers with nothing to do (docs/PORTING.md,
 * "Housekeeping with nothing to do"). The stack's own code runs on stubs:
 *   - ipv6_routing_table.c: the neighbour cache's slow timer (entry
 *     lifetimes, the GC every 20 s) and the destination cache's timer (the
 *     GC every 20 s: entry and path-MTU ageing, removals);
 *   - mld.c: the query timer (125 s; a query only if we are the querier);
 *   - blacklist.c (through bl_access.c): entry expiry and the purge timer
 *     (30-90 s, a random draw at each expiry).
 * They are called as protocol_core.c calls them: every second (periodic,
 * the reference code), or, with skipping, at the second that the modules'
 * ..._next() functions give (at most 250 s ahead; the seconds before that
 * one in one call, then that one on its own) and, before each event, for
 * the seconds passed since the last call (the catch-up). Two interfaces
 * call MLD's timer, as protocol_core.c calls it once per interface.
 *
 * A schedule of events (its own random generator) adds, refreshes and
 * removes neighbour cache entries (garbage-collectible and registered),
 * destinations and path MTUs, blacklist entries, changes the caches'
 * thresholds and MLD's querier flag, and reads every module's state. Each
 * program prints, stamped with the second, every trace of the modules
 * (removals, path-MTU resets, purges, expiries), every MLD query and every
 * state read, and at the end the sequence of random draws. run.sh requires
 * the four outputs to be identical.
 *
 * Variants: periodic (no option), skip_prev (NS_PROTOCOL_TIMERS_SKIP_IDLE:
 * the wakes before this change), skip (with
 * NS_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING, the blacklist purge timer lazy, as
 * on an LFN) and skip_ffn (the purge timer still waking, as on an FFN). The
 * skip variant also checks that, once everything is idle, it wakes only at
 * the core timer's longest wait.
 *
 *   tests/housekeeping/run.sh [build dir]
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nsconfig.h"
#include "ipv6_stack/ipv6_routing_table.h"
#include "common_protocols/ipv6_constants.h"

#define NONE UINT32_MAX
#define MAX_WAIT 250        // CORE_TIMER_MAX_TICKS, in seconds
#define T_ACTIVE 60000u     // events until this second
#define T_IDLE 6000u        // then everything idle for this long

// The modules' timers (their headers need the whole stack's)
void mld_slow_timer(void *interface, uint_fast16_t seconds);
#ifdef NS_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING
void blacklist_ttl_update(uint16_t ticks, bool purge_lazy);
#define BLACKLIST_UPDATE(s) blacklist_ttl_update(s, PURGE_LAZY)
#else
void blacklist_ttl_update(uint16_t ticks);
#define BLACKLIST_UPDATE(s) blacklist_ttl_update(s)
#endif
uint8_t blacklist_init(void);
void blacklist_params_set(uint16_t entry_lifetime, uint16_t timer_max_timeout, uint16_t timer_timeout, uint16_t entry_max_nbr, uint16_t purge_nbr, uint16_t purge_timer_timeout);
bool blacklist_reject(const uint8_t *ll64_address);
void blacklist_update(const uint8_t *ll64_address, bool success);
void blacklist_clear(void);
void test_blacklist_dump(void (*print)(const char *fmt, ...));
extern bool mld_querier;
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
uint32_t mld_slow_timer_next(unsigned calls);
#ifdef NS_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING
uint32_t blacklist_ttl_update_next(bool purge_lazy);
#define BLACKLIST_NEXT() blacklist_ttl_update_next(PURGE_LAZY)
#else
uint32_t blacklist_ttl_update_next(void);
#define BLACKLIST_NEXT() blacklist_ttl_update_next()
#endif
#endif

uint32_t now;                   // the second the modules have been run up to
static ipv6_neighbour_cache_t ncache;
static int if1, if2;            // MLD's two interfaces (only their addresses)

/* ---- output ---- */

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ---- random numbers: the stack's (draws recorded) and the schedule's ---- */

static uint64_t stack_rng = 0x9e3779b97f4a7c15ull;
static char draws[1 << 20];
static size_t draws_len;

static uint32_t xorshift(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return (uint32_t)(*s >> 32);
}

uint32_t rand_get_32bit(void)
{
    uint32_t v = xorshift(&stack_rng);
    if (draws_len < sizeof(draws) - 16) {
        draws_len += sprintf(draws + draws_len, "%08" PRIx32 "\n", v);
    }
    return v;
}

uint16_t rand_get_random_in_range(uint16_t min, uint16_t max)
{
    return min + rand_get_32bit() % (max - min + 1u);
}

uint32_t rand_randomise_base(uint32_t base, uint16_t min_factor, uint16_t max_factor)
{
    return (uint64_t) base * rand_get_random_in_range(min_factor, max_factor) / 0x8000;
}

static uint64_t ev_rng = 0x2545f4914f6cdd1dull;

static uint32_t ev_rand(uint32_t n)
{
    return xorshift(&ev_rng) % n;
}

/* ---- traces ---- */

void mbed_vtracef(uint8_t dlevel, const char *grp, const char *fmt, va_list ap)
{
    printf("%7" PRIu32 " [%s] ", now, grp);
    vprintf(fmt, ap);
    putchar('\n');
}

void mbed_tracef(uint8_t dlevel, const char *grp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mbed_vtracef(dlevel, grp, fmt, ap);
    va_end(ap);
}

static char trace_bufs[8][48];
static unsigned trace_buf_i;

char *mbed_trace_ipv6(const void *addr_ptr)
{
    const uint8_t *a = addr_ptr;
    char *s = trace_bufs[trace_buf_i++ % 8];
    sprintf(s, "%02x%02x..%02x", a[0], a[1], a[15]);
    return s;
}

char *mbed_trace_array(const uint8_t *buf, uint16_t len)
{
    char *s = trace_bufs[trace_buf_i++ % 8];
    sprintf(s, "..%02x", len ? buf[len - 1] : 0);
    return s;
}

/* ---- stubs of the rest of the stack ---- */

const uint8_t ADDR_UNSPECIFIED[16];
const uint8_t ADDR_LINK_LOCAL_PREFIX[8] = { 0xfe, 0x80 };
const uint8_t ADDR_SHORT_ADR_SUFFIC[6] = { 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00 };
const uint8_t ADDR_LINK_LOCAL_ALL_NODES[16] = { 0xff, 0x02, [15] = 0x01 };
const uint8_t ADDR_LINK_LOCAL_ALL_ROUTERS[16] = { 0xff, 0x02, [15] = 0x02 };

bool addr_ipv6_equal(const uint8_t a[static 16], const uint8_t b[static 16])
{
    return !memcmp(a, b, 16);
}

bool addr_is_ipv6_link_local(const uint8_t addr[static 16])
{
    return addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80;
}

uint_fast8_t addr_ipv6_scope(const uint8_t addr[static 16], const struct protocol_interface_info_entry *interface)
{
    if (addr[0] == 0xff) {
        return addr[1] & 0x0f;
    }
    return addr_is_ipv6_link_local(addr) ? IPV6_SCOPE_LINK_LOCAL : IPV6_SCOPE_GLOBAL;
}

uint8_t addr_len_from_type(addrtype_t addr_type)
{
    return 8;
}

uint16_t etx_read(int8_t interface_id, addrtype_t addr_type, const uint8_t *addr_ptr)
{
    return 0x100;
}

uint_fast8_t ip6tos(const void *ip6addr, char *p)
{
    return sprintf(p, "%s", mbed_trace_ipv6(ip6addr));
}

void ipv6_interface_resolution_failed(ipv6_neighbour_cache_t *cache, ipv6_neighbour_t *entry) { }
void ipv6_interface_resolve_send_ns(ipv6_neighbour_cache_t *cache, ipv6_neighbour_t *entry, bool unicast, uint_fast8_t seq) { }
void ipv6_map_ip_to_ll_and_call_ll_addr_handler(void) { }
void ipv6_send_queued(ipv6_neighbour_t *entry) { }
bool protocol_core_buffers_in_event_queue(void)
{
    return false;
}

ipv6_neighbour_cache_t *ipv6_neighbour_cache_by_interface_id(int8_t interface_id)
{
    return interface_id == 1 ? &ncache : NULL;
}

/* ---- the modules' timers, as protocol_core.c calls them ---- */

static void run(uint32_t seconds)
{
    if (seconds > 255) {
        fail("a call for %" PRIu32 " seconds", seconds);
    }
    now += seconds;
    BLACKLIST_UPDATE(seconds);
    mld_slow_timer(&if1, seconds);
    mld_slow_timer(&if2, seconds);
    // The neighbour cache's removals have no trace: print them
    unsigned before = ns_list_count(&ncache.list);
    ipv6_neighbour_cache_slow_timer(&ncache, seconds);
    if (ns_list_count(&ncache.list) != before) {
        printf("%7" PRIu32 " neighbour cache %u -> %u entries\n", now, before, (unsigned) ns_list_count(&ncache.list));
    }
    ipv6_destination_cache_timer(seconds);
}

#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
static uint32_t min32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t next_all(void)
{
    uint32_t next = BLACKLIST_NEXT();
    next = min32(next, mld_slow_timer_next(2));
    next = min32(next, ipv6_neighbour_cache_slow_timer_next(&ncache));
    next = min32(next, ipv6_destination_cache_timer_next());
    return min32(next ? next : 1, MAX_WAIT);
}
#endif

/* ---- the schedule ---- */

static void address(uint8_t addr[16], bool link_local, uint8_t n)
{
    memset(addr, 0, 16);
    if (link_local) {
        addr[0] = 0xfe;
        addr[1] = 0x80;
        addr[8] = 0x02;     // an EUI-64 based IID, not a short address
    } else {
        addr[0] = 0x20;
        addr[1] = 0x01;
    }
    addr[15] = n;
}

static ipv6_neighbour_t *ncache_nth(unsigned n)
{
    unsigned i = 0;
    ns_list_foreach(ipv6_neighbour_t, e, &ncache.list) {
        if (i++ == n) {
            return e;
        }
    }
    return NULL;
}

static unsigned ncache_count(void)
{
    return ns_list_count(&ncache.list);
}

extern ipv6_destination_t *ipv6_destination_lookup_or_create(const uint8_t *address, int8_t interface_id);

static void observe(const char *what)
{
    out("%7" PRIu32 " %s", now, what);
    ns_list_foreach(ipv6_neighbour_t, e, &ncache.list) {
        out(" nc %02x%02x..%02x type %d state %d lifetime %" PRIu32, e->ip_address[0], e->ip_address[1],
            e->ip_address[15], e->type, e->state, e->lifetime);
    }
    ipv6_destination_cache_print(out);
    test_blacklist_dump(out);
}

static void event(void)
{
    uint8_t addr[16];
    ipv6_neighbour_t *e;
    uint32_t op = ev_rand(100);

    if (op < 15) {
        address(addr, true, 1 + ev_rand(24));
        e = ipv6_neighbour_lookup_or_create(&ncache, addr);
        if (e && e->type == IP_NEIGHBOUR_GARBAGE_COLLECTIBLE) {
            e->state = ev_rand(4) ? IP_NEIGHBOUR_STALE : IP_NEIGHBOUR_REACHABLE;
            e->lifetime = ev_rand(900);
        }
    } else if (op < 25) {
        address(addr, false, 1 + ev_rand(12));
        e = ipv6_neighbour_lookup_or_create(&ncache, addr);
        if (e) {
            e->type = ev_rand(4) ? IP_NEIGHBOUR_REGISTERED : IP_NEIGHBOUR_TENTATIVE;
            e->state = IP_NEIGHBOUR_STALE;
            e->lifetime = 1 + ev_rand(700);
        }
    } else if (op < 33) {
        if (ncache_count()) {
            ipv6_neighbour_entry_remove(&ncache, ncache_nth(ev_rand(ncache_count())));
        }
    } else if (op < 48) {
        address(addr, ev_rand(2), 1 + ev_rand(30));
        ipv6_destination_lookup_or_create(addr, 1);
    } else if (op < 56) {
        address(addr, ev_rand(2), 1 + ev_rand(30));
        ipv6_destination_t *d = ipv6_destination_lookup_or_create(addr, 1);
        if (d) {
            d->pmtu = 1280;
            d->pmtu_lifetime = 1 + ev_rand(900);
        }
    } else if (op < 58) {
        ipv6_destination_cache_clean(1);
    } else if (op < 76) {
        address(addr, true, 1 + ev_rand(16));
        blacklist_update(addr, false);
    } else if (op < 78) {
        address(addr, true, 1 + ev_rand(16));
        blacklist_update(addr, true);
    } else if (op < 81) {
        address(addr, true, 1 + ev_rand(16));
        out("%7" PRIu32 " reject ..%02x: %d", now, addr[15], blacklist_reject(addr));
    } else if (op < 84) {
        mld_querier = ev_rand(10) < 3;
    } else if (op < 88) {
        uint16_t nlong = 2 + ev_rand(7);
        uint16_t dlong = 3 + ev_rand(8);
        ipv6_neighbour_cache_configure(16, 10, nlong, 600);
        ipv6_destination_cache_configure(24, 12, dlong, 600);
        out("%7" PRIu32 " long-term %u %u", now, nlong, dlong);
    }
    observe("event");
}

static uint32_t next_gap(void)
{
    uint32_t r = ev_rand(100);
    if (r < 55) {
        return 1 + ev_rand(30);
    } else if (r < 90) {
        return 30 + ev_rand(270);
    }
    return 300 + ev_rand(1200);
}

static void idle_cleanup(void)
{
    ns_list_foreach_safe(ipv6_neighbour_t, e, &ncache.list) {
        ipv6_neighbour_entry_remove(&ncache, e);
    }
    ipv6_destination_cache_clean(1);
    blacklist_clear();
    mld_querier = false;
    observe("idle from here");
}

int main(void)
{
    uint32_t t_event = 0;
    uint32_t wakes = 0, idle_wakes = 0;
    bool idle = false;
    const uint32_t t_end = T_ACTIVE + T_IDLE;

    ipv6_neighbour_cache_init(&ncache, 1);
    blacklist_init();
    // Wi-SUN's blacklist settings, with a longer entry lifetime so that
    // the list fills up and the purge acts
    blacklist_params_set(6000, 400, 100, 10, 3, 60);
    ipv6_neighbour_cache_configure(16, 10, 4, 600);
    ipv6_destination_cache_configure(24, 12, 6, 600);

    t_event = next_gap();
    for (;;) {
        // The next instant: an event (after that second's timers) or the end
        uint32_t t_next;
        if (!idle && t_event > T_ACTIVE) {
            t_next = T_ACTIVE;
        } else {
            t_next = idle ? t_end : t_event;
        }
#ifdef NS_PROTOCOL_TIMERS_SKIP_IDLE
        uint32_t due = now + next_all();
        if (due <= t_next) {
            // A wake of the core timer
            if (due - now > 1) {
                run(due - now - 1);
            }
            run(1);
            wakes++;
            if (idle) {
                idle_wakes++;
            }
            continue;
        }
        // The catch-up before an event
        if (t_next > now) {
            run(t_next - now);
        }
#else
        while (now < t_next) {
            run(1);
            wakes++;
            if (idle) {
                idle_wakes++;
            }
        }
#endif
        if (idle) {
            observe("end");
            break;
        }
        if (t_next == T_ACTIVE && t_event > T_ACTIVE) {
            idle_cleanup();
            idle = true;
            continue;
        }
        event();
        t_event += next_gap();
    }
    fputs(draws, stdout);

#if defined NS_PROTOCOL_TIMERS_SKIP_HOUSEKEEPING && PURGE_LAZY
    if (idle_wakes > T_IDLE / MAX_WAIT + 1) {
        fail("%" PRIu32 " wakes in %u idle seconds (at most %u expected)", idle_wakes, T_IDLE, T_IDLE / MAX_WAIT + 1);
    }
#endif
    fprintf(stderr, "housekeeping: %" PRIu32 " wakes in %u s, %" PRIu32 " of them in the last %u idle seconds\n",
            wakes, t_end, idle_wakes, T_IDLE);
    return 0;
}
