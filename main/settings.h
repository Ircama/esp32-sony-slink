#pragma once
#include <stdbool.h>

/* Persistent settings (NVS namespace "slink"). Everything here survives reboots. */
typedef struct {
    bool ble;               /* UART over BLE GATT (Nordic UART Service) */
    bool web;               /* web portal (HTTP + WebSocket) */
    bool app;               /* WiFi application interface: raw TCP console + MQTT bridge (Home Assistant discovery) */
    bool eco;               /* low-power profile (default ON): WiFi max modem sleep, CPU 40-80 MHz, reduced TX power */
    char ssid[33];
    char pass[65];
    char host[32];          /* hostname / mDNS name */
    char mqtt_uri[96];      /* mqtt://host[:port] or mqtts://... */
    char mqtt_user[32];
    char mqtt_pass[64];
    unsigned char pad;      /* S-Link pin electrical mode (SLINK_PAD_*) */
    unsigned short vol_up;  /* motorised volume knob: UP steps from minimum to maximum (default 420) */
    unsigned short vol_down;/* DOWN steps from maximum to minimum (default 445) */
    unsigned short vol_margin; /* extra steps when going to an end (default 10) */
    unsigned char txp;      /* WiFi TX power in dBm (2-20), 0 = automatic (11 dBm in eco, 20 otherwise) */
    unsigned char wps;      /* WiFi power save: 0 auto (eco: max modem, else min modem), 1 none, 2 min modem, 3 max modem */
    unsigned char lis;      /* WiFi listen interval in beacons (1-10), 0 = automatic (3 in eco, DTIM otherwise) */
    char title_app[33];     /* web interface: header title (empty = amplifier name) */
    char title_pwa[33];     /* name of the installed app (PWA manifest), free text (UITITLE NAME "text") */
    uint8_t pwa_src;        /* what names the installed app: 0 header title, 1 page title, 2 the free text above (UITITLE PWA APP|PAGE|NAME) */
    char title_page[33];    /* web interface: page title (tabs, bookmarks) */
    char ota_url[128];      /* firmware update: custom URL of the .bin (empty = latest GitHub release) */
    char ota_token[64];     /* firmware update: optional Bearer token sent to the download server (empty = none) */
    bool ota_push;          /* firmware update: accept images sent from outside (POST /api/ota). OFF by default: nothing is installed unattended */
    bool ota_pull;          /* firmware update: allow the device to download an image, only when the user asks (OTA UPDATE / portal button). ON by default */
    char ota_pass[33];      /* firmware update: optional password for the push upload (POST /api/ota); empty = no password */
    bool slinke;            /* Slink-e serial protocol on TCP port 2325 (programs written for the Nirvis Slink-e). OFF by default */
    bool https;             /* HTTPS web portal on port 443 with a certificate from the device's own CA (tls.c). OFF by default */
    bool slinke_tx;         /* ... and they may send to the bus (otherwise the connection is a read-only monitor). OFF by default */
} settings_t;

extern settings_t g_set;
#define NET_WANTED (g_set.web || g_set.app || g_set.slinke)   /* something that needs the WiFi is on */

void settings_load(void);                 /* fills g_set from NVS (defaults if absent); NVS must be initialised */
void settings_save(void);                 /* writes g_set to NVS */
