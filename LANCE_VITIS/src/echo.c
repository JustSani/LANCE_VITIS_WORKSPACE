#include <stdio.h>
#include <string.h>
#include <stdlib.h> // Per EXIT_SUCCESS

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "xil_cache.h"

#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif

// Costanti di Memoria e Rete
#define RAM_SOURCE_ADDR  0x10000000
#define RAM_DEST_ADDR    0x11000000
#define BITSTREAM_SIZE   1357084
#define TCP_PORT         7

// Stati del sistema per evitare collisioni (Half-Duplex logico)
#define SYS_IDLE      0
#define SYS_TX_ACTIVE 1
#define SYS_RX_ACTIVE 2

volatile int sys_state = SYS_IDLE;
volatile int transfer_going_on = 0;

// Puntatori per la ricezione
u8 *current_write_ptr = (u8 *)RAM_DEST_ADDR;
u32 total_bytes_received = 0;

// Variabile per gestire la connessione asincrona del Client
volatile int client_connected = 0;
struct tcp_pcb *client_pcb = NULL;

extern struct netif *echo_netif;
extern int load_partial_bitstream(u32 address, u32 size);

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
extern void xemacif_input(struct netif *netif);
extern void tcp_fasttmr(void);
extern void tcp_slowtmr(void);

/* =========================================================================
 * PARTE 1: TCP SERVER (RICEZIONE)
 * ========================================================================= */
static err_t server_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        tcp_close(tpcb);
        sys_state = SYS_IDLE;
        transfer_going_on = 0;
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        if (total_bytes_received + q->len <= BITSTREAM_SIZE) {
            memcpy(current_write_ptr, q->payload, q->len);
            current_write_ptr += q->len;
            total_bytes_received += q->len;
        }
    }

    pbuf_free(p);

    if (total_bytes_received >= BITSTREAM_SIZE) {
        xil_printf("\r\n--> DOWNLOAD TCP COMPLETATO! (%d bytes)\r\n", total_bytes_received);
        Xil_DCacheFlushRange(RAM_DEST_ADDR, BITSTREAM_SIZE);
        xil_printf("--> Avvio Riconfigurazione FPGA...\r\n");

        int status = load_partial_bitstream(RAM_DEST_ADDR, BITSTREAM_SIZE);

        if (status == EXIT_SUCCESS) {
            xil_printf("--> SUCCESSO! FPGA Riconfigurata.\r\n");
        } else {
            xil_printf("--> ERRORE nella riconfigurazione Hardware.\r\n");
        }

        sys_state = SYS_IDLE;
        transfer_going_on = 0;
        tcp_close(tpcb);
    }
    return ERR_OK;
}

static err_t server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    xil_printf("\r\n--- Connessione TCP in ingresso accettata! ---\r\n");
    sys_state = SYS_RX_ACTIVE;
    transfer_going_on = 1;
    total_bytes_received = 0;
    current_write_ptr = (u8 *)RAM_DEST_ADDR;
    tcp_recv(newpcb, server_recv_cb);
    return ERR_OK;
}

int start_application() {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        xil_printf("Errore creazione TCP PCB.\r\n");
        return -1;
    }
    tcp_bind(pcb, IP_ADDR_ANY, TCP_PORT);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, server_accept_cb);

    xil_printf("TCP Server inizializzato e in ascolto sulla porta %d\r\n", TCP_PORT);
    return 0;
}


/* =========================================================================
 * PARTE 2: TCP CLIENT (INVIO)
 * ========================================================================= */
static err_t client_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    if (err == ERR_OK) {
        client_connected = 1;
    }
    return ERR_OK;
}

void send_heartbeat_broadcast(u32 my_ip_octet, u32 current_neorv32_output, u8 current_state); // Prototipo

void send_bitstream_from_ram_tcp(ip_addr_t *dest_ip) {
    if (sys_state != SYS_IDLE) return;

    sys_state = SYS_TX_ACTIVE;
    transfer_going_on = 1;
    client_connected = 0;
    client_pcb = tcp_new();

    tcp_connect(client_pcb, dest_ip, TCP_PORT, client_connected_cb);

    // 1. Attesa connessione (stesso tuo codice)
    int timeout_conn = 0;
    while (!client_connected && timeout_conn < 100) {
        xemacif_input(echo_netif);
        if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; timeout_conn++; }
        if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }
    }

    if (!client_connected) {
        xil_printf("Errore: Timeout connessione.\r\n");
        tcp_abort(client_pcb);
        sys_state = SYS_IDLE;
        transfer_going_on = 0;
        return;
    }

    xil_printf("Connesso! Invio bitstream (%d bytes)...\r\n", BITSTREAM_SIZE);
    Xil_DCacheFlushRange(RAM_SOURCE_ADDR, BITSTREAM_SIZE);

    u8 *ptr = (u8 *)RAM_SOURCE_ADDR;
    u32 bytes_left = BITSTREAM_SIZE;
    int stall_counter = 0;

    // 2. CICLO DI INVIO OTTIMIZZATO
    while (bytes_left > 0) {
        xemacif_input(echo_netif);
        if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }
        if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; }

        u16 available_space = tcp_sndbuf(client_pcb);

        if (available_space > 0) {
            stall_counter = 0; // Reset stallo

            // Usiamo 1460 (1 MSS standard) invece di 2048 per evitare frammentazione
            u16 chunk = (available_space > 1460) ? 1460 : available_space;
            if (chunk > bytes_left) chunk = bytes_left;

            err_t err = tcp_write(client_pcb, ptr, chunk, TCP_WRITE_FLAG_COPY);
            if (err == ERR_OK) {
                tcp_output(client_pcb); // Forza l'uscita del pacchetto
                ptr += chunk;
                bytes_left -= chunk;

                // Stampa progresso ogni 100KB per debug
                if (bytes_left % 102400 < 1460) {
                    xil_printf("Rimanenti: %d bytes...\r\n", bytes_left);
                }
            }
        } else {
            // Buffer pieno: aspettiamo gli ACK dallo Slave
            stall_counter++;
            if (stall_counter > 500000) { // Timeout di sicurezza per stallo
                xil_printf("ERRORE: Lo Slave non risponde (Stallo TCP).\r\n");
                break;
            }
        }
    }

    xil_printf(">>> TRASFERIMENTO TCP COMPLETATO! <<<\r\n");
    tcp_close(client_pcb);
    sys_state = SYS_IDLE;
    transfer_going_on = 0;
}
