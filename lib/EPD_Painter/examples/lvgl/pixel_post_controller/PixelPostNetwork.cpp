#include "PixelPostNetwork.h"

// ── Static data ───────────────────────────────────────────────────────────────

const uint8_t PixelPostNetwork::BROADCAST_MAC[6]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const int     PixelPostNetwork::WIFI_CHANNELS[3]  = { 1, 6, 11 };

// ── Construction ──────────────────────────────────────────────────────────────

PixelPostNetwork::PixelPostNetwork(const uint8_t hmac_key[32]) {
    memcpy(_hmac_key, hmac_key, 32);
}

// ── begin ─────────────────────────────────────────────────────────────────────

bool PixelPostNetwork::begin() {
    _prefs.begin("pixel-post", false);
    _channel_idx = (uint8_t)_prefs.getUInt("wifi-channel", 0);
    if (_channel_idx >= CHANNEL_COUNT) _channel_idx = 0;

    WiFi.mode(WIFI_MODE_STA);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("PixelPostNetwork: esp_now_init failed");
        return false;
    }

    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("PixelPostNetwork: esp_now_add_peer failed");
        return false;
    }

    esp_wifi_set_channel(WIFI_CHANNELS[_channel_idx], WIFI_SECOND_CHAN_NONE);
    Serial.printf("PixelPostNetwork: ready  MAC=%llX  channel=%d\n",
                  ESP.getEfuseMac(), WIFI_CHANNELS[_channel_idx]);
    return true;
}

// ── reinit ────────────────────────────────────────────────────────────────────

bool PixelPostNetwork::reinit() {
    esp_now_deinit();  // harmless if already down

    if (esp_now_init() != ESP_OK) {
        Serial.println("PixelPostNetwork: esp_now_init failed (reinit)");
        return false;
    }

    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("PixelPostNetwork: esp_now_add_peer failed (reinit)");
        return false;
    }

    esp_wifi_set_channel(WIFI_CHANNELS[_channel_idx], WIFI_SECOND_CHAN_NONE);
    Serial.printf("PixelPostNetwork: reinit  channel=%d\n", WIFI_CHANNELS[_channel_idx]);
    return true;
}

// ── setEpoch ──────────────────────────────────────────────────────────────────

void PixelPostNetwork::setEpoch(uint32_t epoch) {
    _epoch = epoch;
}

// ── sendEffectSelect ──────────────────────────────────────────────────────────

void PixelPostNetwork::sendEffectSelect(uint8_t button, uint8_t page) {
    uint8_t effectIdx = button + page * 6;
    Serial.printf("PixelPostNetwork: sendEffectSelect  button=%d  page=%d  idx=%d\n",
                  button, page, effectIdx);
    uint8_t msg[] = { MSG_SELECT_EFFECT, effectIdx };
    esp_err_t err = _send(msg, sizeof(msg));
    Serial.printf("PixelPostNetwork: _send → %d\n", err);
}

// ── sendTapped ────────────────────────────────────────────────────────────────

void PixelPostNetwork::sendTapped() {
    Serial.println("PixelPostNetwork: sendTapped");
    uint8_t msg[] = { MSG_TAPPED };
    _send(msg, sizeof(msg));
}

// ── sendMove ──────────────────────────────────────────────────────────────────

void PixelPostNetwork::sendMove(uint8_t x, uint8_t y, bool down) {
    Serial.printf("PixelPostNetwork: sendMove  x=%d  y=%d  down=%d\n", x, y, (int)down);
    uint8_t msg[] = { MSG_MOVE, x, y, (uint8_t)down };
    _send(msg, sizeof(msg));
}

// ── sendSlider ────────────────────────────────────────────────────────────────

void PixelPostNetwork::sendSlider(uint8_t value, bool down) {
    Serial.printf("PixelPostNetwork: sendSlider  value=%d  down=%d\n", value, (int)down);
    uint8_t msg[] = { MSG_SLIDER, value, (uint8_t)down };
    _send(msg, sizeof(msg));
}

// ── sendTurnOff ───────────────────────────────────────────────────────────────

void PixelPostNetwork::sendTurnOff() {
    Serial.println("PixelPostNetwork: sendTurnOff");
    uint8_t msg[] = { MSG_TURN_OFF };
    // Send three times: e-paper refresh means we can't rely on a single packet
    _send(msg, sizeof(msg));
    delay(30);
    _send(msg, sizeof(msg));
    delay(30);
    _send(msg, sizeof(msg));
}

// ── setWifiChannel ────────────────────────────────────────────────────────────

void PixelPostNetwork::setWifiChannel(uint8_t channelIndex) {
    if (channelIndex >= CHANNEL_COUNT) return;

    _channel_idx = channelIndex;
    uint8_t msg[] = { MSG_CHG_WIFI_CHANNEL, channelIndex };

    // Transmit on all three channels so any receiver hears it regardless of
    // which channel it is currently on.
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        esp_wifi_set_channel(WIFI_CHANNELS[i], WIFI_SECOND_CHAN_NONE);
        delay(50);
        _send(msg, sizeof(msg));
        delay(50);
    }

    // Settle on the newly chosen channel
    esp_wifi_set_channel(WIFI_CHANNELS[_channel_idx], WIFI_SECOND_CHAN_NONE);
    _prefs.putUInt("wifi-channel", _channel_idx);
    Serial.printf("PixelPostNetwork: channel → %d\n", WIFI_CHANNELS[_channel_idx]);
}

// ── _send (private) ───────────────────────────────────────────────────────────

esp_err_t PixelPostNetwork::_send(const uint8_t *data, size_t len) {
    if (len == 0 || len > 200) return ESP_ERR_INVALID_ARG;

    // Build: [HMAC 32][timestamp 4][data len]
    const size_t TOTAL = 32 + 4 + len;
    uint8_t packet[TOTAL];

    uint32_t ts = _epoch + millis() / 1000;
    memcpy(packet + 32, &ts, 4);
    memcpy(packet + 36, data, len);

    // HMAC-SHA256 over (timestamp || data)
    uint8_t hmac[32];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md, 1);
    mbedtls_md_hmac_starts(&ctx, _hmac_key, 32);
    mbedtls_md_hmac_update(&ctx, packet + 32, 4 + len);  // timestamp + data
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    memcpy(packet, hmac, 32);

    return esp_now_send(BROADCAST_MAC, packet, TOTAL);
}
