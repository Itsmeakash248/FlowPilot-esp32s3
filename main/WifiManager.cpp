#include "WifiManager.hpp"
#include <esp_log.h>
#include <esp_phy_init.h>
#include <nvs_flash.h>
#include <cstring>

static const char* TAG = "WIFI_MGR";

WifiManager& WifiManager::getInstance() {
    static WifiManager instance;
    return instance;
}

WifiManager::WifiManager()
    : isInitialized(false)
    , isStarted(false)
    , staNetif(nullptr)
    , apNetif(nullptr)
    , maxReconnectRetries(5)
    , currentRetryCount(0)
{
}

WifiManager::~WifiManager() {
    stop();
}

esp_err_t WifiManager::init() {
    if (isInitialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing NVS and TCP/IP stack...");

    // 1. Initialize Non-Volatile Storage (required by Wi-Fi driver)
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
    // This stops ESP-IDF's esp_phy_load_cal_and_init from falling back to full RF calibration,
    // which spikes USB bus current (>500mA) and causes hardware power-cycles on DevKits.
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

    // 3. Initialize Wi-Fi Driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 4. Register Wi-Fi & IP event listeners
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &eventHandler, this, nullptr));

    isInitialized = true;
    telemetry.state = WifiState::Idle;
    telemetry.mode = WifiMode::Off;

    ESP_LOGI(TAG, "Wi-Fi subsystem initialized successfully.");
    return ESP_OK;
}

esp_err_t WifiManager::startAP(const char* ssid, const char* password, uint8_t channel, uint8_t maxConnections) {
    if (!isInitialized) {
        esp_err_t ret = init();
        if (ret != ESP_OK) return ret;
    }

    ESP_LOGI(TAG, "Starting SoftAP Hotspot: SSID='%s', Ch=%u, MaxClients=%u", ssid, channel, maxConnections);

    // Create AP network interface if not already created
    if (!apNetif) {
        apNetif = esp_netif_create_default_wifi_ap();
    }

    wifi_mode_t targetMode = (telemetry.mode == WifiMode::STA || telemetry.mode == WifiMode::AP_STA)
                                 ? WIFI_MODE_APSTA
                                 : WIFI_MODE_AP;

    ESP_ERROR_CHECK(esp_wifi_set_mode(targetMode));

    wifi_config_t apConfig = {};
    strncpy((char*)apConfig.ap.ssid, ssid, sizeof(apConfig.ap.ssid) - 1);
    apConfig.ap.ssid_len = strlen(ssid);
    apConfig.ap.channel = channel;
    apConfig.ap.max_connection = maxConnections;

    if (password && strlen(password) >= 8) {
        strncpy((char*)apConfig.ap.password, password, sizeof(apConfig.ap.password) - 1);
        apConfig.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (password && strlen(password) > 0) {
            ESP_LOGW(TAG, "Password '%s' < 8 chars. Falling back to OPEN hotspot.", password);
        }
        apConfig.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apConfig));

    if (!isStarted) {
        ESP_ERROR_CHECK(esp_wifi_start());
        isStarted = true;
        esp_wifi_set_max_tx_power(40); // 10 dBm to keep within USB power budget
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

    strncpy(telemetry.apSsid, ssid, sizeof(telemetry.apSsid) - 1);
    telemetry.isApActive = true;
    telemetry.mode = (targetMode == WIFI_MODE_APSTA) ? WifiMode::AP_STA : WifiMode::AP;
    telemetry.state = (targetMode == WIFI_MODE_APSTA && telemetry.isConnected)
                          ? WifiState::ApStaActive
                          : WifiState::ApActive;

    // Fetch and cache AP IP address
    esp_netif_ip_info_t ipInfo;
    if (apNetif && esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK) {
        esp_ip4addr_ntoa(&ipInfo.ip, telemetry.apIp, sizeof(telemetry.apIp));
        ESP_LOGI(TAG, "SoftAP ready. Gateway IP: %s", telemetry.apIp);
    }

    return ESP_OK;
}

esp_err_t WifiManager::startSTA(const char* ssid, const char* password, uint8_t maxRetries) {
    if (!isInitialized) {
        esp_err_t ret = init();
        if (ret != ESP_OK) return ret;
    }

    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID provided for Station mode!");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting Station mode to connect to: '%s'", ssid);

    maxReconnectRetries = maxRetries;
    currentRetryCount = 0;

    if (!staNetif) {
        staNetif = esp_netif_create_default_wifi_sta();
    }

    wifi_mode_t targetMode = (telemetry.isApActive) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(targetMode));

    wifi_config_t staConfig = {};
    strncpy((char*)staConfig.sta.ssid, ssid, sizeof(staConfig.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char*)staConfig.sta.password, password, sizeof(staConfig.sta.password) - 1);
        staConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        staConfig.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &staConfig));

    if (!isStarted) {
        ESP_ERROR_CHECK(esp_wifi_start());
        isStarted = true;
        esp_wifi_set_max_tx_power(60); // 15 dBm to avoid peak current spikes
    } else {
        esp_wifi_connect();
    }

    strncpy(telemetry.staSsid, ssid, sizeof(telemetry.staSsid) - 1);
    telemetry.mode = (targetMode == WIFI_MODE_APSTA) ? WifiMode::AP_STA : WifiMode::STA;
    telemetry.state = WifiState::Connecting;

    return ESP_OK;
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

esp_err_t WifiManager::disconnectSTA() {
    if (!isStarted) return ESP_OK;

    ESP_LOGI(TAG, "Disconnecting station...");
    esp_err_t ret = esp_wifi_disconnect();
    telemetry.isConnected = false;
    telemetry.staIp[0] = '\0';
    if (telemetry.isApActive) {
        telemetry.state = WifiState::ApActive;
    } else {
        telemetry.state = WifiState::Idle;
    }
    return ret;
}

esp_err_t WifiManager::stop() {
    if (!isStarted) return ESP_OK;

    ESP_LOGI(TAG, "Stopping Wi-Fi radio...");
    esp_err_t ret = esp_wifi_stop();
    isStarted = false;
    telemetry.mode = WifiMode::Off;
    telemetry.state = WifiState::Idle;
    telemetry.isConnected = false;
    telemetry.isApActive = false;
    telemetry.staIp[0] = '\0';
    telemetry.apIp[0] = '\0';
    telemetry.connectedClients = 0;
    return ret;
}

int8_t WifiManager::getRssi() {
    if (!telemetry.isConnected) return 0;
    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
        telemetry.rssi = apInfo.rssi;
    }
    return telemetry.rssi;
}

void WifiManager::eventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    WifiManager* self = static_cast<WifiManager*>(arg);
    if (eventBase == WIFI_EVENT) {
        self->handleWifiEvent(eventId, eventData);
    } else if (eventBase == IP_EVENT) {
        self->handleIpEvent(eventId, eventData);
    }
}

void WifiManager::handleWifiEvent(int32_t eventId, void* eventData) {
    switch (eventId) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> Initiating Wi-Fi connection...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED to '%s'", telemetry.staSsid);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            telemetry.isConnected = false;
            telemetry.staIp[0] = '\0';

            if (maxReconnectRetries == 0 || currentRetryCount < maxReconnectRetries) {
                currentRetryCount++;
                ESP_LOGW(TAG, "Wi-Fi disconnected. Reconnecting (%u/%u)...",
                         currentRetryCount, maxReconnectRetries);
                telemetry.state = WifiState::Connecting;
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Failed to connect to '%s' after %u attempts.",
                         telemetry.staSsid, maxReconnectRetries);
                telemetry.state = telemetry.isApActive ? WifiState::ApActive : WifiState::ConnectionFailed;
            }
            break;
        }

        case WIFI_EVENT_AP_START: {
            ESP_LOGI(TAG, "WIFI_EVENT_AP_START -> Hotspot '%s' active.", telemetry.apSsid);
            telemetry.isApActive = true;
            esp_netif_ip_info_t ipInfo;
            if (apNetif && esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK) {
                esp_ip4addr_ntoa(&ipInfo.ip, telemetry.apIp, sizeof(telemetry.apIp));
            }
            break;
        }

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "WIFI_EVENT_AP_STOP");
            telemetry.isApActive = false;
            telemetry.connectedClients = 0;
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* evt = (wifi_event_ap_staconnected_t*)eventData;
            telemetry.connectedClients++;
            ESP_LOGI(TAG, "Client joined hotspot! MAC: %02x:%02x:%02x:%02x:%02x:%02x (AID=%d, Total=%u)",
                     evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5],
                     evt->aid, telemetry.connectedClients);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* evt = (wifi_event_ap_stadisconnected_t*)eventData;
            if (telemetry.connectedClients > 0) telemetry.connectedClients--;
            ESP_LOGI(TAG, "Client disconnected from hotspot! MAC: %02x:%02x:%02x:%02x:%02x:%02x (Total=%u)",
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
        telemetry.state = telemetry.isApActive ? WifiState::ApStaActive : WifiState::Connected;

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "Wi-Fi Connected! IP Address: %s", telemetry.staIp);
        ESP_LOGI(TAG, "Netmask: " IPSTR ", Gateway: " IPSTR,
                 IP2STR(&evt->ip_info.netmask), IP2STR(&evt->ip_info.gw));
        ESP_LOGI(TAG, "========================================");
    } else if (eventId == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t* evt = (ip_event_assigned_ip_to_client_t*)eventData;
        ESP_LOGI(TAG, "DHCP leased IP " IPSTR " to hotspot client", IP2STR(&evt->ip));
    }
}
