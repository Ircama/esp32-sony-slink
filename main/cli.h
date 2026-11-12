#pragma once
#include <stddef.h>
#include <stdbool.h>

/* Text protocol (see README). One command core, many transports:
 * USB Serial/JTAG, BLE Nordic UART, raw TCP, WebSocket (web portal), MQTT.
 * Every transport submits lines with cli_submit() and registers a sink to receive all output. */
void cli_start(void);
void cli_start_rx(void);   /* call after slink_init() */
void cli_report_error(const char *msg);

/* Output sinks: called (under the output lock) with every chunk the CLI prints. Must NOT block. */
typedef void (*cli_sink_t)(const char *data, size_t len);
int  cli_sink_add(cli_sink_t fn);       /* 0 ok, -1 full */
void cli_sink_remove(cli_sink_t fn);

/* Queue a command line for the (single) worker task. false = queue full or line too long. */
bool cli_submit(const char *line);

/* Data for the web/MQTT layers */
int    cli_vol_pct(void);                              /* estimated volume 0-100, -1 unknown */
int    cli_vol_x10(void);                              /* same in tenths of a percent (0-1000), -1 unknown: ~2.4 per motor step */
size_t cli_stations_json(char *buf, size_t sz);        /* {"t":"stations","stations":[{"f":9850,"name":".."}]} */
size_t cli_favs_json(char *buf, size_t sz);            /* {"t":"favs","favs":[{"f":9850,"am":0,"name":".."}]}: the presets stored by the bridge */
