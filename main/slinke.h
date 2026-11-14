#pragma once
#include <stdbool.h>

/* Slink-e serial protocol over TCP (port 2325), see slinke.c. Started by net_apply() when the setting "slinke" is on and the WiFi is up. */
void slinke_start(void);
void slinke_stop(void);
int  slinke_clients(void);
