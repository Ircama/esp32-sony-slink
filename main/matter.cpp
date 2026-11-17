/* Matter, with Espressif's esp-matter (only in the build that has it: SLINK_MATTER, see docs/MATTER.md).
 * ONE endpoint, a "dimmable light":   OnOff -> PWRON / PWROFF      LevelControl (1-254) -> VOL SET, scaled to MATTER MAX % of the knob, never more.
 * The same two things the Philips Hue emulation does (hue.c), for the ecosystems that speak Matter (Google Home, Apple Home, SmartThings, Home Assistant ...).
 *
 * Pairing: a random setup passcode and discriminator (made at the first start, kept in the settings) instead of the well-known test code 20202021.
 * The device identifies itself with the test vendor id 0xFFF1 (not certified): Google Home / Alexa show a warning when pairing.
 * The commands (MATTER ...) and the portal show the manual code and the QR payload. */
#include <string.h>
#include <string>
#include <stdio.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_providers.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/PlatformManager.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConnectivityManager.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Span.h>

extern "C" {
#include "settings.h"
#include "cli.h"
#include "state.h"
}
#include "matter.h"
#include "svcmem.h"

static const char *TAG = "slink_matter";

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static volatile bool s_running;
static volatile int s_fabrics;
static uint16_t s_ep;
static TaskHandle_t s_report_task;
static char s_qr[64], s_manual[24];
static int s_last_on = -1, s_last_level = -1;
static volatile int s_vol_pending = -1;                         /* level asked by a controller, waiting to be sent to the amplifier (debounce) */
static volatile int64_t s_hold_until_us;                        /* until then the controllers are not told the position of the knob: it is still moving to what they asked */
static TaskHandle_t s_vol_task;

/* ------------------------------------------------------------------ setup code (random, kept in the settings) */
static bool pin_ok(uint32_t p)
{
    if (p < 1 || p > 99999998) return false;
    static const uint32_t bad[] = {11111111, 22222222, 33333333, 44444444, 55555555, 66666666, 77777777, 88888888, 12345678, 87654321};
    for (uint32_t b : bad) if (p == b) return false;
    return true;
}

static void ensure_codes(void)
{
    bool changed = false;
    if (!pin_ok(g_set.matter_pass)) { uint32_t p; do p = esp_random() % 100000000u; while (!pin_ok(p)); g_set.matter_pass = p; changed = true; }
    if (g_set.matter_disc == 0 || g_set.matter_disc > 0xFFF) { g_set.matter_disc = (uint16_t)(esp_random() & 0xFFF); if (!g_set.matter_disc) g_set.matter_disc = 0xA5A; changed = true; }
    if (changed) settings_save();
}

namespace {
class SlinkCommissionable : public chip::DeviceLayer::CommissionableDataProvider {
public:
    static constexpr uint32_t kIter = 1000;
    uint8_t salt[16];
    SlinkCommissionable() { esp_fill_random(salt, sizeof salt); }
    CHIP_ERROR GetSetupDiscriminator(uint16_t &d) override { d = g_set.matter_disc; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &c) override { c = kIter; return CHIP_NO_ERROR; }
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &s) override
    {
        if (s.size() < sizeof salt) return CHIP_ERROR_BUFFER_TOO_SMALL;
        memcpy(s.data(), salt, sizeof salt);
        s.reduce_size(sizeof salt);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &v, size_t &outLen) override
    {
        chip::Crypto::Spake2pVerifier ver;
        CHIP_ERROR e = ver.Generate(kIter, chip::ByteSpan(salt, sizeof salt), g_set.matter_pass);
        if (e != CHIP_NO_ERROR) return e;
        e = ver.Serialize(v);
        if (e == CHIP_NO_ERROR) outLen = v.size();
        return e;
    }
    CHIP_ERROR GetSetupPasscode(uint32_t &p) override { p = g_set.matter_pass; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};
SlinkCommissionable s_provider;
}

/* what Apple Home and the others show as manufacturer, model, serial number (Basic Information cluster) */
namespace {
class SlinkInfo : public chip::DeviceLayer::DeviceInstanceInfoProvider {
    static CHIP_ERROR cp(char *b, size_t n, const char *s) { if (strlen(s) >= n) return CHIP_ERROR_BUFFER_TOO_SMALL; strcpy(b, s); return CHIP_NO_ERROR; }
    static void mac(uint8_t m[6]) { esp_read_mac(m, ESP_MAC_WIFI_STA); }
public:
    CHIP_ERROR GetVendorName(char *b, size_t n) override { return cp(b, n, "Ircama"); }
    CHIP_ERROR GetVendorId(uint16_t &v) override { v = 0xFFF1; return CHIP_NO_ERROR; }                    /* the test vendor id: the device is not certified */
    CHIP_ERROR GetProductName(char *b, size_t n) override { return cp(b, n, "S-Link bridge"); }
    CHIP_ERROR GetProductId(uint16_t &p) override { p = 0x8000; return CHIP_NO_ERROR; }
    CHIP_ERROR GetPartNumber(char *b, size_t n) override { return cp(b, n, CONFIG_IDF_TARGET); }
    CHIP_ERROR GetProductURL(char *b, size_t n) override { return cp(b, n, "https://github.com/Ircama/esp32-sony-slink"); }
    CHIP_ERROR GetProductLabel(char *b, size_t n) override { return cp(b, n, "Sony S-Link / Control-A1 bridge"); }
    CHIP_ERROR GetSerialNumber(char *b, size_t n) override { uint8_t m[6]; mac(m); char t[16]; snprintf(t, sizeof t, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]); return cp(b, n, t); }
    CHIP_ERROR GetManufacturingDate(uint16_t &y, uint8_t &mo, uint8_t &d) override { y = 2026; mo = 11; d = 17; return CHIP_NO_ERROR; }
    CHIP_ERROR GetHardwareVersion(uint16_t &h) override { h = 1; return CHIP_NO_ERROR; }
    CHIP_ERROR GetHardwareVersionString(char *b, size_t n) override { return cp(b, n, CONFIG_IDF_TARGET); }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &u) override
    {
        uint8_t m[6]; mac(m);
        if (u.size() < 16) return CHIP_ERROR_BUFFER_TOO_SMALL;
        for (int i = 0; i < 16; i++) u.data()[i] = (uint8_t)(m[i % 6] ^ (0xA5 + 7 * i));
        u.reduce_size(16);
        return CHIP_NO_ERROR;
    }
};
SlinkInfo s_info;
}

static void make_payload_strings(void)
{
    uint16_t vid = 0xFFF1, pid = 0x8000;
    auto *dii = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
    if (dii) { (void)dii->GetVendorId(vid); (void)dii->GetProductId(pid); }
    chip::SetupPayload p;
    p.version = 0;
    p.vendorID = vid;
    p.productID = pid;
    p.setUpPINCode = g_set.matter_pass;
    p.discriminator.SetLongValue(g_set.matter_disc);
    p.commissioningFlow = chip::CommissioningFlow::kStandard;
    p.rendezvousInformation.SetValue(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE, chip::RendezvousInformationFlag::kOnNetwork));
    std::string qr;
    if (chip::QRCodeSetupPayloadGenerator(p).payloadBase38Representation(qr) == CHIP_NO_ERROR && qr.size() < sizeof s_qr) memcpy(s_qr, qr.c_str(), qr.size() + 1);
    else s_qr[0] = 0;
    chip::MutableCharSpan man(s_manual, sizeof s_manual);
    if (chip::ManualSetupPayloadGenerator(p).payloadDecimalStringRepresentation(man) != CHIP_NO_ERROR) s_manual[0] = 0;
}

/* ------------------------------------------------------------------ the light */
static int max_pct(void) { return g_set.matter_max >= 1 && g_set.matter_max <= 100 ? g_set.matter_max : 30; }

static int level_now(void)                                   /* amplifier volume -> level 1..254, 100 % of the level = MATTER MAX of the knob */
{
    int v = cli_vol_x10();                                   /* tenths of a percent of the knob travel, -1 unknown */
    if (v < 0) return 1;
    int l = (int)((double)v / 10.0 / max_pct() * 254.0 + 0.5);
    return l < 1 ? 1 : l > 254 ? 254 : l;
}

static esp_err_t attr_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != s_ep || !val) return ESP_OK;     /* only what a controller asks: our own updates come as POST_UPDATE */
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        if (val->val.b) { if (state_power() != 1) cli_submit("PWRON"); }
        else cli_submit("PWROFF");
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        int l = val->val.u8;                                          /* a slider (and the cluster's own transitions) gives a series of values: only the last one is sent, see vol_task */
        if (l < 1) l = 1;
        if (l > 254) l = 254;
        s_vol_pending = l; s_last_level = l;
        s_hold_until_us = esp_timer_get_time() + 20 * 1000000LL;         /* the knob takes seconds to get there */
        if (s_vol_task) xTaskNotifyGive(s_vol_task);
    }
    return ESP_OK;
}

static esp_err_t identify_cb(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void *) { return ESP_OK; }

static void event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kServerReady:        /* the server is up (we are in the CHIP task): paired with nobody -> open for 15 minutes */
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager &cwm = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!cwm.IsCommissioningWindowOpen())
                (void)cwm.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(900), chip::CommissioningWindowAdvertisement::kAllSupported);
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        s_fabrics = (int)chip::Server::GetInstance().GetFabricTable().FabricCount();
        if (event->Type == chip::DeviceLayer::DeviceEventType::kFabricRemoved && s_fabrics == 0) {
            chip::CommissioningWindowManager &cwm = chip::Server::GetInstance().GetCommissioningWindowManager();   /* paired with nobody: open for the next one */
            if (!cwm.IsCommissioningWindowOpen())
                (void)cwm.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(300), chip::CommissioningWindowAdvertisement::kDnssdOnly);
        }
        break;
    default: break;
    }
}

/* the commissioning window: 15 minutes, over Bluetooth and DNS-SD (the device is already on the WiFi, so a controller on the same network finds it) */
static uint32_t s_open_err;                                  /* result of the last attempt (0 = ok), shown by MATTER */
static bool open_window(void)
{
    chip::DeviceLayer::StackLock lock;
    chip::CommissioningWindowManager &cwm = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (cwm.IsCommissioningWindowOpen()) return true;
    CHIP_ERROR e = cwm.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(900), chip::CommissioningWindowAdvertisement::kAllSupported);
    s_open_err = e.AsInteger();
    return e == CHIP_NO_ERROR;
}

/* the volume asked by a controller: wait until the values stop coming (400 ms), then one VOL SET (never above MATTER MAX) */
static void vol_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(400)) > 0) {}
        int l = s_vol_pending; s_vol_pending = -1;
        if (l < 1) continue;
        if (state_power() == 0) { s_hold_until_us = 0; matter_report(); continue; }   /* standby: the knob does not move, the controller gets the real level back */
        char cmd[32];
        snprintf(cmd, sizeof cmd, "VOL SET %.1f", l * (double)max_pct() / 254.0);
        cli_submit(cmd);
    }
}

/* the amplifier changed (power, volume): update the two attributes so that the controllers see it */
static void report_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));       /* woken by a change; every 5 s anyway (the hold of the volume ends by itself) */
        vTaskDelay(pdMS_TO_TICKS(300));                      /* a burst of frames makes one update */
        ulTaskNotifyTake(pdTRUE, 0);
        if (!s_running) continue;
        int p = state_power();
        if (p >= 0 && p != s_last_on) {
            esp_matter_attr_val_t v = esp_matter_bool(p == 1);
            attribute::update(s_ep, OnOff::Id, OnOff::Attributes::OnOff::Id, &v);
            s_last_on = p;
        }
        int l = level_now();
        bool holding = esp_timer_get_time() < s_hold_until_us;
        if (!holding && cli_vol_x10() >= 0 && (s_last_level < 0 || abs(l - s_last_level) > 2)) {          /* not while the knob is on its way, and no flapping for a rounding */
            esp_matter_attr_val_t v = esp_matter_nullable_uint8((uint8_t)l);
            attribute::update(s_ep, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &v);
            s_last_level = l;
        }
    }
}

static void matter_task(void *)
{
    ensure_codes();
    svc_mark(SVC_MATTER);
    esp_matter::set_custom_commissionable_data_provider(&s_provider);
    esp_matter::set_custom_device_instance_info_provider(&s_info);

    node::config_t node_config;
    const char *nm = g_set.matter_name[0] ? g_set.matter_name : "Amplificatore";
    strlcpy(node_config.root_node.basic_information.node_label, nm, sizeof node_config.root_node.basic_information.node_label);
    node_t *node = node::create(&node_config, attr_cb, identify_cb);
    if (!node) { ESP_LOGE(TAG, "node"); vTaskDelete(NULL); return; }

    dimmable_light::config_t light;
    light.on_off.on_off = (state_power() == 1);
    light.on_off_lighting.start_up_on_off = nullptr;
    light.level_control.current_level = (uint8_t)level_now();
    light.level_control.on_level = nullptr;
    light.level_control_lighting.start_up_current_level = nullptr;
    endpoint_t *ep = dimmable_light::create(node, &light, ENDPOINT_FLAG_NONE, nullptr);
    if (!ep) { ESP_LOGE(TAG, "endpoint"); vTaskDelete(NULL); return; }
    s_ep = endpoint::get_id(ep);

    esp_err_t err = esp_matter::start(event_cb);
    if (err != ESP_OK) { ESP_LOGE(TAG, "start %d", (int)err); vTaskDelete(NULL); return; }
    make_payload_strings();
    s_fabrics = (int)chip::Server::GetInstance().GetFabricTable().FabricCount();
    s_running = true;
    if (s_fabrics == 0) for (int i = 0; i < 60 && !open_window(); i++) vTaskDelay(pdMS_TO_TICKS(500));   /* the server finishes its start in the background: try until it takes it */
    vTaskDelay(pdMS_TO_TICKS(500));
    svc_done(SVC_MATTER);                                      /* the RAM the Matter stack took (Statistics) */
    xTaskCreate(vol_task, "slink_mtr_vol", 3072, NULL, 3, &s_vol_task);
    xTaskCreate(report_task, "slink_mtr_rep", 6144, NULL, 3, &s_report_task);        /* attribute::update goes through the CHIP stack: 3 KB were not enough (panic) */
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ API (matter.h) */
extern "C" {

void matter_start(void)
{
    if (!g_set.matter || s_running) return;
    esp_reset_reason_t rr = esp_reset_reason();                  /* crash loop guard: Matter is the new, big part, and nobody has a USB cable on the amplifier */
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
        g_set.matter = false; settings_save();
        cli_out("MATTER: the last restart was a crash, Matter is switched off (MATTER ON to try again)" "\r\n");
        return;
    }
    xTaskCreate(matter_task, "slink_matter", 8192, NULL, 5, NULL);
}

bool matter_available(void) { return true; }
bool matter_running(void) { return s_running; }
int  matter_fabrics(void) { return s_fabrics; }

void matter_report(void)
{
    if (s_running && s_report_task) xTaskNotifyGive(s_report_task);
}

size_t matter_info(char *b, size_t n)
{
    int k = snprintf(b, n, "MATTER %s%s, name \"%s\", volume at 100%% level = %d%% of the knob, paired with %d ecosystem%s" "\r\n",
                     g_set.matter ? "ON" : "OFF", s_running ? " (running)" : g_set.matter ? " (starts at the next restart)" : "",
                     g_set.matter_name[0] ? g_set.matter_name : "Amplificatore", max_pct(), s_fabrics, s_fabrics == 1 ? "" : "s");
    if (k < 0 || (size_t)k >= n) return 0;
    if (s_manual[0]) k += snprintf(b + k, n - k, "     manual pairing code %s" "\r\n", s_manual);
    if (s_running) {
        chip::DeviceLayer::StackLock lock;
        bool win = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
        k += snprintf(b + k, n - k, "     commissioning window %s, Bluetooth advertising %s, last open 0x%x" "\r\n", win ? "OPEN" : "closed (MATTER OPEN opens it for 15 minutes)",
                      chip::DeviceLayer::ConnectivityMgr().IsBLEAdvertising() ? "yes" : "no", (unsigned)s_open_err);
    }
    if (s_qr[0]) k += snprintf(b + k, n - k, "     QR payload %s  (QR: https://project-chip.github.io/connectedhomeip/qrcode.html?data=%s)" "\r\n", s_qr, s_qr);
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

size_t matter_json(char *b, size_t n)
{
    int k = snprintf(b, n, ",\"mtrAvail\":1,\"mtr\":%d,\"mtrRun\":%d,\"mtrFab\":%d,\"mtrMax\":%d,\"mtrName\":\"%s\",\"mtrCode\":\"%s\",\"mtrQr\":\"%s\"",
                     g_set.matter ? 1 : 0, s_running ? 1 : 0, s_fabrics, max_pct(), g_set.matter_name, s_manual, s_qr);
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

void matter_open(void) { if (s_running) (void)open_window(); }

void matter_new_code(void)
{
    g_set.matter_pass = 0;
    g_set.matter_disc = 0;
    ensure_codes();
}

static void restart_task(void *) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }
void matter_restart_soon(void) { xTaskCreate(restart_task, "slink_rst", 2048, NULL, 5, NULL); }

void matter_factory_reset(void)
{
    if (s_running) esp_matter::factory_reset();
}

}   /* extern "C" */
