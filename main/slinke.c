/* Slink-e serial protocol over TCP (port 2325).
 *
 * The Slink-e (Nirvis Systems) is a PC <-> S-Link interface with a binary serial protocol (documented at woudy.org/slinke).  Programs written for it
 * (SlinkX, Girder / MainLobby plug-ins, the Linux "slinke" tools ...) can use this device instead: open a TCP connection to port 2325 instead of a COM port
 * (a "COM port redirector" or a "serial over network" driver does that for Windows programs).
 *
 * Implemented: S-Link port 0 = the bus of this device (send = "port send" ... "port send end", receive = "port receive" + "port receive end"),
 * enable/disable of the reception, version, serial number, save/load defaults (accepted, nothing to save), resume, reset, the serial-port commands
 * (baud: accepted, the TCP link has no baud rate; "send to serial port" echoes the bytes back, as the Slink-e does).
 * Not implemented: the IR, parallel and extra S-Link ports (4 S-Link ports exist on the Slink-e, this device has one bus: the others are ignored) and the
 * S-Link bit mode (a number of bits that is not a multiple of 8: the hardware here sends whole bytes).  A command that does not exist gives the
 * "illegal command" response and the resume state, as in the original (everything is ignored until FF AA).
 *
 * Off by default (setting "slinke"); sending to the bus needs the second setting ("slinke_tx"), otherwise the connection is a read-only monitor. */
#include "oplog.h"
#include "cli.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "net.h"
#include "cli.h"
#include "slink.h"
#include "settings.h"
#include "slinke.h"
#include "decode.h"

#define PORT 2325
#define MAX_CLIENTS 2
#define MSG_TIMEOUT_MS 2000           /* a message must be completed (port send end) within this time of its last byte */
#define VERSION_BCD 0x20              /* reported firmware version 2.0 */

enum { ST_IDLE, ST_DATA, ST_CMD, ST_ARG };

typedef struct {
    int fd;
    uint8_t st, port, left, cmd;
    uint8_t arg[2], na;
    uint8_t txb[SLINK_MAX_FRAME]; uint8_t txn; bool txover;
    bool resume;                      /* critical error: everything is ignored until FF AA */
    uint8_t prev;
    bool rx_en[4]; bool all_dis;      /* reception enabled per S-Link port / disabled by the system command */
    uint8_t baud;
    int64_t last_ms;                  /* time of the last byte of a message in progress */
} cl_t;

static volatile bool s_run, s_task_alive;
static cl_t s_c[MAX_CLIENTS];

int slinke_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_c[i].fd > 0) n++;
    return n;
}

static void reset_cl(cl_t *c, int fd)
{
    memset(c, 0, sizeof *c);
    c->fd = fd;
    for (int i = 0; i < 4; i++) c->rx_en[i] = true;
    c->baud = 4;
}

static void put(cl_t *c, const void *d, size_t n) { send(c->fd, d, n, 0); }
static void resp(cl_t *c, uint8_t port, uint8_t code, const uint8_t *data, size_t n)
{
    uint8_t b[12]; size_t k = 0;
    b[k++] = port == 7 ? 0xFF : (uint8_t)((port << 5) | 0x1F);
    b[k++] = code;
    for (size_t i = 0; i < n && k < sizeof b; i++) b[k++] = data[i];
    put(c, b, k);
}
static void illegal(cl_t *c, uint8_t port) { resp(c, port, 0xFF, NULL, 0); c->resume = true; }

static void flush_tx(cl_t *c)
{
    if (c->port == 0 && (c->txn || c->txover)) {
        if (c->txover) { resp(c, 0, 0x81, NULL, 0); c->resume = true; }          /* longer than a bus frame: transmission timeout (critical) */
        else if (!g_set.slinke_tx) { /* sending not allowed: read-only monitor, the message is dropped */ }
        else {
            oplog_origin(CLI_SRC_SLINKE);                         /* what the amplifier does in the next seconds comes from this program (operations log) */
            int rc = slink_send(c->txb, c->txn, 1);
            if (rc == 0) {
                char line[8 + 3 * SLINK_MAX_FRAME]; int k = snprintf(line, sizeof line, "TX");
                for (int i = 0; i < c->txn; i++) k += snprintf(line + k, sizeof line - k, " %02X", c->txb[i]);
                char dec[120] = "";
                if (cli_decode_on() && c->txn >= 2) slink_decode(c->txb, c->txn, dec, sizeof dec);
                cli_out("%s   ; Slink-e%s%s\r\n", line, dec[0] ? ": " : "", dec);
            } else { resp(c, 0, 0x81, NULL, 0); c->resume = true; }              /* line never idle / line test active */
        }
    }
    c->txn = 0; c->txover = false;
}

static int nargs(uint8_t port, uint8_t cmd)
{
    if (cmd == 0x04 && port <= 3) return 2;        /* S-Link bit mode: number of bits, data byte */
    if (cmd == 0x08 && port == 6) return 1;        /* set baud rate */
    return 0;
}

static void exec_cmd(cl_t *c)
{
    uint8_t p = c->port, k = c->cmd;
    if (k == 0x02 || k == 0x03) {                  /* disable / enable reception: all ports */
        bool en = k == 0x03;
        if (p == 7) c->all_dis = !en; else if (p <= 3) c->rx_en[p] = en;
        resp(c, p, k, NULL, 0);
        return;
    }
    if (k == 0x04 && p <= 3) return;               /* bit mode: not supported by the hardware, ignored (the following port send end sends the whole bytes) */
    if (p == 7) {
        switch (k) {
        case 0x0B: { uint8_t v = VERSION_BCD; resp(c, 7, 0x0B, &v, 1); return; }
        case 0x0C: { uint8_t sn[8] = {0, 0}; esp_read_mac(sn + 2, ESP_MAC_WIFI_STA); resp(c, 7, 0x0C, sn, 8); return; }
        case 0x0E: resp(c, 7, 0x0E, NULL, 0); return;       /* save defaults: nothing to save, confirmed */
        case 0x0F: resp(c, 7, 0x0F, NULL, 0); return;       /* load defaults */
        case 0xAA: return;                                  /* resume: nothing to resume */
        case 0xFF: { int fd = c->fd; reset_cl(c, fd); return; }   /* warm boot */
        }
    } else if (p == 6) {
        if (k == 0x08) { if (c->arg[0] > 4) { resp(c, 6, 0x84, NULL, 0); return; } c->baud = c->arg[0]; resp(c, 6, 0x08, &c->baud, 1); return; }
        if (k == 0x09) { resp(c, 6, 0x08, &c->baud, 1); return; }
    }
    illegal(c, p);
}

static void feed(cl_t *c, uint8_t b)
{
    uint8_t prev = c->prev; c->prev = b;
    if (c->resume) { if (prev == 0xFF && b == 0xAA) c->resume = false; return; }
    c->last_ms = esp_timer_get_time() / 1000;
    switch (c->st) {
    case ST_IDLE: {
        uint8_t l = b & 31; c->port = b >> 5;
        if (l == 0) flush_tx(c);                   /* port send end */
        else if (l == 31) c->st = ST_CMD;          /* port modify / system command */
        else { c->left = l; c->st = ST_DATA; }     /* port send */
        break; }
    case ST_DATA:
        if (c->port == 0) { if (c->txn < SLINK_MAX_FRAME) c->txb[c->txn++] = b; else c->txover = true; }
        else if (c->port == 6) put(c, &b, 1);      /* "send to the serial port": echoed back */
        if (--c->left == 0) c->st = ST_IDLE;       /* the message goes on with the next port send, or ends with the port send end */
        break;
    case ST_CMD: {
        c->cmd = b; c->na = 0;
        int n = nargs(c->port, b);
        if (n) { c->left = (uint8_t)n; c->st = ST_ARG; } else { c->st = ST_IDLE; exec_cmd(c); }
        break; }
    case ST_ARG:
        if (c->na < sizeof c->arg) c->arg[c->na++] = b;
        if (--c->left == 0) { c->st = ST_IDLE; exec_cmd(c); }
        break;
    }
}

/* a frame seen on the bus (called by the console's receive task): port receive + port receive end, or the receive error */
static void on_frame(const slink_frame_t *f)
{
    if (f->len == 0 && !f->partial_bits && !f->error) return;
    uint8_t b[2 + SLINK_MAX_FRAME]; size_t n = 0;
    if (f->error || f->partial_bits) { b[n++] = 0x1F; b[n++] = 0x80; }
    else { b[n++] = f->len; memcpy(b + n, f->data, f->len); n += f->len; b[n++] = 0x00; }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        cl_t *c = &s_c[i];
        if (c->fd > 0 && c->rx_en[0] && !c->all_dis) send(c->fd, b, n, MSG_DONTWAIT);
    }
}

static void drop(int i) { if (s_c[i].fd > 0) { close(s_c[i].fd); s_c[i].fd = -1; } }

static void srv_task(void *arg)
{
    s_task_alive = true;
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 2) != 0) {
        if (ls >= 0) close(ls);
        s_task_alive = false;
        vTaskDelete(NULL);
        return;
    }
    while (s_run) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        int mx = ls;
        for (int i = 0; i < MAX_CLIENTS; i++) if (s_c[i].fd > 0) { FD_SET(s_c[i].fd, &rs); if (s_c[i].fd > mx) mx = s_c[i].fd; }
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(mx + 1, &rs, NULL, NULL, &tv);
        int64_t now = esp_timer_get_time() / 1000;
        for (int i = 0; i < MAX_CLIENTS; i++) {        /* a message that never ended: transmission timeout */
            cl_t *c = &s_c[i];
            if (c->fd > 0 && !c->resume && (c->txn || c->st != ST_IDLE) && now - c->last_ms > MSG_TIMEOUT_MS) {
                c->txn = 0; c->txover = false; c->st = ST_IDLE;
                resp(c, c->port, 0x81, NULL, 0); c->resume = true;
            }
        }
        if (r <= 0) continue;
        if (FD_ISSET(ls, &rs)) {
            int fd = accept(ls, NULL, NULL);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) if (s_c[i].fd <= 0) slot = i;
                if (slot < 0) close(fd);                   /* binary protocol: no text to explain why */
                else {
                    int nd = 1; struct timeval st = { .tv_sec = 1 };
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof nd);
                    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st);
                    reset_cl(&s_c[slot], fd);
                }
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_c[i].fd <= 0 || !FD_ISSET(s_c[i].fd, &rs)) continue;
            uint8_t buf[64];
            int n = recv(s_c[i].fd, buf, sizeof buf, 0);
            if (n <= 0) { drop(i); continue; }
            for (int k = 0; k < n; k++) feed(&s_c[i], buf[k]);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) drop(i);
    close(ls);
    s_task_alive = false;
    vTaskDelete(NULL);
}

void slinke_start(void)
{
    if (s_run) return;
    while (s_task_alive) vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < MAX_CLIENTS; i++) s_c[i].fd = -1;
    s_run = true;
    cli_frame_hook_add(on_frame);
    xTaskCreate(srv_task, "slink_slke", 4096, NULL, 4, NULL);
}

void slinke_stop(void)
{
    if (!s_run) return;
    s_run = false;
    cli_frame_hook_remove(on_frame);
}
