#pragma once

#include <cstdint>
#include <string>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

/**
 * @brief Wi-Fi operational modes
 */
enum class WifiMode {
    Off,        ///< Wi-Fi radio powered off
    AP,         ///< SoftAP (Hotspot) mode only
    STA,        ///< Station (connect to router) mode only
    AP_STA      ///< Concurrent SoftAP (Hotspot) + Station mode
};

/**
 * @brief Wi-Fi connection and lifecycle states
 */
enum class WifiState {
    Uninitialized,
    Idle,               ///< Radio ready but disconnected
    Connecting,         ///< Station connecting to AP
    Connected,          ///< Station connected with IP assigned
    ConnectionFailed,   ///< Station exceeded retry limit
    ApActive,           ///< SoftAP Hotspot broadcasting
    ApStaActive         ///< SoftAP active and Station connected
};

/**
 * @brief Lightweight snapshot of Wi-Fi status for display HUD and diagnostics
 */
struct WifiTelemetry {
    WifiMode mode = WifiMode::Off;
    WifiState state = WifiState::Idle;
    bool isConnected = false;           ///< True if STA has obtained an IP
    bool isApActive = false;            ///< True if SoftAP hotspot is broadcasting
    char staIp[16] = {0};               ///< Station IPv4 address (e.g. "192.168.1.100")
    char apIp[16] = {0};                ///< SoftAP IPv4 address (e.g. "192.168.4.1")
    char staSsid[33] = {0};             ///< Target router SSID
    char apSsid[33] = {0};              ///< Hotspot broadcast SSID
    int8_t rssi = 0;                    ///< Station RSSI in dBm
    uint8_t connectedClients = 0;       ///< Number of client devices connected to SoftAP
};

/**
 * @brief Robust Wi-Fi Manager supporting SoftAP (Hotspot), Station (STA),
 *        and Concurrent AP+STA modes with auto-reconnect and HUD telemetry.
 */
class WifiManager {
public:
    static WifiManager& getInstance();

    /**
     * @brief Initialize NVS, Netif stack, and default event loop
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t init();

    /**
     * @brief Start SoftAP (Hotspot) mode
     * @param ssid Broadcast SSID (default: "FlowPilot-AP")
     * @param password WPA2-PSK password (min 8 chars, or nullptr/"" for open hotspot)
     * @param channel Wi-Fi channel (1-13, default: 1)
     * @param maxConnections Max concurrent stations (1-10, default: 4)
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t startAP(const char* ssid = "FlowPilot-AP",
                      const char* password = "flowpilot123",
                      uint8_t channel = 1,
                      uint8_t maxConnections = 4);

    /**
     * @brief Start Station (STA) mode to connect to an existing Wi-Fi network
     * @param ssid Wi-Fi network SSID
     * @param password Wi-Fi password (or nullptr/"" for open networks)
     * @param maxRetries Maximum reconnect attempts before giving up (default: 5, 0 = infinite)
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t startSTA(const char* ssid, const char* password = nullptr, uint8_t maxRetries = 5);

    /**
     * @brief Start concurrent SoftAP (Hotspot) + Station mode
     * @param apSsid Hotspot SSID
     * @param apPassword Hotspot password (min 8 chars or empty for open)
     * @param staSsid Target router SSID to connect to
     * @param staPassword Target router password
     * @param apChannel Hotspot channel
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t startApSta(const char* apSsid = "FlowPilot-AP",
                         const char* apPassword = "flowpilot123",
                         const char* staSsid = nullptr,
                         const char* staPassword = nullptr,
                         uint8_t apChannel = 1);

    /**
     * @brief Disconnect station connection
     */
    esp_err_t disconnectSTA();

    /**
     * @brief Stop Wi-Fi radio and transition to Off mode
     */
    esp_err_t stop();

    // Query Methods
    bool isConnected() const { return telemetry.isConnected; }
    bool isApActive() const { return telemetry.isApActive; }
    WifiMode getMode() const { return telemetry.mode; }
    WifiState getState() const { return telemetry.state; }
    const char* getStaIp() const { return telemetry.staIp; }
    const char* getApIp() const { return telemetry.apIp; }
    const char* getStaSsid() const { return telemetry.staSsid; }
    const char* getApSsid() const { return telemetry.apSsid; }
    int8_t getRssi();
    uint8_t getConnectedClients() const { return telemetry.connectedClients; }
    const WifiTelemetry& getTelemetry() const { return telemetry; }

private:
    WifiManager();
    ~WifiManager();
    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

    bool isInitialized;
    bool isStarted;
    esp_netif_t* staNetif;
    esp_netif_t* apNetif;

    WifiTelemetry telemetry;
    uint8_t maxReconnectRetries;
    uint8_t currentRetryCount;

    // Internal event handler
    static void eventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData);
    void handleWifiEvent(int32_t eventId, void* eventData);
    void handleIpEvent(int32_t eventId, void* eventData);
};
