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
    char title_page[33];    /* web interface: page title (tabs, bookmarks) */
} settings_t;

extern settings_t g_set;

void settings_load(void);                 /* fills g_set from NVS (defaults if absent); NVS must be initialised */
void settings_save(void);                 /* writes g_set to NVS */
