#include "network_utils.h"
#include "fault_tolerance.h" // Necessario per usare 'network_nodes' e 'MAX_NODES'

#include "xil_printf.h"
#include "string.h"          // Per memcpy
#include "lwip/etharp.h"     // Per le richieste ARP
#include "xemacps.h"         // Per i registri MAC
#include "xparameters.h"
#include "xil_io.h"

/* --- LIBRERIE MANCANTI AGGIUNTE --- */
#include "netif/xadapter.h"
#include "platform_config.h"

/* Variabile per la connessione UDP (visibile solo in questo file) */
struct udp_pcb *broadcast_pcb = NULL;

/* =========================================================================
 * 4. AUTO-ASSEGNAZIONE MAC/IP E MAIN
 * ========================================================================= */
/* =========================================================================
 * 4. AUTO-ASSEGNAZIONE MAC/IP (VERSIONE ROBUSTA)
 * ========================================================================= */
int reserve_mac_address(struct netif *netif) {
    ip_addr_t target_ip;
    int current_octet = 10;
    int ip_found = 0;

    xil_printf("\r\n[AUTO-IP] Inizio scansione rete dal .10 in poi...\r\n");

    while (!ip_found && current_octet <= 20) {
        IP4_ADDR(&target_ip, 192, 168, 15, current_octet);
        xil_printf("[AUTO-IP] Cerco 192.168.15.%d... ", current_octet);

        int someone_replied = 0;

        // FIX: Effettuiamo 3 tentativi. Se lo switch perde il primo pacchetto
        // per il ritardo di Link-Up, gli altri andranno a segno.
        for (int attempt = 0; attempt < 3; attempt++) {
            etharp_request(netif, &target_ip);

            // Ascoltiamo eventuali risposte per circa 200ms
            for(int j = 0; j < 100; j++) {
                xemacif_input(netif);
                for(volatile int k = 0; k < 500000; k++);
            }

            const ip4_addr_t *ret_ip;
            struct eth_addr *ret_ethaddr;

            // Se troviamo l'IP nella tabella ARP, qualcuno ha risposto!
            if (etharp_find_addr(netif, &target_ip, &ret_ethaddr, &ret_ip) >= 0) {
                someone_replied = 1;
                break; // Usciamo subito dal ciclo dei tentativi
            }
        }

        if (someone_replied) {
            xil_printf("OCCUPATO!\r\n");
            current_octet++;
        } else {
            xil_printf("LIBERO!\r\n");
            ip_found = 1;
        }
    }

    if (!ip_found) return -1;

    xil_printf("[AUTO-IP] Mi assegno l'IP .%d e il relativo MAC Address fisico.\r\n", current_octet);

    netif_set_down(netif);

    ip_addr_t new_ip, netmask, gw;
    IP4_ADDR(&new_ip, 192, 168, 15, current_octet);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 15, 1);
    netif_set_addr(netif, &new_ip, &netmask, &gw);

    netif->hwaddr[5] = current_octet;

    u32 mac_bot = (netif->hwaddr[3] << 24) | (netif->hwaddr[2] << 16) | (netif->hwaddr[1] << 8) | netif->hwaddr[0];
    u32 mac_top = (netif->hwaddr[5] << 8) | netif->hwaddr[4];
    Xil_Out32(PLATFORM_EMAC_BASEADDR + 0x00000088, mac_bot);
    Xil_Out32(PLATFORM_EMAC_BASEADDR + 0x0000008C, mac_top);

    netif_set_up(netif);

    return current_octet;
}

/* =========================================================================
 * 2. FUNZIONI DI RICEZIONE E TRASMISSIONE UDP (HEARTBEAT)
 * ========================================================================= */
void udp_receive_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    if (p != NULL) {
        if (p->len == sizeof(heartbeat_packet_t)) {
            heartbeat_packet_t *incoming_data = (heartbeat_packet_t *)p->payload;

            int found = 0;
            int free_slot = -1;

            for (int i = 0; i < MAX_NODES; i++) {
                if (network_nodes[i].is_active) {
                    if (network_nodes[i].ip_octet == incoming_data->ip_octet) {
                        network_nodes[i].neorv32_output = incoming_data->neorv32_output;
                        network_nodes[i].node_state = incoming_data->node_state;
                        network_nodes[i].time_since_last_seen = 0;
                        found = 1;
                        break;
                    }
                } else if (free_slot == -1) {
                    free_slot = i;
                }
            }

            if (!found && free_slot != -1) {
                network_nodes[free_slot].ip_octet = incoming_data->ip_octet;
                network_nodes[free_slot].neorv32_output = incoming_data->neorv32_output;
                network_nodes[free_slot].node_state = incoming_data->node_state;
                network_nodes[free_slot].time_since_last_seen = 0;
                network_nodes[free_slot].is_active = 1;
                xil_printf("[RETE] Nuovo nodo rilevato: 192.168.15.%d\r\n", incoming_data->ip_octet);
            }
        }
        pbuf_free(p);
    }
}

void setup_udp_listener() {
    struct udp_pcb *listen_pcb = udp_new();
    if (listen_pcb != NULL) {
        udp_bind(listen_pcb, IP_ADDR_ANY, UDP_BROADCAST_PORT);
        udp_recv(listen_pcb, udp_receive_callback, NULL);
    }
}

void init_udp_broadcast() {
    broadcast_pcb = udp_new();
}

void send_heartbeat_broadcast(u32 my_ip_octet, u32 current_neorv32_output, u8 current_state) {
    if (broadcast_pcb == NULL) return;
    heartbeat_packet_t my_info;
    my_info.ip_octet = my_ip_octet;
    my_info.neorv32_output = current_neorv32_output;
    my_info.node_state = current_state;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(heartbeat_packet_t), PBUF_RAM);
    if (p == NULL) return;
    memcpy(p->payload, &my_info, sizeof(heartbeat_packet_t));

    ip_addr_t broadcast_ip;
    IP4_ADDR(&broadcast_ip, 255, 255, 255, 255);
    udp_sendto(broadcast_pcb, p, &broadcast_ip, UDP_BROADCAST_PORT);
    pbuf_free(p);
}
