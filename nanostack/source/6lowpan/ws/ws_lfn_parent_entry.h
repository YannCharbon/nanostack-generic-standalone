/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WS_LFN_PARENT_ENTRY_H_
#define WS_LFN_PARENT_ENTRY_H_

#include <stdbool.h>
#include "service_libs/mac_neighbor_table/mac_neighbor_table.h"
#include "6lowpan/ws/ws_common_defines.h"

/*
 * An LFN's parent's neighbour entry (docs/LFN_STACK.md §3) [I].
 *
 * While it is our parent the entry does not expire: we may hear it once per
 * sync period (Silicon Labs' Eco: 10 h), and an entry gone (2200 s,
 * WS_NEIGHBOR_LINK_TIMEOUT) refused every uplink until the next frame from
 * it. Parent maintenance decides when it is lost. Its link role is
 * PRIORITY_PARENT_NEIGHBOUR: the LLC gives the FHSS the broadcast timing of
 * a frame from that neighbour (ns_fhss_ws_set_parent(), BT-IE). Dropped as
 * parent (a parent change or loss): the usual lifetime and a normal link
 * role again. Before, the old parent kept its role, and each downlink from
 * it (its unicast frames carry a BT-IE) made it the FHSS's parent again
 * after a parent change: our frames to the new parent were then not the
 * view of our slots (they went to "another node"), and our uplinks avoided
 * the old parent's broadcast dwell, until a downlink from the new parent
 * (emulator test_lfn_better seed 1: 40 s; hours for an LFN that rarely
 * gets downlinks).
 */
static inline void ws_lfn_parent_entry_set(mac_neighbor_table_entry_t *n, bool parent)
{
    if (!n) {
        return;
    }
    n->link_lifetime = parent ? 0xffffffff : WS_NEIGHBOR_LINK_TIMEOUT;
    n->lifetime = n->link_lifetime;
    n->link_role = parent ? PRIORITY_PARENT_NEIGHBOUR : NORMAL_NEIGHBOUR;
}

#endif /* WS_LFN_PARENT_ENTRY_H_ */
