#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "slink.h"
#include "cli.h"
#include "net.h"
#include "settings.h"
#include "soc/soc_caps.h"
#if SOC_WIFI_SUPPORTED
#include "ota.h"
#endif

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
#if SOC_WIFI_SUPPORTED
    ota_confirm_start();    /* a firmware installed over the air must prove itself, or the bootloader goes back to the previous one */
#endif
    slink_set_pad(g_set.pad);   /* saved electrical mode of the bus pin (settings are loaded by now) */
}
