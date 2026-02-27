#include "fault_tolerance.h"

/* Definizione effettiva dell'array globale allocato in memoria */
node_info_t network_nodes[MAX_NODES];

void init_network_nodes() {
    for(int i = 0; i < MAX_NODES; i++) {
        network_nodes[i].is_active = 0;
    }
}

/* ... Da qui in poi lascia esattamente il tuo codice:
 * int get_golden_output(u32 *golden_output) { ... }
 * int am_i_the_repair_master(u32 my_ip_octet, u8 my_state) { ... }
 */

/* =========================================================================
 * 3. LOGICA DI ELEZIONE E SUPERVISIONE
 * ========================================================================= */
int get_golden_output(u32 *golden_output) {
    u32 candidate_outputs[MAX_NODES];
    int counts[MAX_NODES] = {0};
    int num_candidates = 0;
    int total_valid_nodes = 0;

    for (int i = 0; i < MAX_NODES; i++) {
        if (network_nodes[i].is_active && network_nodes[i].time_since_last_seen <= MAX_TIMEOUT) {
            u32 out_val = network_nodes[i].neorv32_output;
            total_valid_nodes++;

            int found = 0;
            for (int j = 0; j < num_candidates; j++) {
                if (candidate_outputs[j] == out_val) {
                    counts[j]++;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                candidate_outputs[num_candidates] = out_val;
                counts[num_candidates] = 1;
                num_candidates++;
            }
        }
    }

    if (total_valid_nodes == 0) return 0;

    int max_count = 0;
    u32 best_output = 0;
    for (int j = 0; j < num_candidates; j++) {
        if (counts[j] > max_count) {
            max_count = counts[j];
            best_output = candidate_outputs[j];
        }
    }

    *golden_output = best_output;
    return 1;
}

int am_i_the_repair_master(u32 my_ip_octet, u8 my_state) {
    if (my_state != STATE_SANO) return 0;

    for (int i = 0; i < MAX_NODES; i++) {
        if (network_nodes[i].is_active && network_nodes[i].time_since_last_seen <= MAX_TIMEOUT) {
            if (network_nodes[i].node_state == STATE_SANO) {
                if (network_nodes[i].ip_octet < my_ip_octet) {
                    return 0;
                }
            }
        }
    }
    return 1;
}
