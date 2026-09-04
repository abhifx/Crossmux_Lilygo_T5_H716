#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <functional>

// Maximum message text length (not including null terminator)
#define MESSENGER_MAX_TEXT 200

struct MessengerMessage {
    uint8_t sender_mac[6];
    char    text[MESSENGER_MAX_TEXT + 1];
    bool    is_mine;
};

// ─────────────────────────────────────────────────────────────────────────────
// MessengerNetwork
//
// Minimal ESP-NOW broadcast layer for peer-to-peer text messaging.
// No encryption or authentication — suitable for local use.
//
// Packet wire format (max 250-byte ESP-NOW payload):
//   [0x4D]           — 1 byte  : message type marker
//   [sender_mac]     — 6 bytes : sender's station MAC
//   [text]           — 1..201  : null-terminated UTF-8 string
//
// The receive callback is invoked from the Wi-Fi task (not the Arduino loop),
// so the caller must handle thread safety when sharing state with loop().
// ─────────────────────────────────────────────────────────────────────────────

class MessengerNetwork {
public:
    using RecvCallback = std::function<void(const MessengerMessage &)>;

    MessengerNetwork() = default;

    // Initialise Wi-Fi (STA + LR mode) and ESP-NOW on the given channel.
    // Call once in setup() after Serial is ready.
    bool begin(int wifi_channel = 1);

    // Send a text message to all peers on the broadcast address.
    // Returns true if esp_now_send accepted the packet.
    bool send(const char *text);

    // Register a callback invoked when a message arrives from another device.
    // Called from the Wi-Fi task — protect shared state with a mutex/spinlock.
    void onReceive(RecvCallback cb) { _recv_cb = cb; }

    // Fill mac[6] with this device's station MAC address.
    void getOwnMac(uint8_t mac[6]) const { memcpy(mac, _own_mac, 6); }

private:
    static constexpr uint8_t MSG_TYPE = 0x4D;
    static const uint8_t     BROADCAST_MAC[6];

    uint8_t      _own_mac[6] = {};
    RecvCallback _recv_cb;

    static MessengerNetwork *_instance;
    static void _on_recv(const esp_now_recv_info_t *info,
                         const uint8_t *data, int len);
};
