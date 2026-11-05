/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests of LFN code outside the FHSS (docs/LFN_STACK.md §11), on stubs:
 *
 *   1. MAC (mac_lfn_async.c): an asynchronous sweep waits while a frame
 *      queued for an LFN (its window, an LFN broadcast slot, an LPA at its
 *      time) would be due before the sweep ends; not for FFN frames, not
 *      beyond 10 s of waiting.
 *   2. Duplicate detection (ws_neighbor_class.c): same sequence number, same
 *      payload, within the window (5 s; 5 s + 2 listen intervals on an LFN),
 *      timed from the last frame accepted. A new frame whose 8-bit sequence
 *      number matches the previous one, a listen interval later, is
 *      accepted; a retry in the next window is dropped.
 *   3. Adaptation layer (adaptation_interface.c): a frame to an LFN child
 *      whose MAC TTL expired is not queued again; to an FFN it is (unchanged).
 *   4. LPAS jitter (ws_lfn_jitter.h, GS11): the range, step and mean of each
 *      draw, with the default lpas_interval_s and others; two LFNs that start
 *      their discovery in the same tick, or a search in the same second, do
 *      not sweep together again (the former fixed timing did, every time).
 *   5. LFN restarts (ws_lfn_restart.h): the MAC's restart span, and the
 *      restart values scaled to outlast a parent's two sweeps on large plans.
 *
 * OLD_STACK: built against the stack before these fixes (no parts 1 and 4;
 * the duplicate check's former signature), to show which checks fail there.
 *
 *   tests/lfn_units/run.sh [build dir]
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nsconfig.h"
#include "mbed-client-libservice/ns_list.h"
#include "nanostack/mac/mac_api.h"
#include "nanostack/mac/mac_mcps.h"
#include "nanostack/mac/fhss_api.h"
#include "nanostack/mac/fhss_ws_extension.h"
#include "core/ns_buffer.h"
#include "core/ns_socket.h"
#include "nwk_interface/protocol.h"
#include "nwk_interface/protocol_stats.h"
#include "6lowpan/mac/mac_helper.h"
#include "6lowpan/lowpan_adaptation_interface.h"
#include "6lowpan/ws/ws_common.h"
#include "6lowpan/ws/ws_neighbor_class.h"
#ifndef OLD_STACK
#include "nanostack/mac/mlme.h"
#include "mac_data_buffer.h"
#include "mac_lfn_async.h"
#include "nanostack/ws_management_api.h"
#include "6lowpan/ws/ws_lfn_jitter.h"
#include "6lowpan/ws/ws_lfn_restart.h"
#endif

static unsigned checks, failures;
#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* ------------------------------------------------------------------ stubs */

void mbed_tracef(uint8_t dlevel, const char *grp, const char *fmt, ...) { (void)dlevel; (void)grp; (void)fmt; }
char *mbed_trace_array(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; return ""; }
uint8_t DEVICE_MIN_SENS = 174 - 93;
void ns_fhss_ws_update_neighbor(const uint8_t eui64[8], fhss_ws_neighbor_timing_info_t *fhss_data) { (void)eui64; (void)fhss_data; }
int8_t ws_common_generate_channel_list(const struct protocol_interface_info_entry *cur, uint32_t *channel_mask, uint16_t number_of_channels, uint8_t regulatory_domain, uint8_t operating_class, uint8_t channel_plan_id)
{ (void)cur; (void)channel_mask; (void)number_of_channels; (void)regulatory_domain; (void)operating_class; (void)channel_plan_id; return 0; }
uint16_t ws_common_active_channel_count(uint32_t *channel_mask, uint16_t number_of_channels) { (void)channel_mask; return number_of_channels; }
uint16_t ws_common_channel_number_calc(uint8_t regulatory_domain, uint8_t operating_class, uint8_t channel_plan_id) { (void)regulatory_domain; (void)operating_class; (void)channel_plan_id; return 35; }
uint16_t ws_phy_get_number_of_channels_using_channel_plan_id(uint8_t channel_plan_id) { (void)channel_plan_id; return 35; }
bool ws_wp_nested_pcap_read(uint8_t *data, uint16_t length, struct ws_phy_cap_info *ws_pcap_list) { (void)data; (void)length; (void)ws_pcap_list; return false; }

// Adaptation layer's surroundings
uint32_t protocol_core_monotonic_time = 1000;
static bool dst_is_lfn;
bool ws_lfn_parent_dst_is_lfn(struct protocol_interface_info_entry *cur, const uint8_t *dst64) { (void)cur; (void)dst64; return dst_is_lfn; }
static protocol_interface_info_entry_t iface;
protocol_interface_info_entry_t *protocol_stack_interface_info_get_by_id(int8_t nwk_id) { return nwk_id == iface.id ? &iface : NULL; }
void protocol_stats_update(nwk_stats_type_t type, uint16_t update_val) { (void)type; (void)update_val; }
void protocol_push(buffer_t *b) { (void)b; }
uint16_t mac_helper_panid_get(const struct protocol_interface_info_entry *interface) { (void)interface; return 0x1234; }
uint8_t mac_helper_default_security_level_get(struct protocol_interface_info_entry *interface) { (void)interface; return 6; }
uint8_t mac_helper_default_security_key_id_mode_get(struct protocol_interface_info_entry *interface) { (void)interface; return 1; }
uint8_t mac_helper_default_key_index_get(struct protocol_interface_info_entry *interface) { (void)interface; return 1; }
int8_t mac_helper_mac_channel_set(struct protocol_interface_info_entry *interface, uint8_t new_channel) { (void)interface; (void)new_channel; return 0; }
uint_fast16_t mac_helper_max_payload_size(struct protocol_interface_info_entry *cur, uint_fast16_t frame_overhead) { (void)cur; return 1500 - frame_overhead; }
uint_fast8_t mac_helper_frame_overhead(struct protocol_interface_info_entry *cur, const struct buffer *buf) { (void)cur; (void)buf; return 30; }
mac_neighbor_table_entry_t *mac_neighbor_table_address_discover(mac_neighbor_table_t *table_class, const uint8_t *address, uint8_t address_type)
{ (void)table_class; (void)address; (void)address_type; return NULL; }
uint8_t rand_get_8bit(void) { return 7; }
void mac_mlme_mac_radio_disabled(struct protocol_interface_rf_mac_setup *rf_mac_setup) { (void)rf_mac_setup; }
uint16_t rand_get_16bit(void) { return 77; }
/* xorshift32; one state per simulated LFN (test_lpas_jitter) */
static uint32_t rand_state = 1;
uint32_t rand_get_32bit(void)
{
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return rand_state;
}
uint16_t random_early_detection_aq_calc(struct red_info_s *red_info, uint16_t sampleLen) { (void)red_info; return sampleLen; }
bool random_early_detection_congestion_check(struct red_info_s *red_info) { (void)red_info; return false; }
bool rpl_data_is_rpl_parent_route(ipv6_route_src_t source) { (void)source; return false; }
uint16_t iphc_header_scan(buffer_t *buf, uint16_t *uncompressed_size) { (void)buf; *uncompressed_size = 0; return 0; }
uint8_t addr_check_broadcast(const address_t addr, addrtype_t addr_type) { (void)addr; (void)addr_type; return 1; }
bool addr_ipv6_equal(const uint8_t a[16], const uint8_t b[16]) { return !memcmp(a, b, 16); }
void buffer_data_add(buffer_t *buf, const uint8_t *data_ptr, uint16_t data_len) { (void)buf; (void)data_ptr; (void)data_len; }
uint8_t *buffer_corrupt_check(buffer_t *buf) { return buffer_data_pointer(buf); }
buffer_t *buffer_get(uint16_t size) { buffer_t *b = calloc(1, sizeof(buffer_t) + size); b->size = size; return b; }
static unsigned buffers_freed;
buffer_t *buffer_free(buffer_t *buf) { free(buf); buffers_freed++; return NULL; }
void buffer_free_list(buffer_list_t *list) { (void)list; }
static unsigned tx_events;
static uint8_t tx_event_status;
void socket_tx_buffer_event_and_free(buffer_t *buf, uint8_t status) { tx_events++; tx_event_status = status; buffer_free(buf); }

// The MAC under the adaptation layer
static unsigned mac_requests;
static uint8_t last_handle;
static void fake_mcps_data_req_ext(const mac_api_t *api, const mcps_data_req_t *data, const mcps_data_req_ie_list_t *ie_ext, const channel_list_s *asynch_channel_list, mac_data_priority_t priority, uint8_t phy_mode_id)
{
    (void)api; (void)ie_ext; (void)asynch_channel_list; (void)priority; (void)phy_mode_id;
    mac_requests++;
    last_handle = data->msduHandle;
}

static void fake_mcps_data_req(const mac_api_t *api, const mcps_data_req_t *data)
{
    fake_mcps_data_req_ext(api, data, NULL, NULL, MAC_DATA_NORMAL_PRIORITY, 0);
}

/* ------------------------------------------------- 1. asynchronous sweeps */

#ifndef OLD_STACK
static uint32_t lfn_delay_us, lfn_bc_delay_us;
static bool lfn_dst_known, lfn_bc_known;
static const uint8_t lfn_eui[8] = { 1, 2, 3, 4, 5, 6, 0, 2 };
static bool fake_tx_delay(const fhss_api_t *api, const uint8_t *dst, uint32_t *delay_us)
{
    (void)api;
    if (!dst) {
        *delay_us = lfn_bc_delay_us;
        return lfn_bc_known;
    }
    if (lfn_dst_known && !memcmp(dst, lfn_eui, 8)) {
        *delay_us = lfn_delay_us;
        return true;
    }
    return false;
}

static void test_async_hold(void)
{
    fhss_api_t fhss;
    mac_pre_build_frame_t async, lfn, ffn, bc, lpa;
    const uint32_t now = 0xfffff000u;           // across the 32-bit wrap

    memset(&fhss, 0, sizeof(fhss));
    fhss.lfn_tx_delay = fake_tx_delay;
    memset(&async, 0, sizeof(async));
    async.asynch_request = true;
    async.request_start_time_us = now - 1000;
    for (int i = 0; i < 35; i++) {
        async.asynch_channel_list.channel_mask[i / 32] |= 1u << (i % 32);
    }
    async.mac_payload_length = 0;
    async.headerIeLength = 17;                  // 57-byte PAN configuration with the 40 bytes added
    // 35 channels × (57 × 8 / 150 kbit/s + 3 ms) + 20 ms
    uint32_t sweep = mac_lfn_async_sweep_us(&async, 150000);
    CHECK(sweep == 35 * (57 * 8 * 1000000 / 150000 + 3000) + MAC_LFN_ASYNC_MARGIN_US);

    memset(&lfn, 0, sizeof(lfn));
    lfn.fcf_dsn.ackRequested = true;
    lfn.fcf_dsn.DstAddrMode = MAC_ADDR_MODE_64_BIT;
    memcpy(lfn.DstAddr, lfn_eui, 8);
    ffn = lfn;
    ffn.DstAddr[7] = 9;
    lfn_dst_known = true;

    // Nothing queued, or only an FFN frame: the sweep goes
    CHECK(!mac_lfn_async_hold(&fhss, &async, NULL, NULL, now, 150000));
    CHECK(!mac_lfn_async_hold(&fhss, &async, &ffn, NULL, now, 150000));
    // An LFN frame due before the sweep could end: it waits; due after: not
    lfn_delay_us = sweep - 1;
    CHECK(mac_lfn_async_hold(&fhss, &async, &lfn, NULL, now, 150000));
    ffn.next = &lfn;
    CHECK(mac_lfn_async_hold(&fhss, &async, &ffn, NULL, now, 150000));     // found behind an FFN frame
    ffn.next = NULL;
    lfn_delay_us = sweep;
    CHECK(!mac_lfn_async_hold(&fhss, &async, &lfn, NULL, now, 150000));
    // Not an acknowledged 64-bit frame: not an LFN frame
    lfn_delay_us = 0;
    lfn.fcf_dsn.ackRequested = false;
    CHECK(!mac_lfn_async_hold(&fhss, &async, &lfn, NULL, now, 150000));
    lfn.fcf_dsn.ackRequested = true;
    // A frame for the LFN broadcast slots, in the broadcast queue
    memset(&bc, 0, sizeof(bc));
    bc.lfn_bc = true;
    lfn_bc_known = true;
    lfn_bc_delay_us = 100000;
    CHECK(mac_lfn_async_hold(&fhss, &async, NULL, &bc, now, 150000));
    lfn_bc_known = false;
    CHECK(!mac_lfn_async_hold(&fhss, &async, NULL, &bc, now, 150000));
    // An LPA at its requested time (an LND discovery slot), even past
    memset(&lpa, 0, sizeof(lpa));
    lpa.lfn_req_set = true;
    lpa.lfn_req_time = now + sweep / 2;
    CHECK(mac_lfn_async_hold(&fhss, &async, &lpa, NULL, now, 150000));
    lpa.lfn_req_time = now - 5000;
    CHECK(mac_lfn_async_hold(&fhss, &async, &lpa, NULL, now, 150000));
    lpa.lfn_req_time = now + sweep + 1000;
    CHECK(!mac_lfn_async_hold(&fhss, &async, &lpa, NULL, now, 150000));
    // Another asynchronous request in the queue is not an LFN frame
    mac_pre_build_frame_t other = async;
    CHECK(!mac_lfn_async_hold(&fhss, &async, &other, NULL, now, 150000));
    // Waited more than 10 s: it goes anyway
    lfn_delay_us = 0;
    async.request_start_time_us = now - MAC_LFN_ASYNC_DEFER_MAX_US - 1;
    CHECK(!mac_lfn_async_hold(&fhss, &async, &lfn, NULL, now, 150000));
    async.request_start_time_us = now - MAC_LFN_ASYNC_DEFER_MAX_US;
    CHECK(mac_lfn_async_hold(&fhss, &async, &lfn, NULL, now, 150000));
    // Not an asynchronous frame, or no LFN parenting in the FHSS: never held
    async.asynch_request = false;
    CHECK(!mac_lfn_async_hold(&fhss, &async, &lfn, NULL, now, 150000));
    async.asynch_request = true;
    fhss.lfn_tx_delay = NULL;
    CHECK(!mac_lfn_async_hold(&fhss, &async, &lfn, NULL, now, 150000));
    CHECK(!mac_lfn_async_hold(NULL, &async, &lfn, NULL, now, 150000));
}
#endif

/* ------------------------------------------------ 2. duplicate detection */

// The LLC's call: frame (dsn, payload IEs) received at rx_us, our uptime
// now_s; window 5 s, or 5 s + 2 listen intervals on an LFN. The LLC then
// updates the UTT reference with the frame (an FFN's frames carry a UTT-IE).
static bool rx_frame(ws_neighbor_class_entry_t *n, uint8_t dsn, const char *payload, uint32_t rx_us, uint32_t now_s, uint32_t window_s)
{
#ifdef OLD_STACK
    (void)payload; (void)now_s; (void)window_s;
    bool ok = ws_neighbor_class_neighbor_duplicate_packet_check(n, dsn, rx_us);
#else
    bool ok = ws_neighbor_class_neighbor_duplicate_packet_check(n, dsn, rx_us, now_s, (const uint8_t *)payload, strlen(payload), window_s);
#endif
    if (ok) {
        n->fhss_data.uc_timing_info.utt_rx_timestamp = rx_us;
    }
    return ok;
}

static void test_duplicates(void)
{
    ws_neighbor_class_entry_t n;

    // FFN: a retry within 5 s is dropped; after 5 s, or another sequence
    // number, accepted
    memset(&n, 0, sizeof(n));
    n.unicast_data_rx = true;
    uint32_t t = 0xfff00000u;
    CHECK(rx_frame(&n, 10, "NS 1", t, 100, 5));
    CHECK(!rx_frame(&n, 10, "NS 1", t + 2000000, 102, 5));
    CHECK(rx_frame(&n, 11, "NS 2", t + 2500000, 102, 5));
    CHECK(rx_frame(&n, 11, "NS 2", t + 8500000, 108, 5));

    // LFN, Test profile (5 s interval): the parent's retry in the next
    // window, 5.0 s later, is a duplicate (window 5 + 2 × 5 s). Before: it
    // was delivered twice (better_move seed 3)
    memset(&n, 0, sizeof(n));
    n.unicast_data_rx = true;
    t = 1000000000u;
    CHECK(rx_frame(&n, 111, "UDP Q 111", t, 1500, 15));
    CHECK(!rx_frame(&n, 111, "UDP Q 111", t + 5000000, 1505, 15));
    // Another frame with the same sequence number (the parent's 8-bit
    // counter came round): different payload, accepted
    CHECK(rx_frame(&n, 111, "UDP Q 112", t + 6000000, 1506, 15));

    // LFN, 4 h interval: the previous frame from the parent (DSN 179) was
    // 4 h ago; the 32-bit µs clock wrapped three times since (the age
    // read on it, 1 515 s, is wrong), and the LFN re-anchored its parent's
    // UTT reference to 1 s ago. A new frame, DSN 179 by chance: accepted.
    // Before: dropped, and the NA of the LFN's registration was lost
    memset(&n, 0, sizeof(n));
    n.unicast_data_rx = true;
    t = 4285571569u;
    CHECK(rx_frame(&n, 179, "DHCPv6 reply", t, 110, 5 + 2 * 14400));
    uint32_t t2 = t + (uint32_t)(14400ull * 1000000ull);
    n.fhss_data.uc_timing_info.utt_rx_timestamp = t2 - 1000000;     // the LFN's re-anchoring
    CHECK(rx_frame(&n, 179, "NA ARO", t2, 14510, 5 + 2 * 14400));
    // ... and its retry in the next window, 4 h later: a duplicate
    uint32_t t3 = t2 + (uint32_t)(14400ull * 1000000ull);
    n.fhss_data.uc_timing_info.utt_rx_timestamp = t3 - 1000000;
    CHECK(!rx_frame(&n, 179, "NA ARO", t3, 28910, 5 + 2 * 14400));
}

/* -------------------------------------- 3. TTL of frames to LFN children */

static unsigned send_and_expire(bool to_lfn)
{
    static uint8_t ws_info_mem[4096];
    static mac_api_t mac_api;

    dst_is_lfn = to_lfn;
    memset(&iface, 0, sizeof(iface));
    iface.id = 1;
    memset(&mac_api, 0, sizeof(mac_api));
    mac_api.mcps_data_req_ext = fake_mcps_data_req_ext;
    mac_api.mcps_data_req = fake_mcps_data_req;     // checked for presence; the layer calls the _ext one
    iface.mac_api = &mac_api;
    iface.ws_info = (void *)ws_info_mem;
    CHECK(lowpan_adaptation_interface_init(iface.id, 127) == 0);
    mac_requests = 0;
    tx_events = 0;

    buffer_t *b = buffer_get(200);
    b->buf_ptr = 20;
    b->buf_end = 60;
    b->interface = &iface;
    b->priority = QOS_NORMAL;
    b->dst_sa.addr_type = ADDR_802_15_4_LONG;
    memcpy(&b->dst_sa.address[2], (const uint8_t[8]) { 1, 2, 3, 4, 5, 6, 0, 2 }, 8);
    b->src_sa.addr_type = ADDR_802_15_4_LONG;
    b->link_specific.ieee802_15_4.requestAck = true;
    CHECK(lowpan_adaptation_interface_tx(&iface, b) == 0);
    CHECK(mac_requests == 1);
    // The MAC's TTL for it expired (LFN: tx_ttl_intervals listen intervals)
    mcps_data_conf_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.msduHandle = last_handle;
    conf.status = MLME_TRANSACTION_EXPIRED;
    CHECK(lowpan_adaptation_interface_tx_confirm(&iface, &conf) == 0);
    unsigned again = mac_requests - 1;
    lowpan_adaptation_interface_free(iface.id);
    return again;
}

static void test_lfn_ttl(void)
{
    // To an LFN: dropped with a failure to the socket, not sent again.
    // Before: queued again and sent again (with a new sequence number, at
    // each expiry, for ever: the lost NA always came at last)
    unsigned again = send_and_expire(true);
    CHECK(again == 0);
    CHECK(tx_events == 1 && tx_event_status == SOCKET_TX_FAIL);
    // To an FFN: queued again and sent again, as before
    again = send_and_expire(false);
    CHECK(again == 1);
}

#ifndef OLD_STACK
/* ------------------------------------------------- 4. LPAS jitter (GS11) */

/* n draws in [lo, hi], in steps of 10 ms from lo, mean (lo + hi) / 2 within
 * tol_ms; both ends reached (unless there are too many steps for n) */
static void check_draws(uint32_t (*draw)(uint16_t), uint16_t arg, uint32_t lo, uint32_t hi, double tol_ms)
{
    const unsigned n = 200000;
    double sum = 0;
    bool in = true, step = true, lo_hit = false, hi_hit = false;

    for (unsigned i = 0; i < n; i++) {
        uint32_t v = draw(arg);
        in &= v >= lo && v <= hi;
        step &= (v - lo) % WS_LFN_JITTER_STEP_MS == 0;
        lo_hit |= v == lo;
        hi_hit |= v == hi;
        sum += v;
    }
    double mean = sum / n;
    printf("  draws in [%"PRIu32", %"PRIu32"] ms: mean %.1f ms (expected %.1f)\n", lo, hi, mean, (lo + hi) / 2.0);
    CHECK(in);
    CHECK(step);
    if ((hi - lo) / WS_LFN_JITTER_STEP_MS < n / 100) {
        CHECK(lo_hit && hi_hit);
    }
    CHECK(mean > (lo + hi) / 2.0 - tol_ms && mean < (lo + hi) / 2.0 + tol_ms);
}

static uint32_t draw_next(uint16_t interval_s) { return ws_lfn_lpas_next_ms(interval_s); }
static uint32_t draw_first(uint16_t unused) { (void)unused; return ws_lfn_lpas_first_ms(); }
static uint32_t draw_dither(uint16_t unused) { (void)unused; return ws_lfn_lpas_dither_ms(); }

/* Start times (ms) of the first n sweeps of a discovery started at 0; fixed:
 * the former timing (the next seconds tick, then every lpas_interval_s) */
static void sweep_times(uint32_t *t, unsigned n, uint16_t interval_s, bool fixed)
{
    t[0] = fixed ? 1000 : ws_lfn_lpas_first_ms();
    for (unsigned i = 1; i < n; i++) {
        t[i] = t[i - 1] + (fixed ? interval_s * 1000u : ws_lfn_lpas_next_ms(interval_s));
    }
}

/* Sweeps of A that start within 20 ms of one of B's (a frame's airtime at
 * 50 kbit/s and a tick), and the longest run of them in a row */
static unsigned together(const uint32_t *a, const uint32_t *b, unsigned n, unsigned *run_max)
{
    unsigned count = 0, run = 0, j = 0;

    *run_max = 0;
    for (unsigned i = 0; i < n; i++) {
        while (j + 1 < n && b[j + 1] <= a[i]) {
            j++;
        }
        bool near = (a[i] >= b[j] ? a[i] - b[j] : b[j] - a[i]) < 20 ||
                    (j + 1 < n && b[j + 1] - a[i] < 20);
        if (near) {
            count++;
            run++;
            if (run > *run_max) {
                *run_max = run;
            }
        } else {
            run = 0;
        }
    }
    return count;
}

static void test_lpas_jitter(void)
{
    enum { N = 2000 };
    static uint32_t ta[N], tb[N];
    unsigned run, count;

    printf("LPAS jitter\n");
    // The default configuration: lpas_interval_s 10 s, 7.5-12.5 s, mean 10 s
    ws_lfn_settings_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.lpas_interval_s = WS_LFN_LPAS_INTERVAL_S_DEFAULT;
    CHECK(cfg.lpas_interval_s == 10);
    check_draws(draw_next, cfg.lpas_interval_s, 7500, 12500, 20);
    // Other intervals: +- 25 %, the mean kept
    check_draws(draw_next, 1, 750, 1250, 2);
    check_draws(draw_next, 60, 45000, 75000, 120);
    check_draws(draw_next, 65535, 49151250, 81918750, 150000);
    // The first sweep: 0.5-1.5 s, mean 1 s; a sweep the seconds timer starts:
    // 0-990 ms after that second
    check_draws(draw_first, 0, 500, 1500, 5);
    check_draws(draw_dither, 0, 0, 990, 5);

    // Two LFNs whose discovery starts in the same tick (their own random
    // streams): the former timing kept every sweep together
    sweep_times(ta, N, 10, true);
    sweep_times(tb, N, 10, true);
    count = together(ta, tb, N, &run);
    printf("  fixed timing: %u of %u sweeps together, %u in a row\n", count, N, run);
    CHECK(count == N);
    unsigned worst = 0, worst_run = 0, total = 0;
    for (uint32_t pair = 1; pair <= 20; pair++) {
        rand_state = 0x9e3779b9u * pair;
        sweep_times(ta, N, 10, false);
        rand_state = 0x85ebca6bu * pair + 1;
        sweep_times(tb, N, 10, false);
        count = together(ta, tb, N, &run);
        total += count;
        worst = count > worst ? count : worst;
        worst_run = run > worst_run ? run : worst_run;
    }
    printf("  jitter, 20 pairs: %u of %u sweeps together (worst pair %u), at most %u in a row\n", total, 20 * N, worst, worst_run);
    // Expected about 0.4 % (a gap of 7.5-12.5 s in 10 ms steps against a
    // +-20 ms coincidence)
    CHECK(total < 20 * N / 100);
    CHECK(worst_run <= 2);

    // A search on the same second (children of one silent parent): the
    // sub-second dither, drawn anew at each search
    count = 0;
    run = 0;
    unsigned run_max = 0;
    rand_state = 12345;
    for (unsigned i = 0; i < N; i++) {
        uint32_t a = ws_lfn_lpas_dither_ms(), b = ws_lfn_lpas_dither_ms();
        if ((a > b ? a - b : b - a) < 20) {
            count++;
            run++;
            run_max = run > run_max ? run : run_max;
        } else {
            run = 0;
        }
    }
    printf("  searches on the same second: %u of %u together, at most %u in a row\n", count, N, run_max);
    // Expected about 3 % (3 of 100 steps)
    CHECK(count < N * 5 / 100);
    CHECK(run_max <= 3);
}
#endif

/* An LFN's restarts of a failed uplink against its parent's sweeps
 * (ws_lfn_restart.h). The FG25 host twin (NA plan, 129 channels at
 * 50 kbit/s) lost an uplink whose 4 restarts (250/2000 ms: 2.75 s at the
 * shortest) all fell inside the border router's PAN configuration and PAN
 * advertisement sweeps. Fails before the fix: the configured values were
 * used as they were. */
static void test_restart_span(void)
{
    printf("LFN restarts\n");
    uint16_t mn, mx;
    // The MAC's rule (mcps_set_packet_blacklist): 250-500, 500-1000,
    // 1000-2000, 1000-2000 ms
    CHECK(ws_lfn_restart_span_ms(250, 2000, 4) == 2750);
    CHECK(ws_lfn_restart_span_ms(20, 300, 4) == 20 + 40 + 80 + 150);
    CHECK(ws_lfn_restart_span_ms(250, 2000, 19) == 1750 + 16 * 1000);
    // Both sweeps against the emulator's (NA, 129 channels at 50 kbit/s:
    // 3.57 s and 2.14 s for a 127-byte PAN configuration and a 57-byte PAN
    // advertisement; EU, 35 channels at 100 kbit/s: 12.63 + 8.89 ms per
    // channel): the estimate is above, by less than a quarter
    CHECK(ws_lfn_sweeps_ms(129, 50000) >= 5710 && ws_lfn_sweeps_ms(129, 50000) < 5710 * 5 / 4);
    uint32_t emu = 35 * (12630 + 8890) / 1000;
    CHECK(ws_lfn_sweeps_ms(35, 100000) >= emu && ws_lfn_sweeps_ms(35, 100000) < emu * 3 / 2);
    // The defaults unchanged where they cover both sweeps (35 channels at
    // 100 kbit/s, 129 at 150 kbit/s), scaled where not (62 channels at
    // 50 kbit/s: 3.2 s, just above the 2.75 s; NA 129 at 50 kbit/s)
    const struct { uint16_t ch; uint32_t rate; bool scaled; } plans[] = {
        { 35, 100000, false }, { 62, 50000, true }, { 129, 50000, true }, { 129, 150000, false },
        { 136, 50000, true }, { 255, 10000, true },
    };
    for (unsigned i = 0; i < sizeof(plans) / sizeof(plans[0]); i++) {
        mn = 250;
        mx = 2000;
        ws_lfn_restart_bounds(plans[i].ch, plans[i].rate, 4, &mn, &mx);
        uint32_t need = ws_lfn_sweeps_ms(plans[i].ch, plans[i].rate);
        uint32_t span = ws_lfn_restart_span_ms(mn, mx, 4);
        printf("  %u channels at %u bit/s: both sweeps %u ms, restarts %u-%u ms, at least %u ms in all\n",
               plans[i].ch, (unsigned)plans[i].rate, (unsigned)need, mn, mx, (unsigned)span);
        CHECK((mn != 250) == plans[i].scaled);
        CHECK(span >= need || (mn == 0xffff && mx == 0xffff));
        CHECK(mx >= mn);
        if (plans[i].scaled && mx < 0xffff) {
            // not more than needed: within 5 %
            CHECK(span <= need + need / 20 + 10);
        }
    }
    // NA 129 channels at 50 kbit/s: the restarts outlast both sweeps as
    // the emulator shows them back to back (5.73 s)
    mn = 250;
    mx = 2000;
    ws_lfn_restart_bounds(129, 50000, 4, &mn, &mx);
    CHECK(ws_lfn_restart_span_ms(mn, mx, 4) >= 5730);
    // No rate known: unchanged
    mn = 250;
    mx = 2000;
    ws_lfn_restart_bounds(129, 0, 4, &mn, &mx);
    CHECK(mn == 250 && mx == 2000);
}

int main(void)
{
#ifndef OLD_STACK
    test_async_hold();
    test_lpas_jitter();
    test_restart_span();
#endif
    test_duplicates();
    test_lfn_ttl();
    printf("lfn_units: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
