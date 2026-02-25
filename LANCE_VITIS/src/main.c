/*
 * PROJECT NODE - P2P AUTO-IP CONFIGURATION (TCP VERSION)
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
 * FUNZIONE DI AUTO-ASSEGNAZIONE IP E MAC ADDRESS TRAMITE ARP
 * ========================================================================= */
int reserve_mac_address(struct netif *netif) {
    ip_addr_t target_ip;
    int current_octet = 10; // Partiamo dall'IP 192.168.15.10
    int ip_found = 0;

    xil_printf("\r\n[AUTO-IP] Inizio scansione rete dal .10 in poi...\r\n");

    while (!ip_found && current_octet <= 20) {
        IP4_ADDR(&target_ip, 192, 168, 15, current_octet);
        xil_printf("[AUTO-IP] Cerco 192.168.15.%d... ", current_octet);

        // 1. Invia la richiesta ARP ("C'è qualcuno con questo IP?")
        etharp_request(netif, &target_ip);

        // 2. Aspettiamo ascoltando le risposte per dare tempo alla rete
        for(int j = 0; j < 100; j++) {
            xemacif_input(netif); // Pompaggio pacchetti per ricevere la risposta
            for(volatile int k = 0; k < 500000; k++); // Ritardo software
        }

        // 3. Controlliamo se la tabella ARP ha registrato una risposta
        const ip4_addr_t *ret_ip;
        struct eth_addr *ret_ethaddr;

        // Se restituisce >= 0, significa che l'IP esiste ed ha risposto
        if (etharp_find_addr(netif, &target_ip, &ret_ethaddr, &ret_ip) >= 0) {
            xil_printf("OCCUPATO!\r\n");
            current_octet++; // Passiamo al prossimo IP
        } else {
            xil_printf("LIBERO!\r\n");
            ip_found = 1;
        }
    }

    if (!ip_found) {
        xil_printf("[AUTO-IP] ERRORE CRITICO: Nessun IP libero trovato nel range!\r\n");
        return -1;
    }

    /* --- FASE DI ASSEGNAZIONE DEFINITIVA --- */
        xil_printf("[AUTO-IP] Mi assegno l'IP .%d e il relativo MAC Address fisico.\r\n", current_octet);

        // 1. Spegniamo momentaneamente LwIP per resettare la sua cache ARP
        netif_set_down(netif);

        // 2. Modifichiamo l'IP Software
        ip_addr_t new_ip, netmask, gw;
        IP4_ADDR(&new_ip, 192, 168, 15, current_octet);
        IP4_ADDR(&netmask, 255, 255, 255, 0);
        IP4_ADDR(&gw, 192, 168, 15, 1);
        netif_set_addr(netif, &new_ip, &netmask, &gw);

        // 3. Modifichiamo il MAC Address Software
        netif->hwaddr[5] = current_octet;

        // 4. Modifichiamo il MAC Address HARDWARE scrivendo direttamente nei registri!
        // Bypassiamo le strutture di LwIP. I registri LSA1 (0x88) e HSA1 (0x8C)
        // controllano fisicamente il filtro MAC del controller Ethernet dello Zynq.
        u32 mac_bot = (netif->hwaddr[3] << 24) | (netif->hwaddr[2] << 16) | (netif->hwaddr[1] << 8) | netif->hwaddr[0];
        u32 mac_top = (netif->hwaddr[5] << 8) | netif->hwaddr[4];

        Xil_Out32(PLATFORM_EMAC_BASEADDR + 0x00000088, mac_bot);
        Xil_Out32(PLATFORM_EMAC_BASEADDR + 0x0000008C, mac_top);

        // 5. Riaccendiamo l'interfaccia. LwIP ora invierà automaticamente il Gratuitous ARP corretto!
        netif_set_up(netif);

        return current_octet;
}

/* =========================================================================
 * MAIN
 * ========================================================================= */
int main()
{
    init_platform();

    u32 b_val = 0;
    u32 b_old = 0;
    ip_addr_t ipaddr, netmask, gw;

    // 1. MAC e IP PROVVISORI (Modalità "Scanner")
    // Usiamo il .250 in modo da non creare conflitti con i veri IP
    unsigned char mac_ethernet_address[] = { 0x00, 0x0a, 0x35, 0x00, 0x00, 250 };

    xil_printf("\r\n--- NODO AVVIATO (Fase di Discovery) ---\r\n");

    IP4_ADDR(&ipaddr,  192, 168, 15, 250);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 15, 1);

    echo_netif = &server_netif;

    if (XGpio_Initialize(&Gpio, XPAR_AXI_GPIO_0_DEVICE_ID) != XST_SUCCESS) {
        xil_printf("Errore GPIO!\r\n");
        return -1;
    }
    XGpio_SetDataDirection(&Gpio, 1, 0xFFFFFFFF);

    lwip_init();

    if (!xemac_add(echo_netif, &ipaddr, &netmask, &gw, mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding N/W interface\n\r");
        return -1;
    }

    netif_set_default(echo_netif);

    // QUESTA RIGA È FONDAMENTALE: Attiva gli interrupt che fanno funzionare il tuo echo.c
    platform_enable_interrupts();

    netif_set_up(echo_netif);

    // ======================================================
    // 2. LANCIO L'AUTO-ASSEGNAZIONE (Sostituisce le define)
    // ======================================================
    int my_octet = reserve_mac_address(echo_netif);
    if (my_octet < 0) {
        while(1); // Se fallisce la rete, blocca tutto
    }

    // ======================================================
        // 3. INIZIALIZZAZIONE DEL TARGET DINAMICO
        // ======================================================
        int dest_octet = 10; // Partiamo dal .10 come default
        if (dest_octet == my_octet) {
            dest_octet = 11; // Se il .10 sono io, parto dal .11
        }
        IP4_ADDR(&remote_ip, 192, 168, 15, dest_octet);

        xil_printf("\r\n--- CONFIGURAZIONE COMPLETATA ---\r\n");
        xil_printf("Mio IP: 192.168.15.%d\r\n", my_octet);
        xil_printf("Target attuale: 192.168.15.%d\r\n", dest_octet);
        xil_printf("---------------------------------\r\n");

        start_application();

        xil_printf("\n[COMANDI HARDWARE]\r\n");
        xil_printf("- Premi BTN1 (Secondo bottone) per CAMBIARE destinatario.\r\n");
        xil_printf("- Premi BTN0 (Primo bottone) per INVIARE il bitstream.\r\n\n");

        /* --- Loop Principale --- */
        while (1) {
            xemacif_input(echo_netif);

            if (TcpSlowTmrFlag) {
                tcp_slowtmr();
                TcpSlowTmrFlag = 0;
            }
            if (TcpFastTmrFlag) {
                tcp_fasttmr();
                TcpFastTmrFlag = 0;
            }

            b_val = XGpio_DiscreteRead(&Gpio, 1);

            if (!transfer_going_on && b_val != 0 && b_old == 0) {

                // --- AZIONE 1: INVIA IL BITSTREAM (BTN0 - Bit 0 a 1) ---
                if (b_val & 0x01) {
                    xil_printf("\r\n[TX] -> Avvio connessione TCP verso 192.168.15.%d\r\n", dest_octet);
                    send_bitstream_from_ram_tcp(&remote_ip);
                }

                // --- AZIONE 2: CAMBIA DESTINATARIO (BTN1 - Bit 1 a 1) ---
                else if (b_val & 0x02) {
                    dest_octet++;

                    // Mettiamo un limite massimo fittizio (es. 20) per non cercare all'infinito
                    if (dest_octet > 20) {
                        dest_octet = 10;
                    }

                    // Saltiamo il nostro stesso IP per non auto-inviarci il file
                    if (dest_octet == my_octet) {
                        dest_octet++;
                        if (dest_octet > 20) dest_octet = 10;
                    }

                    // Aggiorniamo l'IP di destinazione nella variabile globale di lwIP
                    IP4_ADDR(&remote_ip, 192, 168, 15, dest_octet);
                    xil_printf("[MENU] Nuovo Target selezionato: 192.168.15.%d\r\n", dest_octet);
                }

                // --- ATTESA RILASCIO PULSANTE COMUNE ---
                do {
                    b_val = XGpio_DiscreteRead(&Gpio, 1);
                    xemacif_input(echo_netif); // Mantiene viva la rete mentre teniamo premuto
                } while (b_val != 0);

                for(volatile int i=0; i<10000000; i++); // Pausa meccanica anti-rimbalzo
            }

            b_old = b_val;
        }

        cleanup_platform();
        return 0;
}
