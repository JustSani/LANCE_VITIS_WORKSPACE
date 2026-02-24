/*
 * PROJECT NODE - P2P PING-PONG CONFIGURATION (TCP VERSION)
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

/* ==========================================================
 * MODIFICA QUESTO NUMERO:
 * 1 -> Compila per la Scheda A (IP .10)
 * 2 -> Compila per la Scheda B (IP .11)
 * ========================================================== */
#define BOARD_ID 1

#if (BOARD_ID == 1)
    #define MY_IP_OCTET   10
    #define DEST_IP_OCTET 11
    #define NODE_NAME     "NODO A"
#elif (BOARD_ID == 2)
    #define MY_IP_OCTET   11
    #define DEST_IP_OCTET 10
    #define NODE_NAME     "NODO B"
#else
    #error "BOARD_ID non valido! Scegli 1 o 2."
#endif

/* Header funzioni TCP */
void send_bitstream_from_ram_tcp(ip_addr_t *dest_ip);
int start_application();

/* Variabili Timer LwIP */
extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;

/* Flag di stato definita in echo.c */
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

    unsigned char mac_ethernet_address[] = { 0x00, 0x0a, 0x35, 0x00, 0x00, MY_IP_OCTET };

    xil_printf("\r\n--- %s AVVIATO (TCP MODE) ---\r\n", NODE_NAME);
    xil_printf("IP Locale: 192.168.15.%d\r\n", MY_IP_OCTET);
    xil_printf("IP Destinatario: 192.168.15.%d\r\n", DEST_IP_OCTET);
    xil_printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                mac_ethernet_address[0], mac_ethernet_address[1], mac_ethernet_address[2],
                mac_ethernet_address[3], mac_ethernet_address[4], mac_ethernet_address[5]);
    xil_printf("-----------------------------------\r\n");

    IP4_ADDR(&ipaddr,  192, 168,   15, MY_IP_OCTET);
    IP4_ADDR(&netmask, 255, 255, 255,  0);
    IP4_ADDR(&gw,      192, 168,   15,  1);
    IP4_ADDR(&remote_ip, 192, 168, 15, DEST_IP_OCTET);

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
    platform_enable_interrupts();
    netif_set_up(echo_netif);

    /* Avvia il Server TCP in ascolto */
    start_application();

    xil_printf("Sistema Pronto. Premi il pulsante per trasmettere.\r\n");

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

        // Se non ci sono trasferimenti attivi e premo il tasto
		if (!transfer_going_on && b_val != 0 && b_old == 0) {
			xil_printf("\r\nBITSTREAM -> Avvio connessione TCP verso 192.168.15.%d\r\n", DEST_IP_OCTET);

            // Chiama la nuova funzione TCP
			send_bitstream_from_ram_tcp(&remote_ip);

			xil_printf("Attesa rilascio pulsante...\r\n");
			do {
				b_val = XGpio_DiscreteRead(&Gpio, 1);
				xemacif_input(echo_netif); // Evita blocchi
			} while (b_val != 0);

			for(volatile int i=0; i<10000000; i++); // Pausa meccanica
		}
		b_old = b_val;
    }

    cleanup_platform();
    return 0;
}
