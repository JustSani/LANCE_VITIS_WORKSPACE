/*
 * PROJECT NODE - P2P AUTO-IP CONFIGURATION & DISTRIBUTED FAULT RECOVERY
 */

#include <stdio.h>
#include <string.h>
#include "xparameters.h"
#include "netif/xadapter.h"
#include "platform_config.h"
#include "xil_printf.h"
#include "lwip/init.h"
#include "lwip/inet.h"
#include "xil_cache.h"
#include "xgpio.h"
#include "sleep.h"
#include "platform.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "xil_io.h"

/* --- LIBRERIE AGGIUNTE PER AUTO-DISCOVERY --- */
#include "lwip/etharp.h"    // Per gestire le richieste ARP
#include "xemacps.h"        // Per accedere ai registri fisici del MAC Address

/* Header funzioni TCP (da echo.c) */
extern void send_bitstream_from_ram_tcp(ip_addr_t *dest_ip);
extern int start_application();

/* Variabili Timer LwIP aggiornate da platform_zynq.c */
extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;

/* Flag di stato definita in echo.c */
extern volatile int transfer_going_on;

static struct netif server_netif;
struct netif *echo_netif;
ip_addr_t remote_ip;

XGpio Gpio;

/* =========================================================================
 * 1. STRUTTURE DATI E DEFINIZIONI DI RETE (UDP & STATI)
 * ========================================================================= */
#define UDP_BROADCAST_PORT 5000
#define MAX_NODES 10
#define MAX_TIMEOUT 5

#define STATE_SANO     0
#define STATE_GUASTO   1
#define STATE_RECOVERY 2

/* Struttura del pacchetto UDP inviato/ricevuto */
typedef struct __attribute__((packed)) {
    u32 ip_octet;
    u32 neorv32_output;
    u8  node_state;
} heartbeat_packet_t;

/* Struttura della Tabella di Stato in RAM */
typedef struct {
    u32 ip_octet;
    u32 neorv32_output;
    u8  node_state;
    u32 time_since_last_seen;
    int is_active;
} node_info_t;

node_info_t network_nodes[MAX_NODES];
struct udp_pcb *broadcast_pcb = NULL;

void init_network_nodes() {
    for(int i = 0; i < MAX_NODES; i++) {
        network_nodes[i].is_active = 0;
    }
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

/* =========================================================================
 * 4. AUTO-ASSEGNAZIONE MAC/IP E MAIN
 * ========================================================================= */
int reserve_mac_address(struct netif *netif) {
    ip_addr_t target_ip;
    int current_octet = 10;
    int ip_found = 0;

    xil_printf("\r\n[AUTO-IP] Inizio scansione rete dal .10 in poi...\r\n");

    while (!ip_found && current_octet <= 20) {
        IP4_ADDR(&target_ip, 192, 168, 15, current_octet);
        xil_printf("[AUTO-IP] Cerco 192.168.15.%d... ", current_octet);

        etharp_request(netif, &target_ip);

        for(int j = 0; j < 100; j++) {
            xemacif_input(netif);
            for(volatile int k = 0; k < 500000; k++);
        }

        const ip4_addr_t *ret_ip;
        struct eth_addr *ret_ethaddr;

        if (etharp_find_addr(netif, &target_ip, &ret_ethaddr, &ret_ip) >= 0) {
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

int main()
{
    init_platform();
    u32 b_val = 0;
    u32 b_old = 0;
    ip_addr_t ipaddr, netmask, gw;

    unsigned char mac_ethernet_address[] = { 0x00, 0x0a, 0x35, 0x00, 0x00, 250 };

    xil_printf("\r\n--- NODO AVVIATO (Fase di Discovery) ---\r\n");

    IP4_ADDR(&ipaddr,  192, 168, 15, 250);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 15, 1);

    echo_netif = &server_netif;

    if (XGpio_Initialize(&Gpio, XPAR_AXI_GPIO_0_DEVICE_ID) != XST_SUCCESS) {
        return -1;
    }
    XGpio_SetDataDirection(&Gpio, 1, 0xFFFFFFFF);

    lwip_init();

    if (!xemac_add(echo_netif, &ipaddr, &netmask, &gw, mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
        return -1;
    }

    netif_set_default(echo_netif);
    platform_enable_interrupts();
    netif_set_up(echo_netif);

    int my_octet = reserve_mac_address(echo_netif);
    if (my_octet < 0) { while(1); }

    int dest_octet = 10;
    if (dest_octet == my_octet) dest_octet = 11;
    IP4_ADDR(&remote_ip, 192, 168, 15, dest_octet);

    xil_printf("\r\n--- CONFIGURAZIONE COMPLETATA ---\r\n");
    xil_printf("Mio IP: 192.168.15.%d\r\n", my_octet);
    xil_printf("---------------------------------\r\n");

    /* Avvio Server TCP */
    start_application();

    /* Avvio Protocollo Distribuito UDP */
    init_network_nodes();
    init_udp_broadcast();
    setup_udp_listener();

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
    // Dati fittizi per test (in futuro leggerai dal NEORV32)
    u32 my_neorv32_output = 12346;
    u8 my_state = STATE_SANO;
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------


    // Contatore per l'invio temporizzato del broadcast
    u32 loop_counter = 0;

    /* --- Loop Principale --- */
    while (1) {
        xemacif_input(echo_netif);

        if (TcpSlowTmrFlag) {
            tcp_slowtmr();
            TcpSlowTmrFlag = 0;

            // Ogni mezzo secondo (approx), eseguiamo la routine di supervisione
            loop_counter++;

            // 1. Invio Heartbeat a tutti
            send_heartbeat_broadcast(my_octet, my_neorv32_output, my_state);

            // 2. Invecchiamento nodi
            for(int i=0; i<MAX_NODES; i++) {
                if(network_nodes[i].is_active) network_nodes[i].time_since_last_seen++;
            }

            // 3. Autodiagnosi e Recovery (eseguita ogni 2 secondi per stabilizzare la rete)
            if (loop_counter >= 4 && !transfer_going_on) {
                loop_counter = 0;
                u32 golden_output;

                if (get_golden_output(&golden_output)) {

                    if (my_neorv32_output != golden_output) {
                        my_state = STATE_GUASTO;
                    } else {
                        my_state = STATE_SANO;
                    }

                    if (my_state == STATE_SANO) {
                        int ip_da_riparare = -1;
                        for (int i = 0; i < MAX_NODES; i++) {
                            if (network_nodes[i].is_active && network_nodes[i].time_since_last_seen <= MAX_TIMEOUT) {
                                if (network_nodes[i].node_state == STATE_GUASTO) {
                                    ip_da_riparare = network_nodes[i].ip_octet;
                                    break;
                                }
                            }
                        }

                        if (ip_da_riparare != -1) {
                            if (am_i_the_repair_master(my_octet, my_state)) {
                                xil_printf("\r\n[RECOVERY] Il nodo .%d e' guasto! Sono il Master, avvio ripristino...\r\n", ip_da_riparare);
                                ip_addr_t target_ip;
                                IP4_ADDR(&target_ip, 192, 168, 15, ip_da_riparare);
                                send_bitstream_from_ram_tcp(&target_ip);
                                for(volatile int k=0; k<50000000; k++);
                            }
                        }
                    }
                }
            }
        }

        if (TcpFastTmrFlag) {
            tcp_fasttmr();
            TcpFastTmrFlag = 0;
        }

        b_val = XGpio_DiscreteRead(&Gpio, 1);
        if (!transfer_going_on && b_val != 0 && b_old == 0) {
            if (b_val & 0x01) {
                xil_printf("\r\n[TX] -> Avvio connessione TCP verso 192.168.15.%d\r\n", dest_octet);
                send_bitstream_from_ram_tcp(&remote_ip);
            }
            else if (b_val & 0x02) {
                dest_octet++;
                if (dest_octet > 20) dest_octet = 10;
                if (dest_octet == my_octet) {
                    dest_octet++;
                    if (dest_octet > 20) dest_octet = 10;
                }
                IP4_ADDR(&remote_ip, 192, 168, 15, dest_octet);
                xil_printf("[MENU] Nuovo Target selezionato: 192.168.15.%d\r\n", dest_octet);
            }
            do {
                b_val = XGpio_DiscreteRead(&Gpio, 1);
                xemacif_input(echo_netif);
            } while (b_val != 0);
            for(volatile int i=0; i<10000000; i++);
        }
        b_old = b_val;
    }

    cleanup_platform();
    return 0;
}
