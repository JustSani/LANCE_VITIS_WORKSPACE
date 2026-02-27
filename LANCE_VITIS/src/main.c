/*
 * PROJECT NODE - P2P AUTO-IP & DISTRIBUTED FAULT RECOVERY
 */

#include <stdio.h>
#include "xparameters.h"
#include "netif/xadapter.h"
#include "platform_config.h"
#include "xil_printf.h"
#include "lwip/init.h"
#include "lwip/inet.h"
#include "xgpio.h"
#include "sleep.h"
#include "platform.h"
#include "xil_io.h"

/* --- I nostri Moduli Personalizzati --- */
#include "network_utils.h"
#include "fault_tolerance.h"

/* Header funzioni TCP (da echo.c) */
extern void send_bitstream_from_ram_tcp(ip_addr_t *dest_ip);
extern int start_application();

/* Variabili LwIP e di Rete */
extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
extern volatile int transfer_going_on;

static struct netif server_netif;
struct netif *echo_netif;
ip_addr_t remote_ip;
XGpio Gpio;

int main()
{
    init_platform();
    u32 b_val = 0;
    u32 b_old = 0;
    ip_addr_t ipaddr, netmask, gw;
    u32 my_neorv32_output;
    u8 my_state = STATE_SANO;
    u32 loop_counter = 0;

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

    // 1. Assegnazione IP dinamico
    int my_octet = reserve_mac_address(echo_netif);
    if (my_octet < 0) { while(1); }

    int dest_octet = 10;
    if (dest_octet == my_octet) dest_octet = 11;
    IP4_ADDR(&remote_ip, 192, 168, 15, dest_octet);

    xil_printf("\r\n--- CONFIGURAZIONE COMPLETATA ---\r\n");
    xil_printf("Mio IP: 192.168.15.%d\r\n", my_octet);
    xil_printf("---------------------------------\r\n");

    // 2. Avvio Server TCP ed Elementi UDP
    start_application();
    init_network_nodes();
    init_udp_broadcast();
    setup_udp_listener();

    /* --- Loop Principale --- */
    while (1) {
        xemacif_input(echo_netif);

        if (TcpSlowTmrFlag) {
            tcp_slowtmr();
            TcpSlowTmrFlag = 0;
            loop_counter++;

            // Lettura hardware dal NEORV32
            my_neorv32_output = Xil_In32(XPAR_GPIO_NEORV32_4BITS_BASEADDR);


            // Trasmissione Heartbeat
            send_heartbeat_broadcast(my_octet, my_neorv32_output, my_state);

            // Invecchiamento nodi
            for(int i=0; i<MAX_NODES; i++) {
                if(network_nodes[i].is_active) network_nodes[i].time_since_last_seen++;
            }

            // Autodiagnosi e Recovery (ogni 2 secondi)
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

        // Controllo pulsanti per test e debug manuale
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
