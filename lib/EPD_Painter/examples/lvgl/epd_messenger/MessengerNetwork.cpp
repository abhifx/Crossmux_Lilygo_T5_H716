#include "MessengerNetwork.h"
#include "esp_private/wifi.h"

// ── Static data ───────────────────────────────────────────────────────────────

const uint8_t  MessengerNetwork::BROADCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
MessengerNetwork *MessengerNetwork::_instance = nullptr;

// ── begin ─────────────────────────────────────────────────────────────────────

bool MessengerNetwork::begin(int wifi_channel) {
    _instance = this;

    WiFi.mode(WIFI_MODE_STA);
    WiFi.disconnect();
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);

    esp_wifi_get_mac(WIFI_IF_STA, _own_mac);

    if (esp_now_init() != ESP_OK) {
        Serial.println("MessengerNetwork: esp_now_init failed");
        return false;
    }

    // Fix PHY rate to 1 Mbps long-preamble — lowest available, maximises range
    esp_err_t rate_err = esp_wifi_internal_set_fix_rate(WIFI_IF_STA, true, WIFI_PHY_RATE_LORA_250K);
    if (rate_err != ESP_OK)
        Serial.printf("MessengerNetwork: set_fix_rate warn %d\n", rate_err);

    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("MessengerNetwork: esp_now_add_peer failed");
        return false;
    }

    esp_now_register_recv_cb(_on_recv);

    Serial.printf("MessengerNetwork: ready  MAC=%02X:%02X:%02X:%02X:%02X:%02X  ch=%d  phy=LoRa_250K\n",
                  _own_mac[0], _own_mac[1], _own_mac[2],
                  _own_mac[3], _own_mac[4], _own_mac[5],
                  wifi_channel);
    return true;
}

// ── send ──────────────────────────────────────────────────────────────────────

bool MessengerNetwork::send(const char *text) {
    size_t tlen = strlen(text);
    if (tlen == 0 || tlen > MESSENGER_MAX_TEXT) return false;

    // [type(1)] [mac(6)] [text+nul(tlen+1)]
    const size_t plen = 1 + 6 + tlen + 1;
    uint8_t packet[plen];
    packet[0] = MSG_TYPE;
    memcpy(packet + 1, _own_mac, 6);
    memcpy(packet + 7, text, tlen + 1);

    esp_err_t err = esp_now_send(BROADCAST_MAC, packet, plen);
    Serial.printf("MessengerNetwork: send len=%u err=%d\n", (unsigned)plen, err);
    return err == ESP_OK;
}

// ── _on_recv (static) ─────────────────────────────────────────────────────────

void MessengerNetwork::_on_recv(const esp_now_recv_info_t * /*info*/,
                                const uint8_t *data, int len) {
    if (!_instance || !_instance->_recv_cb) return;
    if (len < 8) return;                    // too short: type + mac + ≥1 char + nul
    if (data[0] != MSG_TYPE) return;        // not our packet type

    MessengerMessage msg;
    memcpy(msg.sender_mac, data + 1, 6);

    // Ignore our own broadcast echoes (shouldn't happen, but be safe)
    if (memcmp(msg.sender_mac, _instance->_own_mac, 6) == 0) return;

    int tlen = len - 7;                     // bytes remaining after type + mac
    if (tlen <= 0 || tlen > MESSENGER_MAX_TEXT + 1) return;
    memcpy(msg.text, data + 7, tlen);
    msg.text[MESSENGER_MAX_TEXT] = '\0';    // always null-terminate
    msg.is_mine = false;

    _instance->_recv_cb(msg);
}
