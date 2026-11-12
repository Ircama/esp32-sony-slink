/* Raw TCP console (port 2323): the same text protocol as the USB serial port.  Try:  nc slink.local 2323 */
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "cli.h"

#define PORT 2323
#define MAX_CLIENTS 2

static volatile bool s_run;
static volatile bool s_task_alive;
static int s_fd[MAX_CLIENTS] = { -1, -1 };

int tcp_srv_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fd[i] >= 0) n++;
    return n;
}

/* never blocks: if a client cannot keep up, its data is dropped */
static void tcp_sink(const char *d, size_t n)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = s_fd[i];
        if (fd >= 0) send(fd, d, n, MSG_DONTWAIT);
    }
}

static void drop(int i)
{
    if (s_fd[i] >= 0) { close(s_fd[i]); s_fd[i] = -1; }
}

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
    char line[2][200];
    size_t ll[2] = {0, 0};
    while (s_run) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        int mx = ls;
        for (int i = 0; i < MAX_CLIENTS; i++) if (s_fd[i] >= 0) { FD_SET(s_fd[i], &rs); if (s_fd[i] > mx) mx = s_fd[i]; }
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };      /* wake-ups cost power: only needed to notice s_run going false */
        int r = select(mx + 1, &rs, NULL, NULL, &tv);
        if (r <= 0) continue;
        if (FD_ISSET(ls, &rs)) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) if (s_fd[i] < 0) slot = i;
                if (slot < 0) { send(c, "ERR too many clients\r\n", 22, 0); close(c); }
                else {
                    int nd = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof nd);
                    ll[slot] = 0;
                    s_fd[slot] = c;
                    send(c, "S-Link bridge ready. Type HELP.\r\n", 33, 0);
                }
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_fd[i] < 0 || !FD_ISSET(s_fd[i], &rs)) continue;
            char buf[64];
            int n = recv(s_fd[i], buf, sizeof buf, 0);
            if (n <= 0) { drop(i); continue; }
            for (int k = 0; k < n; k++) {
                char ch = buf[k];
                if (ch == '\r' || ch == '\n') {
                    if (ll[i]) { line[i][ll[i]] = 0; cli_submit(line[i]); ll[i] = 0; }
                } else if (ch == 0x08 || ch == 0x7F) { if (ll[i]) ll[i]--; }
                else if ((unsigned char)ch >= 32 && ll[i] < sizeof line[i] - 1) line[i][ll[i]++] = ch;
            }
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) drop(i);
    close(ls);
    s_task_alive = false;
    vTaskDelete(NULL);
}

void tcp_srv_start(void)
{
    if (s_run) return;
    while (s_task_alive) vTaskDelay(pdMS_TO_TICKS(50));      /* previous instance still winding down */
    s_run = true;
    cli_sink_add(tcp_sink);
    xTaskCreate(srv_task, "slink_tcp", 4096, NULL, 4, NULL);
}

void tcp_srv_stop(void)
{
    if (!s_run) return;
    s_run = false;
    cli_sink_remove(tcp_sink);
}
