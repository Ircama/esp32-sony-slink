#pragma once
#include <stddef.h>
#include <stdbool.h>

/* Network layer: WiFi (STA only), mDNS and the optional services, all driven by the persistent settings
 * (g_set.ble / g_set.web / g_set.app, WiFi and MQTT credentials). */

void net_init(void);                        /* after cli_start(): applies the saved settings */
void net_apply(void);                       /* re-read g_set and start/stop BLE, WiFi and services accordingly */
void net_status(char *buf, size_t n);       /* multi-line human readable status (CRLF separated) */
bool net_have_ip(void);
void net_notify_state(void);                /* state changed: wake the publisher (any task) */
void net_notify_ws_client(void);            /* a WebSocket client connected: send it everything */

/* WiFi 6 Target Wake Time (individual TWT). Only chips with WiFi 6 (ESP32-C6/C5/C61) can use it; on ESP32-C3/S3 the
 * state is always NET_TWT_NO_CHIP and none of the TWT code is compiled in. */
enum { NET_TWT_NO_CHIP = 0, NET_TWT_OFF, NET_TWT_NO_AP, NET_TWT_PENDING, NET_TWT_ACTIVE, NET_TWT_REFUSED };
int net_twt_state(unsigned *interval_ms);   /* NET_TWT_*; interval_ms = agreed wake interval when ACTIVE (else 0) */

/* services (idempotent start/stop; called by net.c) */
void web_start(void);
void web_stop(void);
bool web_running(void);
void web_push(const char *json, size_t n);  /* queue a JSON text frame for every WebSocket client (non blocking) */
int  web_clients(void);                     /* WebSocket clients connected now */
size_t stats_json(char *buf, size_t sz);    /* {"t":"stats",...}: device statistics, read on request (stats.c) */

void tcp_srv_start(void);
void tcp_srv_stop(void);
int  tcp_srv_clients(void);

void mqtt_bridge_start(void);
void mqtt_bridge_stop(void);
bool mqtt_bridge_running(void);
bool mqtt_bridge_connected(void);
void mqtt_bridge_push(const char *json, size_t n);

void ble_nus_start(void);
void ble_nus_stop(void);
bool ble_nus_running(void);
bool ble_nus_connected(void);
const char *ble_nus_name(void);
