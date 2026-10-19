#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"

#if CONFIG_LWIP_IPV4_NAPT
extern "C" {
#include "lwip/lwip_napt.h"
}
#endif

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
 * @brief Wi-Fi connection and lifecycle states (FlxOS parity)
 */
enum class WifiState {
    Uninitialized,
    Idle,               ///< Radio ready but disconnected
    Connecting,         ///< Station connecting to AP
    Connected,          ///< Station connected with IP assigned
    ConnectionFailed,   ///< Station exceeded retry limit
    AuthFailed,         ///< Wrong password or handshake failure
    ApActive,           ///< SoftAP Hotspot broadcasting
    ApStaActive         ///< SoftAP active and Station connected
};

/**
 * @brief Stored Wi-Fi credential with metadata (FlxOS WiFiCredential parity)
 */
struct WiFiCredential {
    std::string ssid;
    std::string password;
    bool autoConnect = true;            ///< Auto-connect candidate
    int priority = 0;                  ///< Higher = preferred when multiple known networks visible
    int32_t lastChannel = 0;           ///< Channel of last successful connection (for channel pinning)
    int64_t lastConnectedMs = 0;       ///< Timestamp of last connection
};

/**
 * @brief Connected Hotspot client info (FlxOS HotspotManager parity)
 */
struct HotspotClient {
    uint8_t mac[6] = {0};
    char ip[16] = {0};
    int8_t rssi = 0;
    int aid = 0;
    uint32_t connectedDuration = 0;     ///< Connected duration in seconds
    uint64_t connectedTimestamp = 0;
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
    bool isNatEnabled = false;          ///< True if IP NAPT routing is active
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
};

/**
 * @brief FlowPilot Wi-Fi & Networking Subsystem matching FlxOS Architecture:
 *        - Thread-safe recursive mutex guarding all mode switches and operations
 *        - Safe Wi-Fi mode transitions (WIFI_MODE_NULL/STA/AP/APSTA)
 *        - RAM storage mode (esp_wifi_set_storage(WIFI_STORAGE_RAM)) avoiding flash wear
 *        - Timer-based non-blocking exponential backoff auto-reconnect (1s..16s)
 *        - Auth failure and manual disconnect detection
 *        - IP NAPT (Network Address Translation) router mode on SoftAP
 *        - DNS server option & sync from Station to Hotspot clients
 *        - Channel pinning for rapid reconnections
 *        - Asynchronous network scanning (scan())
 *        - Multi-network credential store & auto-connect to best visible network
 *        - Boot-media auto-provisioning from SD Card (/sdcard/provisioning/wifi or /sdcard/wifi.json)
 *        - Hotspot client tracking and bandwidth/traffic metrics
 */
class WifiManager {
public:
    static WifiManager& getInstance();

    using ScanCallback = std::function<void(const std::vector<wifi_ap_record_t>&)>;

    /**
     * @brief Initialize NVS, Netif stack, default event loop, and timers
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t init();

    /**
     * @brief Thread-safe mode transition (FlxOS ConnectivityManager parity)
     */
    esp_err_t setWifiMode(wifi_mode_t mode, bool autoStart = true);
    std::recursive_mutex& getWifiMutex() { return wifiMutex; }

    /**
     * @brief Start Station (STA) mode to connect to an existing Wi-Fi network
     * @param ssid Wi-Fi network SSID
     * @param password Wi-Fi password (or nullptr/"" for open networks)
     * @param maxRetries Maximum reconnect attempts (default: 5)
     * @param remember Store in multi-network credential store
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t startSTA(const char* ssid, const char* password = nullptr, uint8_t maxRetries = 5, bool remember = true);

    /**
     * @brief User-initiated manual disconnect from station (suppresses auto-reconnect)
     */
    esp_err_t disconnectSTA();

    /**
     * @brief Start SoftAP (Hotspot) mode (FlxOS HotspotManager parity)
     * @param ssid Broadcast SSID (default: "FlowPilot-AP")
     * @param password WPA2-PSK password (min 8 chars, or nullptr/"" for open hotspot)
     * @param channel Wi-Fi channel (1-13, default: 1)
     * @param maxConnections Max concurrent stations (1-10, default: 4)
     * @param hidden Hide SSID broadcast (default: false)
     * @param authMode Wi-Fi auth mode
     * @param maxTxPower Maximum RF transmit power (dBm * 4)
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t startAP(const char* ssid = "FlowPilot-AP",
                      const char* password = "flowpilot123",
                      uint8_t channel = 1,
                      uint8_t maxConnections = 4,
                      bool hidden = false,
                      wifi_auth_mode_t authMode = WIFI_AUTH_WPA2_PSK,
                      int8_t maxTxPower = 60);

    /**
     * @brief Stop SoftAP mode
     */
    esp_err_t stopAP();

    /**
     * @brief Start concurrent SoftAP (Hotspot) + Station mode
     */
    esp_err_t startApSta(const char* apSsid = "FlowPilot-AP",
                         const char* apPassword = "flowpilot123",
                         const char* staSsid = nullptr,
                         const char* staPassword = nullptr,
                         uint8_t apChannel = 1);

    /**
     * @brief Stop all Wi-Fi activity and power down radio
     */
    esp_err_t stop();

    /**
     * @brief Enable/Disable IP NAPT routing on SoftAP (FlxOS parity)
     * @param enabled True to enable NAPT routing between STA and AP
     * @param syncDns Sync DNS from STA connection to DHCP clients
     */
    esp_err_t setNatEnabled(bool enabled, bool syncDns = true);
    bool isNatEnabled() const { return natEnabled; }

    /**
     * @brief Asynchronous Wi-Fi network scan
     */
    esp_err_t scan(ScanCallback callback);

    /**
     * @brief Multi-network credential store operations (FlxOS WiFiCredentialStore parity)
     */
    esp_err_t saveCredential(const WiFiCredential& cred);
    bool loadCredential(const std::string& ssid, WiFiCredential& out) const;
    std::vector<WiFiCredential> loadAllCredentials() const;
    esp_err_t removeCredential(const std::string& ssid);
    void updateLastConnected(const std::string& ssid, int32_t channel = 0);

    /**
     * @brief Scan visible networks and connect to the best known saved network (FlxOS parity)
     */
    esp_err_t connectBestKnownNetwork();

    /**
     * @brief Check SD card boot media for provisioning files (/sdcard/provisioning/wifi or /sdcard/wifi.json)
     * @return true if credentials were found and imported
     */
    bool importFromBootMedia();

    /**
     * @brief Get list of currently connected Hotspot client devices
     */
    std::vector<HotspotClient> getConnectedClients();

    // Query Methods (backwards compatible with RobotFace HUD)
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

    std::recursive_mutex wifiMutex;
    WifiTelemetry telemetry;

    // Reconnection and exponential backoff state (FlxOS parity)
    bool shouldReconnect;
    bool manualDisconnect;
    uint8_t currentRetryCount;
    uint8_t maxReconnectRetries;
    uint32_t retryDelayMs;
    esp_timer_handle_t retryTimer;

    // NAPT & routing
    bool natEnabled;

    // Scanning state
    std::atomic<bool> isScanning;
    ScanCallback scanCallback;

    // Connected clients tracking
    std::vector<HotspotClient> clients;
    uint64_t apStartTime;

    // Boot-media provisioning helpers
    bool importFromDirectory(const char* dir);
    bool importJsonFile(const std::string& path);
    bool importTxtFile(const std::string& path);

    // Internal event handler
    static void eventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData);
    void handleWifiEvent(int32_t eventId, void* eventData);
    void handleIpEvent(int32_t eventId, void* eventData);

    // Timer callback
    static void onRetryTimer(void* arg);
};
