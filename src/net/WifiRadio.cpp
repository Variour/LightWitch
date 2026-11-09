#include "WifiRadio.h"

#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_task.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>

#include <cstring>

#if CONFIG_IDF_TARGET_ESP32C3
// Keeps the USB serial/JTAG port clocked while the radio runs.
extern "C" void phy_bbpll_en_usb(bool en);
#endif

namespace {

// Interface state, kept by the event handler on the system event task.
constexpr EventBits_t kStaStarted = BIT0;
constexpr EventBits_t kStaAssociated = BIT1;
constexpr EventBits_t kStaHasIp = BIT2;
constexpr EventBits_t kStaOnline = BIT3;  // WifiRadio::connected()
constexpr EventBits_t kApStarted = BIT4;

// Work the event handler hands to the radio task.
constexpr uint32_t kWorkStaStarted = BIT0;
constexpr uint32_t kWorkDisconnected = BIT1;
constexpr uint32_t kWorkRetry = BIT2;

constexpr uint32_t kRadioTaskStack = 4096;
constexpr TickType_t kStartWait = pdMS_TO_TICKS(1000);

#if CONFIG_SOC_WIFI_HE_SUPPORT
constexpr uint8_t kProtocols2g =
    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
#else
constexpr uint8_t kProtocols2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
#endif
#if SOC_WIFI_SUPPORT_5G
constexpr uint16_t kProtocols5g =
    WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
#endif

EventGroupHandle_t sState = nullptr;
TaskHandle_t sTask = nullptr;
esp_netif_t* sStaNetif = nullptr;
esp_netif_t* sApNetif = nullptr;
bool sStarted = false;
bool sApEnabled = false;
volatile uint8_t sDisconnectReason = 0;
// The one new attempt after the first dropped or failed connection.
bool sRetryAvailable = true;

EventBits_t state() { return sState ? xEventGroupGetBits(sState) : 0; }

// Copies a string into a fixed driver field, with its terminator where it fits.
void copyField(uint8_t* dst, const char* src, size_t dstLen) {
    size_t n = strlen(src);
    memcpy(dst, src, n >= dstLen ? dstLen : n + 1);
}

// An interface the driver's stored settings left in long-range-only mode
// returns to the standard protocols.
bool useStandardProtocols(wifi_interface_t ifx) {
#if SOC_WIFI_SUPPORT_5G
    wifi_band_mode_t band = WIFI_BAND_MODE_AUTO;
    if (esp_wifi_get_band_mode(&band) != ESP_OK) return true;
    if (band == WIFI_BAND_MODE_AUTO) {
        wifi_protocols_t current = {};
        if (esp_wifi_get_protocols(ifx, &current) != ESP_OK) return true;
        if (current.ghz_2g != WIFI_PROTOCOL_LR) return true;
        wifi_protocols_t standard = {kProtocols2g, kProtocols5g};
        return esp_wifi_set_protocols(ifx, &standard) == ESP_OK;
    }
    if (band == WIFI_BAND_MODE_5G_ONLY) return true;
#endif
    uint8_t current = 0;
    if (esp_wifi_get_protocol(ifx, &current) != ESP_OK) return true;
    if (current != WIFI_PROTOCOL_LR) return true;
    return esp_wifi_set_protocol(ifx, kProtocols2g) == ESP_OK;
}

wifi_mode_t currentMode() {
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (!sStarted || esp_wifi_get_mode(&mode) != ESP_OK) return WIFI_MODE_NULL;
    return mode;
}

// The station takes its address from DHCP: restarts the client with the
// address and the DNS servers cleared.
bool restartDhcpClient() {
    esp_err_t err = esp_netif_dhcpc_stop(sStaNetif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        log_e("DHCP client stop: %s", esp_err_to_name(err));
        return false;
    }
    xEventGroupClearBits(sState, kStaHasIp);

    esp_netif_ip_info_t info = {};
    err = esp_netif_set_ip_info(sStaNetif, &info);
    if (err != ESP_OK) {
        log_e("station address clear: %s", esp_err_to_name(err));
        return false;
    }
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(sStaNetif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_set_dns_info(sStaNetif, ESP_NETIF_DNS_BACKUP, &dns);
    esp_netif_set_dns_info(sStaNetif, ESP_NETIF_DNS_FALLBACK, &dns);

    err = esp_netif_dhcpc_start(sStaNetif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        log_w("DHCP client start: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// Connects again to the network the driver has stored.
bool connectStored() {
    if (state() & kStaAssociated) return true;
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &conf) != ESP_OK) {
        log_e("station config failed");
        return false;
    }
    if (!restartDhcpClient()) return false;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        log_e("station connect: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// Runs on the system event task: keeps the state bits and hands driver calls
// to the radio task.
void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                xEventGroupClearBits(sState, kStaOnline);
                xEventGroupSetBits(sState, kStaStarted);
                xTaskNotify(sTask, kWorkStaStarted, eSetBits);
                break;
            case WIFI_EVENT_STA_STOP:
                xEventGroupClearBits(sState, kStaStarted | kStaAssociated | kStaHasIp | kStaOnline);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                xEventGroupClearBits(sState, kStaOnline);
                xEventGroupSetBits(sState, kStaAssociated);
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                uint8_t reason = ((const wifi_event_sta_disconnected_t*)data)->reason;
                xEventGroupClearBits(sState, kStaAssociated | kStaHasIp);
                if (reason != WIFI_REASON_AUTH_EXPIRE) xEventGroupClearBits(sState, kStaOnline);
                sDisconnectReason = reason;
                uint32_t work = kWorkDisconnected;
                // ASSOC_LEAVE: the firmware disconnected.
                if (reason != WIFI_REASON_ASSOC_LEAVE && sRetryAvailable) {
                    sRetryAvailable = false;
                    work |= kWorkRetry;
                }
                xTaskNotify(sTask, work, eSetBits);
                break;
            }
            case WIFI_EVENT_AP_START:
                xEventGroupSetBits(sState, kApStarted);
                break;
            case WIFI_EVENT_AP_STOP:
                xEventGroupClearBits(sState, kApStarted);
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP)
            xEventGroupSetBits(sState, kStaHasIp | kStaOnline);
        else if (id == IP_EVENT_STA_LOST_IP)
            xEventGroupClearBits(sState, kStaHasIp | kStaOnline);
    }
}

void radioTask(void*) {
    for (;;) {
        uint32_t work = 0;
        xTaskNotifyWait(0, UINT32_MAX, &work, portMAX_DELAY);
        if (work & kWorkStaStarted) {
            // Modem sleep between beacons while connected, the driver's default.
            if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK) log_e("power save failed");
        }
        if (work & kWorkDisconnected)
            log_w("station disconnected, reason %u", (unsigned)sDisconnectReason);
        if (work & kWorkRetry) {
            log_d("first drop since boot: connecting again");
            esp_wifi_disconnect();
            connectStored();
        }
    }
}

// Switches the softAP interface on or off next to the station.
bool enableAp(bool enable) {
    wifi_mode_t mode = currentMode();
    if (mode == WIFI_MODE_NULL) return false;
    if (((mode & WIFI_MODE_AP) != 0) == enable) return true;
    wifi_mode_t next = enable ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(next);
    if (err != ESP_OK) {
        log_e("mode %d: %s", next, esp_err_to_name(err));
        return false;
    }
    sApEnabled = enable;
    if (!enable) xEventGroupClearBits(sState, kApStarted);
    if (!useStandardProtocols(WIFI_IF_STA)) return false;
    if (enable && !useStandardProtocols(WIFI_IF_AP)) return false;
    return true;
}

// The softAP interface on and started.
bool apReady() {
    if (!enableAp(true)) return false;
    if (!(xEventGroupWaitBits(sState, kApStarted, pdFALSE, pdTRUE, kStartWait) & kApStarted)) {
        log_e("softAP did not start");
        return false;
    }
    return true;
}

bool sameAp(const wifi_config_t& a, const wifi_config_t& b) {
    return strncmp((const char*)a.ap.ssid, (const char*)b.ap.ssid, 32) == 0 &&
           strncmp((const char*)a.ap.password, (const char*)b.ap.password, 64) == 0 &&
           a.ap.channel == b.ap.channel && a.ap.authmode == b.ap.authmode &&
           a.ap.ssid_hidden == b.ap.ssid_hidden && a.ap.max_connection == b.ap.max_connection &&
           a.ap.pairwise_cipher == b.ap.pairwise_cipher && a.ap.ftm_responder == b.ap.ftm_responder;
}

}  // namespace

bool WifiRadio::begin(bool withAp) {
    if (sStarted) return true;

#if CONFIG_IDF_TARGET_ESP32
    uint8_t baseMac[8];
    if (esp_base_mac_addr_get(baseMac) != ESP_OK && esp_efuse_mac_get_default(baseMac) == ESP_OK)
        esp_base_mac_addr_set(baseMac);
#endif
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        log_e("esp_netif_init: %s", esp_err_to_name(err));
        return false;
    }
    // The default event loop exists once; whoever comes second finds it there.
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        log_e("event loop: %s", esp_err_to_name(err));
        return false;
    }

    if (!sState) sState = xEventGroupCreate();
    if (!sTask)
        xTaskCreatePinnedToCore(radioTask, "wifi_radio", kRadioTaskStack, nullptr,
                                ESP_TASKD_EVENT_PRIO - 1, &sTask,
                                CONFIG_ARDUINO_EVENT_RUNNING_CORE);
    if (!sState || !sTask) {
        log_e("out of memory");
        return false;
    }

    if (!sStaNetif) {
        // Dynamic transmit buffers and a small static receive pool.
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.static_tx_buf_num = 0;
        cfg.dynamic_tx_buf_num = 32;
        cfg.tx_buf_type = 1;
        cfg.cache_tx_buf_num = 4;
        cfg.static_rx_buf_num = 4;
        cfg.dynamic_rx_buf_num = 32;
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            log_e("esp_wifi_init: %s", esp_err_to_name(err));
            return false;
        }
#if CONFIG_IDF_TARGET_ESP32C3
        phy_bbpll_en_usb(true);
#endif
        sApNetif = esp_netif_create_default_wifi_ap();
        sStaNetif = esp_netif_create_default_wifi_sta();
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr,
                                            nullptr);
        esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr, nullptr);
    }

    uint8_t mac[6] = {};
    esp_base_mac_addr_get(mac);
    char hostname[32];
    snprintf(hostname, sizeof(hostname), CONFIG_IDF_TARGET "-%02X%02X%02X", mac[3], mac[4], mac[5]);
    err = esp_netif_set_hostname(sStaNetif, hostname);
    if (err != ESP_OK) {
        log_e("hostname: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_mode(withAp ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        log_e("mode: %s", esp_err_to_name(err));
        return false;
    }
    sApEnabled = withAp;
    if (!useStandardProtocols(WIFI_IF_STA)) return false;
    if (withAp && !useStandardProtocols(WIFI_IF_AP)) return false;

    err = esp_wifi_start();
    if (err != ESP_OK) {
        log_e("esp_wifi_start: %s", esp_err_to_name(err));
        return false;
    }
    sStarted = true;
#if SOC_WIFI_SUPPORT_5G
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    return true;
}

bool WifiRadio::connect(const char* ssid, const char* password) {
    if (!sStarted) return false;
    if (!(xEventGroupWaitBits(sState, kStaStarted, pdFALSE, pdTRUE, kStartWait) & kStaStarted)) {
        log_e("station did not start");
        return false;
    }

    // Still associated: leave that network first and drop its stored settings.
    if (state() & kStaAssociated) {
        if (esp_wifi_disconnect() != ESP_OK) return false;
        for (int waited = 0; (state() & kStaAssociated) && waited < 1000; waited += 5)
            vTaskDelay(pdMS_TO_TICKS(5));
        if (state() & kStaAssociated) return false;
        wifi_config_t empty;
        memset(&empty, 0, sizeof(empty));
        if (esp_wifi_set_config(WIFI_IF_STA, &empty) != ESP_OK) return false;
    }

    if (!ssid || !*ssid || strlen(ssid) > 32) {
        log_e("SSID too long or missing");
        return false;
    }
    if (password && strlen(password) > 64) {
        log_e("password too long");
        return false;
    }

    // First matching AP on any channel; a password asks for WPA2 or better.
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.sta.scan_method = WIFI_FAST_SCAN;
    conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    conf.sta.threshold.rssi = -127;
    conf.sta.pmf_cfg.capable = true;
    copyField(conf.sta.ssid, ssid, 32);
    if (password && *password) {
        conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        copyField(conf.sta.password, password, 64);
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
    if (err != ESP_OK) {
        log_e("station config: %s", esp_err_to_name(err));
        return false;
    }
    if (!restartDhcpClient()) return false;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        log_e("station connect: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool WifiRadio::disconnect() {
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        log_e("station disconnect: %s", esp_err_to_name(err));
        return false;
    }
    for (int waited = 0; (state() & kStaAssociated) && waited < 100; waited += 5)
        vTaskDelay(pdMS_TO_TICKS(5));
    return !(state() & kStaAssociated);
}

bool WifiRadio::connected() { return (state() & kStaOnline) != 0; }

bool WifiRadio::associatedWithAddress() {
    const EventBits_t both = kStaAssociated | kStaHasIp;
    return (state() & both) == both;
}

IPAddress WifiRadio::localIp() {
    esp_netif_ip_info_t info;
    if (!sStaNetif || esp_netif_get_ip_info(sStaNetif, &info) != ESP_OK) return IPAddress();
    return IPAddress(info.ip.addr);
}

bool WifiRadio::startAp(const char* ssid, const char* password, uint8_t channel) {
    if (!apReady()) return false;
    if (!ssid || !*ssid) {
        log_e("SSID missing");
        return false;
    }
    size_t passwordLen = password ? strlen(password) : 0;
    if (passwordLen > 0 && passwordLen < 8) {
        log_e("password too short");
        return false;
    }

    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.ap.channel = channel;
    conf.ap.max_connection = 4;
    conf.ap.beacon_interval = 100;
    copyField(conf.ap.ssid, ssid, 32);
    conf.ap.ssid_len = strlen(ssid);
    if (passwordLen > 0) {
        conf.ap.authmode = WIFI_AUTH_WPA2_PSK;
        conf.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
        copyField(conf.ap.password, password, 64);
    }

    wifi_config_t current;
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &current);
    if (err != ESP_OK) {
        log_e("softAP config read: %s", esp_err_to_name(err));
        return false;
    }
    if (sameAp(conf, current)) return true;
    err = esp_wifi_set_config(WIFI_IF_AP, &conf);
    if (err != ESP_OK) {
        log_e("softAP config: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool WifiRadio::clearAp() {
    if (!apReady()) return false;
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.ap.channel = 1;
    conf.ap.max_connection = 4;
    conf.ap.beacon_interval = 100;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &conf);
    if (err != ESP_OK) {
        log_e("softAP config: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool WifiRadio::stopAp() { return clearAp() && enableAp(false); }

bool WifiRadio::apEnabled() { return (currentMode() & WIFI_MODE_AP) != 0; }

IPAddress WifiRadio::apIp() {
    esp_netif_ip_info_t info;
    if (!sApEnabled || !sApNetif || esp_netif_get_ip_info(sApNetif, &info) != ESP_OK)
        return IPAddress();
    return IPAddress(info.ip.addr);
}

bool WifiRadio::setTxPower() {
    if (!(state() & (kStaStarted | kApStarted))) {
        log_w("neither station nor softAP has started");
        return false;
    }
    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_TX_POWER);
    if (err != ESP_OK) log_e("tx power: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

uint8_t WifiRadio::channel() {
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (sStaNetif) esp_wifi_get_channel(&primary, &second);
    return primary;
}

void WifiRadio::macAddress(uint8_t mac[6]) {
    if (sStaNetif) esp_netif_get_mac(sStaNetif, mac);
}

String WifiRadio::macString() {
    uint8_t mac[6] = {};
    macAddress(mac);
    char text[18];
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return String(text);
}
