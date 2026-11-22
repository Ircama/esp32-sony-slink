/* Philips Hue "bridge" for voice assistants and apps that speak the Hue v1 API (Alexa with an Echo that has the Hue support, Hue apps ...).
 * The amplifier shows up as ONE dimmable light:
 *     on / off    -> power on / power off (PWRON / PWROFF)
 *     brightness  -> volume: 100 % of the light is the "maximum volume" setting (HUE MAX, default 30 % of the knob), never more
 * Parts (the same ones the open-source ESP "Alexa" libraries have):
 *     - SSDP: answers the M-SEARCH of the assistant (UDP 1900, multicast 239.255.255.250) with the address of /description.xml
 *     - HTTP, on the same web server (port 80): /description.xml, POST /api (the "link button" is always pressed), GET /api/<user>[/lights[/1]],
 *       PUT /api/<user>/lights/1/state
 * Off by default (Settings, or HUE ON). There is NO authentication in the Hue protocol of this kind: anyone on the local network can control the light, so the volume is capped. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "esp_http_server.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "cli.h"
#include "state.h"

#define USER "slinkhue0000000000000000000000000000000000"    /* the name the "link button" gives to everybody */

static volatile bool s_run, s_alive;

static void ids(char *mac12, size_t n12, char *bridge, size_t nb, char *uuid, size_t nu, char *uniq, size_t nq, char *macc, size_t nm)
{
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac12, n12, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(bridge, nb, "%02X%02X%02XFFFE%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(uuid, nu, "2f402f80-da50-11e1-9b23-%s", mac12);
    snprintf(uniq, nq, "%02x:%02x:%02x:%02x:%02x:%02x:00:11-0b", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(macc, nm, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void ip_str(char *o, size_t n)
{
    uint32_t ip = net_ip4();
    snprintf(o, n, "%u.%u.%u.%u", (unsigned)(ip & 255), (unsigned)((ip >> 8) & 255), (unsigned)((ip >> 16) & 255), (unsigned)(ip >> 24));
}

/* ---- the light: on = amplifier on, bri = volume scaled to the maximum ---- */
static int hue_max(void) { return g_set.hue_max >= 1 && g_set.hue_max <= 100 ? g_set.hue_max : 30; }

static int bri_now(void)
{
    int v = cli_vol_x10();                                   /* tenths of a percent of the knob travel, -1 unknown */
    if (v < 0) return 1;
    int b = (int)((double)v / 10.0 / hue_max() * 254.0 + 0.5);
    return b < 1 ? 1 : b > 254 ? 254 : b;
}

static size_t light_json(char *b, size_t sz)
{
    char mac12[16], bridge[24], uuid[48], uniq[40], macc[24];
    ids(mac12, sizeof mac12, bridge, sizeof bridge, uuid, sizeof uuid, uniq, sizeof uniq, macc, sizeof macc);
    const char *name = g_set.hue_name[0] ? g_set.hue_name : "Amplificatore";
    int n = snprintf(b, sz,
        "{\"state\":{\"on\":%s,\"bri\":%d,\"alert\":\"none\",\"mode\":\"homeautomation\",\"reachable\":true},"
        "\"swupdate\":{\"state\":\"noupdates\",\"lastinstall\":null},\"type\":\"Dimmable light\",\"name\":\"%s\",\"modelid\":\"LWB010\","
        "\"manufacturername\":\"Philips\",\"productname\":\"Hue white lamp\",\"capabilities\":{\"certified\":false,\"control\":{\"mindimlevel\":5000,\"maxlumen\":806}},"
        "\"config\":{\"archetype\":\"classicbulb\",\"function\":\"functional\",\"direction\":\"omnidirectional\"},\"uniqueid\":\"%s\",\"swversion\":\"1.46.13_r26312\"}",
        state_power() == 1 ? "true" : "false", bri_now(), name, uniq);
    return n > 0 && (size_t)n < sz ? (size_t)n : 0;
}

static size_t config_json(char *b, size_t sz)
{
    char mac12[16], bridge[24], uuid[48], uniq[40], macc[24], ip[20];
    ids(mac12, sizeof mac12, bridge, sizeof bridge, uuid, sizeof uuid, uniq, sizeof uniq, macc, sizeof macc);
    ip_str(ip, sizeof ip);
    int n = snprintf(b, sz,
        "{\"name\":\"S-Link bridge\",\"datastoreversion\":\"126\",\"swversion\":\"1946053000\",\"apiversion\":\"1.46.0\",\"mac\":\"%s\",\"bridgeid\":\"%s\",\"factorynew\":false,"
        "\"replacesbridgeid\":null,\"modelid\":\"BSB002\",\"starterkitid\":\"\",\"ipaddress\":\"%s\",\"netmask\":\"255.255.255.0\",\"gateway\":\"%s\",\"dhcp\":true,\"linkbutton\":true,"
        "\"portalservices\":false,\"UTC\":\"2026-01-01T00:00:00\",\"localtime\":\"2026-01-01T00:00:00\",\"timezone\":\"Europe/Rome\",\"whitelist\":{\"" USER "\":{\"name\":\"alexa\"}}}",
        macc, bridge, ip, ip);
    return n > 0 && (size_t)n < sz ? (size_t)n : 0;
}

/* ---- HTTP ---- */
static esp_err_t send_json(httpd_req_t *r, const char *s, size_t n)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, s, (ssize_t)n);
}

static esp_err_t off(httpd_req_t *r) { return httpd_resp_send_404(r); }

static esp_err_t h_desc(httpd_req_t *r)
{
    if (!g_set.hue) return off(r);
    char mac12[16], bridge[24], uuid[48], uniq[40], macc[24], ip[20];
    ids(mac12, sizeof mac12, bridge, sizeof bridge, uuid, sizeof uuid, uniq, sizeof uniq, macc, sizeof macc);
    ip_str(ip, sizeof ip);
    char b[1100];
    int n = snprintf(b, sizeof b,
        "<?xml version=\"1.0\" ?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><URLBase>http://%s:80/</URLBase>"
        "<device><deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType><friendlyName>S-Link bridge (%s)</friendlyName><manufacturer>Royal Philips Electronics</manufacturer>"
        "<manufacturerURL>http://www.philips.com</manufacturerURL><modelDescription>Philips hue Personal Wireless Lighting</modelDescription><modelName>Philips hue bridge 2012</modelName>"
        "<modelNumber>929000226503</modelNumber><modelURL>http://www.meethue.com</modelURL><serialNumber>%s</serialNumber><UDN>uuid:%s</UDN><presentationURL>index.html</presentationURL></device></root>",
        ip, ip, mac12, uuid);
    httpd_resp_set_type(r, "text/xml");
    return httpd_resp_send(r, b, n);
}

static esp_err_t h_create(httpd_req_t *r)                     /* POST /api: the app asks for a user name (the link button is "pressed") */
{
    if (!g_set.hue) return off(r);
    char junk[160]; int left = (int)r->content_len;
    while (left > 0) { int n = httpd_req_recv(r, junk, left < (int)sizeof junk ? left : (int)sizeof junk); if (n <= 0) break; left -= n; }
    const char *s = "[{\"success\":{\"username\":\"" USER "\"}}]";
    return send_json(r, s, strlen(s));
}

static int split(const char *uri, char seg[6][48])            /* "/api/u/lights/1/state?x" -> segments after "/api" */
{
    int n = 0; const char *p = uri;
    if (strncmp(p, "/api", 4)) return 0;
    p += 4;
    while (*p && *p != '?' && n < 6) {
        while (*p == '/') p++;
        if (!*p || *p == '?') break;
        size_t k = 0;
        while (p[k] && p[k] != '/' && p[k] != '?') k++;
        size_t c = k < 47 ? k : 47;
        memcpy(seg[n], p, c); seg[n][c] = 0; n++;
        p += k;
    }
    return n;
}

static esp_err_t h_get(httpd_req_t *r)                         /* GET /api/... */
{
    if (!g_set.hue) return off(r);
    char seg[6][48], b[1500], l[900];
    int n = split(r->uri, seg);
    if (n == 1 && !strcmp(seg[0], "config")) return send_json(r, b, config_json(b, sizeof b));
    if (n == 2 && !strcmp(seg[1], "config")) return send_json(r, b, config_json(b, sizeof b));
    if (n == 1) {                                               /* the whole state of the "bridge" for a user */
        char c[900];
        size_t ln = light_json(l, sizeof l), cn = config_json(c, sizeof c);
        static char full[2400];
        int k = snprintf(full, sizeof full, "{\"lights\":{\"1\":%.*s},\"groups\":{},\"config\":%.*s,\"schedules\":{},\"scenes\":{},\"rules\":{},\"sensors\":{},\"resourcelinks\":{}}", (int)ln, l, (int)cn, c);
        return send_json(r, full, k > 0 && (size_t)k < sizeof full ? (size_t)k : 0);
    }
    if (n == 2 && !strcmp(seg[1], "lights")) {
        size_t ln = light_json(l, sizeof l);
        int k = snprintf(b, sizeof b, "{\"1\":%.*s}", (int)ln, l);
        return send_json(r, b, k > 0 && (size_t)k < sizeof b ? (size_t)k : 0);
    }
    if (n == 3 && !strcmp(seg[1], "lights") && !strcmp(seg[2], "1")) return send_json(r, l, light_json(l, sizeof l));
    if (n == 2) return send_json(r, "{}", 2);                   /* groups, scenes, rules, sensors ... : none */
    return send_json(r, "[{\"error\":{\"type\":3,\"address\":\"/\",\"description\":\"resource, not available\"}}]", 78);
}

static esp_err_t h_put(httpd_req_t *r)                         /* PUT /api/<user>/lights/1/state  {"on":true,"bri":127} */
{
    if (!g_set.hue) return off(r);
    char seg[6][48];
    int n = split(r->uri, seg);
    char body[200] = {0};
    int len = (int)r->content_len;
    if (len > (int)sizeof body - 1) len = (int)sizeof body - 1;
    int got = 0;
    while (got < len) { int k = httpd_req_recv(r, body + got, len - got); if (k <= 0) break; got += k; }
    body[got] = 0;
    if (!(n == 4 && !strcmp(seg[1], "lights") && !strcmp(seg[2], "1") && !strcmp(seg[3], "state")))
        return send_json(r, "[{\"error\":{\"type\":3,\"address\":\"/\",\"description\":\"resource, not available\"}}]", 78);
    char out[300]; int o = 0;
    o += snprintf(out + o, sizeof out - o, "[");
    const char *p = strstr(body, "\"on\"");
    int on = -1;
    if (p && (p = strchr(p, ':'))) { while (*++p == ' '); on = !strncmp(p, "true", 4) ? 1 : !strncmp(p, "false", 5) ? 0 : -1; }
    const char *q = strstr(body, "\"bri\"");
    int bri = -1;
    if (q && (q = strchr(q, ':'))) { while (*++q == ' '); if (isdigit((unsigned char)*q)) bri = atoi(q); }
    if (on == 1 && state_power() != 1) cli_submit_from(CLI_SRC_HUE, "PWRON");
    if (on == 0) cli_submit_from(CLI_SRC_HUE, "PWROFF");
    if (on >= 0) o += snprintf(out + o, sizeof out - o, "{\"success\":{\"/lights/1/state/on\":%s}}", on ? "true" : "false");
    if (bri >= 0 && on != 0) {
        if (bri < 1) bri = 1;
        if (bri > 254) bri = 254;
        char cmd[32];
        snprintf(cmd, sizeof cmd, "VOL SET %.1f", bri * (double)hue_max() / 254.0);          /* never above the maximum volume setting */
        cli_submit_from(CLI_SRC_HUE, cmd);
        o += snprintf(out + o, sizeof out - o, "%s{\"success\":{\"/lights/1/state/bri\":%d}}", on >= 0 ? "," : "", bri);
    }
    if (o == 1) o += snprintf(out + o, sizeof out - o, "{\"success\":{}}");
    snprintf(out + o, sizeof out - o, "]");
    return send_json(r, out, strlen(out));
}

void hue_register(httpd_handle_t srv)
{
    static const httpd_uri_t u[] = {
        { .uri = "/description.xml", .method = HTTP_GET,  .handler = h_desc },
        { .uri = "/api",             .method = HTTP_POST, .handler = h_create },
        { .uri = "/api/*",           .method = HTTP_GET,  .handler = h_get },     /* last: the exact /api/... URIs of the portal were registered first and win */
        { .uri = "/api/*",           .method = HTTP_PUT,  .handler = h_put },
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(srv, &u[i]);
}

/* ---- SSDP: "who is a Hue bridge?" ---- */
static void ssdp_task(void *arg)
{
    s_alive = true;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(1900), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            struct ip_mreq mq = { .imr_multiaddr.s_addr = inet_addr("239.255.255.250"), .imr_interface.s_addr = htonl(INADDR_ANY) };
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mq, sizeof mq);
            struct timeval tv = { .tv_sec = 1 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            char buf[600];
            while (s_run) {
                struct sockaddr_in from; socklen_t fl = sizeof from;
                int n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
                if (n <= 0 || !web_running()) continue;                 /* the Hue API lives on the web server */
                buf[n] = 0;
                if (strncasecmp(buf, "M-SEARCH", 8)) continue;
                const char *st = NULL;
                if (strcasestr(buf, "urn:schemas-upnp-org:device:basic:1")) st = "urn:schemas-upnp-org:device:basic:1";
                else if (strcasestr(buf, "upnp:rootdevice")) st = "upnp:rootdevice";
                else if (strcasestr(buf, "ssdp:all")) st = "upnp:rootdevice";
                if (!st) continue;
                char mac12[16], bridge[24], uuid[48], uniq[40], macc[24], ip[20], resp[520];
                ids(mac12, sizeof mac12, bridge, sizeof bridge, uuid, sizeof uuid, uniq, sizeof uniq, macc, sizeof macc);
                ip_str(ip, sizeof ip);
                int k = snprintf(resp, sizeof resp,
                    "HTTP/1.1 200 OK\r\nHOST: 239.255.255.250:1900\r\nEXT:\r\nCACHE-CONTROL: max-age=100\r\nLOCATION: http://%s:80/description.xml\r\n"
                    "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.46.0\r\nhue-bridgeid: %s\r\nST: %s\r\nUSN: uuid:%s::%s\r\n\r\n", ip, bridge, st, uuid, st);
                if (k > 0 && k < (int)sizeof resp) sendto(fd, resp, (size_t)k, 0, (struct sockaddr *)&from, fl);
            }
        }
        close(fd);
    }
    s_alive = false;
    vTaskDelete(NULL);
}

void hue_start(void)
{
    if (s_run) return;
    for (int i = 0; i < 30 && s_alive; i++) vTaskDelay(pdMS_TO_TICKS(100));       /* a previous task is still leaving */
    s_run = true;
    if (xTaskCreate(ssdp_task, "slink_ssdp", 4096, NULL, 3, NULL) != pdPASS) s_run = false;
}

void hue_stop(void) { s_run = false; }
bool hue_running(void) { return s_run; }
