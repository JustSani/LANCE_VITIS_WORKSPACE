#ifndef FAULT_TOLERANCE_H
#define FAULT_TOLERANCE_H

#include "xil_types.h"

#define MAX_NODES 10
#define MAX_TIMEOUT 5

#define STATE_SANO     0
#define STATE_GUASTO   1
#define STATE_RECOVERY 2

/* Struttura della Tabella di Stato in RAM */
typedef struct {
    u32 ip_octet;
    u32 neorv32_output;
    u8  node_state;
    u32 time_since_last_seen;
    int is_active;
} node_info_t;

/* Esportiamo l'array globale in modo che network_utils.c possa scriverci dentro */
extern node_info_t network_nodes[MAX_NODES];

/* --- Prototipi delle funzioni pubbliche --- */
void init_network_nodes(void);
int get_golden_output(u32 *golden_output);
int am_i_the_repair_master(u32 my_ip_octet, u8 my_state);

#endif /* FAULT_TOLERANCE_H */
