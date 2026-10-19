#include "WifiManager.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_phy_init.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"

extern "C" {
#include "dhcpserver/dhcpserver.h"
#if CONFIG_LWIP_IPV4_NAPT
#include "lwip/lwip_napt.h"
#endif
}

static const char* TAG = "WIFI_MGR";
static const char* NVS_NAMESPACE = "wifi_creds";

static inline void safeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t len = strnlen(src, destSize - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

WifiManager& WifiManager::getInstance() {
    static WifiManager instance;
    return instance;
}

WifiManager::WifiManager()
    : isInitialized(false)
    , isStarted(false)
    , staNetif(nullptr)
    , apNetif(nullptr)
    , shouldReconnect(false)
    , manualDisconnect(false)
    , currentRetryCount(0)
    , maxReconnectRetries(5)
    , retryDelayMs(1000)
    , retryTimer(nullptr)
    , natEnabled(true)
    , isScanning(false)
    , scanCallback(nullptr)
    , apStartTime(0)
{
}

WifiManager::~WifiManager() {
    stop();
    if (retryTimer) {
        esp_timer_stop(retryTimer);
        esp_timer_delete(retryTimer);
        retryTimer = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialization
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::init() {
    std::lock_guard<std::recursive_mutex> lock(wifiMutex);
    if (isInitialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing FlxOS-aligned Wi-Fi & Networking Stack...");

    // 1. Initialize NVS (required for Wi-Fi hardware and persistent credentials)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing corrupted/outdated NVS flash...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Pre-seed default PHY calibration data in NVS if not already present.
    // Stops full RF calibration spike (>500mA) on USB bus.
    esp_phy_calibration_data_t cal_data = {};
    if (esp_phy_load_cal_data_from_nvs(&cal_data) != ESP_OK) {
        ESP_LOGI(TAG, "Pre-seeding default PHY calibration data to bypass full RF calibration...");
        memset(&cal_data, 0, sizeof(cal_data));
        esp_phy_store_cal_data_to_nvs(&cal_data);
    }

    // 2. Initialize TCP/IP and Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Create default network interfaces
    staNetif = esp_netif_create_default_wifi_sta();
    apNetif = esp_netif_create_default_wifi_ap();

    // 4. Initialize Wi-Fi Driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Crucial FlxOS setting: Use RAM storage so credentials are not written to flash
    // on every connection, preventing flash wear and SPI bus write stalls
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    // 5. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &eventHandler, this, nullptr));

    // 6. Create Non-Blocking Exponential Backoff Retry Timer (FlxOS parity)
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &onRetryTimer;
    timerArgs.arg = this;
    timerArgs.name = "wifi_retry";
    esp_timer_create(&timerArgs, &retryTimer);

    isInitialized = true;
    telemetry.state = WifiState::Idle;
    telemetry.mode = WifiMode::Off;

    ESP_LOGI(TAG, "Wi-Fi subsystem initialized (RAM storage, NAPT enabled, exponential backoff).");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mode Management (FlxOS ConnectivityManager parity)
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::setWifiMode(wifi_mode_t mode, bool autoStart) {
    std::lock_guard<std::recursive_mutex> lock(wifiMutex);

    wifi_mode_t currentMode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&currentMode);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    if (err != ESP_ERR_WIFI_NOT_INIT && currentMode == mode) {
        if (autoStart && !isStarted && mode != WIFI_MODE_NULL) {
            isStarted = true;
            return esp_wifi_start();
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Switching Wi-Fi mode: %d -> %d", (int)currentMode, (int)mode);

    if (mode == WIFI_MODE_NULL) {
        if (isStarted) {
            esp_wifi_stop();
            isStarted = false;
        }
        telemetry.mode = WifiMode::Off;
        telemetry.state = WifiState::Idle;
        return ESP_OK;
    }

    err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi mode %d: %s", (int)mode, esp_err_to_name(err));
        return err;
    }

    if (autoStart && !isStarted) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            isStarted = true;
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        }
    }

    if (mode == WIFI_MODE_STA) telemetry.mode = WifiMode::STA;
    else if (mode == WIFI_MODE_AP) telemetry.mode = WifiMode::AP;
    else if (mode == WIFI_MODE_APSTA) telemetry.mode = WifiMode::AP_STA;

    return err;
}

// ─────────────────────────────────────────────────────────────────────────────
// Station Mode (STA) & Reconnection
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::startSTA(const char* ssid, const char* password, uint8_t maxRetries, bool remember) {
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!isInitialized) {
        esp_err_t ret = init();
        if (ret != ESP_OK) return ret;
    }

    std::lock_guard<std::recursive_mutex> lock(wifiMutex);
    ESP_LOGI(TAG, "Connecting to SSID: '%s'...", ssid);

    shouldReconnect = true;
    manualDisconnect = false;
    currentRetryCount = 0;
    maxReconnectRetries = maxRetries;
    retryDelayMs = 1000;
    if (retryTimer) {
        esp_timer_stop(retryTimer);
    }

    safeStrCopy(telemetry.staSsid, sizeof(telemetry.staSsid), ssid);
    telemetry.state = WifiState::Connecting;

    // Save to multi-network store if requested
    if (remember) {
        WiFiCredential cred;
        cred.ssid = ssid;
        cred.password = password ? password : "";
        cred.autoConnect = true;
        saveCredential(cred);
    }

    // Stop existing connection cleanly
    esp_wifi_disconnect();

    wifi_config_t wifiConfig = {};
    safeStrCopy((char*)wifiConfig.sta.ssid, sizeof(wifiConfig.sta.ssid), ssid);
    if (password && strlen(password) > 0) {
        safeStrCopy((char*)wifiConfig.sta.password, sizeof(wifiConfig.sta.password), password);
        wifiConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifiConfig.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    // Channel pinning (FlxOS parity): if known network has previous channel, speed up sync
    WiFiCredential savedCred;
    if (loadCredential(ssid, savedCred) && savedCred.lastChannel > 0 && savedCred.lastChannel <= 14) {
        wifiConfig.sta.channel = savedCred.lastChannel;
        ESP_LOGI(TAG, "Pinning connection to channel %d", (int)savedCred.lastChannel);
    }

    // Target mode: Preserve AP if running
    wifi_mode_t currentMode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&currentMode);
    wifi_mode_t targetMode = (currentMode == WIFI_MODE_AP || currentMode == WIFI_MODE_APSTA || telemetry.isApActive)
                                 ? WIFI_MODE_APSTA
                                 : WIFI_MODE_STA;

    esp_err_t err = setWifiMode(targetMode);
    if (err != ESP_OK) {
        telemetry.state = WifiState::Idle;
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
    if (err != ESP_OK) {
        telemetry.state = WifiState::Idle;
        return err;
    }

    return esp_wifi_connect();
}

esp_err_t WifiManager::disconnectSTA() {
    std::lock_guard<std::recursive_mutex> lock(wifiMutex);
    ESP_LOGI(TAG, "Manual station disconnect requested.");

    shouldReconnect = false;
    manualDisconnect = true; // Suppress auto-reconnect
    if (retryTimer) {
        esp_timer_stop(retryTimer);
    }
    currentRetryCount = 0;
    retryDelayMs = 1000;

    telemetry.isConnected = false;
    telemetry.staIp[0] = '\0';

    esp_err_t ret = esp_wifi_disconnect();

    // If Hotspot is active, switch to AP-only mode cleanly
    if (telemetry.isApActive) {
        setWifiMode(WIFI_MODE_AP);
        telemetry.state = WifiState::ApActive;
    } else {
        telemetry.state = WifiState::Idle;
    }
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// SoftAP (Hotspot) Mode
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::startAP(const char* ssid, const char* password, uint8_t channel,
                               uint8_t maxConnections, bool hidden, wifi_auth_mode_t authMode,
                               int8_t maxTxPower) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!isInitialized) {
        esp_err_t ret = init();
        if (ret != ESP_OK) return ret;
    }

    std::lock_guard<std::recursive_mutex> lock(wifiMutex);
    ESP_LOGI(TAG, "Starting SoftAP Hotspot: SSID='%s', Ch=%u, MaxClients=%u", ssid, channel, maxConnections);

    wifi_config_t apConfig = {};
    size_t ssidLen = strnlen(ssid, sizeof(apConfig.ap.ssid));
    memcpy(apConfig.ap.ssid, ssid, ssidLen);
    apConfig.ap.ssid_len = (uint8_t)ssidLen;
    apConfig.ap.channel = channel;
    apConfig.ap.max_connection = maxConnections;
    apConfig.ap.ssid_hidden = hidden ? 1 : 0;
    apConfig.ap.authmode = authMode;

    if (authMode != WIFI_AUTH_OPEN && password && strlen(password) >= 8) {
        safeStrCopy((char*)apConfig.ap.password, sizeof(apConfig.ap.password), password);
    } else if (authMode != WIFI_AUTH_OPEN) {
        apConfig.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Preserve Station connection if active
    wifi_mode_t currentMode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&currentMode);
    wifi_mode_t targetMode = (currentMode == WIFI_MODE_STA || currentMode == WIFI_MODE_APSTA || telemetry.isConnected)
                                 ? WIFI_MODE_APSTA
                                 : WIFI_MODE_AP;

    esp_err_t err = setWifiMode(targetMode, false);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_config(WIFI_IF_AP, &apConfig);
    if (err != ESP_OK) return err;

    esp_wifi_set_max_tx_power(maxTxPower);

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    isStarted = true;

    safeStrCopy(telemetry.apSsid, sizeof(telemetry.apSsid), ssid);
    telemetry.isApActive = true;
    apStartTime = esp_timer_get_time();

    // Cache Gateway IP
    esp_netif_ip_info_t ipInfo;
    if (apNetif && esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK) {
        esp_ip4addr_ntoa(&ipInfo.ip, telemetry.apIp, sizeof(telemetry.apIp));
        ESP_LOGI(TAG, "SoftAP ready. Gateway IP: %s", telemetry.apIp);
    }

    // If Station is already connected, activate IP NAPT routing
    if (telemetry.isConnected && natEnabled) {
        setNatEnabled(true, true);
    }

    return ESP_OK;
}

esp_err_t WifiManager::stopAP() {
    std::lock_guard<std::recursive_mutex> lock(wifiMutex);
    ESP_LOGI(TAG, "Stopping SoftAP Hotspot...");

    if (natEnabled) {
        setNatEnabled(false);
    }

    wifi_mode_t currentMode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&currentMode);

    esp_err_t err = ESP_OK;
    if (currentMode == WIFI_MODE_APSTA) {
        err = setWifiMode(WIFI_MODE_STA);
    } else if (currentMode == WIFI_MODE_AP) {
        err = setWifiMode(WIFI_MODE_NULL);
    }

    telemetry.isApActive = false;
    telemetry.connectedClients = 0;
    telemetry.apIp[0] = '\0';
    clients.clear();
    apStartTime = 0;

    return err;
}

esp_err_t WifiManager::startApSta(const char* apSsid, const char* apPassword,
                                  const char* staSsid, const char* staPassword,
                                  uint8_t apChannel) {
    esp_err_t ret = startAP(apSsid, apPassword, apChannel);
    if (ret != ESP_OK) return ret;

    if (staSsid && strlen(staSsid) > 0) {
        ret = startSTA(staSsid, staPassword);
    }
    return ret;
}

esp_err_t WifiManager::stop() {
    std::lock_guard<std::recursive_mutex> lock(wifiMutex);
    ESP_LOGI(TAG, "Stopping all Wi-Fi subsystems...");

    if (retryTimer) {
        esp_timer_stop(retryTimer);
    }

    if (natEnabled) {
        setNatEnabled(false);
    }

    shouldReconnect = false;
    manualDisconnect = true;

    setWifiMode(WIFI_MODE_NULL);

    telemetry.mode = WifiMode::Off;
    telemetry.state = WifiState::Idle;
    telemetry.isConnected = false;
    telemetry.isApActive = false;
    telemetry.staIp[0] = '\0';
    telemetry.apIp[0] = '\0';
    telemetry.connectedClients = 0;
    clients.clear();
    isStarted = false;

    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// IP NAPT Routing (FlxOS HotspotManager parity)
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::setNatEnabled(bool enabled, bool syncDns) {
    natEnabled = enabled;
    if (!apNetif) {
        return ESP_ERR_NOT_FOUND;
    }

    if (telemetry.isApActive && enabled) {
        esp_netif_ip_info_t ipInfo;
        esp_netif_get_ip_info(apNetif, &ipInfo);

#if CONFIG_LWIP_IPV4_NAPT
        ESP_LOGI(TAG, "Enabling IP NAPT routing on SoftAP (Gateway: " IPSTR ")", IP2STR(&ipInfo.ip));
        ip_napt_enable(ipInfo.ip.addr, 1);
#else
        ESP_LOGW(TAG, "CONFIG_LWIP_IPV4_NAPT not compiled; skipping kernel NAPT.");
#endif

        // Configure DHCP server DNS option (offer DNS to connecting devices)
        esp_netif_dhcps_stop(apNetif);
        dhcps_offer_t dhcps_dns_value = OFFER_DNS;
        esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value));

        // Sync upstream DNS from STA connection, fallback to Google DNS (8.8.8.8)
        esp_netif_dns_info_t dns;
        bool dnsSynced = false;
        if (syncDns && staNetif) {
            if (esp_netif_get_dns_info(staNetif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.u_addr.ip4.addr != 0) {
                dnsSynced = true;
            }
        }
        if (!dnsSynced) {
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
        }
        esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_dhcps_start(apNetif);

        // Disable modem power save for minimum latency while routing
        esp_wifi_set_ps(WIFI_PS_NONE);
        telemetry.isNatEnabled = true;

    } else if (!enabled) {
#if CONFIG_LWIP_IPV4_NAPT
        esp_netif_ip_info_t ipInfo;
        if (apNetif && esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK) {
            ip_napt_enable(ipInfo.ip.addr, 0);
        }
#endif
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        telemetry.isNatEnabled = false;
    }
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Asynchronous Network Scan
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::scan(ScanCallback callback) {
    if (!isInitialized) {
        esp_err_t ret = init();
        if (ret != ESP_OK) return ret;
    }

    bool expected = false;
    if (!isScanning.compare_exchange_strong(expected, true)) {
        return ESP_ERR_INVALID_STATE; // Scan already in progress
    }

    std::lock_guard<std::recursive_mutex> lock(wifiMutex);
    ESP_LOGI(TAG, "Scanning for Wi-Fi networks...");
    scanCallback = callback;

    wifi_mode_t currentMode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&currentMode);
    wifi_mode_t targetMode = (currentMode == WIFI_MODE_AP || currentMode == WIFI_MODE_APSTA || telemetry.isApActive)
                                 ? WIFI_MODE_APSTA
                                 : WIFI_MODE_STA;

    esp_err_t err = setWifiMode(targetMode);
    if (err != ESP_OK) {
        isScanning.store(false);
        scanCallback = nullptr;
        return err;
    }

    wifi_scan_config_t scanConfig = {};
    scanConfig.show_hidden = false;
    err = esp_wifi_scan_start(&scanConfig, false);
    if (err != ESP_OK) {
        isScanning.store(false);
        scanCallback = nullptr;
    }
    return err;
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-Network Credential Store (FlxOS WiFiCredentialStore parity via NVS)
// ─────────────────────────────────────────────────────────────────────────────

static std::string ssidToNvsKey(const std::string& ssid) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : ssid) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 16777619u;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "w_%08lx", (unsigned long)hash);
    return std::string(buf);
}

esp_err_t WifiManager::saveCredential(const WiFiCredential& cred) {
    if (cred.ssid.empty()) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    std::string key = ssidToNvsKey(cred.ssid);

    // Serialization format: ssid|password|autoConnect|priority|lastChannel|lastConnectedMs
    std::string val = cred.ssid + "\t" +
                      cred.password + "\t" +
                      (cred.autoConnect ? "1" : "0") + "\t" +
                      std::to_string(cred.priority) + "\t" +
                      std::to_string(cred.lastChannel) + "\t" +
                      std::to_string(cred.lastConnectedMs);

    err = nvs_set_str(handle, key.c_str(), val.c_str());
    if (err == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Saved network '%s' to credential store (key: %s)", cred.ssid.c_str(), key.c_str());
    }
    nvs_close(handle);
    return err;
}

bool WifiManager::loadCredential(const std::string& ssid, WiFiCredential& out) const {
    if (ssid.empty()) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    std::string key = ssidToNvsKey(ssid);
    size_t reqLen = 0;
    if (nvs_get_str(handle, key.c_str(), nullptr, &reqLen) != ESP_OK || reqLen == 0) {
        nvs_close(handle);
        return false;
    }

    std::string val(reqLen, '\0');
    nvs_get_str(handle, key.c_str(), &val[0], &reqLen);
    nvs_close(handle);

    // Parse tab-delimited values
    std::vector<std::string> parts;
    size_t start = 0, end = 0;
    while ((end = val.find('\t', start)) != std::string::npos) {
        parts.push_back(val.substr(start, end - start));
        start = end + 1;
    }
    parts.push_back(val.substr(start));

    if (parts.size() >= 2) {
        out.ssid = parts[0];
        out.password = parts[1];
        out.autoConnect = (parts.size() > 2) ? (parts[2] == "1") : true;
        out.priority = (parts.size() > 3) ? std::atoi(parts[3].c_str()) : 0;
        out.lastChannel = (parts.size() > 4) ? std::atoi(parts[4].c_str()) : 0;
        out.lastConnectedMs = (parts.size() > 5) ? std::atoll(parts[5].c_str()) : 0;
        return true;
    }
    return false;
}

std::vector<WiFiCredential> WifiManager::loadAllCredentials() const {
    std::vector<WiFiCredential> list;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return list;
    }

    nvs_iterator_t it = nullptr;
    esp_err_t res = nvs_entry_find_in_handle(handle, NVS_TYPE_STR, &it);
    while (res == ESP_OK && it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        size_t reqLen = 0;
        if (nvs_get_str(handle, info.key, nullptr, &reqLen) == ESP_OK && reqLen > 0) {
            std::string val(reqLen, '\0');
            nvs_get_str(handle, info.key, &val[0], &reqLen);

            std::vector<std::string> parts;
            size_t start = 0, end = 0;
            while ((end = val.find('\t', start)) != std::string::npos) {
                parts.push_back(val.substr(start, end - start));
                start = end + 1;
            }
            parts.push_back(val.substr(start));

            if (parts.size() >= 2) {
                WiFiCredential cred;
                cred.ssid = parts[0];
                cred.password = parts[1];
                cred.autoConnect = (parts.size() > 2) ? (parts[2] == "1") : true;
                cred.priority = (parts.size() > 3) ? std::atoi(parts[3].c_str()) : 0;
                cred.lastChannel = (parts.size() > 4) ? std::atoi(parts[4].c_str()) : 0;
                cred.lastConnectedMs = (parts.size() > 5) ? std::atoll(parts[5].c_str()) : 0;
                list.push_back(cred);
            }
        }
        res = nvs_entry_next(&it);
    }
    if (it != nullptr) {
        nvs_release_iterator(it);
    }
    nvs_close(handle);

    // Sort by priority DESC, then lastConnectedMs DESC
    std::sort(list.begin(), list.end(), [](const WiFiCredential& a, const WiFiCredential& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.lastConnectedMs > b.lastConnectedMs;
    });

    return list;
}

esp_err_t WifiManager::removeCredential(const std::string& ssid) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    std::string key = ssidToNvsKey(ssid);
    err = nvs_erase_key(handle, key.c_str());
    if (err == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Erased network '%s' from credential store", ssid.c_str());
    }
    nvs_close(handle);
    return err;
}

void WifiManager::updateLastConnected(const std::string& ssid, int32_t channel) {
    WiFiCredential cred;
    if (loadCredential(ssid, cred)) {
        cred.lastConnectedMs = esp_timer_get_time() / 1000;
        if (channel > 0) cred.lastChannel = channel;
        saveCredential(cred);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Auto-Connect to Best Known Network (FlxOS parity)
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t WifiManager::connectBestKnownNetwork() {
    if (manualDisconnect) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Scanning for best known network...");
    return scan([this](const std::vector<wifi_ap_record_t>& visible) {
        std::vector<WiFiCredential> saved = loadAllCredentials();
        if (saved.empty()) {
            ESP_LOGI(TAG, "No saved networks in credential store.");
            return;
        }

        const WiFiCredential* best = nullptr;
        int8_t bestRssi = -128;

        for (const auto& cred : saved) {
            if (!cred.autoConnect) continue;
            for (const auto& ap : visible) {
                std::string apSsid(reinterpret_cast<const char*>(ap.ssid), strnlen(reinterpret_cast<const char*>(ap.ssid), 32));
                if (apSsid == cred.ssid && ap.rssi > bestRssi) {
                    best = &cred;
                    bestRssi = ap.rssi;
                    break;
                }
            }
            if (best != nullptr) break; // Priority ordered
        }

        if (best) {
            ESP_LOGI(TAG, "Auto-connecting to best match: '%s' (RSSI: %d dBm)", best->ssid.c_str(), (int)bestRssi);
            startSTA(best->ssid.c_str(), best->password.c_str(), 5, false);
        } else {
            ESP_LOGI(TAG, "None of the %u visible networks matched saved credentials.", (unsigned)visible.size());
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Boot-Media Auto-Provisioning from SD Card (FlxOS WiFiProvisioning parity)
// ─────────────────────────────────────────────────────────────────────────────

bool WifiManager::importFromBootMedia() {
    ESP_LOGI(TAG, "Checking for boot-media Wi-Fi provisioning files...");
    bool imported = false;

    // 1. FlxOS Standard Directory: /sdcard/provisioning/wifi/
    if (importFromDirectory("/sdcard/provisioning/wifi")) {
        imported = true;
    }

    // 2. Direct Root JSON: /sdcard/wifi.json
    if (importJsonFile("/sdcard/wifi.json")) {
        imported = true;
    }

    // 3. Simple text config: /sdcard/wifi.txt
    if (importTxtFile("/sdcard/wifi.txt")) {
        imported = true;
    }

    if (imported) {
        ESP_LOGI(TAG, "Wi-Fi provisioning complete. Triggering auto-connect...");
        connectBestKnownNetwork();
    }
    return imported;
}

bool WifiManager::importFromDirectory(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return false;

    bool anyImported = false;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.size() >= 5 && filename.substr(filename.size() - 5) == ".json") {
            std::string path = std::string(dir) + "/" + filename;
            if (importJsonFile(path)) {
                anyImported = true;
            }
        }
    }
    closedir(d);
    return anyImported;
}

bool WifiManager::importJsonFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 4096) {
        fclose(f);
        return false;
    }

    std::string content(len, '\0');
    fread(&content[0], 1, len, f);
    fclose(f);

    // Simple JSON extraction for "ssid" and "password"
    auto extractValue = [](const std::string& json, const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        pos = json.find('\"', pos);
        if (pos == std::string::npos) return "";
        size_t endPos = json.find('\"', pos + 1);
        if (endPos == std::string::npos) return "";
        return json.substr(pos + 1, endPos - pos - 1);
    };

    std::string ssid = extractValue(content, "ssid");
    std::string pass = extractValue(content, "password");

    if (!ssid.empty()) {
        WiFiCredential cred;
        cred.ssid = ssid;
        cred.password = pass;
        cred.autoConnect = true;
        cred.priority = 10;
        esp_err_t err = saveCredential(cred);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Successfully provisioned network '%s' from %s", ssid.c_str(), path.c_str());
            return true;
        }
    }
    return false;
}

bool WifiManager::importTxtFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    char line[128];
    std::string ssid;
    std::string pass;

    while (fgets(line, sizeof(line), f)) {
        char* newline = strchr(line, '\r');
        if (newline) *newline = '\0';
        newline = strchr(line, '\n');
        if (newline) *newline = '\0';

        if (strncmp(line, "ssid=", 5) == 0) {
            ssid = line + 5;
        } else if (strncmp(line, "password=", 9) == 0) {
            pass = line + 9;
        }
    }
    fclose(f);

    if (!ssid.empty()) {
        WiFiCredential cred;
        cred.ssid = ssid;
        cred.password = pass;
        cred.autoConnect = true;
        cred.priority = 10;
        if (saveCredential(cred) == ESP_OK) {
            ESP_LOGI(TAG, "Successfully provisioned network '%s' from %s", ssid.c_str(), path.c_str());
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hotspot Client Tracking & Queries
// ─────────────────────────────────────────────────────────────────────────────

std::vector<HotspotClient> WifiManager::getConnectedClients() {
    std::lock_guard<std::recursive_mutex> lock(wifiMutex);

    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) == ESP_OK) {
        uint64_t nowSec = esp_timer_get_time() / 1000000;
        for (auto& client : clients) {
            for (int i = 0; i < staList.num; i++) {
                if (memcmp(client.mac, staList.sta[i].mac, 6) == 0) {
                    client.rssi = staList.sta[i].rssi;
                    break;
                }
            }
            if (client.connectedTimestamp > 0) {
                client.connectedDuration = (uint32_t)(nowSec - client.connectedTimestamp);
            }
        }
    }
    return clients;
}

int8_t WifiManager::getRssi() {
    if (!telemetry.isConnected) return 0;
    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
        telemetry.rssi = apInfo.rssi;
    }
    return telemetry.rssi;
}

// ─────────────────────────────────────────────────────────────────────────────
// Event Handlers & Retry Timer
// ─────────────────────────────────────────────────────────────────────────────

void WifiManager::eventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    WifiManager* self = static_cast<WifiManager*>(arg);
    if (eventBase == WIFI_EVENT) {
        self->handleWifiEvent(eventId, eventData);
    } else if (eventBase == IP_EVENT) {
        self->handleIpEvent(eventId, eventData);
    }
}

void WifiManager::onRetryTimer(void* /*arg*/) {
    ESP_LOGI(TAG, "Retry timer fired -> Calling esp_wifi_connect()...");
    esp_wifi_connect();
}

void WifiManager::handleWifiEvent(int32_t eventId, void* eventData) {
    switch (eventId) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
            if (shouldReconnect) {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t* evt = (wifi_event_sta_connected_t*)eventData;
            std::string connectedSsid(reinterpret_cast<const char*>(evt->ssid), evt->ssid_len);
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED to '%s' (Channel %u)", connectedSsid.c_str(), evt->channel);
            updateLastConnected(connectedSsid, evt->channel);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* evt = (wifi_event_sta_disconnected_t*)eventData;
            ESP_LOGW(TAG, "STA disconnected, reason: %d", evt->reason);

            telemetry.isConnected = false;
            telemetry.staIp[0] = '\0';

            // FlxOS Auth Failure Detection
            bool const isAuthFailure = (evt->reason == WIFI_REASON_AUTH_EXPIRE ||
                                        evt->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                                        evt->reason == WIFI_REASON_AUTH_FAIL ||
                                        evt->reason == WIFI_REASON_HANDSHAKE_TIMEOUT);

            if (isAuthFailure) {
                shouldReconnect = false;
                telemetry.state = WifiState::AuthFailed;
                ESP_LOGE(TAG, "Authentication failed (wrong password/handshake timeout). Disabling auto-reconnect.");
            } else if (manualDisconnect) {
                ESP_LOGI(TAG, "Manual disconnect — suppressing auto-reconnect");
                telemetry.state = WifiState::Idle;
            } else if (shouldReconnect && currentRetryCount < maxReconnectRetries) {
                telemetry.state = WifiState::Connecting;
                uint32_t delay = retryDelayMs;
                ESP_LOGI(TAG, "Scheduling reconnect attempt %d in %lu ms", currentRetryCount + 1, (unsigned long)delay);
                if (retryTimer) {
                    esp_timer_start_once(retryTimer, delay * 1000ULL);
                } else {
                    esp_wifi_connect();
                }
                currentRetryCount++;
                retryDelayMs = std::min(retryDelayMs * 2, (uint32_t)16000);
            } else if (currentRetryCount >= maxReconnectRetries) {
                shouldReconnect = false;
                telemetry.state = telemetry.isApActive ? WifiState::ApActive : WifiState::ConnectionFailed;
                ESP_LOGW(TAG, "Exceeded retry limit (%u attempts). Falling back to other known networks...", maxReconnectRetries);
                if (!manualDisconnect) {
                    connectBestKnownNetwork();
                }
            } else {
                telemetry.state = telemetry.isApActive ? WifiState::ApActive : WifiState::Idle;
            }
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {
            uint16_t apCount = 0;
            esp_wifi_scan_get_ap_num(&apCount);
            ESP_LOGI(TAG, "Scan completed: found %u APs", apCount);

            std::vector<wifi_ap_record_t> records;
            if (apCount > 0) {
                records.resize(apCount);
                if (esp_wifi_scan_get_ap_records(&apCount, records.data()) != ESP_OK) {
                    records.clear();
                }
            }

            if (scanCallback) {
                scanCallback(records);
                scanCallback = nullptr;
            }
            isScanning.store(false);
            break;
        }

        case WIFI_EVENT_AP_START: {
            ESP_LOGI(TAG, "WIFI_EVENT_AP_START -> Hotspot '%s' is broadcasting.", telemetry.apSsid);
            telemetry.isApActive = true;
            esp_netif_ip_info_t ipInfo;
            if (apNetif && esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK) {
                esp_ip4addr_ntoa(&ipInfo.ip, telemetry.apIp, sizeof(telemetry.apIp));
            }
            if (telemetry.isConnected && natEnabled) {
                setNatEnabled(true, true);
            }
            break;
        }

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "WIFI_EVENT_AP_STOP");
            telemetry.isApActive = false;
            telemetry.connectedClients = 0;
            clients.clear();
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* evt = (wifi_event_ap_staconnected_t*)eventData;
            telemetry.connectedClients++;

            HotspotClient client;
            memcpy(client.mac, evt->mac, 6);
            client.aid = evt->aid;
            client.connectedTimestamp = esp_timer_get_time() / 1000000;
            clients.push_back(client);

            ESP_LOGI(TAG, "Client connected to SoftAP! MAC: %02x:%02x:%02x:%02x:%02x:%02x (AID=%d, Total=%u)",
                     evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5],
                     evt->aid, telemetry.connectedClients);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* evt = (wifi_event_ap_stadisconnected_t*)eventData;
            if (telemetry.connectedClients > 0) telemetry.connectedClients--;

            for (auto it = clients.begin(); it != clients.end(); ++it) {
                if (memcmp(it->mac, evt->mac, 6) == 0) {
                    clients.erase(it);
                    break;
                }
            }

            ESP_LOGI(TAG, "Client disconnected from SoftAP! MAC: %02x:%02x:%02x:%02x:%02x:%02x (Total=%u)",
                     evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5],
                     telemetry.connectedClients);
            break;
        }

        default:
            break;
    }
}

void WifiManager::handleIpEvent(int32_t eventId, void* eventData) {
    if (eventId == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* evt = (ip_event_got_ip_t*)eventData;
        esp_ip4addr_ntoa(&evt->ip_info.ip, telemetry.staIp, sizeof(telemetry.staIp));
        telemetry.isConnected = true;
        currentRetryCount = 0;
        retryDelayMs = 1000;
        if (retryTimer) {
            esp_timer_stop(retryTimer);
        }
        telemetry.state = telemetry.isApActive ? WifiState::ApStaActive : WifiState::Connected;

        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "Wi-Fi Connected! IP Address: %s", telemetry.staIp);
        ESP_LOGI(TAG, "Netmask: " IPSTR ", Gateway: " IPSTR,
                 IP2STR(&evt->ip_info.netmask), IP2STR(&evt->ip_info.gw));
        ESP_LOGI(TAG, "==================================================");

        // If SoftAP is active, enable IP NAPT routing and synchronize DNS
        if (telemetry.isApActive && natEnabled) {
            setNatEnabled(true, true);
        }

    } else if (eventId == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t* evt = (ip_event_assigned_ip_to_client_t*)eventData;
        char ipStr[16];
        esp_ip4addr_ntoa(&evt->ip, ipStr, sizeof(ipStr));
        ESP_LOGI(TAG, "DHCP leased IP %s to SoftAP client (MAC: %02x:%02x:%02x:%02x:%02x:%02x)",
                 ipStr, evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5]);

        for (auto& client : clients) {
            if (memcmp(client.mac, evt->mac, 6) == 0) {
                safeStrCopy(client.ip, sizeof(client.ip), ipStr);
                break;
            }
        }
    }
}
