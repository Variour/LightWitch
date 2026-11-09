#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// The WiFi radio, in one place: driver and network interface bring-up, the
// station connection, the softAP and the transmit power, on esp_wifi,
// esp_netif and esp_event. begin() starts the radio once; the station
// interface stays on from then on. ESP-NOW (src/mesh/, src/game/, src/fleet/)
// shares the radio and sets channel, power save and promiscuous mode on the
// driver itself.
//
// The station connects when connect() says so. A dropped or failed connection
// stays down, with one exception: the first drop after boot gets one new
// attempt with the same network.
//
// The driver keeps the station and softAP settings in its own NVS area.
class WifiRadio {
   public:
    // Initialises lwIP, the default event loop and the driver, creates the
    // default station and softAP network interfaces and starts the radio as a
    // station, or as station + softAP. The station's DHCP hostname is
    // "<chip>-<last three MAC bytes>".
    static bool begin(bool withAp);

    // Station: stores the network and starts connecting to it. The result
    // shows in connected().
    static bool connect(const char* ssid, const char* password);
    // Station: leaves the network, or stops connecting to it. Returns once
    // the station is no longer associated, after 100 ms at the latest.
    static bool disconnect();
    // Station: associated and an address received from DHCP. A disconnect
    // with reason AUTH_EXPIRE leaves the value as it is.
    static bool connected();
    // Station: associated and holding an address.
    static bool associatedWithAddress();
    static IPAddress localIp();

    // SoftAP: switches the softAP interface on and gives it this SSID,
    // WPA2 password (empty: open) and channel, for up to four clients.
    static bool startAp(const char* ssid, const char* password, uint8_t channel);
    // SoftAP: replaces its SSID and password with empty ones; the interface
    // stays on.
    static bool clearAp();
    // SoftAP: clearAp(), then the interface goes off.
    static bool stopAp();
    static bool apEnabled();
    static IPAddress apIp();

    // Applies the build's maximum transmit power (WIFI_TX_POWER, in steps of
    // 0.25 dBm) once the station or the softAP has started.
    static bool setTxPower();

    // Primary channel the radio is on.
    static uint8_t channel();

    // Station MAC address, the device's identity in the mesh.
    static void macAddress(uint8_t mac[6]);
    // The same as "AA:BB:CC:DD:EE:FF".
    static String macString();
};
