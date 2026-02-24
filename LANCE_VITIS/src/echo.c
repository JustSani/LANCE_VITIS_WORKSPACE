#include <stdio.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "xil_cache.h"

#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif

// Costanti di Memoria e Rete
#define RAM_SOURCE_ADDR  0x10000000
#define RAM_DEST_ADDR    0x11000000
#define BITSTREAM_SIZE   301028
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

extern struct netif *echo_netif; // Riferimento all'interfaccia di rete del main
extern int load_partial_bitstream(u32 address, u32 size);


/* =========================================================================
 * PARTE 1: TCP SERVER (RICEZIONE)
 * ========================================================================= */

// 1.C Callback chiamata quando arrivano dati
static err_t server_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        // Se p è NULL, significa che il Client ha chiuso la connessione
        tcp_close(tpcb);
        sys_state = SYS_IDLE;
        transfer_going_on = 0;
        return ERR_OK;
    }

    /* Informiamo il TCP che abbiamo elaborato i dati (invia l'ACK al mittente)
     * Questo è FONDAMENTALE in TCP per far avanzare la finestra di scorrimento */
    tcp_recved(tpcb, p->tot_len);

    // Copia i dati dal buffer alla RAM
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        if (total_bytes_received + q->len <= BITSTREAM_SIZE) {
            memcpy(current_write_ptr, q->payload, q->len);
            current_write_ptr += q->len;
            total_bytes_received += q->len;
        }
    }

    // Libera la memoria di LwIP
    pbuf_free(p);

    // Controllo fine trasferimento
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

        // Reset per la prossima connessione
        sys_state = SYS_IDLE;
        transfer_going_on = 0;
        tcp_close(tpcb);
    }

    return ERR_OK;
}

// 1.B Callback chiamata quando un Client si connette a noi
static err_t server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    xil_printf("\r\n--- Connessione TCP in ingresso accettata! ---\r\n");

    // Inizializza i contatori per il nuovo file
    sys_state = SYS_RX_ACTIVE;
    transfer_going_on = 1;
    total_bytes_received = 0;
    current_write_ptr = (u8 *)RAM_DEST_ADDR;

    // Registra la funzione che leggerà i dati
    tcp_recv(newpcb, server_recv_cb);
    return ERR_OK;
}

// 1.A Avvio del Server (Chiamata dal main)
int start_application() {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        xil_printf("Errore creazione TCP PCB.\r\n");
        return -1;
    }

    tcp_bind(pcb, IP_ADDR_ANY, TCP_PORT);
    pcb = tcp_listen(pcb);           // Mette il socket in ascolto
    tcp_accept(pcb, server_accept_cb); // Cosa fare quando qualcuno bussa

    xil_printf("TCP Server inizializzato e in ascolto sulla porta %d\r\n", TCP_PORT);
    return 0;
}


/* =========================================================================
 * PARTE 2: TCP CLIENT (INVIO)
 * ========================================================================= */

// Callback chiamata quando il tentativo di connessione ha successo
static err_t client_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    if (err == ERR_OK) {
        client_connected = 1;
    }
    return ERR_OK;
}

void send_bitstream_from_ram_tcp(ip_addr_t *dest_ip) {
    if (sys_state != SYS_IDLE) {
        xil_printf("Errore: Sistema occupato!\r\n");
        return;
    }

    sys_state = SYS_TX_ACTIVE;
    transfer_going_on = 1;
    client_connected = 0;

    client_pcb = tcp_new();
    xil_printf("Tentativo di connessione a %d.%d.%d.%d...\r\n",
            ip4_addr1(dest_ip), ip4_addr2(dest_ip), ip4_addr3(dest_ip), ip4_addr4(dest_ip));

    // Richiesta di connessione asincrona
    tcp_connect(client_pcb, dest_ip, TCP_PORT, client_connected_cb);

    // Attesa bloccante (con timeout) affinché la connessione si stabilisca
    extern volatile int TcpFastTmrFlag;
    extern volatile int TcpSlowTmrFlag;
    extern void xemacif_input(struct netif *netif);

    int timeout = 0;
    while (!client_connected && timeout < 20) {
        xemacif_input(echo_netif); // Fa girare lo stack di rete
        if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }
        if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; timeout++; }
    }

    if (!client_connected) {
        xil_printf("Errore: Timeout connessione TCP (scheda non trovata o spenta).\r\n");
        tcp_close(client_pcb);
        sys_state = SYS_IDLE;
        transfer_going_on = 0;
        return;
    }

    xil_printf("Connesso! Inizio invio...\r\n");
    Xil_DCacheFlushRange(RAM_SOURCE_ADDR, BITSTREAM_SIZE);

    u8 *ptr = (u8 *)RAM_SOURCE_ADDR;
    u32 bytes_left = BITSTREAM_SIZE;
    err_t err;

    /* LOOP DI INVIO TCP */
    while (bytes_left > 0) {
        // Calcola quanto spazio c'è nel buffer TCP in questo istante
        u16 send_len = tcp_sndbuf(client_pcb);

        // Se il buffer di invio è pieno (il ricevitore è lento), aspettiamo gli ACK
        if (send_len == 0) {
            xemacif_input(echo_netif);
            if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }
            if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; }
            continue;
        }

        // Limitiamo la scrittura a 2048 byte alla volta per non saturare la heap
        if (send_len > 2048) send_len = 2048;
        if (send_len > bytes_left) send_len = bytes_left;

        // Scriviamo nel buffer TCP
        err = tcp_write(client_pcb, ptr, send_len, TCP_WRITE_FLAG_COPY);

        if (err == ERR_OK) {
            tcp_output(client_pcb); // Forza l'invio fisico dei pacchetti
            ptr += send_len;
            bytes_left -= send_len;
        } else if (err == ERR_MEM) {
            // Memoria LwIP temporaneamente piena, facciamo girare la rete e riproviamo
            xemacif_input(echo_netif);
        } else {
            xil_printf("Errore TCP Fatale durante l'invio: %d\r\n", err);
            break;
        }

        // FONDAMENTALE: Manteniamo in vita la ricezione mentre inviamo,
        // altrimenti non leggiamo gli ACK in ingresso e il trasferimento si blocca.
        xemacif_input(echo_netif);
    }

    xil_printf(">>> TRASFERIMENTO TCP COMPLETATO! <<<\r\n");
    tcp_close(client_pcb);
    sys_state = SYS_IDLE;
    transfer_going_on = 0;
}
