#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#include "xil_types.h"
#include "lwip/netif.h"
#include "lwip/udp.h"

#define UDP_BROADCAST_PORT 5000

/* Struttura del pacchetto UDP inviato/ricevuto */
typedef struct __attribute__((packed)) {
    u32 ip_octet;
    u32 neorv32_output;
    u8  node_state;
} heartbeat_packet_t;

/* --- Prototipi delle funzioni pubbliche --- */
int reserve_mac_address(struct netif *netif);
void setup_udp_listener(void);
void init_udp_broadcast(void);
void send_heartbeat_broadcast(u32 my_ip_octet, u32 current_neorv32_output, u8 current_state);

#endif /* NETWORK_UTILS_H */
