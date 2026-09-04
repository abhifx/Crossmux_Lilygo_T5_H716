#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <mbedtls/md.h>
#include <Preferences.h>

// ─────────────────────────────────────────────────────────────────────────────
// PixelPostNetwork
//
// Handles all ESP-NOW communication for the Pixel Post Controller.
// Every outgoing message is signed with HMAC-SHA256 and carries a timestamp
// so that receivers can reject replayed or tampered packets.
//
// Packet wire format (max 250 bytes ESP-NOW limit):
//   [HMAC  – 32 bytes]
//   [epoch – 4 bytes, little-endian Unix seconds]
//   [data  – 1..200 bytes]
//
// Message IDs must match the firmware running on the pixel posts.
// ─────────────────────────────────────────────────────────────────────────────

class PixelPostNetwork {
public:
    // ── Message type IDs ──────────────────────────────────────────────────────
    static constexpr uint8_t MSG_TAPPED           = 201;
    static constexpr uint8_t MSG_SELECT_EFFECT    = 202;
    static constexpr uint8_t MSG_MOVE             = 203;
    static constexpr uint8_t MSG_SLIDER           = 204;
    static constexpr uint8_t MSG_CHG_WIFI_CHANNEL = 205;
    static constexpr uint8_t MSG_TURN_OFF         = 206;

    // ── Wi-Fi channel table (index 0-2) ───────────────────────────────────────
    static constexpr int CHANNEL_COUNT = 3;
    static const int WIFI_CHANNELS[CHANNEL_COUNT];  // { 1, 6, 11 }

    // hmac_key must be 32 bytes and must match the key burned into the posts.
    explicit PixelPostNetwork(const uint8_t hmac_key[32]);

    // Initialise Wi-Fi (STA + LR), ESP-NOW, and load saved channel preference.
    // Call once in setup() after Serial is ready.
    bool begin();

    // Re-initialise ESP-NOW and restore the saved channel after a WiFi
    // stop/start cycle (e.g. waking from light sleep).  Call after
    // esp_wifi_start() returns.
    bool reinit();

    // Supply the current Unix epoch (seconds) so every packet carries a fresh
    // timestamp.  Call after reading the RTC at boot.
    void setEpoch(uint32_t epoch);

    // ── UI event senders ──────────────────────────────────────────────────────

    // An effect button was pressed.
    //   button : 0-5  (position in the current page)
    //   page   : 0-6  (active menu page)
    // The global effect index broadcast = button + page * 6.
    void sendEffectSelect(uint8_t button, uint8_t page);

    // Initial press on the trackpad.
    void sendTapped();

    // Finger position on the trackpad.
    //   x, y : 0-255 (scaled from display coords)
    //   down : true while finger is touching
    void sendMove(uint8_t x, uint8_t y, bool down);

    // Brightness slider changed.
    //   value : 0-255
    //   down  : true while finger is on the slider
    void sendSlider(uint8_t value, bool down);

    // Broadcast power-off command (sent three times for reliability).
    void sendTurnOff();

    // Change the ESP-NOW Wi-Fi channel.
    //   channelIndex : 0-2, selects from WIFI_CHANNELS[]
    // Broadcasts the change on every channel so all receivers hear it,
    // then settles on the new channel and persists the choice.
    void setWifiChannel(uint8_t channelIndex);

    uint8_t getWifiChannelIndex() const { return _channel_idx; }
    int     getWifiChannel()      const { return WIFI_CHANNELS[_channel_idx]; }

private:
    uint8_t    _hmac_key[32];
    uint32_t   _epoch      = 0;
    uint8_t    _channel_idx = 0;
    Preferences _prefs;

    static const uint8_t BROADCAST_MAC[6];

    // Sign payload with HMAC-SHA256 and send via ESP-NOW.
    esp_err_t _send(const uint8_t *data, size_t len);
};
