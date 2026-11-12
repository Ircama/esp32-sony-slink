#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "slink.h"
#include "cli.h"
#include "net.h"
#include "settings.h"

void app_main(void)
{
    cli_start();            /* USB console first: banner shows even if S-Link init fails */
    int rc = slink_init();
    if (rc != 0) {
        cli_report_error("ERR slink_init failed");
        return;
    }
    cli_start_rx();
    net_init();             /* BLE / WiFi / web / app interface, as saved in the settings */
    slink_set_pad(g_set.pad);   /* saved electrical mode of the bus pin (settings are loaded by now) */
}
