#include "mesh_service.h"
#include "bsp_papermono.h"
#include "epd_driver.h"
#include "storage_manager.h"
#include "ui_engine.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <mbedtls/aes.h>
#include <mbedtls/ccm.h>
#include <mbedtls/sha256.h>
#include <cmath>
#include <algorithm>
#include <climits>

static constexpr const char* TAG = "MonoMesh-Radio";

// Official Meshtastic Default PSK (AES-128 key index 1: "1PG7OiApBqPvdXDLFaDisabled")
static const uint8_t MESHTASTIC_DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

// Meshtastic channel encryption: AES-CTR (AES-128 for 16-byte keys, AES-256 for 32-byte keys)
// Nonce layout: packetId (64-bit LE) || fromNode (32-bit LE) || 32-bit zero block counter
static void aesCtrCryptN(const uint8_t* key, size_t keyLen, uint64_t packetId, uint32_t fromNode,
                         const uint8_t* input, size_t len, uint8_t* output) {
    uint8_t iv[16] = {0};
    memcpy(iv, &packetId, sizeof(uint64_t));
    memcpy(iv + 8, &fromNode, sizeof(uint32_t));

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, (keyLen == 32) ? 256 : 128);

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    mbedtls_aes_crypt_ctr(&aes, len, &nc_off, iv, stream_block, input, output);
    mbedtls_aes_free(&aes);
}

static void aes128CtrCrypt(const uint8_t* key, uint64_t packetId, uint32_t fromNode,
                           const uint8_t* input, size_t len, uint8_t* output) {
    aesCtrCryptN(key, 16, packetId, fromNode, input, len, output);
}

// Meshtastic PKI (X25519 ECDH -> SHA-256 -> AES-256-CCM, 8-byte tag, 12-byte overhead)
// Shared key KDF: SHA256(X25519(local_private, remote_public))
static bool pkiDeriveKey(const uint8_t* peerPubKey, const uint8_t* myPrivKey, uint8_t outKey[32]) {
    if (!peerPubKey || !myPrivKey || !outKey) return false;
    uint8_t sharedSecret[crypto_scalarmult_curve25519_BYTES] = {0};
    if (crypto_scalarmult_curve25519(sharedSecret, myPrivKey, peerPubKey) != 0) {
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, sharedSecret, sizeof(sharedSecret));
    mbedtls_sha256_finish(&sha, outKey);
    mbedtls_sha256_free(&sha);
    return true;
}

// CCM nonce (13 bytes): packetId[0..3] || extraNonce[4..7] || fromNode[8..11] || 0x00
static void pkiBuildNonce(uint8_t nonce[13], uint64_t packetId, uint32_t fromNode, uint32_t extraNonce) {
    memset(nonce, 0, 13);
    memcpy(nonce, &packetId, 4);
    memcpy(nonce + 4, &extraNonce, 4);
    memcpy(nonce + 8, &fromNode, 4);
}

static bool pkiEncryptPayload(const uint8_t key[32], uint64_t packetId, uint32_t fromNode,
                              const uint8_t* plain, size_t len, uint8_t* out) {
    uint32_t extraNonce = (uint32_t)esp_random();
    uint8_t nonce[13];
    pkiBuildNonce(nonce, packetId, fromNode, extraNonce);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_ccm_encrypt_and_tag(&ctx, len, nonce, sizeof(nonce), nullptr, 0, plain, out, out + len, 8);
    }
    mbedtls_ccm_free(&ctx);
    if (rc != 0) return false;
    memcpy(out + len + 8, &extraNonce, 4);
    return true;
}

// Authenticated decryption; inLen includes the 12-byte PKI overhead
static bool pkiDecryptPayload(const uint8_t key[32], uint64_t packetId, uint32_t fromNode,
                              const uint8_t* in, size_t inLen, uint8_t* out, size_t& outLen) {
    if (inLen <= 12) return false;
    size_t cipherLen = inLen - 12;
    uint32_t extraNonce = 0;
    memcpy(&extraNonce, in + cipherLen + 8, 4);
    uint8_t nonce[13];
    pkiBuildNonce(nonce, packetId, fromNode, extraNonce);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_ccm_auth_decrypt(&ctx, cipherLen, nonce, sizeof(nonce), nullptr, 0, in, out, in + cipherLen, 8);
    }
    mbedtls_ccm_free(&ctx);
    if (rc != 0) return false;
    outLen = cipherLen;
    return true;
}

// Sanity check of a decrypted Data message: well formed protobuf with a non-zero portnum.
// Used to reject garbage produced by decrypting with the wrong key.
static bool pbValidateData(const uint8_t* data, size_t len, uint32_t& portnumOut,
                           const uint8_t*& payloadOut, size_t& payloadLenOut) {
    size_t offset = 0;
    uint32_t portnum = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    bool portnumSeen = false;

    while (offset < len) {
        uint64_t key = 0;
        size_t n = 0;
        {
            uint64_t val = 0;
            size_t i = 0;
            int shift = 0;
            while (offset + i < len) {
                uint8_t b = data[offset + i];
                i++;
                val |= ((uint64_t)(b & 0x7F)) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
                if (shift > 64) return false;
            }
            if (i == 0 || i > 10) return false;
            key = val;
            n = i;
        }
        offset += n;
        uint32_t fieldNum = key >> 3;
        uint32_t wireType = key & 0x07;
        if (fieldNum == 0) return false;

        if (wireType == 0) {
            uint64_t val = 0;
            size_t i = 0;
            int shift = 0;
            while (offset + i < len) {
                uint8_t b = data[offset + i];
                i++;
                val |= ((uint64_t)(b & 0x7F)) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
                if (shift > 64) return false;
            }
            if (i == 0 || i > 10) return false;
            offset += i;
            if (fieldNum == 1) { portnum = (uint32_t)val; portnumSeen = true; }
        } else if (wireType == 1) {
            if (offset + 8 > len) return false;
            offset += 8;
        } else if (wireType == 2) {
            uint64_t subLen = 0;
            size_t i = 0;
            int shift = 0;
            while (offset + i < len) {
                uint8_t b = data[offset + i];
                i++;
                subLen |= ((uint64_t)(b & 0x7F)) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
                if (shift > 64) return false;
            }
            if (i == 0 || i > 10) return false;
            offset += i;
            if (subLen > len - offset) return false;
            if (fieldNum == 2) { payload = data + offset; payloadLen = (size_t)subLen; }
            offset += subLen;
        } else if (wireType == 5) {
            if (offset + 4 > len) return false;
            offset += 4;
        } else {
            return false;
        }
    }

    if (!portnumSeen || portnum == 0 || portnum > 300) return false;
    portnumOut = portnum;
    payloadOut = payload;
    payloadLenOut = payloadLen;
    return true;
}

// Meshtastic builds may emit non-ASCII names; this rejects binary garbage in text payloads.
static bool looksLikeText(const uint8_t* data, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = data[i];
        if (c == 0) return false;
        if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) return false;
        if (c == 0x7F) return false;
    }
    return true;
}

static const char* routingErrorName(uint32_t code) {
    switch (code) {
        case 0: return "NONE";
        case 1: return "NO_ROUTE";
        case 2: return "GOT_NAK";
        case 3: return "TIMEOUT";
        case 4: return "NO_INTERFACE";
        case 5: return "MAX_RETRANSMIT";
        case 6: return "NO_CHANNEL";
        case 7: return "TOO_LARGE";
        case 8: return "NO_RESPONSE";
        case 9: return "DUTY_CYCLE";
        case 32: return "BAD_REQUEST";
        case 33: return "NOT_AUTHORIZED";
        case 34: return "PKI_FAILED";
        case 35: return "PKI_UNKNOWN_KEY";
        case 36: return "ADMIN_BAD_SESSION_KEY";
        case 37: return "ADMIN_KEY_UNAUTHORIZED";
        case 38: return "RATE_LIMIT";
        case 39: return "PKI_NO_PUBKEY";
        default: return "ROUTING_ERROR";
    }
}

// Handle of the core-0 radio task, notified from the DIO1 ISR so the FIFO is drained immediately
static volatile TaskHandle_t s_radioTaskHandle = nullptr;

// Upper bound for the managed-flooding relay queue: bounds RAM on a busy mesh (one entry is ~272 B)
static constexpr size_t MAX_PENDING_RELAYS = 12;

static void IRAM_ATTR onRadioDio1Action() {
    if (s_radioTaskHandle) {
        BaseType_t higherPriWoken = pdFALSE;
        vTaskNotifyGiveFromISR(s_radioTaskHandle, &higherPriWoken);
        if (higherPriWoken) portYIELD_FROM_ISR();
    }
}

namespace MonoMesh {

// Meshtastic Header
struct __attribute__((packed)) MeshtasticHeader {
    uint32_t to;
    uint32_t from;
    uint32_t id;
    uint8_t flags;
    uint8_t channel;
    uint8_t next_hop;
    uint8_t relay_node;
};

// Protobuf helper: write varint
static size_t pbWriteVarint(uint8_t* buf, uint64_t val) {
    size_t i = 0;
    while (val >= 0x80) {
        buf[i++] = (val & 0x7F) | 0x80;
        val >>= 7;
    }
    buf[i++] = val & 0x7F;
    return i;
}

// Protobuf helper: read varint
static size_t pbReadVarint(const uint8_t* buf, size_t maxLen, uint64_t& val) {
    val = 0;
    size_t i = 0;
    int shift = 0;
    while (i < maxLen) {
        uint8_t b = buf[i++];
        val |= ((uint64_t)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) return i;
        shift += 7;
        if (shift > 64) return 0;
    }
    return 0;
}

// Extract Data.request_id (field 6, fixed32) from a decrypted Data protobuf.
// Used to tell a traceroute request (request_id == 0) from its reply.
static uint32_t pbDataRequestId(const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        uint64_t key = 0;
        size_t n = pbReadVarint(data + offset, len - offset, key);
        if (n == 0) break;
        offset += n;
        uint32_t fieldNum = key >> 3;
        uint32_t wireType = key & 0x07;
        if (wireType == 0) {
            uint64_t val = 0;
            size_t n2 = pbReadVarint(data + offset, len - offset, val);
            if (n2 == 0) break;
            offset += n2;
        } else if (wireType == 1) {
            if (offset + 8 > len) break;
            offset += 8;
        } else if (wireType == 5) {
            if (offset + 4 > len) break;
            if (fieldNum == 6) {
                uint32_t v = 0;
                memcpy(&v, data + offset, 4);
                return v;
            }
            offset += 4;
        } else if (wireType == 2) {
            uint64_t subLen = 0;
            size_t n2 = pbReadVarint(data + offset, len - offset, subLen);
            if (n2 == 0) break;
            offset += n2 + (size_t)subLen;
        } else {
            break;
        }
    }
    return 0;
}

// Wall-clock label for the chat bubbles. When the RTC has never been set the bubble used to show a
// bogus 00:00 (which reads as 1970 in the peer's client): "--:--" is honest about it.
static void formatClockLabel(char* out, size_t outLen) {
    if (!BSP::getInstance().isRtcValid()) {
        snprintf(out, outLen, "--:--");
        return;
    }
    int h = 0, m = 0, s = 0;
    BSP::getInstance().getRtcTime(h, m, s);
    snprintf(out, outLen, "%02d:%02d", h, m);
}

MeshService::MeshService() : _spiBus(HSPI) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    _macNodeNum = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    _localNodeNum = _macNodeNum;
    snprintf(_localIdStr, sizeof(_localIdStr), "!%08x", (unsigned int)_localNodeNum);
    snprintf(_localShortName, sizeof(_localShortName), "%02X%02X", mac[4], mac[5]);
    snprintf(_localLongName, sizeof(_localLongName), "MonoMesh-%02X%02X", mac[4], mac[5]);
    // Default primary channel key = official Meshtastic default PSK (psk index 1).
    // Without this a factory-fresh unit would hash/encrypt with an all-zero key and be invisible on the mesh.
    memcpy(_primaryPsk, MESHTASTIC_DEFAULT_KEY, sizeof(MESHTASTIC_DEFAULT_KEY));
    _primaryPskLen = sizeof(MESHTASTIC_DEFAULT_KEY);
}

bool MeshService::init() {
    // 0. Load or generate PKI keypair (X25519)
    _hasLocalKeys = StorageManager::getInstance().getPkiKeyPair(_localPublicKey, _localPrivateKey);
    if (_hasLocalKeys) {
        char hexPub[65] = {0};
        for (int i = 0; i < 32; i++) sprintf(hexPub + i * 2, "%02x", _localPublicKey[i]);
        ESP_LOGI(TAG, "PKI X25519 KeyPair loaded. Public: %.16s...", hexPub);
    } else {
        ESP_LOGW(TAG, "PKI X25519 KeyPair unavailable");
        ESP_LOGW(TAG, "Without a keypair direct messages cannot be decrypted: peers must learn our key again");
    }

    // 1. Load persisted configuration from StorageManager (NVS + SD)
    SettingsConfig cfg = {};
    strncpy(cfg.longName, _localLongName, sizeof(cfg.longName));
    strncpy(cfg.shortName, _localShortName, sizeof(cfg.shortName));
    cfg.frequency = _frequency;
    cfg.txPower = _txPower;
    cfg.modemPreset = static_cast<uint8_t>(_modemPreset);
    cfg.hopLimit = _hopLimit;
    cfg.frontlight = 30;
    cfg.antiGhostThreshold = 0; // automatic anti-ghosting full clear disabled by default
    cfg.nodeRole = static_cast<uint8_t>(_nodeRole);
    cfg.nodeInfoIntervalSec = _nodeInfoIntervalSec;
    cfg.telemetryIntervalSec = _telemetryIntervalSec;
    cfg.pkiEnabled = _pkiEnabled;
    cfg.gpsEnabled = _gpsEnabled;
    cfg.timezoneOffset = _timezoneOffset;

    if (StorageManager::getInstance().loadSettings(cfg)) {
        // Never let an empty stored value wipe the identity: keep the MAC-derived default instead
        if (cfg.longName[0] != '\0') {
            strncpy(_localLongName, cfg.longName, sizeof(_localLongName) - 1);
            _localLongName[sizeof(_localLongName) - 1] = '\0';
        } else {
            ESP_LOGW(TAG, "Stored long name was empty, keeping '%s'", _localLongName);
        }
        if (cfg.shortName[0] != '\0') {
            strncpy(_localShortName, cfg.shortName, sizeof(_localShortName) - 1);
            _localShortName[sizeof(_localShortName) - 1] = '\0';
        } else {
            ESP_LOGW(TAG, "Stored short name was empty, keeping '%s'", _localShortName);
        }
        _frequency = cfg.frequency;
        _txPower = cfg.txPower;
        _modemPreset = static_cast<ModemPreset>(cfg.modemPreset);
        _hopLimit = cfg.hopLimit;
        _frontlight = cfg.frontlight;
        _antiGhostThreshold = cfg.antiGhostThreshold;
        // The counter is disabled by default (0): the automatic full clear used to be dead code
        // because the value was never applied here.
        EPDDriver::getInstance().setAntiGhostingThreshold(_antiGhostThreshold);
        // 0..3 = PowerOff/Standby/Clock/Standby+Clock; anything else (stale blob) falls back to Standby
        _powerBtnAction = (cfg.powerBtnAction <= 3) ? cfg.powerBtnAction : 1;
        _clockLandscape = cfg.clockLandscape;
        _clockFlip180 = cfg.clockFlip180;
        _nodeRole = static_cast<NodeRole>(cfg.nodeRole);
        _nodeInfoIntervalSec = cfg.nodeInfoIntervalSec > 0 ? cfg.nodeInfoIntervalSec : 10800;
        _telemetryIntervalSec = cfg.telemetryIntervalSec > 0 ? cfg.telemetryIntervalSec : 1800;
        // 0 is a valid value here (periodic NeighborInfo disabled)
        _neighborInfoIntervalSec = cfg.neighborInfoIntervalSec;
        _dutyCyclePct = (cfg.dutyCyclePct <= 100) ? cfg.dutyCyclePct : 0;
        _rebroadcastMode = (cfg.rebroadcastMode <= 2) ? static_cast<RebroadcastMode>(cfg.rebroadcastMode)
                                                      : RebroadcastMode::All;
        _pkiEnabled = cfg.pkiEnabled;
        _gpsEnabled = cfg.gpsEnabled;
        _timezoneOffset = cfg.timezoneOffset;
        BSP::getInstance().setLedNotifications(cfg.ledNotifications);
        BSP::getInstance().setBuzzerVolume(cfg.buzzerVolume);
        BSP::getInstance().setBuzzerMode((cfg.buzzerMode <= 4) ? static_cast<BSP::BuzzerMode>(cfg.buzzerMode)
                                                               : BSP::BuzzerMode::All);

        _secondaryChannels.clear();
        for (size_t i = 0; i < cfg.secondaryChannelCount && i < 7; i++) {
            _secondaryChannels.push_back(cfg.secondaryChannels[i]);
        }

        // Identity salt: lets the user escape a stale public key that peers may still hold
        _nodeIdSalt = cfg.nodeIdSalt;
        if (_nodeIdSalt != 0) {
            uint32_t salted = _macNodeNum ^ _nodeIdSalt;
            if (salted == 0 || salted == NODENUM_BROADCAST) salted ^= 0x5A5A5A5A;
            _localNodeNum = salted;
            snprintf(_localIdStr, sizeof(_localIdStr), "!%08x", (unsigned int)_localNodeNum);
            ESP_LOGW(TAG, "Identity salt active -> node number %s", _localIdStr);
        }

        // Primary channel (Ch 0) configuration
        memset(_primaryChannelName, 0, sizeof(_primaryChannelName));
        strncpy(_primaryChannelName, cfg.primaryChannelName, sizeof(_primaryChannelName) - 1);
        _primaryPskLen = (cfg.primaryPskLen > 32) ? 16 : cfg.primaryPskLen;
        memset(_primaryPsk, 0, sizeof(_primaryPsk));
        if (_primaryPskLen > 0) memcpy(_primaryPsk, cfg.primaryPsk, _primaryPskLen);
        _positionIntervalSec = cfg.positionIntervalSec;
        _meshTimeSync = cfg.meshTimeSync;
        BSP::getInstance().setTimezoneOffset(_timezoneOffset);

        // Restore the channel pair learned from peers (survives reboots; TX falls back to our own
        // channel as soon as a peer confirms it again)
        if (cfg.learnedChanValid && cfg.learnedChanKeyLen > 0 && cfg.learnedChanKeyLen <= 32) {
            _learnedHashValid = true;
            _learnedHash = cfg.learnedChanHash;
            _learnedKeyLen = cfg.learnedChanKeyLen;
            memcpy(_learnedKey, cfg.learnedChanKey, cfg.learnedChanKeyLen);
            _learnedCount = cfg.learnedChanHits;
            _learnedLastMillis = millis();
            ESP_LOGI(TAG, "Restored learned channel hash 0x%02x (%u hit) from NVS",
                     (unsigned)_learnedHash, (unsigned)_learnedCount);
        }
        if (!_pkiEnabled) {
            ESP_LOGW(TAG, "PKI disabled by configuration: public key will not be advertised");
        }
        _bootMillis = millis();

        ESP_LOGI(TAG, "Applied persisted settings: Name=[%s / %s], Freq=%.3f MHz, Power=%d dBm, Role=%u, PKI=%u, 2ndChan=%u",
                 _localShortName, _localLongName, _frequency, _txPower, (unsigned)_nodeRole, _pkiEnabled ? 1 : 0, (unsigned)_secondaryChannels.size());
    }

    ESP_LOGI(TAG, "Initializing SX1262 LoRa Radio (SPI3)... Local Node: %s (%s)", _localIdStr, _localLongName);

    _spiBus.begin(Pins::LORA_SCLK, Pins::LORA_MISO, Pins::LORA_MOSI, Pins::LORA_NSS);
    _radioModule = new Module(Pins::LORA_NSS, Pins::LORA_DIO1, RADIOLIB_NC, Pins::LORA_BUSY, _spiBus);
    _radio = new SX1262(_radioModule);

    // Initial begin with loaded parameters
    int state = _radio->begin(_frequency, 250.0, 11, 5, 0x2B, _txPower, 16, 3.0, true);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1262 Radio init failed with code: %d", state);
        return false;
    }

    _radio->setDio2AsRfSwitch(true);
    _radio->setCurrentLimit(140.0);
    _radio->setPacketReceivedAction(onRadioDio1Action);

    // Apply loaded modem preset (bandwidth, SF, CR)
    setModemPreset(_modemPreset);

    state = _radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to start SX1262 receive mode: %d", state);
        return false;
    }

    _radioReady = true;
    _airtimeWindowStart = millis();
    ESP_LOGI(TAG, "SX1262 Radio initialized and listening on %.3f MHz (DIO1=%d, irq=0x%04x)",
             _frequency, digitalRead(Pins::LORA_DIO1), (unsigned)_radio->getIrqFlags());

    // Add local node to NodeDB
    MeshNode self;
    self.nodeNum = _localNodeNum;
    strncpy(self.idStr, _localIdStr, sizeof(self.idStr));
    strncpy(self.longName, _localLongName, sizeof(self.longName));
    strncpy(self.shortName, _localShortName, sizeof(self.shortName));
    self.role = static_cast<uint8_t>(_nodeRole);
    if (_hasLocalKeys) {
        memcpy(self.publicKey, _localPublicKey, 32);
        self.hasPublicKey = true;
    }
    BatteryState bs = BSP::getInstance().getBatteryState();
    self.battery = bs.percentage;
    self.voltage = (float)bs.voltageMv / 1000.0f;
    self.lastHeard = millis();
    _nodes.push_back(self);

    // Restore saved nodes from MicroSD card
    std::vector<NodeRecord> savedRecords;
    if (StorageManager::getInstance().loadNodes(savedRecords)) {
        for (const auto& sr : savedRecords) {
            if (_nodes.size() >= MAX_NODES) break; // never exceed the NodeDB cap on restore
            // Our own record is rebuilt from scratch above, but a position set from the map ("set
            // position here") must survive the reboot: nodes.csv holds it, only this loop dropped it.
            if (sr.nodeNum == _localNodeNum) {
                if (!_nodes.empty() && (sr.lat != 0.0 || sr.lon != 0.0)) {
                    _nodes[0].hasPosition = true;
                    _nodes[0].lat = sr.lat;
                    _nodes[0].lon = sr.lon;
                    _nodes[0].alt = sr.alt;
                    ESP_LOGI(TAG, "Restored local position %.5f, %.5f from storage", sr.lat, sr.lon);
                }
                continue;
            }
            if (!findNode(sr.nodeNum)) {
                MeshNode mn;
                mn.nodeNum = sr.nodeNum;
                strncpy(mn.idStr, sr.idStr, sizeof(mn.idStr));
                strncpy(mn.longName, sr.longName, sizeof(mn.longName));
                strncpy(mn.shortName, sr.shortName, sizeof(mn.shortName));
                mn.lat = sr.lat;
                mn.lon = sr.lon;
                mn.alt = sr.alt;
                mn.hasPosition = (sr.lat != 0.0 || sr.lon != 0.0);
                mn.snr = sr.snr;
                mn.rssi = sr.rssi;
                mn.lastHeard = sr.lastHeard;
                mn.battery = sr.battery;
                mn.voltage = sr.voltage;
                mn.role = sr.role;
                mn.hasPublicKey = sr.hasPublicKey;
                memcpy(mn.publicKey, sr.publicKey, 32);
                mn.hasMac = sr.hasMac;
                memcpy(mn.mac, sr.mac, 6);
                // Restore "last heard" as an absolute instant. Older files only carried a millis()
                // value from a previous boot, which is meaningless now: drop it instead of showing
                // a bogus date.
                if (sr.lastHeardEpoch > 1700000000UL) {
                    mn.lastHeardEpoch = sr.lastHeardEpoch;
                    uint32_t nowE = currentEpoch();
                    if (nowE > sr.lastHeardEpoch) {
                        uint32_t ageMs = (nowE - sr.lastHeardEpoch) * 1000UL;
                        mn.lastHeard = (ageMs < millis()) ? (millis() - ageMs) : 1;
                    } else {
                        mn.lastHeard = millis();
                    }
                } else {
                    mn.lastHeard = 0;
                }
                _nodes.push_back(mn);
            }
        }
        ESP_LOGI(TAG, "Restored %u saved nodes into active NodeDB (cap %u nodes, %u B/node, %u B total)",
                 (unsigned)(_nodes.size() - 1), (unsigned)MAX_NODES, (unsigned)sizeof(MeshNode),
                 (unsigned)(MAX_NODES * sizeof(MeshNode)));
    }

    // Restore saved chat and direct messages
    std::vector<SavedMessage> savedMsgs;
    if (StorageManager::getInstance().loadChatMessages(savedMsgs)) {
        for (const auto& sm : savedMsgs) {
            ChatMessage cm = {};
            cm.id = sm.id;
            cm.fromNode = sm.fromNode;
            cm.toNode = sm.toNode;
            strncpy(cm.senderShort, sm.senderShort, sizeof(cm.senderShort) - 1);
            strncpy(cm.text, sm.text, sizeof(cm.text) - 1);
            cm.timestamp = sm.timestamp;
            cm.isOutgoing = sm.isOutgoing;
            cm.isRead = true;
            cm.isAcked = sm.isAcked;
            cm.isFailed = false;
            strncpy(cm.timeStr, sm.timeStr, sizeof(cm.timeStr) - 1);
            strncpy(cm.channelPreset, sm.channelPreset, sizeof(cm.channelPreset) - 1);
            // Messages saved before the channel label was stored belong to the current preset
            if (cm.channelPreset[0] == '\0' && sm.toNode == NODENUM_BROADCAST) {
                strncpy(cm.channelPreset, getPresetName(_modemPreset), sizeof(cm.channelPreset) - 1);
            }
            cm.seq = ++_globalMsgSeq;
            if (sm.toNode == NODENUM_BROADCAST) {
                _broadcastMessages.push_back(cm);
                if (_broadcastMessages.size() > MAX_MESSAGES) _broadcastMessages.pop_front();
            } else {
                _directMessages.push_back(cm);
                if (_directMessages.size() > MAX_MESSAGES) _directMessages.pop_front();
            }
        }
        ESP_LOGI(TAG, "Restored %u saved messages (Channel: %u, DMs: %u)",
                 (unsigned)savedMsgs.size(), (unsigned)_broadcastMessages.size(), (unsigned)_directMessages.size());
    }

    // First NodeInfo is sent 30s after boot (like official firmware) to let the radio/network settle
    _bootMillis = millis();
    _lastNodeInfoTx = 0;
    _bootNodeInfoSent = false;
    _nodeOrderDirty = true; // sort the restored node list by recency on the first loop

    // Core-0 radio pump: it owns every FIFO read, so incoming packets survive the long e-paper
    // refreshes that block the main loop. Protocol handling stays in the main loop (core 1).
    _rxQueue = xQueueCreate(8, sizeof(RadioRxFrame));
    _radioMutex = xSemaphoreCreateMutex();
    if (_rxQueue && _radioMutex && _radioReady) {
        if (xTaskCreatePinnedToCore(radioTaskEntry, "meshRadio", 4096, this, 4, &_radioTask, 0) == pdPASS) {
            s_radioTaskHandle = _radioTask;
            ESP_LOGI(TAG, "Radio task started on core 0");
        } else {
            ESP_LOGE(TAG, "Radio task creation failed: the main loop will poll the FIFO (in-loop fallback)");
        }
    }

    return true;
}

void MeshService::recordAirtimeTx(size_t packetLen) {
    if (_radio) {
        uint32_t toaUs = _radio->getTimeOnAir(packetLen);
        _accumulatedTxUs += toaUs;
        // Rolling-hour budget for the duty-cycle limiter (see canTransmit()).
        _txAirtimeUs[_airtimeBucketIndex] += toaUs;
        _hourlyAirtimeUs += toaUs;
    }
}

// Advance (and expire) the one-minute buckets. Usually one step, but a millis() gap (long e-paper
// refresh, light-sleep chunks) can be many: handle them in a loop, and reset everything when the
// whole hour has gone by. Unsigned arithmetic keeps this safe across the millis() wrap.
void MeshService::rotateAirtimeBuckets(uint32_t now) {
    if (_airtimeBucketStartMs == 0) {
        _airtimeBucketStartMs = now;
        return;
    }
    uint32_t elapsed = now - _airtimeBucketStartMs;
    if (elapsed < AIRTIME_BUCKET_MS) return;

    uint32_t steps = elapsed / AIRTIME_BUCKET_MS;
    if (steps >= AIRTIME_BUCKETS) {
        memset(_txAirtimeUs, 0, sizeof(_txAirtimeUs));
        _hourlyAirtimeUs = 0;
        _airtimeBucketIndex = 0;
        _airtimeBucketStartMs = now;
        return;
    }
    for (uint32_t i = 0; i < steps; ++i) {
        _airtimeBucketIndex = (_airtimeBucketIndex + 1) % AIRTIME_BUCKETS;
        _hourlyAirtimeUs -= _txAirtimeUs[_airtimeBucketIndex];
        _txAirtimeUs[_airtimeBucketIndex] = 0;
    }
    _airtimeBucketStartMs += steps * AIRTIME_BUCKET_MS;
}

// EU868 (869.40-869.65 MHz) and EU433 are limited to 10% of a rolling hour by ETSI EN 300 220.
// The other regions we support have no software duty-cycle limit.
uint8_t MeshService::getRegionDutyCyclePct() const {
    if (fabsf(_frequency - 869.525f) < 0.05f) return 10;
    if (fabsf(_frequency - 433.175f) < 0.05f) return 10;
    return 0;
}

uint8_t MeshService::getEffectiveDutyCyclePct() const {
    return (_dutyCyclePct == 0) ? getRegionDutyCyclePct() : _dutyCyclePct;
}

float MeshService::getHourlyAirtimePct() const {
    return (float)((double)_hourlyAirtimeUs / (3600.0 * 1000000.0) * 100.0);
}

bool MeshService::canTransmit() const {
    const uint8_t pct = getEffectiveDutyCyclePct();
    if (pct == 0 || pct >= 100) return true;
    const uint64_t budgetUs = (uint64_t)(3600ULL * 1000000ULL) * pct / 100ULL;
    return _hourlyAirtimeUs < budgetUs;
}

void MeshService::updateAirtimeMetrics() {
    uint32_t now = millis();
    rotateAirtimeBuckets(now);
    if (_airtimeWindowStart == 0) {
        _airtimeWindowStart = now;
        return;
    }
    uint32_t elapsedMs = now - _airtimeWindowStart;
    if (elapsedMs >= 60000) {
        uint64_t windowUs = (uint64_t)elapsedMs * 1000ULL;
        _airtimeTx = ((float)_accumulatedTxUs / (float)windowUs) * 100.0f;
        _channelUtilization = ((float)(_accumulatedTxUs + _accumulatedRxUs) / (float)windowUs) * 100.0f;
        if (_airtimeTx > 100.0f) _airtimeTx = 100.0f;
        if (_channelUtilization > 100.0f) _channelUtilization = 100.0f;

        _accumulatedTxUs = 0;
        _accumulatedRxUs = 0;
        _airtimeWindowStart = now;
    }
}

void MeshService::processAckQueue() {
    // Duty cycle exhausted: leave the retries queued (do not burn them and do not lose the ACK
    // tracking) - they will be sent as soon as the rolling window frees some airtime.
    if (!canTransmit()) return;
    uint32_t now = millis();
    for (auto it = _pendingAcks.begin(); it != _pendingAcks.end(); ) {
        if (now - it->lastSentMillis >= it->timeoutMs) {
            if (it->retriesLeft > 0) {
                it->retriesLeft--;
                it->lastSentMillis = now;
                it->timeoutMs = (uint32_t)(it->timeoutMs * 1.4f);
                ESP_LOGI(TAG, "Retrying unacked DM 0x%08x to !%08x (%u retries left)",
                         (unsigned)it->packetId, (unsigned)it->toNode, (unsigned)it->retriesLeft);
                transmitRaw(it->rawPacket, it->rawPacketLen);
                ++it;
            } else {
                ESP_LOGW(TAG, "DM packet 0x%08x to !%08x failed after max retries",
                         (unsigned)it->packetId, (unsigned)it->toNode);
                for (auto& m : _directMessages) {
                    if (m.id == it->packetId && m.isOutgoing) {
                        m.isFailed = true;
                    }
                }
                UIEngine::getInstance().requestFullRefresh();
                it = _pendingAcks.erase(it);
            }
        } else {
            ++it;
        }
    }
}

void MeshService::processRelayQueue() {
    // Relays are the first traffic to sacrifice when the hourly budget is exhausted.
    if (!canTransmit()) return;
    uint32_t now = millis();
    for (auto it = _pendingRelays.begin(); it != _pendingRelays.end(); ) {
        if (now >= it->transmitAtMillis) {
            ESP_LOGI(TAG, "Transmitting relayed packet 0x%08x (len=%u)", (unsigned)it->id, (unsigned)it->len);
            transmitRaw(it->packet, it->len);
            it = _pendingRelays.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- Radio task (core 0): drains the SX1262 FIFO into a queue for the main loop ----

void MeshService::radioTaskEntry(void* arg) {
    static_cast<MeshService*>(arg)->radioTaskLoop();
}

void MeshService::radioTaskLoop() {
    const TickType_t waitTicks = pdMS_TO_TICKS(50); // also a safety net if an edge is missed

    for (;;) {
        ulTaskNotifyTake(pdTRUE, waitTicks);
        // The CLOCK power mode parks the radio in sleep: touching the SPI bus here would read
        // garbage from an unconfigured chip (and waste power on pointless transactions).
        if (!_radioReady || _radioSuspended || !_radio || !_rxQueue) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!lockRadio(200)) continue;
        // Re-check under the lock: suspendRadio() may have parked the chip after the check above,
        // and touching the SPI bus of a sleeping SX1262 would read garbage (and re-arm it in RX).
        if (!_radioSuspended && _radioReady) moveRadioFramesToQueue(4);
        unlockRadio();
    }
}

// Shared FIFO drain: only RX_DONE means there is a frame. DIO1 also pulses for TX_DONE, and reading
// on that stale flag would return our own transmission out of the shared buffer.
void MeshService::moveRadioFramesToQueue(size_t maxFrames) {
    if (!_radio || !_rxQueue) return;

    size_t frames = 0;
    bool needRearm = false;
    while (frames < maxFrames && (_radio->getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE)) {
        size_t len = _radio->getPacketLength();
        if (len == 0 || len > 255) {
            _radio->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
            needRearm = true;
            break;
        }
        RadioRxFrame frame;
        frame.len = (uint16_t)len;
        int state = _radio->readData(frame.data, len);
        if (state == RADIOLIB_ERR_NONE) {
            frame.snr = _radio->getSNR();
            frame.rssi = _radio->getRSSI();
            frame.toaUs = _radio->getTimeOnAir(len);
            if (xQueueSend(_rxQueue, &frame, 0) != pdTRUE) {
                ESP_LOGW(TAG, "RX queue full, dropping frame (len=%u)", (unsigned)len);
            }
        } else {
            ESP_LOGW(TAG, "Radio readData error: %d (len=%u)", state, (unsigned)len);
            needRearm = true;
        }
        ++frames;
    }

    // CRITICAL: re-arm only when the RX state was actually disturbed (read error / bad length).
    // startReceive() restarts the receiver, so calling it on every timeout aborts any packet that is
    // still being demodulated - a MediumFast frame takes 200-800 ms on air, so the node would become
    // deaf while still transmitting normally.
    if (needRearm) {
        _radio->startReceive();
    }
}

bool MeshService::lockRadio(uint32_t timeoutMs) {
    if (!_radioMutex) return true;
    return xSemaphoreTake(_radioMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void MeshService::unlockRadio() {
    if (_radioMutex) xSemaphoreGive(_radioMutex);
}

bool MeshService::hasPendingRadioWork() {
    // Only the radio FIFO matters for "can I sleep now?"; queued frames are processed by the loop
    // and do not need the radio.
    if (_radioSuspended) return false;
    if (_radioReady && _radio && _radioMutex && xSemaphoreTake(_radioMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        bool pending = (_radio->getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) != 0;
        xSemaphoreGive(_radioMutex);
        return pending;
    }
    return false;
}

bool MeshService::hasQueuedFrames() {
    return _rxQueue && uxQueueMessagesWaiting(_rxQueue) > 0;
}

bool MeshService::waitRadioIdle(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (!hasPendingRadioWork()) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return !hasPendingRadioWork();
}

// Park the SX1262 in sleep for the CLOCK power mode. The rail stays on: the chip keeps its
// frequency/preset configuration, so resuming is just standby() + startReceive() instead of a full
// begin() (which would also have to re-run the preset and DIO setup). Fails (returns false, radio
// left in RX) only if the mutex cannot be taken, e.g. a TX is holding it.
bool MeshService::suspendRadio() {
    if (_radioSuspended) return true;
    if (!_radioReady || !_radio) return false;
    // Give a TX in flight the time to finish before parking the chip.
    if (!waitRadioIdle(300) || !lockRadio(1500)) {
        ESP_LOGW(TAG, "Radio suspend skipped: mutex busy (a TX is in flight)");
        return false;
    }
    int state = _radio->sleep();
    _radio->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    _radioSuspended = true;
    unlockRadio();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "SX1262 sleep returned %d (radio may still be drawing RX current)", state);
    } else {
        ESP_LOGI(TAG, "SX1262 parked in sleep (CLOCK mode, rail on, ~uA class)");
    }
    return true;
}

void MeshService::resumeRadio() {
    if (!_radioSuspended) return;
    if (!_radioReady || !_radio) {
        _radioSuspended = false;
        return;
    }
    if (!lockRadio(1500)) {
        // Leave the flag set: the next call (or the next loop iteration) retries.
        ESP_LOGW(TAG, "Radio resume postponed: mutex busy");
        return;
    }
    _radio->standby();
    _radio->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    int state = _radio->startReceive();
    _radioSuspended = false;
    unlockRadio();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1262 resume failed (%d): radio deaf until the next rearm", state);
    } else {
        ESP_LOGI(TAG, "SX1262 resumed continuous RX after CLOCK mode");
    }
}

// Queue a byte-for-byte relay of a packet we could not decrypt (unknown channel / foreign key).
void MeshService::queueOpaqueRelay(const uint8_t* payload, size_t len, uint32_t id, uint8_t hopLimit, float snr) {
    if (!payload || len < sizeof(MeshtasticHeader) || len > 256 || hopLimit == 0) return;
    if (_pendingRelays.size() >= MAX_PENDING_RELAYS) {
        _pendingRelays.erase(_pendingRelays.begin());
    }

    float clampedSnr = (snr < -20.0f) ? -20.0f : (snr > 10.0f ? 10.0f : snr);
    uint32_t cwDelayMs = 300 + (uint32_t)((clampedSnr + 20.0f) * 55.0f) + (esp_random() % 300);

    PendingRelay pr;
    memcpy(pr.packet, payload, len);
    pr.len = len;
    pr.id = id;
    pr.packet[12] = (uint8_t)((pr.packet[12] & 0xF8) | (hopLimit - 1));
    pr.packet[15] = (uint8_t)(_localNodeNum & 0xFF);
    pr.transmitAtMillis = millis() + cwDelayMs;
    _pendingRelays.push_back(pr);
    _opaqueRelays++;
    ESP_LOGI(TAG, "Queued opaque relay 0x%08x in %lu ms (hops left: %u)",
             (unsigned)id, (unsigned long)cwDelayMs, (unsigned)(hopLimit - 1));
}

void MeshService::update() {
    uint32_t now = millis();

    // Keep the node list ordered by most recent activity (local node stays on top)
    if (_nodeOrderDirty) {
        sortNodesByRecency();
    }

    // Once the wall clock becomes usable, stamp the nodes heard before it (their session-relative
    // timestamps would otherwise be lost at the next save).
    if (!_epochBackfilled) {
        uint32_t e = currentEpoch();
        if (e > 1700000000UL) {
            _epochBackfilled = true;
            uint32_t nowMs = millis();
            for (auto& n : _nodes) {
                if (n.lastHeardEpoch == 0 && n.lastHeard != 0 && n.lastHeard <= nowMs) {
                    uint32_t ageSec = (nowMs - n.lastHeard) / 1000;
                    if (ageSec < e) n.lastHeardEpoch = e - ageSec;
                }
            }
            _nodeOrderDirty = true;
        }
    }

    // 0. Airtime and Queue updates
    updateAirtimeMetrics();
    processRelayQueue();
    processAckQueue();
    processDeferredSends();

    // A radio reconfiguration that lost the mutex race (a TX holds it for the whole airtime) is
    // retried here, so the chip can never stay on a different frequency/preset than the settings.
    if (_radioConfigDirty && applyRadioConfig()) {
        _radioConfigDirty = false;
        ESP_LOGI(TAG, "Pending radio config applied (%.3f MHz, %s)", _frequency,
                 getPresetCanonicalName(_modemPreset));
        broadcastNodeInfo(false);
    }

    // Reset activity flags after 2 seconds
    if (_rxActive && (now - _rxActivityTimer > 2000)) _rxActive = false;
    if (_txActive && (now - _txActivityTimer > 2000)) _txActive = false;

    // Real fallback when the core-0 task could not be created (RAM): read the FIFO from the loop,
    // otherwise the node would silently never receive anything.
    if (!_radioTask && _radioReady && !_radioSuspended && _radio) {
        if (lockRadio(50)) {
            moveRadioFramesToQueue(4);
            unlockRadio();
        }
    }

    // Drain the frames read by the core-0 radio task. All protocol work stays here, in the loop.
    if (_rxQueue) {
        RadioRxFrame frame;
        int drained = 0;
        while (drained < 8 && xQueueReceive(_rxQueue, &frame, 0) == pdTRUE) {
            ++drained;
            _rxActive = true;
            _rxActivityTimer = now;
            if (frame.toaUs) _accumulatedRxUs += frame.toaUs;
            BSP::getInstance().setLedRxActivity(50);
            ESP_LOGI(TAG, "Packet received! Len=%u, RSSI=%.1f, SNR=%.1f", (unsigned)frame.len,
                     frame.rssi, frame.snr);
            processPacket(frame.data, frame.len, frame.snr, frame.rssi);
        }
    }

    // Periodic RX health summary: makes it obvious from the serial log whether the node is deaf
    // (RX total stuck at 0) or hearing traffic it cannot decrypt (KO growing).
    if ((now - _lastRxSummaryLog) > 60000) {
        _lastRxSummaryLog = now;
        ESP_LOGI(TAG, "RX stats: total=%u ok=%u ko=%u tx=%u nodes=%u",
                 (unsigned)_rxTotal, (unsigned)_rxDecoded, (unsigned)_rxUndecodable, (unsigned)_txTotal,
                 (unsigned)_nodes.size());
    }

    // First NodeInfo announcement 30s after boot (gives the mesh time to settle, like official firmware)
    if (!_bootNodeInfoSent && (now - _bootMillis) > 30000UL) {
        // When the hourly budget is exhausted the flag stays false and the announcement is retried as
        // soon as some airtime frees up (nothing is lost, it is only delayed).
        if (canTransmit()) {
            _bootNodeInfoSent = true;
            _lastBroadcastTime = now;
            // want_response is intentionally NOT set on broadcasts: peers only answer unicast requests,
            // and a broadcast carrying want_response just registers us in their 12h reply-suppression
            // window, blocking the unicast request we later need to fetch their public key.
            ESP_LOGI(TAG, "Initial NodeInfo broadcast");
            broadcastNodeInfo(false);
        }
    }

    // Periodic broadcast of NodeInfo (default 3h, configurable)
    if (_bootNodeInfoSent && (now - _lastBroadcastTime > (_nodeInfoIntervalSec * 1000UL))) {
        if (canTransmit()) {
            _lastBroadcastTime = now;
            broadcastNodeInfo(false);
        }
    }

    // Telemetry: one early announcement so peers can populate battery / ch-util / air-util without
    // waiting a full interval, then the regular periodic broadcast.
    if (!_bootTelemetrySent && (now - _bootMillis) > 45000UL) {
        if (canTransmit()) {
            _bootTelemetrySent = true;
            _lastTelemetryTime = now;
            ESP_LOGI(TAG, "Initial telemetry broadcast (battery + ch/air util)");
            broadcastTelemetry();
        }
    } else if (_bootTelemetrySent && (now - _lastTelemetryTime > (_telemetryIntervalSec * 1000UL))) {
        if (canTransmit()) {
            _lastTelemetryTime = now;
            broadcastTelemetry();
        }
    }

    // Periodic NeighborInfo broadcast (topology for the mesh map; 1h by default)
    if (_neighborInfoIntervalSec > 0 && (now - _lastNeighborInfoTime > (_neighborInfoIntervalSec * 1000UL))) {
        if (canTransmit()) {
            _lastNeighborInfoTime = now;
            broadcastNeighborInfo();
        }
    }

    // Periodic persistence of the discovered node list (keys survive reboots)
    if (_nodeDbDirty && (now - _lastNodeSaveTime > 300000UL)) {
        _lastNodeSaveTime = now;
        std::vector<NodeRecord> records;
        for (const auto& n : _nodes) {
            NodeRecord r = {};
            r.nodeNum = n.nodeNum;
            strncpy(r.idStr, n.idStr, sizeof(r.idStr) - 1);
            strncpy(r.longName, n.longName, sizeof(r.longName) - 1);
            strncpy(r.shortName, n.shortName, sizeof(r.shortName) - 1);
            r.lat = n.lat;
            r.lon = n.lon;
            r.alt = n.alt;
            r.snr = n.snr;
            r.rssi = n.rssi;
            r.lastHeard = n.lastHeard;
            r.battery = n.battery;
            r.voltage = n.voltage;
            r.role = n.role;
            r.hasPublicKey = n.hasPublicKey;
            memcpy(r.publicKey, n.publicKey, 32);
            r.hasMac = n.hasMac;
            memcpy(r.mac, n.mac, 6);
            r.lastHeardEpoch = n.lastHeardEpoch;
            records.push_back(r);
        }
        // Keep the dirty flag if the write failed (e.g. SD unmounted during standby) so the next
        // attempt persists the nodes instead of losing them.
        if (StorageManager::getInstance().saveNodes(records)) {
            _nodeDbDirty = false;
            ESP_LOGI(TAG, "Periodic NodeDB save (%u nodes)", (unsigned)records.size());
        } else {
            _lastNodeSaveTime = now - 240000UL; // retry in ~1 minute
        }
    }

    // Periodic broadcast of our position (only when one is configured)
    if (_positionIntervalSec > 0 && !_nodes.empty() && _nodes[0].hasPosition &&
        (now - _lastPositionTime > (_positionIntervalSec * 1000UL))) {
        if (canTransmit()) {
            _lastPositionTime = now;
            broadcastPosition();
        }
    }
}

bool MeshService::isPacketDuplicate(uint32_t from, uint32_t id) {
    uint32_t now = millis();
    for (size_t i = 0; i < SEEN_PACKETS_MAX; ++i) {
        if (_seenPackets[i].from == from && _seenPackets[i].id == id) {
            if (now - _seenPackets[i].timestamp < 120000) { // 2 minute cache
                return true;
            }
        }
    }
    return false;
}

void MeshService::recordSeenPacket(uint32_t from, uint32_t id) {
    _seenPackets[_seenPacketIndex].from = from;
    _seenPackets[_seenPacketIndex].id = id;
    _seenPackets[_seenPacketIndex].timestamp = millis();
    _seenPacketIndex = (_seenPacketIndex + 1) % SEEN_PACKETS_MAX;
}

void MeshService::processPacket(uint8_t* payload, size_t len, float snr, float rssi) {
    if (len < sizeof(MeshtasticHeader)) return;

    MeshtasticHeader* hdr = reinterpret_cast<MeshtasticHeader*>(payload);
    // A sender of 0xFFFFFFFF is never valid on the wire (official firmware drops it as well):
    // accepting it would create a bogus node in the NodeDB.
    if (hdr->from == NODENUM_BROADCAST) return;

    const uint32_t to = hdr->to;
    const uint32_t from = hdr->from;
    const uint32_t id = hdr->id;
    const uint8_t hopLimit = hdr->flags & 0x07;
    const uint8_t hopStart = (hdr->flags & 0xE0) >> 5;
    const uint8_t hopsTaken = (hopStart >= hopLimit) ? (uint8_t)(hopStart - hopLimit) : 0;
    const bool isBroadcastTo = (to == NODENUM_BROADCAST);
    const bool isToUs = (to == _localNodeNum);

    _rxTotal++;

    // Packets echoed back by the mesh (our own transmission relayed by another node)
    if (from == _localNodeNum) {
        bool found = false;
        for (auto& m : _broadcastMessages) {
            if (m.id == id && m.isOutgoing && !m.isAcked) {
                m.isAcked = true;
                found = true;
                ESP_LOGI(TAG, "Implicit Relay ACK: broadcast message 0x%08x repeated by mesh!", (unsigned)id);
            }
        }
        if (found) {
            StorageManager::getInstance().updateChatMessageAck(id);
            UIEngine::getInstance().requestFullRefresh();
        }
        return;
    }

    // Deduplication + flood suppression
    const bool wantAckFlag = (hdr->flags & 0x08) != 0;
    bool duplicateRetransmit = false;
    if (isPacketDuplicate(from, id)) {
        for (auto it = _pendingRelays.begin(); it != _pendingRelays.end(); ) {
            if (it->id == id) {
                ESP_LOGI(TAG, "Flood suppression: packet 0x%08x retransmitted by mesh, canceling relay", (unsigned)id);
                it = _pendingRelays.erase(it);
            } else {
                ++it;
            }
        }
        // A repeated want_ack unicast addressed to us still needs its ACK: the sender only repeated it
        // because our ACK (and its implicit relay ACK) went missing, and dropping the duplicate made the
        // peer burn all 3 retries and report a DM that we did receive as "failed". Official firmware
        // answers these on purpose (ReliableRouter::sniffReceived: "might generate multiple ack sends in
        // case the first ack gets lost"). The payload itself is still delivered exactly once.
        if (!(wantAckFlag && to == _localNodeNum)) return;
        duplicateRetransmit = true;
        ESP_LOGI(TAG, "Duplicate want_ack packet 0x%08x from !%08x: re-ACK only", (unsigned)id, (unsigned)from);
    } else {
        recordSeenPacket(from, id);
    }

    // ---- Decode the payload (needed before deciding how to relay) ----
    const size_t cipherLen = len - sizeof(MeshtasticHeader);
    uint8_t* cipherData = payload + sizeof(MeshtasticHeader);
    uint8_t plainData[256];
    size_t plainLen = 0;
    uint32_t portnum = 0;
    const uint8_t* appPayload = nullptr;
    size_t appPayloadLen = 0;
    bool decoded = false;
    bool pkiDecoded = false;
    uint8_t keyUsed[32] = {0};
    uint8_t keyUsedLen = 0;

    if (cipherLen > 0) {
        // 1) PKI: only channel 0, addressed to us, with known sender key
        if (hdr->channel == 0 && isToUs && _hasLocalKeys && _pkiEnabled && cipherLen > 12) {
            const MeshNode* peer = findNode(from);
            if (peer && peer->hasPublicKey) {
                uint8_t pkiKey[32];
                if (pkiDeriveKey(peer->publicKey, _localPrivateKey, pkiKey)) {
                    size_t outLen = 0;
                    if (pkiDecryptPayload(pkiKey, id, from, cipherData, cipherLen, plainData, outLen) &&
                        pbValidateData(plainData, outLen, portnum, appPayload, appPayloadLen)) {
                        decoded = true;
                        pkiDecoded = true;
                        plainLen = outLen;
                        ESP_LOGI(TAG, "PKI decrypted packet 0x%08x from !%08x (portnum %u)", (unsigned)id, (unsigned)from, portnum);
                    }
                }
            }
            if (!decoded) {
                ESP_LOGW(TAG, "PKI packet 0x%08x from !%08x not decryptable (key unknown or stale)", (unsigned)id, (unsigned)from);
                if (millis() - _lastDiscoveryRequestTime > 20000) {
                    _lastDiscoveryRequestTime = millis();
                    requestNodeInfo(from);
                }
                // The peer may hold a missing or stale copy of *our* public key (a regenerated keypair
                // is the classic case: their NodeDB then refuses our NodeInfo updates). Re-announce
                // ourselves, rate-limited by the normal NodeInfo interval check.
                if (millis() - _lastNodeInfoTx > 60000) {
                    ESP_LOGW(TAG, "Re-announcing our NodeInfo so !%08x can refresh our public key", (unsigned)from);
                    broadcastNodeInfo(false);
                }
            }
        }

        // 2) Channel keys
        if (!decoded) {
            uint8_t candidates[5][32] = {{0}};
            uint8_t candLens[5] = {0};
            int candCount = 0;

            auto addCandidate = [&](const uint8_t* k, uint8_t klen) {
                if (!k || klen == 0 || klen > 32) return;
                for (int i = 0; i < candCount; ++i) {
                    if (candLens[i] == klen && memcmp(candidates[i], k, klen) == 0) return;
                }
                if (candCount >= 5) return;
                memcpy(candidates[candCount], k, klen);
                candLens[candCount] = klen;
                candCount++;
            };

            if (hdr->channel == getChannelHash()) addCandidate(_primaryPsk, _primaryPskLen);
            for (const auto& ch : _secondaryChannels) {
                if (ch.enabled && ch.pskLen > 0 && hdr->channel == getChannelHashFor(ch.name, ch.psk, ch.pskLen)) {
                    addCandidate(ch.psk, ch.pskLen);
                }
            }
            // Peers may use a default preset channel name while we run a different preset name
            for (uint8_t presetIdx = 0; presetIdx <= 6; ++presetIdx) {
                const char* pn = getPresetCanonicalName(static_cast<ModemPreset>(presetIdx));
                if (hdr->channel == getChannelHashFor(pn, MESHTASTIC_DEFAULT_KEY, 16)) {
                    addCandidate(MESHTASTIC_DEFAULT_KEY, 16);
                    break;
                }
            }
            // Last resort: our own primary key (helps when peer channel names differ)
            addCandidate(_primaryPsk, _primaryPskLen);

            for (int i = 0; i < candCount && !decoded; ++i) {
                aesCtrCryptN(candidates[i], candLens[i], id, from, cipherData, cipherLen, plainData);
                uint32_t candPort = 0;
                const uint8_t* candApp = nullptr;
                size_t candAppLen = 0;
                if (pbValidateData(plainData, cipherLen, candPort, candApp, candAppLen)) {
                    if (candPort == 1 && !looksLikeText(candApp, candAppLen)) continue;
                    decoded = true;
                    plainLen = cipherLen;
                    portnum = candPort;
                    appPayload = candApp;
                    appPayloadLen = candAppLen;
                    memcpy(keyUsed, candidates[i], candLens[i]);
                    keyUsedLen = candLens[i];
                }
            }
        }
    }

    // ---- Managed flooding relay ----
    // Past the decode step the duplicate has a portnum: answer it again (ACK for a decoded packet) and
    // stop - no second delivery in the chat, no relay, no NodeDB/telemetry side effect.
    if (duplicateRetransmit) {
        if (decoded && portnum != 5) sendAck(from, id);
        return;
    }

    // Official firmware never rebroadcasts packets addressed to itself.
    // Relay only while the queue has room, so a burst of decodable traffic cannot grow it unbounded
    if (decoded && hopLimit > 0 && !isToUs && _nodeRole != NodeRole::ClientMute && relayAllowedFor(from) &&
        _pendingRelays.size() < MAX_PENDING_RELAYS) {
        const uint8_t newHop = hopLimit - 1;
        bool queued = false;

        // Traceroute packets must be re-encoded hop by hop (each hop appends id + SNR)
        if (decoded && portnum == 70 && keyUsedLen > 0 && appPayload && appPayloadLen > 0) {
            // request_id == 0 -> request (append to route/snr_towards);
            // request_id != 0 -> reply  (append to route_back/snr_back)
            const bool tracerouteIsReply = (pbDataRequestId(plainData, plainLen) != 0);
            uint8_t newPlain[233];
            size_t newPlainLen = 0;
            if (buildTraceroutePayload(newPlain, sizeof(newPlain), newPlainLen, appPayload, appPayloadLen,
                                       tracerouteIsReply, /*appendId=*/true, _localNodeNum, snr)) {
                uint8_t encBuf[256];
                aesCtrCryptN(keyUsed, keyUsedLen, id, from, newPlain, newPlainLen, encBuf);
                PendingRelay pr;
                memcpy(pr.packet, payload, sizeof(MeshtasticHeader));
                memcpy(pr.packet + sizeof(MeshtasticHeader), encBuf, newPlainLen);
                pr.len = sizeof(MeshtasticHeader) + newPlainLen;
                pr.id = id;
                pr.packet[12] = (hdr->flags & 0xF8) | newHop;
                pr.packet[15] = (uint8_t)(_localNodeNum & 0xFF);
                pr.transmitAtMillis = millis() + 60 + (esp_random() % 160);
                _pendingRelays.push_back(pr);
                queued = true;
                ESP_LOGI(TAG, "Traceroute hop: appended !%08x (snr %.1f)", (unsigned)_localNodeNum, snr);
            }
        }

        if (!queued) {
            float clampedSnr = (snr < -20.0f) ? -20.0f : (snr > 10.0f ? 10.0f : snr);
            uint32_t cwDelayMs = 300 + (uint32_t)((clampedSnr + 20.0f) * 55.0f) + (esp_random() % 300);
            PendingRelay pr;
            memcpy(pr.packet, payload, len);
            pr.len = len;
            pr.id = id;
            pr.packet[12] = (hdr->flags & 0xF8) | newHop;
            pr.packet[15] = (uint8_t)(_localNodeNum & 0xFF);
            pr.transmitAtMillis = millis() + cwDelayMs;
            _pendingRelays.push_back(pr);
            ESP_LOGI(TAG, "Queued relay for packet 0x%08x in %lu ms (hops left: %u)", (unsigned)id, (unsigned long)cwDelayMs, newHop);
        }
    }

    // Not for us (and not broadcast): nothing left to do beyond tracking the sender
    if (!isToUs && !isBroadcastTo) {
        updateNode(from, "", "", snr, rssi);
        updateNodeHops(from, hopsTaken);
        return;
    }

    if (!decoded || plainLen == 0) {
        _rxUndecodable++;
        // Rate-limited diagnostic: tells "we hear nothing" apart from "we hear but cannot decode"
        if (_rxUndecodable <= 5 || (millis() - _lastUndecodableLog) > 30000) {
            _lastUndecodableLog = millis();
            ESP_LOGW(TAG, "Undecodable packet: from !%08x to !%08x id 0x%08x chHash 0x%02x len %u (our hash 0x%02x)",
                     (unsigned)from, (unsigned)to, (unsigned)id, (unsigned)hdr->channel,
                     (unsigned)(len - sizeof(MeshtasticHeader)), (unsigned)getChannelHash());
        }
        // Opaque relay: like the official firmware, forward packets we cannot decrypt (traffic on a
        // channel we are not on) so we still act as a repeater for the rest of the mesh. The
        // payload is relayed byte-for-byte, we only decrement the hop limit and stamp relay_node.
        if (hopLimit > 0 && !isToUs && _nodeRole != NodeRole::ClientMute && relayAllowedFor(from)) {
            queueOpaqueRelay(payload, len, id, hopLimit, snr);
        }
        return;
    }

    _rxDecoded++;
    _lastRxMillis = millis();
    _lastRxFrom = from;
    _lastRxPortnum = portnum;
    _lastRxHash = hdr->channel;
    _lastRxPki = pkiDecoded;

    // Channel learning: remember which (hash,key) pair actually works on air. Never learn from the
    // PKI pseudo-channel (0) or from one of our own secondary channels: adapting the primary TX to
    // those would take our broadcasts off the primary channel entirely.
    if (!pkiDecoded && keyUsedLen > 0 && !isForeignChannelHash(hdr->channel)) {
        if (hdr->channel == getChannelHash()) {
            _localHashConfirmed = true;
            _localHashConfirmedMillis = millis();
            _localHashConfirmedBy = from;
            ESP_LOGI(TAG, "Channel hash 0x%02x confirmed by !%08x", (unsigned)hdr->channel, (unsigned)from);
        } else {
            if (!_learnedHashValid || _learnedHash != hdr->channel || _learnedKeyLen != keyUsedLen ||
                memcmp(_learnedKey, keyUsed, keyUsedLen) != 0) {
                _learnedHash = hdr->channel;
                memcpy(_learnedKey, keyUsed, keyUsedLen);
                _learnedKeyLen = keyUsedLen;
                _learnedHashValid = true;
                _learnedCount = 1;
            } else if (_learnedCount < 100) {
                _learnedCount++;
            }
            _learnedLastMillis = millis();
            ESP_LOGI(TAG, "Heard peer channel hash 0x%02x (hits=%u) - our hash is 0x%02x",
                     (unsigned)hdr->channel, (unsigned)_learnedCount, (unsigned)getChannelHash());
            // Persist once the pair becomes usable for TX, so the adaptation survives a reboot.
            // Only on the transition (count == 2): writing NVS on every packet would wear the flash.
            if (_learnedCount == 2) {
                StorageManager::getInstance().saveLearnedChannel(true, _learnedHash, _learnedKey, _learnedKeyLen, 2);
            }
        }
    }

    const bool wantAck = (hdr->flags & 0x08) != 0;
    handleDecryptedData(from, to, id, plainData, plainLen, snr, rssi, wantAck, pkiDecoded,
                        keyUsedLen ? keyUsed : nullptr, keyUsedLen, hopsTaken);
}

void MeshService::handleDecryptedData(uint32_t from, uint32_t to, uint32_t id, const uint8_t* data, size_t len,
                                      float snr, float rssi, bool wantAck, bool pkiDecoded,
                                      const uint8_t* chanKey, uint8_t chanKeyLen, uint8_t hopsTaken) {
    // Parse the Protobuf Data message
    size_t offset = 0;
    uint32_t portnum = 0;
    const uint8_t* appPayload = nullptr;
    size_t appPayloadLen = 0;
    bool wantResponse = false;
    uint32_t requestId = 0;
    uint32_t replyId = 0;
    uint32_t bitfield = 0;

    while (offset < len) {
        uint64_t key = 0;
        size_t n = pbReadVarint(data + offset, len - offset, key);
        if (n == 0) break;
        offset += n;

        uint32_t fieldNum = key >> 3;
        uint32_t wireType = key & 0x07;

        if (wireType == 0) { // Varint
            uint64_t val = 0;
            n = pbReadVarint(data + offset, len - offset, val);
            if (n == 0) break;
            offset += n;
            if (fieldNum == 1) portnum = (uint32_t)val;
            else if (fieldNum == 3) wantResponse = (val != 0);
            else if (fieldNum == 6) requestId = (uint32_t)val;
            else if (fieldNum == 7) replyId = (uint32_t)val;
            else if (fieldNum == 9) bitfield = (uint32_t)val;
        } else if (wireType == 1) { // 64-bit fixed
            if (offset + 8 > len) break;
            offset += 8;
        } else if (wireType == 2) { // Length-delimited
            uint64_t subLen = 0;
            n = pbReadVarint(data + offset, len - offset, subLen);
            if (n == 0) break;
            offset += n;
            if (offset + subLen > len) break;
            if (fieldNum == 2) {
                appPayload = data + offset;
                appPayloadLen = (size_t)subLen;
            }
            offset += subLen;
        } else if (wireType == 5) { // 32-bit fixed
            if (offset + 4 > len) break;
            if (fieldNum == 6) memcpy(&requestId, data + offset, 4);
            else if (fieldNum == 7) memcpy(&replyId, data + offset, 4);
            offset += 4;
        } else {
            break;
        }
    }

    if (portnum == 0 && requestId == 0) {
        ESP_LOGD(TAG, "Decrypted packet from !%08x had no usable portnum", (unsigned)from);
        return;
    }
    // Official firmware duplicates want_response in the Data bitfield
    if (bitfield & 0x02) wantResponse = true;

    const bool isToUs = (to == _localNodeNum);
    bool uiNeedsRefresh = false;

    // Track sender and hop distance
    updateNode(from, "", "", snr, rssi);
    updateNodeHops(from, hopsTaken);
    if (pkiDecoded) {
        for (auto& n : _nodes) {
            if (n.nodeNum == from) n.keyVerified = true;
        }
    }

    // Learn the primary channel actually used by peers (helps diagnose hash mismatches)
    if (from != _localNodeNum) {
        for (auto& n : _nodes) {
            if (n.nodeNum == from) n.channelIndex = 0;
        }
    }

    // Auto-discovery: ask NodeInfo (and therefore the public key) to unknown nodes
    if (from != _localNodeNum && portnum != 4) {
        const MeshNode* peer = findNode(from);
        if (!peer || strlen(peer->longName) == 0 || (!peer->hasPublicKey && _pkiEnabled)) {
            uint32_t now = millis();
            if (now - _lastDiscoveryRequestTime > 15000) {
                _lastDiscoveryRequestTime = now;
                ESP_LOGI(TAG, "Unknown node or missing key for !%08x -> sending NodeInfo request", (unsigned)from);
                requestNodeInfo(from);
            }
        }
    }

    // Reliable unicast: acknowledge every packet addressed to us, whatever the port number.
    // Official firmware ACKs any want_ack unicast (the app requests NodeInfo/telemetry/position this
    // way); without an ACK the peer retransmits three times and reports the exchange as failed.
    // ACKs themselves (portnum 5) are never acknowledged.
    if (wantAck && to == _localNodeNum && portnum != 5) {
        sendAck(from, id);
    }

    // ---------------- ROUTING_APP (ACK / NAK) ----------------
    if (portnum == 5) {
        uint32_t errCode = 0;
        bool haveErr = false;
        size_t rOff = 0;
        while (rOff < appPayloadLen) {
            uint64_t key = 0;
            size_t n = pbReadVarint(appPayload + rOff, appPayloadLen - rOff, key);
            if (n == 0) break;
            rOff += n;
            uint32_t field = key >> 3;
            uint32_t wtype = key & 0x07;
            if (wtype == 0) {
                uint64_t v = 0;
                n = pbReadVarint(appPayload + rOff, appPayloadLen - rOff, v);
                if (n == 0) break;
                rOff += n;
                if (field == 3) { errCode = (uint32_t)v; haveErr = true; }
            } else if (wtype == 2) {
                uint64_t sl = 0;
                n = pbReadVarint(appPayload + rOff, appPayloadLen - rOff, sl);
                if (n == 0) break;
                rOff += n;
                if (sl > appPayloadLen - rOff) break;
                rOff += sl;
            } else if (wtype == 5) {
                if (rOff + 4 > appPayloadLen) break;
                rOff += 4;
            } else break;
        }

        if (requestId != 0) {
            if (!haveErr || errCode == 0) {
                ESP_LOGI(TAG, "ACK for packet 0x%08x from !%08x", (unsigned)requestId, (unsigned)from);
                markMessageAcked(requestId);
            } else {
                ESP_LOGW(TAG, "NAK for packet 0x%08x from !%08x: %s (%u)", (unsigned)requestId, (unsigned)from,
                         routingErrorName(errCode), (unsigned)errCode);
                markMessageFailed(requestId, routingErrorName(errCode));
            }
            uiNeedsRefresh = true;
        }
        // Official ReliableRouter behaviour: an ACK/NAK coming back cancels the pending relay of the
        // packet it acknowledges, but keeps flooding itself. In a multi-hop route (A-B-C) the sender
        // only sees the ACK if every relay forwards it, so cancelling the ACK's own relay would make
        // the DM look failed on the sender even though it was delivered.
        if (!isToUs && to != NODENUM_BROADCAST && requestId != 0) cancelRelayFor(requestId);

    // ---------------- TEXT_MESSAGE_APP ----------------
    } else if (portnum == 1 && appPayload && appPayloadLen > 0) {
        ChatMessage msg = {};
        msg.id = id;
        msg.fromNode = from;
        msg.toNode = to;
        uint32_t curUnix = BSP::getInstance().getRtcUnix();
        msg.timestamp = (curUnix > 1700000000UL) ? curUnix : millis();
        msg.snr = snr;
        msg.rssi = rssi;
        msg.isOutgoing = false;
        msg.isRead = false;
        msg.isAcked = false;
        msg.isFailed = false;

        formatClockLabel(msg.timeStr, sizeof(msg.timeStr));
        strncpy(msg.channelPreset, getPresetName(_modemPreset), sizeof(msg.channelPreset) - 1);
        msg.seq = ++_globalMsgSeq;

        const MeshNode* node = findNode(from);
        if (node && strlen(node->shortName) > 0) {
            strncpy(msg.senderShort, node->shortName, sizeof(msg.senderShort) - 1);
            msg.senderShort[sizeof(msg.senderShort) - 1] = '\0';
        } else {
            snprintf(msg.senderShort, sizeof(msg.senderShort), "%04x", (unsigned)(from & 0xFFFF));
        }

        size_t cpyLen = appPayloadLen < (sizeof(msg.text) - 1) ? appPayloadLen : (sizeof(msg.text) - 1);
        memcpy(msg.text, appPayload, cpyLen);
        msg.text[cpyLen] = '\0';

        if (to == NODENUM_BROADCAST) {
            _broadcastMessages.push_back(msg);
            if (_broadcastMessages.size() > MAX_MESSAGES) _broadcastMessages.pop_front();
        } else {
            _directMessages.push_back(msg);
            if (_directMessages.size() > MAX_MESSAGES) _directMessages.pop_front();
        }

        _unreadCount++;
        SavedMessage sm = {};
        sm.id = msg.id;
        sm.fromNode = msg.fromNode;
        sm.toNode = msg.toNode;
        strncpy(sm.senderShort, msg.senderShort, sizeof(sm.senderShort) - 1);
        strncpy(sm.text, msg.text, sizeof(sm.text) - 1);
        sm.timestamp = msg.timestamp;
        sm.isOutgoing = false;
        sm.isAcked = false;
        strncpy(sm.timeStr, msg.timeStr, sizeof(sm.timeStr) - 1);
        strncpy(sm.channelPreset, msg.channelPreset, sizeof(sm.channelPreset) - 1);
        StorageManager::getInstance().appendChatMessage(sm);
        ESP_LOGI(TAG, "Chat message received from %s (hops=%u, pki=%u): %s", msg.senderShort, hopsTaken, pkiDecoded ? 1 : 0, msg.text);

        BSP::getInstance().beep(BSP::NOTIFY_TONE_HZ, 80,
                                (to == NODENUM_BROADCAST) ? BSP::BuzzerEvent::Message
                                                          : BSP::BuzzerEvent::DirectMessage);
        BSP::getInstance().setLedRxActivity(100);
        uiNeedsRefresh = true;

    // ---------------- NODEINFO_APP ----------------
    } else if (portnum == 4) {
        // A NodeInfo packet with an empty payload is a *request* ("tell me about yourself"): reply
        // with our User and stop, there is nothing to parse. Missing this meant peers/apps could
        // only learn about us from our periodic broadcast.
        if (!appPayload || appPayloadLen == 0) {
            ESP_LOGI(TAG, "NodeInfo request from !%08x (no payload)", (unsigned)from);
            if (wantResponse && from != _localNodeNum) {
                handleNodeInfoRequest(from, true, id);
            }
            return;
        }
        size_t uOff = 0;
        char longName[40] = "";
        char shortName[5] = "";
        uint8_t pubKey[32] = {0};
        bool hasPubKey = false;
        uint8_t roleVal = 0;
        uint8_t hwModel = 0;
        bool isLicensed = false;
        bool isUnmessagable = false;
        uint8_t niMac[6] = {0};
        bool hasMac = false;

        while (uOff < appPayloadLen) {
            uint64_t key = 0;
            size_t n = pbReadVarint(appPayload + uOff, appPayloadLen - uOff, key);
            if (n == 0) break;
            uOff += n;
            uint32_t field = key >> 3;
            uint32_t wtype = key & 0x07;

            if (wtype == 2) {
                uint64_t slen = 0;
                n = pbReadVarint(appPayload + uOff, appPayloadLen - uOff, slen);
                if (n == 0) break;
                uOff += n;
                if (uOff + slen > appPayloadLen) break;

                if (field == 2) { // long_name
                    size_t c = slen < (sizeof(longName) - 1) ? slen : (sizeof(longName) - 1);
                    if (c > 24) c = 24; // Meshtastic 2.8 stores at most 24 bytes
                    memcpy(longName, appPayload + uOff, c);
                    longName[c] = '\0';
                } else if (field == 3) { // short_name
                    size_t c = slen < (sizeof(shortName) - 1) ? slen : (sizeof(shortName) - 1);
                    memcpy(shortName, appPayload + uOff, c);
                    shortName[c] = '\0';
                } else if (field == 8 && slen == 32) { // public_key
                    memcpy(pubKey, appPayload + uOff, 32);
                    hasPubKey = true;
                } else if (field == 4 && slen == 6) { // macaddr
                    memcpy(niMac, appPayload + uOff, 6);
                    hasMac = true;
                }
                uOff += slen;
            } else if (wtype == 0) {
                uint64_t v = 0;
                size_t n2 = pbReadVarint(appPayload + uOff, appPayloadLen - uOff, v);
                if (n2 == 0) break;
                uOff += n2;
                if (field == 5) hwModel = (uint8_t)v;
                else if (field == 6) isLicensed = (v != 0);
                else if (field == 7) roleVal = (uint8_t)v;
                else if (field == 9) isUnmessagable = (v != 0);
            } else if (wtype == 5) {
                if (uOff + 4 > appPayloadLen) break;
                uOff += 4;
            } else break;
        }

        updateNode(from, longName, shortName, snr, rssi, hasPubKey ? pubKey : nullptr, 0, 0.0f, roleVal,
                   hwModel, isLicensed, isUnmessagable);
        if (hasMac) {
            for (auto& n : _nodes) {
                if (n.nodeNum == from) {
                    memcpy(n.mac, niMac, 6);
                    n.hasMac = true;
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "NodeInfo from !%08x: '%s'/'%s' role=%u hw=%u PKI=%s", (unsigned)from, longName, shortName,
                 (unsigned)roleVal, (unsigned)hwModel, hasPubKey ? "YES" : "NO");
        uiNeedsRefresh = true;

        if (wantResponse && from != _localNodeNum) {
            handleNodeInfoRequest(from, true, id);
        }

    // ---------------- TELEMETRY_APP ----------------
    } else if (portnum == 67) {
        // Empty payload = app asking for our device metrics (battery / ch-util / air-util)
        if (!appPayload || appPayloadLen == 0) {
            ESP_LOGI(TAG, "Telemetry request from !%08x (no payload)", (unsigned)from);
            if (wantResponse && from != _localNodeNum) {
                queueDeferred(DeferredKind::TelemetryUnicast, from, 400 + (esp_random() % 600), id);
            }
            return;
        }
        size_t tOff = 0;
        uint32_t batVal = 0;
        float voltVal = 0.0f;
        bool hasEnv = false;
        float envTemp = 0.0f, envHum = 0.0f, envPress = 0.0f, envIaq = 0.0f;
        bool hasAir = false;
        float airPm25 = 0.0f;

        while (tOff < appPayloadLen) {
            uint64_t key = 0;
            size_t n = pbReadVarint(appPayload + tOff, appPayloadLen - tOff, key);
            if (n == 0) break;
            tOff += n;
            uint32_t field = key >> 3;
            uint32_t wtype = key & 0x07;

            if (wtype == 2) {
                uint64_t subLen = 0;
                n = pbReadVarint(appPayload + tOff, appPayloadLen - tOff, subLen);
                if (n == 0) break;
                tOff += n;
                if (tOff + subLen > appPayloadLen) break;

                if (field == 2) { // DeviceMetrics
                    size_t mOff = 0;
                    while (mOff < subLen) {
                        uint64_t mKey = 0;
                        size_t mn = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, mKey);
                        if (mn == 0) break;
                        mOff += mn;
                        uint32_t mField = mKey >> 3;
                        uint32_t mWtype = mKey & 0x07;

                        if (mWtype == 0) {
                            uint64_t mVal = 0;
                            size_t mn2 = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, mVal);
                            if (mn2 == 0) break;
                            mOff += mn2;
                            if (mField == 1) batVal = (uint32_t)mVal;
                        } else if (mWtype == 5) {
                            if (mOff + 4 > subLen) break;
                            if (mField == 2) memcpy(&voltVal, appPayload + tOff + mOff, 4);
                            mOff += 4;
                        } else if (mWtype == 2) {
                            uint64_t sl = 0;
                            size_t mn2 = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, sl);
                            if (mn2 == 0) break;
                            mOff += mn2;
                            if (sl > subLen - mOff) break;
                            mOff += sl;
                        } else break;
                    }
                } else if (field == 3) { // EnvironmentMetrics
                    // temperature=1, relative_humidity=2, barometric_pressure=3, iaq=7
                    size_t mOff = 0;
                    while (mOff < subLen) {
                        uint64_t mKey = 0;
                        size_t mn = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, mKey);
                        if (mn == 0) break;
                        mOff += mn;
                        uint32_t mField = mKey >> 3;
                        uint32_t mWtype = mKey & 0x07;
                        if (mWtype == 5) {
                            if (mOff + 4 > subLen) break;
                            float fv = 0.0f;
                            memcpy(&fv, appPayload + tOff + mOff, 4);
                            mOff += 4;
                            if (mField == 1) { envTemp = fv; hasEnv = true; }
                            else if (mField == 2) { envHum = fv; hasEnv = true; }
                            else if (mField == 3) { envPress = fv; hasEnv = true; }
                        } else if (mWtype == 0) {
                            uint64_t mv = 0;
                            size_t mn2 = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, mv);
                            if (mn2 == 0) break;
                            mOff += mn2;
                            if (mField == 7) { envIaq = (float)mv; hasEnv = true; }
                        } else if (mWtype == 2) {
                            uint64_t sl = 0;
                            size_t mn2 = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, sl);
                            if (mn2 == 0) break;
                            mOff += mn2;
                            if (sl > subLen - mOff) break;
                            mOff += sl;
                        } else break;
                    }
                } else if (field == 4) { // AirQualityMetrics (pm25_environmental = 5)
                    size_t mOff = 0;
                    while (mOff < subLen) {
                        uint64_t mKey = 0;
                        size_t mn = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, mKey);
                        if (mn == 0) break;
                        mOff += mn;
                        uint32_t mField = mKey >> 3;
                        uint32_t mWtype = mKey & 0x07;
                        if (mWtype == 0) {
                            uint64_t mv = 0;
                            size_t mn2 = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, mv);
                            if (mn2 == 0) break;
                            mOff += mn2;
                            if (mField == 5) { airPm25 = (float)mv; hasAir = true; }
                        } else if (mWtype == 2) {
                            uint64_t sl = 0;
                            size_t mn2 = pbReadVarint(appPayload + tOff + mOff, subLen - mOff, sl);
                            if (mn2 == 0) break;
                            mOff += mn2;
                            if (sl > subLen - mOff) break;
                            mOff += sl;
                        } else if (mWtype == 5) {
                            if (mOff + 4 > subLen) break;
                            mOff += 4;
                        } else break;
                    }
                }
                tOff += subLen;
            } else if (wtype == 0) {
                uint64_t v = 0;
                size_t n2 = pbReadVarint(appPayload + tOff, appPayloadLen - tOff, v);
                if (n2 == 0) break;
                tOff += n2;
                if ((field == 1 || field == 4) && v > 1700000000UL) {
                    maybeSyncRtcFromMesh((uint32_t)v);
                }
            } else if (wtype == 5) {
                if (tOff + 4 > appPayloadLen) break; // malformed fixed32 at the end of the payload
                if (field == 1 || field == 4) {
                    uint32_t ut = 0;
                    memcpy(&ut, appPayload + tOff, 4);
                    if (ut > 1700000000UL) maybeSyncRtcFromMesh(ut);
                }
                tOff += 4;
            } else break;
        }

        if (batVal > 0 || voltVal > 0.0f) {
            updateNode(from, "", "", snr, rssi, nullptr, (uint8_t)batVal, voltVal, /*role=*/-1);
            ESP_LOGI(TAG, "Telemetry for !%08x: Bat=%u%%, Volt=%.2fV", (unsigned)from, (unsigned)batVal, voltVal);
            uiNeedsRefresh = true;
        }
        if (hasEnv || hasAir) {
            for (auto& n : _nodes) {
                if (n.nodeNum == from) {
                    n.hasEnvMetrics = true;
                    if (hasEnv) {
                        n.temperature = envTemp;
                        n.humidity = envHum;
                        n.pressure = envPress;
                        n.iaq = envIaq;
                    }
                    if (hasAir) n.pm25 = airPm25;
                    ESP_LOGI(TAG, "Env telemetry for !%08x: T=%.1fC H=%.1f%% P=%.1fhPa IAQ=%.0f PM2.5=%.0f",
                             (unsigned)from, n.temperature, n.humidity, n.pressure, n.iaq, n.pm25);
                    uiNeedsRefresh = true;
                    break;
                }
            }
        }
        if (wantResponse && from != _localNodeNum) {
            queueDeferred(DeferredKind::TelemetryUnicast, from, 400 + (esp_random() % 600), id);
        }

    // ---------------- POSITION_APP ----------------
    } else if (portnum == 3) {
        // Empty payload = app asking for our position
        if (!appPayload || appPayloadLen == 0) {
            ESP_LOGI(TAG, "Position request from !%08x (no payload)", (unsigned)from);
            if (wantResponse && from != _localNodeNum) {
                queueDeferred(DeferredKind::PositionUnicast, from, 400 + (esp_random() % 600), id);
            }
            return;
        }
        size_t pOff = 0;
        int32_t lat_i = 0, lon_i = 0, alt = 0;

        while (pOff < appPayloadLen) {
            uint64_t key = 0;
            size_t n = pbReadVarint(appPayload + pOff, appPayloadLen - pOff, key);
            if (n == 0) break;
            pOff += n;
            uint32_t field = key >> 3;
            uint32_t wtype = key & 0x07;

            if (wtype == 5) {
                if (pOff + 4 > appPayloadLen) break;
                int32_t val = (int32_t)(appPayload[pOff] | (appPayload[pOff+1]<<8) | (appPayload[pOff+2]<<16) | (appPayload[pOff+3]<<24));
                pOff += 4;
                if (field == 1) lat_i = val;
                else if (field == 2) lon_i = val;
                else if (field == 4) {
                    uint32_t ut = (uint32_t)val;
                    if (ut > 1700000000UL) maybeSyncRtcFromMesh(ut);
                }
            } else if (wtype == 0) {
                uint64_t val = 0;
                size_t n2 = pbReadVarint(appPayload + pOff, appPayloadLen - pOff, val);
                if (n2 == 0) break;
                pOff += n2;
                if (field == 3) alt = (int32_t)val;
                else if (field == 4 && val > 1700000000UL) {
                    maybeSyncRtcFromMesh((uint32_t)val);
                }
            } else if (wtype == 2) {
                uint64_t slen = 0;
                size_t n2 = pbReadVarint(appPayload + pOff, appPayloadLen - pOff, slen);
                if (n2 == 0) break;
                pOff += n2;
                if (slen > appPayloadLen - pOff) break;
                pOff += slen;
            } else break;
        }

        if (lat_i != 0 || lon_i != 0) {
            double lat = lat_i * 1e-7;
            double lon = lon_i * 1e-7;
            updateNodePosition(from, lat, lon, alt);
            ESP_LOGI(TAG, "Position for !%08x: %.5f, %.5f, alt=%d", (unsigned)from, lat, lon, alt);
            uiNeedsRefresh = true;
        }
        if (wantResponse && from != _localNodeNum) {
            queueDeferred(DeferredKind::PositionUnicast, from, 400 + (esp_random() % 600), id);
        }

    // ---------------- TRACEROUTE_APP ----------------
    } else if (portnum == 70 && appPayload && appPayloadLen > 0) {
        handleTracerouteReceived(from, to, id, appPayload, appPayloadLen, requestId != 0, chanKey, chanKeyLen, snr);
        uiNeedsRefresh = true;

    // ---------------- WAYPOINT_APP ----------------
    } else if (portnum == 8 && appPayload && appPayloadLen > 0) {
        uint32_t wpId = 0;
        int32_t wLatI = 0, wLonI = 0;
        uint32_t wExpire = 0;
        char wName[31] = "";
        bool hasLat = false, hasLon = false;
        size_t wOff = 0;
        while (wOff < appPayloadLen) {
            uint64_t key = 0;
            size_t n = pbReadVarint(appPayload + wOff, appPayloadLen - wOff, key);
            if (n == 0) break;
            wOff += n;
            uint32_t field = key >> 3;
            uint32_t wtype = key & 0x07;
            if (wtype == 0) {
                uint64_t v = 0;
                size_t n2 = pbReadVarint(appPayload + wOff, appPayloadLen - wOff, v);
                if (n2 == 0) break;
                wOff += n2;
                if (field == 1) wpId = (uint32_t)v;
                else if (field == 4) wExpire = (uint32_t)v;
            } else if (wtype == 5) {
                if (wOff + 4 > appPayloadLen) break;
                int32_t v = (int32_t)(appPayload[wOff] | (appPayload[wOff + 1] << 8) |
                                      (appPayload[wOff + 2] << 16) | (appPayload[wOff + 3] << 24));
                wOff += 4;
                if (field == 2) { wLatI = v; hasLat = true; }
                else if (field == 3) { wLonI = v; hasLon = true; }
            } else if (wtype == 2) {
                uint64_t slen = 0;
                size_t n2 = pbReadVarint(appPayload + wOff, appPayloadLen - wOff, slen);
                if (n2 == 0) break;
                wOff += n2;
                if (wOff + slen > appPayloadLen) break;
                if (field == 6) {
                    size_t c = slen < (sizeof(wName) - 1) ? slen : (sizeof(wName) - 1);
                    memcpy(wName, appPayload + wOff, c);
                    wName[c] = '\0';
                }
                wOff += slen;
            } else break;
        }
        if ((hasLat || hasLon) && (wLatI != 0 || wLonI != 0)) {
            MeshWaypoint wp;
            wp.id = wpId;
            wp.fromNode = from;
            wp.lat = wLatI * 1e-7;
            wp.lon = wLonI * 1e-7;
            strncpy(wp.name, wName, sizeof(wp.name) - 1);
            wp.expire = wExpire;
            wp.receivedAt = millis();
            bool replaced = false;
            for (auto& w : _waypoints) {
                if (w.id == wpId) { w = wp; replaced = true; break; }
            }
            if (!replaced) {
                if (_waypoints.size() >= 16) _waypoints.erase(_waypoints.begin());
                _waypoints.push_back(wp);
            }
            ESP_LOGI(TAG, "Waypoint '%s' from !%08x: %.5f, %.5f", wp.name, (unsigned)from, wp.lat, wp.lon);
            uiNeedsRefresh = true;
        } else {
            ESP_LOGD(TAG, "Waypoint from !%08x without position (removal?)", (unsigned)from);
        }

    // ---------------- NODE_STATUS_APP (36) ----------------
    } else if (portnum == 36 && appPayload && appPayloadLen > 0) {
        // StatusMessage { string status = 1; }
        char statusText[41] = "";
        if (appPayloadLen > 2 && (appPayload[0] & 0x07) == 2 && (appPayload[0] >> 3) == 1) {
            size_t sOff = 1;
            uint64_t slen = 0;
            size_t n = pbReadVarint(appPayload + sOff, appPayloadLen - sOff, slen);
            if (n > 0) {
                sOff += n;
                if (sOff + slen <= appPayloadLen) {
                    size_t c = slen < (sizeof(statusText) - 1) ? slen : (sizeof(statusText) - 1);
                    memcpy(statusText, appPayload + sOff, c);
                    statusText[c] = '\0';
                }
            }
        }
        if (statusText[0] != '\0') {
            for (auto& n : _nodes) {
                if (n.nodeNum == from) {
                    strncpy(n.statusText, statusText, sizeof(n.statusText) - 1);
                    n.statusText[sizeof(n.statusText) - 1] = '\0';
                    ESP_LOGI(TAG, "Node status from !%08x: %s", (unsigned)from, statusText);
                    uiNeedsRefresh = true;
                    break;
                }
            }
        }

    // ---------------- MESH_BEACON_APP (37) ----------------
    } else if (portnum == 37 && appPayload && appPayloadLen > 0) {
        // MeshBeacon { string message = 1; bool offer_channel = 2; bool offer_region = 3; bool offer_preset = 4; }
        char beaconText[81] = "";
        bool offersChannel = false, offersRegion = false, offersPreset = false;
        size_t bOff = 0;
        while (bOff < appPayloadLen) {
            uint64_t key = 0;
            size_t n = pbReadVarint(appPayload + bOff, appPayloadLen - bOff, key);
            if (n == 0) break;
            bOff += n;
            uint32_t field = key >> 3;
            uint32_t wtype = key & 0x07;
            if (wtype == 2) {
                uint64_t slen = 0;
                size_t n2 = pbReadVarint(appPayload + bOff, appPayloadLen - bOff, slen);
                if (n2 == 0) break;
                bOff += n2;
                if (bOff + slen > appPayloadLen) break;
                if (field == 1) {
                    size_t c = slen < (sizeof(beaconText) - 1) ? slen : (sizeof(beaconText) - 1);
                    memcpy(beaconText, appPayload + bOff, c);
                    beaconText[c] = '\0';
                } else if (field == 2) {
                    // offer_channel is a ChannelSettings message, not a bool: its presence is the flag
                    offersChannel = true;
                }
                bOff += slen;
            } else if (wtype == 0) {
                uint64_t v = 0;
                size_t n2 = pbReadVarint(appPayload + bOff, appPayloadLen - bOff, v);
                if (n2 == 0) break;
                bOff += n2;
                if (field == 2) offersChannel = (v != 0);
                else if (field == 3) offersRegion = (v != 0);
                else if (field == 4) offersPreset = (v != 0);
            } else break;
        }
        ESP_LOGI(TAG, "MeshBeacon from !%08x: '%s' (chan=%d region=%d preset=%d)", (unsigned)from, beaconText,
                 offersChannel ? 1 : 0, offersRegion ? 1 : 0, offersPreset ? 1 : 0);
    }

    if (uiNeedsRefresh) {
        UIEngine::getInstance().requestFullRefresh();
    }
}

void MeshService::updateNode(uint32_t nodeNum, const char* longName, const char* shortName, float snr, float rssi,
                             const uint8_t* pubKey, uint8_t battery, float voltage, int8_t role,
                             uint8_t hwModel, int8_t isLicensed, int8_t isUnmessagable) {
    for (auto& node : _nodes) {
        if (node.nodeNum == nodeNum) {
            node.snr = snr;
            node.rssi = rssi;
            markNodeHeard(node);
            if (longName && strlen(longName) > 0) {
                strncpy(node.longName, longName, sizeof(node.longName) - 1);
                node.longName[sizeof(node.longName) - 1] = '\0';
            }
            if (shortName && strlen(shortName) > 0) {
                strncpy(node.shortName, shortName, sizeof(node.shortName) - 1);
                node.shortName[sizeof(node.shortName) - 1] = '\0';
            }
            if (pubKey) {
                if (node.hasPublicKey && memcmp(node.publicKey, pubKey, 32) != 0) {
                    ESP_LOGW(TAG, "Public key changed for !%08x (peer regenerated keys)", (unsigned)nodeNum);
                    node.keyVerified = false;
                }
                memcpy(node.publicKey, pubKey, 32);
                node.hasPublicKey = true;
            }
            if (battery > 0) node.battery = battery;
            if (voltage > 0.0f) node.voltage = voltage;
            // Only the NODEINFO path carries these; "seen on air" updates must not reset them to
            // their defaults (a router used to show up as CLIENT after any text message).
            if (role >= 0) node.role = (uint8_t)role;
            if (hwModel > 0) node.hwModel = hwModel;
            if (isLicensed >= 0) node.isLicensed = (isLicensed != 0);
            if (isUnmessagable >= 0) node.isUnmessagable = (isUnmessagable != 0);
            _nodeDbDirty = true;
            return;
        }
    }

    if (_nodes.size() >= MAX_NODES) {
        // NodeDB full: drop the node we heard from the longest ago instead of ignoring the new one.
        // On a busy mesh the 64 entries fill up quickly and the nodes still arriving are exactly the
        // interesting ones (the alternative was a silent "node ignored" with no trace).
        size_t victim = SIZE_MAX;
        uint32_t maxAge = 0;
        for (size_t i = 0; i < _nodes.size(); ++i) {
            if (_nodes[i].nodeNum == _localNodeNum) continue;
            const uint32_t age = getNodeAgeSeconds(_nodes[i]); // UINT32_MAX when unknown -> oldest
            if (victim == SIZE_MAX || age > maxAge) {
                maxAge = age;
                victim = i;
            }
        }
        if (victim == SIZE_MAX) return; // only our own entry is present
        ESP_LOGW(TAG, "NodeDB full (%u nodes): replacing !%08x (last heard %us ago)",
                 (unsigned)MAX_NODES, (unsigned)_nodes[victim].nodeNum,
                 (unsigned)(maxAge == UINT32_MAX ? 0 : maxAge));
        _nodes.erase(_nodes.begin() + victim);
        _nodeOrderDirty = true;
    }

    {
        MeshNode newNode;
        newNode.nodeNum = nodeNum;
        snprintf(newNode.idStr, sizeof(newNode.idStr), "!%08x", (unsigned int)nodeNum);
        if (longName && strlen(longName) > 0) {
            strncpy(newNode.longName, longName, sizeof(newNode.longName) - 1);
            newNode.longName[sizeof(newNode.longName) - 1] = '\0';
        } else {
            strncpy(newNode.longName, newNode.idStr, sizeof(newNode.longName) - 1);
            newNode.longName[sizeof(newNode.longName) - 1] = '\0';
        }
        if (shortName && strlen(shortName) > 0) {
            strncpy(newNode.shortName, shortName, sizeof(newNode.shortName) - 1);
            newNode.shortName[sizeof(newNode.shortName) - 1] = '\0';
        } else {
            snprintf(newNode.shortName, sizeof(newNode.shortName), "%04X", (unsigned int)(nodeNum & 0xFFFF));
        }
        newNode.snr = snr;
        newNode.rssi = rssi;
        markNodeHeard(newNode);
        if (pubKey) {
            memcpy(newNode.publicKey, pubKey, 32);
            newNode.hasPublicKey = true;
        }
        newNode.battery = battery;
        newNode.voltage = voltage;
        if (role >= 0) newNode.role = (uint8_t)role;
        newNode.hwModel = hwModel;
        if (isLicensed >= 0) newNode.isLicensed = (isLicensed != 0);
        if (isUnmessagable >= 0) newNode.isUnmessagable = (isUnmessagable != 0);
        _nodes.push_back(newNode);
        _nodeDbDirty = true;
    }
}

void MeshService::updateNodeHops(uint32_t nodeNum, uint8_t hops) {
    for (auto& n : _nodes) {
        if (n.nodeNum == nodeNum) {
            n.hopsAway = hops;
            return;
        }
    }
}

// Cached wall clock (1s): the RTC is on I2C and updateNode() runs for every received packet.
uint32_t MeshService::currentEpoch() const {
    uint32_t now = millis();
    if (_epochCacheMillis == 0 || (now - _epochCacheMillis) > 1000) {
        _epochCacheMillis = now;
        BSP& bsp = BSP::getInstance();
        _epochCacheValue = bsp.isRtcValid() ? bsp.getRtcUnix() : 0;
    }
    return _epochCacheValue;
}

void MeshService::maybeSyncRtcFromMesh(uint32_t unixUtc) {
    if (!_meshTimeSync) {
        ESP_LOGD(TAG, "RTC sync from mesh disabled - ignoring timestamp %u", (unsigned)unixUtc);
        return;
    }
    BSP::getInstance().syncRtcFromUnix(unixUtc, _timezoneOffset);
    _epochCacheMillis = 0; // the clock just jumped, refresh the cache
}

void MeshService::markNodeHeard(MeshNode& node) {
    node.lastHeard = millis();
    uint32_t e = currentEpoch();
    if (e > 1700000000UL) node.lastHeardEpoch = e;
    _nodeOrderDirty = true;
}

void MeshService::sortNodesByRecency() {
    _nodeOrderDirty = false;
    if (_nodes.size() < 2) return;
    // Our own entry stays on top (the position/telemetry code reads _nodes[0]); everything else is
    // ordered by the last time we heard it, most recent first.
    const uint32_t local = _localNodeNum;
    std::stable_sort(_nodes.begin(), _nodes.end(), [local](const MeshNode& a, const MeshNode& b) {
        const bool aLocal = (a.nodeNum == local);
        const bool bLocal = (b.nodeNum == local);
        if (aLocal != bLocal) return aLocal;
        uint32_t ka = (a.lastHeardEpoch > 1700000000UL) ? a.lastHeardEpoch : 0;
        uint32_t kb = (b.lastHeardEpoch > 1700000000UL) ? b.lastHeardEpoch : 0;
        if (ka != kb) return ka > kb;
        return a.lastHeard > b.lastHeard; // same-second ties, and sessions without a valid RTC
    });
}

uint32_t MeshService::getNodeAgeSeconds(const MeshNode& node) const {
    uint32_t nowEpoch = currentEpoch();
    if (node.lastHeardEpoch > 1700000000UL && nowEpoch > 1700000000UL) {
        return (node.lastHeardEpoch >= nowEpoch) ? 0 : (nowEpoch - node.lastHeardEpoch);
    }
    // No wall clock for this entry: trust the session counter only if it is in the past
    uint32_t nowMs = millis();
    if (node.lastHeard == 0 || node.lastHeard > nowMs) return UINT32_MAX;
    return (nowMs - node.lastHeard) / 1000;
}

void MeshService::formatAgeLabel(uint32_t seconds, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    if (seconds == UINT32_MAX) {
        snprintf(out, outLen, "--");
    } else if (seconds < 60) {
        snprintf(out, outLen, "now");
    } else if (seconds < 3600) {
        snprintf(out, outLen, "%um ago", (unsigned)(seconds / 60));
    } else if (seconds < 86400) {
        snprintf(out, outLen, "%uh ago", (unsigned)(seconds / 3600));
    } else {
        uint32_t days = seconds / 86400;
        if (days < 7) snprintf(out, outLen, "%u D ago", (unsigned)days);
        else if (days < 30) snprintf(out, outLen, "%u W ago", (unsigned)(days / 7));
        else if (days < 365) snprintf(out, outLen, "%u M ago", (unsigned)(days / 30));
        else snprintf(out, outLen, "%u Y ago", (unsigned)(days / 365));
    }
}

void MeshService::setTimezoneOffset(int8_t hours) {
    if (hours < -12) hours = -12;
    if (hours > 14) hours = 14;
    const int8_t delta = (int8_t)(hours - _timezoneOffset);
    _timezoneOffset = hours;
    BSP::getInstance().setTimezoneOffset(hours);
    // The RTC stores local wall-clock time: shift it by the offset delta so the displayed time
    // follows the new timezone at once (fixes "clock one hour behind" with a single tap).
    if (delta != 0 && BSP::getInstance().isRtcValid()) {
        BSP::getInstance().addRtcSeconds((int32_t)delta * 3600);
    }
    _epochCacheMillis = 0;
    ESP_LOGI(TAG, "Timezone offset set to UTC%+d", (int)hours);
    // Persistence is left to the caller (the settings screen debounces it): saving here also rewrote
    // the whole node CSV on every single stepper tap.
}

void MeshService::setMeshTimeSync(bool en) {
    _meshTimeSync = en;
    ESP_LOGI(TAG, "Mesh time sync %s", en ? "enabled" : "disabled");
    // Persistence is left to the caller (debounced by the settings screen).
}

bool MeshService::relayAllowedFor(uint32_t from) const {
    switch (_rebroadcastMode) {
        case RebroadcastMode::All:
            return true;
        case RebroadcastMode::KnownOnly:
            // "Known" = the sender is already in the NodeDB (name/key exchanged at least once).
            return findNode(from) != nullptr;
        case RebroadcastMode::Off:
            return false;
    }
    return true;
}

void MeshService::cancelRelayFor(uint32_t id) {
    for (auto it = _pendingRelays.begin(); it != _pendingRelays.end(); ) {
        if (it->id == id) {
            it = _pendingRelays.erase(it);
        } else {
            ++it;
        }
    }
}

void MeshService::updateNodePosition(uint32_t nodeNum, double lat, double lon, int32_t alt) {
    for (auto& node : _nodes) {
        if (node.nodeNum == nodeNum) {
            node.hasPosition = true;
            node.lat = lat;
            node.lon = lon;
            node.alt = alt;

            // Distance and bearing are relative to our own position when it is known. The old code
            // always used a fixed origin (Milan), which was meaningless for everybody else.
            const bool haveOrigin = (!_nodes.empty() && _nodes[0].hasPosition &&
                                     (_nodes[0].lat != 0.0 || _nodes[0].lon != 0.0));
            if (!haveOrigin) {
                node.distanceKm = 0.0; // unknown: no reference position on this device
                node.bearingDeg = 0.0;
            } else {
                const double originLat = _nodes[0].lat;
                const double originLon = _nodes[0].lon;
                const double dlat = (lat - originLat) * 111.0;
                const double dlon = (lon - originLon) * 111.0 * cos(originLat * 0.0174532925);
                node.distanceKm = sqrt(dlat * dlat + dlon * dlon);
                node.bearingDeg = atan2(dlon, dlat) * 57.2957795;
                if (node.bearingDeg < 0) node.bearingDeg += 360.0;
            }
            return;
        }
    }
}

void MeshService::updateLocalPosition(double lat, double lon, int32_t alt) {
    if (_nodes.empty()) return;
    _nodes[0].hasPosition = true;
    _nodes[0].lat = lat;
    _nodes[0].lon = lon;
    _nodes[0].alt = alt;

    for (size_t i = 1; i < _nodes.size(); ++i) {
        if (_nodes[i].hasPosition) {
            double dlat = (_nodes[i].lat - lat) * 111.0;
            double dlon = (_nodes[i].lon - lon) * 111.0 * cos(lat * 0.0174532925);
            _nodes[i].distanceKm = sqrt(dlat * dlat + dlon * dlon);
            _nodes[i].bearingDeg = atan2(dlon, dlat) * 57.2957795;
            if (_nodes[i].bearingDeg < 0) _nodes[i].bearingDeg += 360.0;
        }
    }
}

const MeshNode* MeshService::findNode(uint32_t nodeNum) const {
    for (const auto& node : _nodes) {
        if (node.nodeNum == nodeNum) return &node;
    }
    return nullptr;
}

MeshNode* MeshService::findNode(uint32_t nodeNum) {
    for (auto& node : _nodes) {
        if (node.nodeNum == nodeNum) return &node;
    }
    return nullptr;
}

std::vector<ChatMessage> MeshService::getDirectMessages(uint32_t nodeNum) const {
    std::vector<ChatMessage> res;
    for (const auto& msg : _directMessages) {
        if (msg.fromNode == nodeNum || msg.toNode == nodeNum) {
            res.push_back(msg);
        }
    }
    return res;
}

uint32_t MeshService::getDirectMessageCount(uint32_t nodeNum) const {
    uint32_t n = 0;
    for (const auto& msg : _directMessages) {
        if (msg.fromNode == nodeNum || msg.toNode == nodeNum) ++n;
    }
    return n;
}

uint32_t MeshService::getBroadcastMessageCountForPreset(const char* preset) const {
    uint32_t n = 0;
    const char* current = getPresetName(_modemPreset);
    for (const auto& msg : _broadcastMessages) {
        const char* label = (msg.channelPreset[0] != '\0') ? msg.channelPreset : current;
        if (preset && strcmp(label, preset) == 0) ++n;
    }
    return n;
}

uint32_t MeshService::getConversationMessageCount() const {
    return (uint32_t)(_broadcastMessages.size() + _nodes.size());
}

std::vector<ChatMessage> MeshService::getBroadcastMessagesForPreset(const char* preset) const {
    std::vector<ChatMessage> res;
    for (const auto& msg : _broadcastMessages) {
        if (msg.channelPreset[0] != '\0') {
            if (strcmp(msg.channelPreset, preset) == 0) res.push_back(msg);
        } else {
            if (strcmp(getPresetName(_modemPreset), preset) == 0) res.push_back(msg);
        }
    }
    return res;
}

std::vector<std::string> MeshService::getActiveBroadcastPresets() const {
    std::vector<std::string> presets;
    std::string curPreset = getPresetName(_modemPreset);
    presets.push_back(curPreset);

    for (const auto& msg : _broadcastMessages) {
        if (msg.channelPreset[0] != '\0' && msg.channelPreset != curPreset) {
            bool exists = false;
            for (const auto& p : presets) {
                if (p == msg.channelPreset) {
                    exists = true;
                    break;
                }
            }
            if (!exists) presets.push_back(msg.channelPreset);
        }
    }
    return presets;
}

uint32_t MeshService::getUnreadCountForChannel(const char* preset) const {
    uint32_t cnt = 0;
    for (const auto& msg : _broadcastMessages) {
        if (!msg.isRead && !msg.isOutgoing) {
            if (msg.channelPreset[0] != '\0') {
                if (strcmp(msg.channelPreset, preset) == 0) cnt++;
            } else {
                if (strcmp(getPresetName(_modemPreset), preset) == 0) cnt++;
            }
        }
    }
    return cnt;
}

uint32_t MeshService::getUnreadCountForNode(uint32_t nodeNum) const {
    uint32_t cnt = 0;
    for (const auto& msg : _directMessages) {
        if (msg.fromNode == nodeNum && !msg.isRead && !msg.isOutgoing) {
            cnt++;
        }
    }
    return cnt;
}

void MeshService::markChannelMessagesRead(const char* preset) {
    for (auto& msg : _broadcastMessages) {
        if (msg.channelPreset[0] != '\0') {
            if (strcmp(msg.channelPreset, preset) == 0) msg.isRead = true;
        } else {
            if (strcmp(getPresetName(_modemPreset), preset) == 0) msg.isRead = true;
        }
    }
    uint32_t u = 0;
    for (const auto& m : _broadcastMessages) if (!m.isRead && !m.isOutgoing) u++;
    for (const auto& m : _directMessages) if (!m.isRead && !m.isOutgoing) u++;
    _unreadCount = u;
}

void MeshService::markDirectMessagesRead(uint32_t nodeNum) {
    for (auto& msg : _directMessages) {
        if (msg.fromNode == nodeNum) msg.isRead = true;
    }
    uint32_t u = 0;
    for (const auto& m : _broadcastMessages) if (!m.isRead && !m.isOutgoing) u++;
    for (const auto& m : _directMessages) if (!m.isRead && !m.isOutgoing) u++;
    _unreadCount = u;
}

void MeshService::markMessagesRead() {
    _unreadCount = 0;
    for (auto& m : _broadcastMessages) m.isRead = true;
    for (auto& m : _directMessages) m.isRead = true;
}

bool MeshService::transmitRaw(const uint8_t* buf, size_t len, bool bypassDutyCycle) {
    if (!_radioReady || !_radio) return false;
    // Defensive: in the CLOCK power mode the SX1262 is parked in sleep with its rail on; touching the
    // SPI bus then would only push garbage to an unconfigured chip. Today no TX path runs in that
    // mode, but the invariant is cheap to state.
    if (_radioSuspended) return false;
    // 255 is the hard on-air limit (RadioLib rejects more: the SX1262 length field is 8 bit).
    if (len < sizeof(MeshtasticHeader) || len > 255) {
        ESP_LOGE(TAG, "Frame of %u bytes not transmittable (limit 255)", (unsigned)len);
        return false;
    }

    // Duty-cycle limiter: it stops automatic traffic (periodic broadcasts, relays, retries and the
    // queue processors, which check it up front so no retry is burned) and explicit user messages,
    // whose text is preserved by the callers. ACKs and protocol replies bypass it (see sendAck).
    if (!bypassDutyCycle && !canTransmit()) {
        ESP_LOGW(TAG, "TX blocked by duty cycle: %.2f%% of the hour already used (limit %u%%; set duty to 100%% to override)",
                 (double)getHourlyAirtimePct(), (unsigned)getEffectiveDutyCyclePct());
        return false;
    }

    // Every transmission carries our last node-number byte in relay_node, matching official firmware
    uint8_t txBuf[256];
    memcpy(txBuf, buf, len);
    txBuf[15] = (uint8_t)(_localNodeNum & 0xFF);

    _txActive = true;
    _txActivityTimer = millis();

    BSP::getInstance().setLedTxActivity(80);

    // Transmit under the radio mutex: the core-0 task must not touch the SPI while we do.
    int state = RADIOLIB_ERR_UNKNOWN;
    if (lockRadio(500)) {
        recordAirtimeTx(len);
        _radio->standby();
        state = _radio->transmit(txBuf, len);
        // A completed TX pulses DIO1 for TX_DONE: clear the radio IRQs before re-arming the receiver,
        // otherwise a read could return our own packet out of the shared FIFO.
        _radio->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
        _radio->startReceive();
        unlockRadio();
    } else {
        ESP_LOGE(TAG, "Radio mutex timeout, packet not transmitted");
    }

    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "TX failed with code: %d", state);
        _lastTxErrorCode = (uint32_t)state;
        _lastTxErrorMillis = millis();
        return false;
    }

    _txTotal++;
    _lastTxMillis = millis();
    ESP_LOGI(TAG, "TX transmitted %u bytes successfully (chHash 0x%02x)", (unsigned)len, (unsigned)txBuf[13]);
    return true;
}

// Generic outbound packet builder: Protobuf Data -> encrypt (PKI or channel) -> header -> radio
bool MeshService::sendDataPacket(uint32_t dest, uint32_t portnum, const uint8_t* payload, size_t payloadLen,
                                 bool wantResponse, bool wantAck, bool allowPki, uint8_t* rawOut, size_t* rawLenOut,
                                 uint32_t* packetIdOut, bool* usedPkiOut, uint32_t requestId, uint32_t forcedId,
                                 bool bypassDutyCycle) {
    if (dest == 0 || dest == _localNodeNum) return false;
    const bool isBroadcast = (dest == NODENUM_BROADCAST);
    const uint32_t pktId = (forcedId != 0) ? forcedId : ((uint32_t)esp_random() | 1u);

    // Build the Data protobuf. 256 B is the natural cap: the trailing fields (want_response,
    // request_id, bitfield) are appended after the payload, so a 240 B buffer overflowed by 5 bytes
    // with a 233 B payload and a request_id.
    uint8_t dataBuf[256];
    size_t dLen = 0;
    dataBuf[dLen++] = (1 << 3) | 0; // portnum
    dLen += pbWriteVarint(dataBuf + dLen, portnum);

    if (payload && payloadLen > 0) {
        if (payloadLen > 233) payloadLen = 233;
        if (dLen + 2 + payloadLen > sizeof(dataBuf)) return false;
        dataBuf[dLen++] = (2 << 3) | 2; // payload
        dLen += pbWriteVarint(dataBuf + dLen, payloadLen);
        memcpy(dataBuf + dLen, payload, payloadLen);
        dLen += payloadLen;
    }

    if (dLen + 2 + 5 + 2 > sizeof(dataBuf)) return false; // room for the trailing fields

    if (wantResponse) {
        dataBuf[dLen++] = (3 << 3) | 0; // want_response
        dLen += pbWriteVarint(dataBuf + dLen, 1);
    }
    if (requestId != 0) {
        dataBuf[dLen++] = (6 << 3) | 5; // request_id (fixed32)
        memcpy(dataBuf + dLen, &requestId, 4);
        dLen += 4;
    }
    // Data.bitfield: bit 1 mirrors want_response (newer firmware) / bit 0 = ok_to_mqtt
    {
        uint32_t bitfield = wantResponse ? 0x02u : 0x00u;
        if (bitfield != 0) {
            dataBuf[dLen++] = (9 << 3) | 0;
            dLen += pbWriteVarint(dataBuf + dLen, bitfield);
        }
    }

    // Decide the crypto transport: PKI for eligible direct messages when the peer key is known
    bool usePki = false;
    uint8_t pkiKey[32] = {0};
    if (allowPki && !isBroadcast && _pkiEnabled && _hasLocalKeys && portnum != 4 && portnum != 5 && portnum != 70 &&
        portnum != 3) {
        const MeshNode* peer = findNode(dest);
        if (peer && peer->hasPublicKey && !peer->isLicensed) {
            if (pkiDeriveKey(peer->publicKey, _localPrivateKey, pkiKey)) usePki = true;
        }
    }

    uint8_t cipherData[265];
    size_t cipherLen = 0;
    uint8_t chanHash = getTxChannelHash();
    if (usePki) {
        if (!pkiEncryptPayload(pkiKey, pktId, _localNodeNum, dataBuf, dLen, cipherData)) {
            ESP_LOGE(TAG, "PKI encryption failed for !%08x", (unsigned)dest);
            return false;
        }
        cipherLen = dLen + 12;
    } else {
        aesCtrCryptN(getTxChannelKey(), getTxChannelKeyLen(), pktId, _localNodeNum, dataBuf, dLen, cipherData);
        cipherLen = dLen;
        if (!isBroadcast && allowPki && (portnum == 1 || portnum == 67)) {
            const MeshNode* peer = findNode(dest);
            if (!peer || !peer->hasPublicKey) {
                ESP_LOGW(TAG, "No public key for !%08x: sending channel-encrypted (legacy) packet", (unsigned)dest);
            }
        }
    }

    uint8_t packet[300];
    MeshtasticHeader* hdr = reinterpret_cast<MeshtasticHeader*>(packet);
    hdr->to = dest;
    hdr->from = _localNodeNum;
    hdr->id = pktId;
    const uint8_t hop = _hopLimit & 0x07;
    hdr->flags = hop | (hop << 5) | ((wantAck && !isBroadcast) ? 0x08 : 0x00);
    hdr->channel = usePki ? 0 : chanHash;
    hdr->next_hop = 0;
    hdr->relay_node = (uint8_t)(_localNodeNum & 0xFF);

    memcpy(packet + sizeof(MeshtasticHeader), cipherData, cipherLen);
    const size_t totalLen = sizeof(MeshtasticHeader) + cipherLen;

    // Never hand an over-long frame to the radio: RadioLib would reject it (or, worse, a caller would
    // silently lose the packet). Report it so the failure is visible in the log/settings Info page.
    if (totalLen > 255) {
        ESP_LOGE(TAG, "Packet too large (%u bytes, portnum %u, pki=%u): not sent",
                 (unsigned)totalLen, (unsigned)portnum, usePki ? 1u : 0u);
        return false;
    }

    if (rawOut && rawLenOut && *rawLenOut >= totalLen) {
        memcpy(rawOut, packet, totalLen);
        *rawLenOut = totalLen;
    }
    if (packetIdOut) *packetIdOut = pktId;
    if (usedPkiOut) *usedPkiOut = usePki;

    return transmitRaw(packet, totalLen, bypassDutyCycle);
}

bool MeshService::sendBroadcastMessage(const char* text, const char* channelName) {
    if (!text || strlen(text) == 0) return false;

    const uint8_t* encKey = getTxChannelKey();
    uint8_t encKeyLen = getTxChannelKeyLen();
    uint8_t chanHash = getTxChannelHash();
    const char* presetName = getPresetName(_modemPreset);

    const char* primaryLabel = getPresetName(_modemPreset);
    const char* primaryCanonical = getPrimaryChannelName();
    const bool isPrimaryLabel = !channelName || channelName[0] == '\0' ||
                                strcmp(channelName, primaryLabel) == 0 ||
                                strcmp(channelName, primaryCanonical) == 0;
    if (!isPrimaryLabel) {
        // Secondary channel: use its own name/PSK
        presetName = channelName;
        bool found = false;
        for (const auto& ch : _secondaryChannels) {
            if (ch.enabled && ch.pskLen > 0 && strcmp(ch.name, channelName) == 0) {
                encKey = ch.psk;
                encKeyLen = ch.pskLen;
                found = true;
                break;
            }
        }
        // Unknown label: keep the primary channel crypto but tag the message with the label
        if (!found) {
            encKey = getTxChannelKey();
            encKeyLen = getTxChannelKeyLen();
            chanHash = getTxChannelHash();
        } else {
            chanHash = getChannelHashFor(channelName, encKey, encKeyLen);
        }
    }

    // Encrypt directly so secondary channels can be used (sendDataPacket only handles the primary)
    uint8_t dataBuf[240];
    size_t dLen = 0;
    dataBuf[dLen++] = (1 << 3) | 0;
    dLen += pbWriteVarint(dataBuf + dLen, 1);
    size_t textLen = strlen(text);
    // 223 B: fits the worst-case frame (16 + 5 + 223 = 244 < 255) and the 224 B history buffers.
    if (textLen > 223) {
        ESP_LOGW(TAG, "Broadcast text truncated to 223 bytes");
        textLen = 223;
    }
    dataBuf[dLen++] = (2 << 3) | 2;
    dLen += pbWriteVarint(dataBuf + dLen, textLen);
    memcpy(dataBuf + dLen, text, textLen);
    dLen += textLen;

    const uint32_t pktId = (uint32_t)esp_random() | 1u;
    uint8_t cipherData[256];
    aesCtrCryptN(encKey, encKeyLen, pktId, _localNodeNum, dataBuf, dLen, cipherData);

    uint8_t packet[300];
    MeshtasticHeader* hdr = reinterpret_cast<MeshtasticHeader*>(packet);
    hdr->to = NODENUM_BROADCAST;
    hdr->from = _localNodeNum;
    hdr->id = pktId;
    const uint8_t hop = _hopLimit & 0x07;
    hdr->flags = hop | (hop << 5);
    hdr->channel = chanHash;
    hdr->next_hop = 0;
    hdr->relay_node = (uint8_t)(_localNodeNum & 0xFF);
    memcpy(packet + sizeof(MeshtasticHeader), cipherData, dLen);
    const size_t totalLen = sizeof(MeshtasticHeader) + dLen;

    bool ok = transmitRaw(packet, totalLen);
    if (ok) {
        ChatMessage msg = {};
        msg.id = pktId;
        msg.fromNode = _localNodeNum;
        msg.toNode = NODENUM_BROADCAST;
        strncpy(msg.senderShort, _localShortName, sizeof(msg.senderShort) - 1);
        strncpy(msg.text, text, sizeof(msg.text) - 1);
        uint32_t curUnix = BSP::getInstance().getRtcUnix();
        msg.timestamp = (curUnix > 1700000000UL) ? curUnix : millis();
        msg.isOutgoing = true;
        msg.isRead = true;
        msg.isAcked = false;
        msg.isFailed = false;

        formatClockLabel(msg.timeStr, sizeof(msg.timeStr));
        strncpy(msg.channelPreset, presetName, sizeof(msg.channelPreset) - 1);
        msg.seq = ++_globalMsgSeq;

        _broadcastMessages.push_back(msg);
        if (_broadcastMessages.size() > MAX_MESSAGES) _broadcastMessages.pop_front();
        SavedMessage sm = {};
        sm.id = msg.id;
        sm.fromNode = msg.fromNode;
        sm.toNode = msg.toNode;
        strncpy(sm.senderShort, _localShortName, sizeof(sm.senderShort) - 1);
        strncpy(sm.text, text, sizeof(sm.text) - 1);
        sm.timestamp = msg.timestamp;
        sm.isOutgoing = true;
        sm.isAcked = false;
        strncpy(sm.timeStr, msg.timeStr, sizeof(sm.timeStr) - 1);
        strncpy(sm.channelPreset, msg.channelPreset, sizeof(sm.channelPreset) - 1);
        StorageManager::getInstance().appendChatMessage(sm);
    }
    return ok;
}

bool MeshService::sendDirectMessage(uint32_t toNode, const char* text) {
    if (!text || strlen(text) == 0) return false;
    if (toNode == 0 || toNode == NODENUM_BROADCAST || toNode == _localNodeNum) return false;

    // The on-air frame is capped at 255 B: 16 B header + (12 B with PKI) + Data{portnum,payload}.
    // With PKI the text must stop at 222 B, without it 223 B is the limit imposed by the message
    // buffers; the old 233 B ceiling overflowed the frame and dropped the DM without any feedback.
    const MeshNode* dmPeer = findNode(toNode);
    const bool pkiExpected = _pkiEnabled && _hasLocalKeys && dmPeer && dmPeer->hasPublicKey &&
                             dmPeer->isLicensed == 0;
    const size_t maxText = pkiExpected ? 222u : 223u;

    char dmText[234];
    size_t textLen = strlen(text);
    if (textLen > maxText) {
        ESP_LOGW(TAG, "DM text truncated to %u bytes (%s transport)", (unsigned)maxText,
                 pkiExpected ? "PKI" : "channel");
        textLen = maxText;
    }
    memcpy(dmText, text, textLen);
    dmText[textLen] = '\0';

    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;

    if (!sendDataPacket(toNode, 1, (const uint8_t*)dmText, textLen, false, true, true,
                        raw, &rawLen, &pktId, &usedPki)) {
        return false;
    }

    ESP_LOGI(TAG, "DM to !%08x sent (%s, %u bytes)", (unsigned)toNode, usedPki ? "PKI" : "channel-encrypted",
             (unsigned)rawLen);

    // Meshtastic >= 2.5 drops channel-encrypted ("legacy") DMs, so when the peer key is still unknown ask
    // for a NodeInfo and silently re-send the very same packet id as a real PKI DM once the key arrives.
    if (!usedPki && _pkiEnabled && _hasLocalKeys) {
        const MeshNode* peer = findNode(toNode);
        if (!peer || (!peer->hasPublicKey && !peer->isLicensed)) {
            requestNodeInfo(toNode);
            queueDeferredDm(toNode, dmText, pktId, 5000);
        }
    }

    ChatMessage msg = {};
    msg.id = pktId;
    msg.fromNode = _localNodeNum;
    msg.toNode = toNode;
    strncpy(msg.senderShort, _localShortName, sizeof(msg.senderShort) - 1);
    // dmText, not text: the on-air frame was clamped to maxText, so the bubble/history must show what
    // actually left the radio (the untruncated copy made the chat look like the full DM was sent).
    strncpy(msg.text, dmText, sizeof(msg.text) - 1);
    uint32_t curUnix = BSP::getInstance().getRtcUnix();
    msg.timestamp = (curUnix > 1700000000UL) ? curUnix : millis();
    msg.isOutgoing = true;
    msg.isRead = true;
    msg.isAcked = false;
    msg.isFailed = false;

    formatClockLabel(msg.timeStr, sizeof(msg.timeStr));
    strncpy(msg.channelPreset, getPresetName(_modemPreset), sizeof(msg.channelPreset) - 1);
    msg.seq = ++_globalMsgSeq;

    _directMessages.push_back(msg);
    if (_directMessages.size() > MAX_MESSAGES) _directMessages.pop_front();
    SavedMessage sm = {};
    sm.id = msg.id;
    sm.fromNode = msg.fromNode;
    sm.toNode = msg.toNode;
    strncpy(sm.senderShort, _localShortName, sizeof(sm.senderShort) - 1);
    strncpy(sm.text, dmText, sizeof(sm.text) - 1);
    sm.timestamp = msg.timestamp;
    sm.isOutgoing = true;
    sm.isAcked = false;
    strncpy(sm.timeStr, msg.timeStr, sizeof(sm.timeStr) - 1);
    strncpy(sm.channelPreset, msg.channelPreset, sizeof(sm.channelPreset) - 1);
    StorageManager::getInstance().appendChatMessage(sm);

    PendingAck pa;
    pa.packetId = pktId;
    pa.toNode = toNode;
    strncpy(pa.text, dmText, sizeof(pa.text) - 1);
    pa.retriesLeft = 3;
    uint32_t baseTimeoutMs = 6000;
    if (_modemPreset == ModemPreset::LongSlow) baseTimeoutMs = 25000;
    else if (_modemPreset == ModemPreset::LongModerate || _modemPreset == ModemPreset::LongFast) baseTimeoutMs = 12000;
    pa.timeoutMs = baseTimeoutMs;
    pa.lastSentMillis = millis();
    if (rawLen > sizeof(pa.rawPacket)) {
        // Never copy more than the retry buffer holds: this silently corrupted the heap before
        ESP_LOGE(TAG, "DM frame too large for retry buffer (%u > %u), not queued for ACK",
                 (unsigned)rawLen, (unsigned)sizeof(pa.rawPacket));
        return true; // the packet went out, we just cannot retransmit it
    }
    memcpy(pa.rawPacket, raw, rawLen);
    pa.rawPacketLen = rawLen;
    _pendingAcks.push_back(pa);
    return true;
}

// Builds ONLY the User protobuf of a NODEINFO_APP message.
// The Data{portnum, payload} wrapper is added by sendDataPacket(): building it here too meant the
// User payload was thrown away and peers received an EMPTY NodeInfo (no name, no key, no hw_model).
size_t MeshService::buildNodeInfoPayload(uint8_t* out, size_t cap) const {
    uint8_t userBuf[200];
    size_t uLen = 0;

    auto writeBytesField = [&](uint8_t field, const void* data, size_t len) -> bool {
        if (uLen + 2 + len > sizeof(userBuf)) return false;
        userBuf[uLen++] = (field << 3) | 2;
        uLen += pbWriteVarint(userBuf + uLen, len);
        memcpy(userBuf + uLen, data, len);
        uLen += len;
        return true;
    };

    writeBytesField(1, _localIdStr, strlen(_localIdStr));
    writeBytesField(2, _localLongName, strlen(_localLongName));
    writeBytesField(3, _localShortName, strlen(_localShortName));

    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    writeBytesField(4, mac, 6);

    userBuf[uLen++] = (5 << 3) | 0; // hw_model (255 = PRIVATE_HW, same as official Paper Mono build)
    uLen += pbWriteVarint(userBuf + uLen, 255);
    userBuf[uLen++] = (7 << 3) | 0; // role
    uLen += pbWriteVarint(userBuf + uLen, static_cast<uint64_t>(_nodeRole));

    // Public key: required for peer-side PKI (closed padlock) and for them to be able to DM us
    if (_pkiEnabled && _hasLocalKeys) {
        writeBytesField(8, _localPublicKey, 32);
    }

    if (uLen > cap) return 0;
    memcpy(out, userBuf, uLen);
    return uLen;
}

void MeshService::sendNodeInfoTo(uint32_t dest, bool wantResponse, uint32_t requestId) {
    if (dest == 0 || (dest == _localNodeNum && dest != NODENUM_BROADCAST)) return;
    uint8_t userBuf[200];
    size_t uLen = buildNodeInfoPayload(userBuf, sizeof(userBuf));
    if (uLen == 0) return;

    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;
    if (sendDataPacket(dest, 4, userBuf, uLen, wantResponse, false, false, raw, &rawLen, &pktId, &usedPki, requestId)) {
        _lastNodeInfoTx = millis();
        ESP_LOGI(TAG, "NodeInfo sent to %s (user=%u bytes, wantResp=%u)",
                 (dest == NODENUM_BROADCAST) ? "broadcast" : "peer", (unsigned)uLen, wantResponse ? 1 : 0);
    }
}

void MeshService::broadcastNodeInfo(bool wantResponse) {
    sendNodeInfoTo(NODENUM_BROADCAST, wantResponse);
}

void MeshService::handleNodeInfoRequest(uint32_t from, bool wantResponse, uint32_t requestId) {
    (void)wantResponse;
    if (from == 0 || from == NODENUM_BROADCAST || from == _localNodeNum) return;

    uint32_t now = millis();
    // Global rate limit: at most one unicast NodeInfo reply every 4 seconds to protect airtime & battery
    if (now - _lastNodeInfoRequestReply < 4000) {
        ESP_LOGD(TAG, "NodeInfo reply to !%08x suppressed (global rate limit)", (unsigned)from);
        return;
    }

    // Per-node rate limit: at most one unicast NodeInfo reply per peer every 60s
    MeshNode* node = findNode(from);
    if (node) {
        if (now - node->lastNodeInfoReply < 60000) {
            ESP_LOGD(TAG, "NodeInfo reply to !%08x suppressed (node reply within 60s)", (unsigned)from);
            return;
        }
        node->lastNodeInfoReply = now;
    }

    _lastNodeInfoRequestFrom = from;
    _lastNodeInfoRequestReply = now;
    queueDeferred(DeferredKind::NodeInfoUnicast, from, 400 + (esp_random() % 800), requestId);
    ESP_LOGI(TAG, "Peer !%08x asked for our NodeInfo -> queued unicast reply", (unsigned)from);
}

void MeshService::requestNodeInfo(uint32_t nodeNum) {
    if (nodeNum == 0 || nodeNum == NODENUM_BROADCAST || nodeNum == _localNodeNum) return;

    // Official semantics: a unicast NodeInfo of our own with want_response set makes the peer reply
    // with its NodeInfo (which carries the public key we need for PKI direct messages). Empty-payload
    // requests carry no identity and are throttled/dropped the same way, so send the real thing.
    uint32_t now = millis();
    NodeInfoRequest* slot = nullptr;        // entry already tracking this node
    NodeInfoRequest* freeSlot = nullptr;    // never used entry
    NodeInfoRequest* expiredSlot = nullptr; // entry whose 12h window is already over
    for (auto& r : _nodeInfoRequests) {
        if (r.nodeNum == nodeNum) { slot = &r; break; }
        if (!freeSlot && r.nodeNum == 0) freeSlot = &r;
        if (!expiredSlot && r.nodeNum != 0 && (now - r.lastMillis) >= NODEINFO_REQUEST_COOLDOWN_MS) {
            expiredSlot = &r;
        }
    }

    if (slot) {
        if ((now - slot->lastMillis) < NODEINFO_REQUEST_COOLDOWN_MS) {
            ESP_LOGD(TAG, "NodeInfo request to !%08x still in cooldown (%us ago)", (unsigned)nodeNum,
                     (unsigned)((now - slot->lastMillis) / 1000));
            return;
        }
    } else {
        slot = freeSlot ? freeSlot : expiredSlot;
        if (!slot) {
            // Every slot is inside its 12h window. Evicting one would let us re-ask a peer too early,
            // which resets the peer's reply-suppression window and makes its answer impossible.
            ESP_LOGW(TAG, "NodeInfo request table full: skipping request to !%08x", (unsigned)nodeNum);
            return;
        }
    }
    slot->nodeNum = nodeNum;
    slot->lastMillis = now;

    ESP_LOGI(TAG, "Asking !%08x for its NodeInfo (unicast, want_response) - next attempt in 12h", (unsigned)nodeNum);
    sendNodeInfoTo(nodeNum, /*wantResponse=*/true, 0);
}

void MeshService::sendTelemetryTo(uint32_t dest, bool wantResponse, uint32_t requestId) {
    BatteryState bs = BSP::getInstance().getBatteryState();

    uint8_t dmBuf[64];
    size_t dmLen = 0;
    dmBuf[dmLen++] = (1 << 3) | 0;
    dmLen += pbWriteVarint(dmBuf + dmLen, bs.percentage);

    float volt = (float)bs.voltageMv / 1000.0f;
    dmBuf[dmLen++] = (2 << 3) | 5;
    memcpy(dmBuf + dmLen, &volt, 4);
    dmLen += 4;

    dmBuf[dmLen++] = (3 << 3) | 5;
    memcpy(dmBuf + dmLen, &_channelUtilization, 4);
    dmLen += 4;

    dmBuf[dmLen++] = (4 << 3) | 5;
    memcpy(dmBuf + dmLen, &_airtimeTx, 4);
    dmLen += 4;

    uint32_t uptimeSec = millis() / 1000;
    dmBuf[dmLen++] = (5 << 3) | 0;
    dmLen += pbWriteVarint(dmBuf + dmLen, uptimeSec);

    uint8_t telemBuf[96];
    size_t tLen = 0;
    // Telemetry.time (field 1, fixed32): peers' apps show the measurement time from this, and it is
    // what lets them sync their clock from the mesh like we do. Only sent when our RTC is valid, so
    // we never advertise a bogus 1970 date.
    const uint32_t nowUnix = BSP::getInstance().getRtcUnix();
    if (nowUnix > 1700000000UL) {
        telemBuf[tLen++] = (1 << 3) | 5;
        memcpy(telemBuf + tLen, &nowUnix, 4);
        tLen += 4;
    }
    telemBuf[tLen++] = (2 << 3) | 2;
    tLen += pbWriteVarint(telemBuf + tLen, dmLen);
    memcpy(telemBuf + tLen, dmBuf, dmLen);
    tLen += dmLen;

    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;
    if (sendDataPacket(dest, 67, telemBuf, tLen, wantResponse, false, true, raw, &rawLen, &pktId, &usedPki, requestId)) {
        ESP_LOGI(TAG, "Telemetry sent to %s (Bat=%d%%, Volt=%.2fV, pki=%u)",
                 (dest == NODENUM_BROADCAST) ? "broadcast" : "peer", bs.percentage, volt, usedPki ? 1 : 0);
    }
}

void MeshService::broadcastTelemetry() {
    sendTelemetryTo(NODENUM_BROADCAST, false);
}

// Official PositionModule::allocReply() refuses a reply sent less than 3 minutes ago: a peer asking in
// a loop must not be able to turn us into a position beacon (each reply also costs airtime/battery).
static constexpr uint32_t POSITION_REPLY_MIN_INTERVAL_MS = 3UL * 60UL * 1000UL;

void MeshService::sendPositionTo(uint32_t dest, uint8_t precisionBits, bool wantResponse, uint32_t requestId) {
    if (_nodes.empty() || !_nodes[0].hasPosition) return;

    uint8_t pBuf[64];
    size_t pLen = 0;
    int32_t lat_i = (int32_t)lround(_nodes[0].lat * 1e7);
    int32_t lon_i = (int32_t)lround(_nodes[0].lon * 1e7);

    pBuf[pLen++] = (1 << 3) | 5;
    memcpy(pBuf + pLen, &lat_i, 4);
    pLen += 4;
    pBuf[pLen++] = (2 << 3) | 5;
    memcpy(pBuf + pLen, &lon_i, 4);
    pLen += 4;

    if (_nodes[0].alt != 0) {
        pBuf[pLen++] = (3 << 3) | 0;
        pLen += pbWriteVarint(pBuf + pLen, (uint64_t)(int64_t)_nodes[0].alt);
    }

    uint32_t unixTime = BSP::getInstance().getRtcUnix();
    if (unixTime > 1700000000UL) {
        pBuf[pLen++] = (4 << 3) | 5;
        memcpy(pBuf + pLen, &unixTime, 4);
        pLen += 4;
        pBuf[pLen++] = (7 << 3) | 5;
        memcpy(pBuf + pLen, &unixTime, 4);
        pLen += 4;
    }

    pBuf[pLen++] = (5 << 3) | 0; // location_source = LOC_MANUAL
    pLen += pbWriteVarint(pBuf + pLen, 1);
    pBuf[pLen++] = (23 << 3) | 0; // precision_bits
    pLen += pbWriteVarint(pBuf + pLen, precisionBits ? precisionBits : 32);

    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;
    if (sendDataPacket(dest, 3, pBuf, pLen, wantResponse, false, false, raw, &rawLen, &pktId, &usedPki, requestId)) {
        ESP_LOGI(TAG, "Position sent to %s (%.5f, %.5f)", (dest == NODENUM_BROADCAST) ? "broadcast" : "peer",
                 _nodes[0].lat, _nodes[0].lon);
    }
}

void MeshService::broadcastPosition() {
    sendPositionTo(NODENUM_BROADCAST, 32, /*wantResponse=*/false);
}

// NeighborInfo (portnum 71): our list of direct neighbours (hopsAway == 0) with their SNR.
// Used by peers/clients to draw the mesh topology; harmless when nobody listens.
void MeshService::broadcastNeighborInfo() {
    struct NeighborEntry {
        uint32_t nodeNum;
        float snr;
    };
    NeighborEntry best[10];
    size_t count = 0;

    uint32_t now = millis();
    for (const auto& n : _nodes) {
        if (n.nodeNum == _localNodeNum) continue;
        if (n.hopsAway != 0) continue;                       // only direct (0-hop) neighbours
        if (n.lastHeard == 0 || (now - n.lastHeard) > 7200000UL) continue; // stale (2h)
        // Keep the 10 strongest, insertion sort (list is tiny)
        if (count < 10) {
            best[count].nodeNum = n.nodeNum;
            best[count].snr = n.snr;
            size_t j = count;
            while (j > 0 && best[j - 1].snr < best[j].snr) {
                NeighborEntry tmp = best[j - 1];
                best[j - 1] = best[j];
                best[j] = tmp;
                --j;
            }
            ++count;
        } else if (n.snr > best[9].snr) {
            best[9].nodeNum = n.nodeNum;
            best[9].snr = n.snr;
            size_t j = 9;
            while (j > 0 && best[j - 1].snr < best[j].snr) {
                NeighborEntry tmp = best[j - 1];
                best[j - 1] = best[j];
                best[j] = tmp;
                --j;
            }
        }
    }

    uint8_t buf[220];
    size_t len = 0;
    buf[len++] = (1 << 3) | 0; // node_id
    len += pbWriteVarint(buf + len, _localNodeNum);
    buf[len++] = (2 << 3) | 0; // last_sent_by_id
    len += pbWriteVarint(buf + len, _localNodeNum);
    buf[len++] = (3 << 3) | 0; // node_broadcast_interval_secs
    len += pbWriteVarint(buf + len, _neighborInfoIntervalSec);

    for (size_t i = 0; i < count; ++i) {
        uint8_t sub[12];
        size_t sLen = 0;
        sub[sLen++] = (1 << 3) | 0; // Neighbor.node_id
        sLen += pbWriteVarint(sub + sLen, best[i].nodeNum);
        sub[sLen++] = (2 << 3) | 5; // Neighbor.snr (float)
        memcpy(sub + sLen, &best[i].snr, 4);
        sLen += 4;

        if (len + 2 + sLen > sizeof(buf)) break;
        buf[len++] = (4 << 3) | 2; // NeighborInfo.neighbors (repeated)
        len += pbWriteVarint(buf + len, sLen);
        memcpy(buf + len, sub, sLen);
        len += sLen;
    }

    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;
    if (sendDataPacket(NODENUM_BROADCAST, 71, buf, len, false, false, false, raw, &rawLen, &pktId, &usedPki)) {
        ESP_LOGI(TAG, "NeighborInfo broadcast (%u neighbours, %u bytes)", (unsigned)count, (unsigned)len);
    }
}

void MeshService::clearChatHistory() {
    _broadcastMessages.clear();
    _directMessages.clear();
    _unreadCount = 0;
    _pendingAcks.clear();
    _globalMsgSeq = 0;
    StorageManager::getInstance().clearChatHistory();
    ESP_LOGW(TAG, "Chat history cleared (RAM + storage)");
    UIEngine::getInstance().requestFullRefresh();
}

void MeshService::clearDiscoveredNodes() {
    // Keep our own entry (identity + locally configured position) across the reset
    MeshNode selfPrev;
    bool hadSelf = false;
    for (const auto& n : _nodes) {
        if (n.nodeNum == _localNodeNum) {
            selfPrev = n;
            hadSelf = true;
            break;
        }
    }

    _nodes.clear();
    StorageManager::getInstance().clearNodes();

    // Re-add ourselves so the local node stays visible in the node list
    MeshNode self;
    self.nodeNum = _localNodeNum;
    strncpy(self.idStr, _localIdStr, sizeof(self.idStr) - 1);
    strncpy(self.longName, _localLongName, sizeof(self.longName) - 1);
    strncpy(self.shortName, _localShortName, sizeof(self.shortName) - 1);
    self.role = static_cast<uint8_t>(_nodeRole);
    if (_hasLocalKeys) {
        memcpy(self.publicKey, _localPublicKey, 32);
        self.hasPublicKey = true;
    }
    BatteryState bs = BSP::getInstance().getBatteryState();
    self.battery = bs.percentage;
    self.voltage = (float)bs.voltageMv / 1000.0f;
    self.lastHeard = millis();
    if (hadSelf) {
        self.hasPosition = selfPrev.hasPosition;
        self.lat = selfPrev.lat;
        self.lon = selfPrev.lon;
        self.alt = selfPrev.alt;
    }
    _nodes.push_back(self);
    _nodeDbDirty = false;
    ESP_LOGW(TAG, "Discovered node list cleared (kept only the local node)");
    UIEngine::getInstance().requestFullRefresh();
}

bool MeshService::buildTraceroutePayload(uint8_t* out, size_t outCap, size_t& outLen, const uint8_t* inPayload, size_t inLen,
                                         bool isResponse, bool appendId, uint32_t nodeId, float rxSnr) {
    uint32_t route[8];
    int32_t snrTowards[8];
    uint32_t routeBack[8];
    int32_t snrBack[8];
    int routeCount = 0, snrTowardsCount = 0, routeBackCount = 0, snrBackCount = 0;

    auto addFixed = [](uint32_t* arr, int& count, uint32_t v) {
        if (count < 8) arr[count++] = v;
    };
    auto addSnr = [](int32_t* arr, int& count, int32_t v) {
        if (count < 8) arr[count++] = v;
    };

    // ---- Decode the incoming RouteDiscovery ----
    // Official firmware (nanopb) always writes repeated scalars as *packed* arrays, but unpacked
    // encodings are legal protobuf too, so both forms are accepted here.
    size_t off = 0;
    while (off < inLen) {
        uint64_t key = 0;
        size_t n = pbReadVarint(inPayload + off, inLen - off, key);
        if (n == 0) return false;
        off += n;
        uint32_t field = key >> 3;
        uint32_t wtype = key & 0x07;

        if (wtype == 2) { // packed scalar array
            uint64_t sl = 0;
            size_t n2 = pbReadVarint(inPayload + off, inLen - off, sl);
            if (n2 == 0) return false;
            off += n2;
            if (off + sl > inLen) return false;
            size_t pOff = 0;
            if (field == 1 || field == 3) {
                while (pOff + 4 <= sl) {
                    uint32_t v = 0;
                    memcpy(&v, inPayload + off + pOff, 4);
                    pOff += 4;
                    if (field == 1) addFixed(route, routeCount, v);
                    else addFixed(routeBack, routeBackCount, v);
                }
            } else if (field == 2 || field == 4) {
                while (pOff < sl) {
                    uint64_t v = 0;
                    size_t vn = pbReadVarint(inPayload + off + pOff, sl - pOff, v);
                    if (vn == 0) break;
                    pOff += vn;
                    if (field == 2) addSnr(snrTowards, snrTowardsCount, (int32_t)(int8_t)v);
                    else addSnr(snrBack, snrBackCount, (int32_t)(int8_t)v);
                }
            }
            off += sl;
            continue;
        }
        if (wtype == 5) {
            if (off + 4 > inLen) return false;
            uint32_t v = 0;
            memcpy(&v, inPayload + off, 4);
            off += 4;
            if (field == 1) addFixed(route, routeCount, v);
            else if (field == 3) addFixed(routeBack, routeBackCount, v);
        } else if (wtype == 0) {
            uint64_t v = 0;
            size_t n2 = pbReadVarint(inPayload + off, inLen - off, v);
            if (n2 == 0) return false;
            off += n2;
            if (field == 2) addSnr(snrTowards, snrTowardsCount, (int32_t)(int8_t)v);
            else if (field == 4) addSnr(snrBack, snrBackCount, (int32_t)(int8_t)v);
        } else if (wtype == 1) {
            if (off + 8 > inLen) return false;
            off += 8;
        } else {
            return false;
        }
    }

    // ---- Append this hop ----
    int32_t q4 = (int32_t)lroundf(rxSnr * 4.0f);
    if (q4 < -127) q4 = -127;
    if (q4 > 127) q4 = 127;

    if (isResponse) {
        if (appendId && routeBackCount < 8) routeBack[routeBackCount++] = nodeId;
        addSnr(snrBack, snrBackCount, q4);
    } else {
        if (appendId && routeCount < 8) route[routeCount++] = nodeId;
        addSnr(snrTowards, snrTowardsCount, q4);
    }

    // ---- Encode as packed arrays (matches official nanopb output) ----
    size_t o = 0;
    auto putPackedFixed32 = [&](uint8_t tag, const uint32_t* arr, int n) -> bool {
        if (n <= 0) return true;
        size_t blen = (size_t)n * 4;
        if (o + 1 + 5 + blen > outCap) return false;
        out[o++] = tag;
        o += pbWriteVarint(out + o, blen);
        for (int i = 0; i < n; ++i) {
            memcpy(out + o, &arr[i], 4);
            o += 4;
        }
        return true;
    };
    auto putPackedInt32 = [&](uint8_t tag, const int32_t* arr, int n) -> bool {
        if (n <= 0) return true;
        uint8_t tmp[8 * 11];
        size_t t = 0;
        for (int i = 0; i < n; ++i) {
            t += pbWriteVarint(tmp + t, (uint64_t)(int64_t)arr[i]);
        }
        if (o + 1 + 5 + t > outCap) return false;
        out[o++] = tag;
        o += pbWriteVarint(out + o, t);
        memcpy(out + o, tmp, t);
        o += t;
        return true;
    };

    if (!putPackedFixed32((1 << 3) | 2, route, routeCount)) return false;
    if (!putPackedInt32((2 << 3) | 2, snrTowards, snrTowardsCount)) return false;
    if (!putPackedFixed32((3 << 3) | 2, routeBack, routeBackCount)) return false;
    if (!putPackedInt32((4 << 3) | 2, snrBack, snrBackCount)) return false;

    outLen = o;
    return true;
}

bool MeshService::sendTraceroute(uint32_t dest) {
    if (dest == 0 || dest == NODENUM_BROADCAST || dest == _localNodeNum) return false;

    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;
    if (!sendDataPacket(dest, 70, nullptr, 0, true, true, false, raw, &rawLen, &pktId, &usedPki)) {
        return false;
    }

    _pendingTracerouteId = pktId;
    _lastTraceroute = TracerouteResult();
    _lastTraceroute.pending = true;
    _lastTraceroute.target = dest;
    _lastTraceroute.startMillis = millis();
    snprintf(_lastTraceroute.text, sizeof(_lastTraceroute.text), "Traceroute to !%08x in progress...", (unsigned)dest);
    ESP_LOGI(TAG, "Traceroute request sent to !%08x (id 0x%08x)", (unsigned)dest, (unsigned)pktId);
    return true;
}

void MeshService::sendTracerouteReply(uint32_t dest, uint32_t requestId, const uint8_t* payload, size_t len) {
    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;
    if (sendDataPacket(dest, 70, payload, len, false, false, false, raw, &rawLen, &pktId, &usedPki, requestId)) {
        ESP_LOGI(TAG, "Traceroute reply sent to !%08x (request 0x%08x)", (unsigned)dest, (unsigned)requestId);
    }
}

void MeshService::handleTracerouteReceived(uint32_t from, uint32_t to, uint32_t id, const uint8_t* payload, size_t len,
                                           bool isResponse, const uint8_t* chanKey, uint8_t chanKeyLen, float snr) {
    (void)chanKey;
    (void)chanKeyLen;
    const bool isToUs = (to == _localNodeNum);

    if (isToUs && !isResponse) {
        // We are the destination: append our SNR and reply to the originator
        uint8_t replyPayload[233];
        size_t replyLen = 0;
        if (buildTraceroutePayload(replyPayload, sizeof(replyPayload), replyLen, payload, len,
                                   /*isResponse=*/false, /*appendId=*/false, _localNodeNum, snr)) {
            sendTracerouteReply(from, id, replyPayload, replyLen);
        }
        return;
    }

    if (isToUs && isResponse) {
        // We are the originator: final payload carries the full route discovered
        uint8_t finalPayload[233];
        size_t finalLen = 0;
        if (!buildTraceroutePayload(finalPayload, sizeof(finalPayload), finalLen, payload, len,
                                    /*isResponse=*/true, /*appendId=*/false, _localNodeNum, snr)) {
            return;
        }

        TracerouteResult res;
        res.valid = true;
        res.pending = false;
        res.target = from;
        res.startMillis = millis();

        uint32_t route[8], routeBack[8];
        int32_t snrT[8], snrB[8];
        int rc = 0, rbc = 0, stc = 0, sbc = 0;
        size_t off = 0;
        while (off < finalLen) {
            uint64_t key = 0;
            size_t n = pbReadVarint(finalPayload + off, finalLen - off, key);
            if (n == 0) break;
            off += n;
            uint32_t field = key >> 3;
            uint32_t wtype = key & 0x07;
            if (wtype == 5) {
                if (off + 4 > finalLen) break;
                uint32_t v = 0;
                memcpy(&v, finalPayload + off, 4);
                off += 4;
                if (field == 1 && rc < 8) route[rc++] = v;
                else if (field == 3 && rbc < 8) routeBack[rbc++] = v;
            } else if (wtype == 0) {
                uint64_t v = 0;
                size_t n2 = pbReadVarint(finalPayload + off, finalLen - off, v);
                if (n2 == 0) break;
                off += n2;
                if (field == 2 && stc < 8) snrT[stc++] = (int32_t)(int8_t)v;
                else if (field == 4 && sbc < 8) snrB[sbc++] = (int32_t)(int8_t)v;
            } else if (wtype == 2) { // packed scalar array (official nanopb encoding)
                uint64_t sl = 0;
                size_t n2 = pbReadVarint(finalPayload + off, finalLen - off, sl);
                if (n2 == 0) break;
                off += n2;
                if (off + sl > finalLen) break;
                size_t pOff = 0;
                if (field == 1 || field == 3) {
                    while (pOff + 4 <= sl) {
                        uint32_t v = 0;
                        memcpy(&v, finalPayload + off + pOff, 4);
                        pOff += 4;
                        if (field == 1) { if (rc < 8) route[rc++] = v; }
                        else { if (rbc < 8) routeBack[rbc++] = v; }
                    }
                } else if (field == 2 || field == 4) {
                    while (pOff < sl) {
                        uint64_t v = 0;
                        size_t vn = pbReadVarint(finalPayload + off + pOff, sl - pOff, v);
                        if (vn == 0) break;
                        pOff += vn;
                        if (field == 2) { if (stc < 8) snrT[stc++] = (int32_t)(int8_t)v; }
                        else { if (sbc < 8) snrB[sbc++] = (int32_t)(int8_t)v; }
                    }
                }
                off += sl;
            } else if (wtype == 1) {
                off += 8;
            } else break;
        }

        for (int i = 0; i < rc; ++i) {
            TracerouteHop hop;
            hop.nodeNum = route[i];
            if (route[i] == NODENUM_BROADCAST) hop.nodeNum = 0;
            if (i < stc && snrT[i] != INT8_MIN) { hop.snr = snrT[i] / 4.0f; hop.snrKnown = true; }
            res.forward.push_back(hop);
        }
        for (int i = 0; i < rbc; ++i) {
            TracerouteHop hop;
            hop.nodeNum = routeBack[i];
            if (routeBack[i] == NODENUM_BROADCAST) hop.nodeNum = 0;
            if (i < sbc && snrB[i] != INT8_MIN) { hop.snr = snrB[i] / 4.0f; hop.snrKnown = true; }
            res.back.push_back(hop);
        }

        char txt[192];
        size_t o = 0;
        o += snprintf(txt + o, sizeof(txt) - o, "Route (%u hops) to !%08x:", (unsigned)(rc + 2), (unsigned)from);
        for (size_t i = 0; i < res.forward.size() && o < sizeof(txt) - 24; ++i) {
            const MeshNode* n = findNode(res.forward[i].nodeNum);
            o += snprintf(txt + o, sizeof(txt) - o, " %s", n ? n->shortName : "?");
            if (res.forward[i].snrKnown) o += snprintf(txt + o, sizeof(txt) - o, "(%.1f)", res.forward[i].snr);
            o += snprintf(txt + o, sizeof(txt) - o, " >");
        }
        if (res.back.size() > 0) {
            o += snprintf(txt + o, sizeof(txt) - o, " | back:");
            for (size_t i = res.back.size(); i-- > 0 && o < sizeof(txt) - 24;) {
                const MeshNode* n = findNode(res.back[i].nodeNum);
                o += snprintf(txt + o, sizeof(txt) - o, " %s", n ? n->shortName : "?");
                if (res.back[i].snrKnown) o += snprintf(txt + o, sizeof(txt) - o, "(%.1f)", res.back[i].snr);
            }
        }
        snprintf(res.text, sizeof(res.text), "%s", txt);
        _lastTraceroute = res;
        _pendingTracerouteId = 0;
        ESP_LOGI(TAG, "Traceroute result: %s", res.text);
        UIEngine::getInstance().requestFullRefresh();
    }
}

void MeshService::queueDeferred(DeferredKind kind, uint32_t dest, uint32_t delayMs, uint32_t arg, float snr) {
    if (dest == 0 || dest == _localNodeNum) return;

    // Deduplication: if an action of the same kind is already queued for this destination, do not add duplicate
    for (auto& item : _deferredSends) {
        if (item.kind == kind && item.dest == dest) {
            return;
        }
    }

    constexpr size_t MAX_DEFERRED_SENDS = 12;
    if (_deferredSends.size() >= MAX_DEFERRED_SENDS) {
        ESP_LOGW(TAG, "Deferred sends queue full (%u), dropping kind=%u to !%08x",
                 (unsigned)_deferredSends.size(), (unsigned)kind, (unsigned)dest);
        return;
    }

    DeferredSend d;
    d.kind = kind;
    d.dest = dest;
    d.atMillis = millis() + delayMs;
    d.arg = arg;
    d.snr = snr;
    _deferredSends.push_back(d);
}

void MeshService::queueDeferredDm(uint32_t dest, const char* text, uint32_t packetId, uint32_t delayMs) {
    if (dest == 0 || dest == _localNodeNum) return;

    for (auto& item : _deferredSends) {
        if (item.kind == DeferredKind::DirectMessagePkiRetry && item.dest == dest && item.arg == packetId) {
            return;
        }
    }

    constexpr size_t MAX_DEFERRED_SENDS = 12;
    if (_deferredSends.size() >= MAX_DEFERRED_SENDS) {
        ESP_LOGW(TAG, "Deferred sends queue full (%u), dropping DM retry to !%08x",
                 (unsigned)_deferredSends.size(), (unsigned)dest);
        return;
    }

    DeferredSend d;
    d.kind = DeferredKind::DirectMessagePkiRetry;
    d.dest = dest;
    d.atMillis = millis() + delayMs;
    d.arg = packetId;
    d.attemptsLeft = 20;   // ~5 minutes of patience: a peer's NodeInfo (with its key) may arrive from
                           // its own periodic broadcast while we still have the DM queued
    if (text) {
        strncpy(d.text, text, sizeof(d.text) - 1);
        d.text[sizeof(d.text) - 1] = '\0';
    } else {
        d.text[0] = '\0';
    }
    _deferredSends.push_back(d);
    ESP_LOGW(TAG, "DM to !%08x queued for PKI retry (peer key not known yet)", (unsigned)dest);
}

void MeshService::processDeferredSends() {
    if (_deferredSends.empty()) return;
    // Same rationale as the ACK queue: automatic replies and PKI retries wait for budget instead of
    // being dropped (the entries have their own deadlines/attempt counters).
    if (!canTransmit()) return;

    static uint32_t s_lastDeferredTx = 0;
    uint32_t now = millis();

    // Respect radio duty cycle: wait at least 600ms between deferred transmissions
    if (now - s_lastDeferredTx < 600) {
        return;
    }

    for (auto it = _deferredSends.begin(); it != _deferredSends.end(); ) {
        if (now < it->atMillis) {
            ++it;
            continue;
        }

        bool shouldErase = true;
        bool didTx = false;

        switch (it->kind) {
            case DeferredKind::NodeInfoUnicast:
                sendNodeInfoTo(it->dest, false, it->arg);
                didTx = true;
                break;
            case DeferredKind::TelemetryUnicast:
                sendTelemetryTo(it->dest, false, it->arg);
                didTx = true;
                break;
            case DeferredKind::PositionUnicast:
                // Reply with want_response cleared. Asking for a reply to our own reply made two
                // MonoMesh nodes answer each other's position forever (one frame + one full e-paper
                // redraw per second, per direction). The official firmware also caps these replies at
                // one every 3 minutes, which additionally stops a peer from using us as a beacon.
                if (_lastPositionReplyMillis != 0 &&
                    (now - _lastPositionReplyMillis) < POSITION_REPLY_MIN_INTERVAL_MS) {
                    ESP_LOGD(TAG, "Position reply to !%08x suppressed (one sent <3 min ago)", (unsigned)it->dest);
                    break;
                }
                _lastPositionReplyMillis = millis();
                sendPositionTo(it->dest, 32, /*wantResponse=*/false, it->arg);
                didTx = true;
                break;
            case DeferredKind::DirectMessagePkiRetry: {
                const MeshNode* peer = findNode(it->dest);
                if (peer && peer->hasPublicKey && !peer->isLicensed) {
                    uint8_t raw[300];
                    size_t rawLen = sizeof(raw);
                    uint32_t pid = 0;
                    bool usedPki = false;
                    // Same packet id as the first (channel-encrypted) attempt: receivers dedup on (from,id)
                    if (sendDataPacket(it->dest, 1, (const uint8_t*)it->text, strlen(it->text), false, true, true,
                                       raw, &rawLen, &pid, &usedPki, 0, it->arg) && usedPki) {
                        ESP_LOGI(TAG, "Deferred DM re-sent with PKI to !%08x (id 0x%08x)", (unsigned)it->dest,
                                 (unsigned)it->arg);
                    } else {
                        ESP_LOGW(TAG, "Deferred PKI re-send to !%08x failed", (unsigned)it->dest);
                    }
                    didTx = true;
                    shouldErase = true;
                } else if (it->attemptsLeft > 0) {
                    it->attemptsLeft--;
                    it->atMillis = millis() + 15000;
                    requestNodeInfo(it->dest);
                    didTx = true;
                    shouldErase = false;
                } else {
                    ESP_LOGW(TAG, "Giving up PKI retry for DM to !%08x (no public key)", (unsigned)it->dest);
                    shouldErase = true;
                }
                break;
            }
            default:
                shouldErase = true;
                break;
        }

        if (shouldErase) {
            it = _deferredSends.erase(it);
        } else {
            ++it;
        }

        if (didTx) {
            s_lastDeferredTx = millis();
            // Process at most one deferred transmission per tick to prevent radio flooding and brownouts
            break;
        }
    }
}

const char* MeshService::getPresetCanonicalName(ModemPreset preset) {
    switch (preset) {
        case ModemPreset::LongFast: return "LongFast";
        case ModemPreset::LongModerate: return "LongMod";
        case ModemPreset::LongSlow: return "LongSlow";
        case ModemPreset::MediumSlow: return "MediumSlow";
        case ModemPreset::MediumFast: return "MediumFast";
        case ModemPreset::ShortFast: return "ShortFast";
        case ModemPreset::ShortSlow: return "ShortSlow";
        default: return "LongFast";
    }
}

const char* MeshService::getPrimaryChannelName() const {
    if (_primaryChannelName[0] != '\0') return _primaryChannelName;
    return getPresetCanonicalName(_modemPreset);
}

void MeshService::setPrimaryChannelName(const char* name) {
    memset(_primaryChannelName, 0, sizeof(_primaryChannelName));
    if (name && strlen(name) > 0) {
        strncpy(_primaryChannelName, name, sizeof(_primaryChannelName) - 1);
    }
    ESP_LOGI(TAG, "Primary channel name: '%s' (hash 0x%02x)", getPrimaryChannelName(), getChannelHash());
    broadcastNodeInfo(false);
}

void MeshService::setPrimaryChannelPsk(const uint8_t* psk, uint8_t len) {
    if (len != 0 && len != 16 && len != 32) {
        ESP_LOGW(TAG, "Unsupported PSK length %u (must be 0, 16 or 32)", (unsigned)len);
        return;
    }
    memset(_primaryPsk, 0, sizeof(_primaryPsk));
    if (len > 0 && psk) memcpy(_primaryPsk, psk, len);
    _primaryPskLen = len;
    ESP_LOGI(TAG, "Primary channel PSK updated (%u bytes, hash 0x%02x)", (unsigned)len, getChannelHash());
    broadcastNodeInfo(false);
}

bool MeshService::isPrimaryChannelDefaultPsk() const {
    return (_primaryPskLen == 16) && (memcmp(_primaryPsk, MESHTASTIC_DEFAULT_KEY, 16) == 0);
}

// If we never hear anybody using our own channel hash, adopt the (hash,key) pair observed on air.
uint8_t MeshService::getTxChannelHash() const {
    return shouldUseLearnedChannel() ? _learnedHash : getChannelHash();
}

const uint8_t* MeshService::getTxChannelKey() const {
    return shouldUseLearnedChannel() ? _learnedKey : _primaryPsk;
}

uint8_t MeshService::getTxChannelKeyLen() const {
    return shouldUseLearnedChannel() ? _learnedKeyLen : _primaryPskLen;
}

bool MeshService::isTxChannelAdapted() const {
    return shouldUseLearnedChannel();
}

bool MeshService::isForeignChannelHash(uint8_t hash) const {
    if (hash == 0) return true; // PKI pseudo-channel, never a primary broadcast channel
    for (const auto& ch : _secondaryChannels) {
        if (ch.enabled && ch.pskLen > 0 && hash == getChannelHashFor(ch.name, ch.psk, ch.pskLen)) return true;
    }
    return false;
}

bool MeshService::shouldUseLearnedChannel() const {
    if (!_learnedHashValid || _learnedKeyLen == 0) return false;
    if (_learnedCount < 2) return false;                       // too few confirmations, keep our own
    if (_learnedHash == getChannelHash()) return false;        // nothing to adapt
    // The channel we heard most recently wins. If peers keep confirming our own hash, we keep using
    // it; if that hash has gone quiet and another one is active in range, follow the active one so
    // our broadcasts stay readable.
    if (!_localHashConfirmed) return true;
    return _learnedLastMillis > _localHashConfirmedMillis;
}

uint8_t MeshService::getChannelHash() const {
    return getChannelHashFor(getPrimaryChannelName(), _primaryPsk, _primaryPskLen);
}

uint8_t MeshService::getChannelHashFor(const char* name, const uint8_t* psk, uint8_t pskLen) const {
    uint8_t hName = 0;
    if (name) {
        const char* p = name;
        while (*p) hName ^= (uint8_t)(*p++);
    }
    uint8_t hKey = 0;
    if (psk && pskLen > 0) {
        for (uint8_t i = 0; i < pskLen; ++i) hKey ^= psk[i];
    }
    return hName ^ hKey;
}

uint32_t MeshService::getPkiKeyFingerprint() const {
    if (!_hasLocalKeys) return 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 32; ++i) {
        h ^= _localPublicKey[i];
        h *= 16777619u;
    }
    return h;
}

void MeshService::regenerateIdentity(bool changeNodeNumber) {
    if (sodium_init() < 0) {
        ESP_LOGE(TAG, "libsodium init failed, cannot regenerate identity");
        return;
    }
    uint8_t pub[32] = {0};
    uint8_t priv[32] = {0};
    crypto_box_keypair(pub, priv);
    memcpy(_localPublicKey, pub, 32);
    memcpy(_localPrivateKey, priv, 32);
    _hasLocalKeys = true;
    StorageManager::getInstance().savePkiKeyPair(pub, priv);

    if (changeNodeNumber) {
        _nodeIdSalt = esp_random() | 1u;
        uint32_t salted = _macNodeNum ^ _nodeIdSalt;
        if (salted == 0 || salted == NODENUM_BROADCAST) salted ^= 0x5A5A5A5A;
        _localNodeNum = salted;
        snprintf(_localIdStr, sizeof(_localIdStr), "!%08x", (unsigned)_localNodeNum);
    }
    saveConfig();
    ESP_LOGW(TAG, "Identity regenerated: node %s (newKeys=1, newNum=%u)", _localIdStr, changeNodeNumber ? 1 : 0);
    delay(150);
    ESP.restart();
}

void MeshService::setLocalName(const char* longName, const char* shortName) {
    if (longName && strlen(longName) > 0) {
        strncpy(_localLongName, longName, sizeof(_localLongName) - 1);
        _localLongName[sizeof(_localLongName) - 1] = '\0';
    }
    if (shortName && strlen(shortName) > 0) {
        strncpy(_localShortName, shortName, sizeof(_localShortName) - 1);
        _localShortName[sizeof(_localShortName) - 1] = '\0';
    }
    // Keep our own entry in the NodeDB in sync with the new identity
    if (!_nodes.empty()) {
        strncpy(_nodes[0].longName, _localLongName, sizeof(_nodes[0].longName) - 1);
        strncpy(_nodes[0].shortName, _localShortName, sizeof(_nodes[0].shortName) - 1);
    }
    broadcastNodeInfo(false);
}

void MeshService::setTxPower(int8_t powerDbm) {
    _txPower = powerDbm;
    if (!_radio) return;
    if (!applyRadioConfig()) {
        _radioConfigDirty = true;
        ESP_LOGW(TAG, "Radio busy: TX power %d dBm pending, will retry", (int)_txPower);
    } else {
        _radioConfigDirty = false;
    }
}

void MeshService::setFrequency(float freqMhz) {
    _frequency = freqMhz;
    if (!_radio) return;
    if (!applyRadioConfig()) {
        _radioConfigDirty = true;
        ESP_LOGW(TAG, "Radio busy: frequency %.3f MHz pending, will retry", _frequency);
    } else {
        _radioConfigDirty = false;
    }
}

void MeshService::setHopLimit(uint8_t hops) {
    if (hops < 1) hops = 1;
    if (hops > 7) hops = 7;
    _hopLimit = hops;
}

void MeshService::setModemPreset(ModemPreset preset) {
    _modemPreset = preset;
    if (!_radio) return;
    // transmitRaw() holds _radioMutex for the whole airtime of a frame (up to ~2 s at LongFast), so a
    // change during a TX can time out: keep the request pending and let update() retry, otherwise the
    // chip would stay on the old BW/SF/CR while the UI (and the channel hash) show the new preset.
    if (!applyRadioConfig()) {
        _radioConfigDirty = true;
        ESP_LOGW(TAG, "Radio busy: preset %s pending, will retry", getPresetCanonicalName(preset));
        return;
    }
    _radioConfigDirty = false;
    ESP_LOGI(TAG, "Modem preset set to %s (channel hash now 0x%02x)", getPresetCanonicalName(_modemPreset), getChannelHash());
    // Re-announce ourselves on the new configuration
    broadcastNodeInfo(false);
}

// Requires _radioMutex to be held by the caller.
void MeshService::applyModemPresetLocked(ModemPreset preset) {
    switch (preset) {
        case ModemPreset::LongFast:
            _radio->setBandwidth(250.0);
            _radio->setSpreadingFactor(11);
            _radio->setCodingRate(5);
            break;
        case ModemPreset::LongModerate:
            _radio->setBandwidth(125.0);
            _radio->setSpreadingFactor(11);
            _radio->setCodingRate(8);
            break;
        case ModemPreset::LongSlow:
            _radio->setBandwidth(125.0);
            _radio->setSpreadingFactor(12);
            _radio->setCodingRate(8);
            break;
        case ModemPreset::MediumSlow:
            _radio->setBandwidth(250.0);
            _radio->setSpreadingFactor(10);
            _radio->setCodingRate(5);
            break;
        case ModemPreset::MediumFast:
            _radio->setBandwidth(250.0);
            _radio->setSpreadingFactor(9);
            _radio->setCodingRate(5);
            break;
        case ModemPreset::ShortFast:
            _radio->setBandwidth(250.0);
            _radio->setSpreadingFactor(7);
            _radio->setCodingRate(5);
            break;
        case ModemPreset::ShortSlow:
            _radio->setBandwidth(125.0);
            _radio->setSpreadingFactor(8);
            _radio->setCodingRate(5);
            break;
    }
}

// Programs frequency, output power and modulation on the chip in one go. All three are retried
// together: a partial reconfiguration (e.g. new frequency but old preset) would make the node deaf.
bool MeshService::applyRadioConfig() {
    if (!_radio) return false;
    if (!lockRadio(300)) return false;
    _radio->setOutputPower(_txPower);
    _radio->setFrequency(_frequency);
    applyModemPresetLocked(_modemPreset);
    _radio->startReceive();
    unlockRadio();
    return true;
}

void MeshService::setNodeRoleAndPersist(NodeRole role) {
    _nodeRole = role;
    if (!_nodes.empty() && _nodes[0].nodeNum == _localNodeNum) {
        _nodes[0].role = static_cast<uint8_t>(role);
    }
    ESP_LOGW(TAG, "Node role set to %u (0=CLIENT, 1=CLIENT_MUTE, 2=ROUTER)", (unsigned)role);
    saveConfig();
    broadcastNodeInfo(false);
}

void MeshService::requestNodePersist() {
    // Node updates collected while the SD card was unmounted only live in the NVS mirror; make the
    // periodic save rewrite the CSV as soon as possible.
    _nodeDbDirty = true;
    _lastNodeSaveTime = millis() - 300001UL;
}

void MeshService::saveConfig(bool withNodes) {
    SettingsConfig cfg = {};
    // Never persist an empty identity: if the RAM name was lost, fall back to the MAC-derived default
    if (_localLongName[0] == '\0' || _localShortName[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        if (_localLongName[0] == '\0') snprintf(_localLongName, sizeof(_localLongName), "MonoMesh-%02X%02X", mac[4], mac[5]);
        if (_localShortName[0] == '\0') snprintf(_localShortName, sizeof(_localShortName), "%02X%02X", mac[4], mac[5]);
        ESP_LOGW(TAG, "Identity was empty, restored default name '%s'/'%s'", _localLongName, _localShortName);
    }
    strncpy(cfg.longName, _localLongName, sizeof(cfg.longName) - 1);
    strncpy(cfg.shortName, _localShortName, sizeof(cfg.shortName) - 1);
    cfg.frequency = _frequency;
    cfg.txPower = _txPower;
    cfg.modemPreset = static_cast<uint8_t>(_modemPreset);
    cfg.hopLimit = _hopLimit;
    cfg.frontlight = _frontlight;
    cfg.powerBtnAction = _powerBtnAction;
    cfg.antiGhostThreshold = _antiGhostThreshold;
    cfg.clockLandscape = _clockLandscape;
    cfg.clockFlip180 = _clockFlip180;
    cfg.nodeRole = static_cast<uint8_t>(_nodeRole);
    cfg.nodeInfoIntervalSec = _nodeInfoIntervalSec;
    cfg.telemetryIntervalSec = _telemetryIntervalSec;
    cfg.neighborInfoIntervalSec = _neighborInfoIntervalSec;
    cfg.dutyCyclePct = _dutyCyclePct;
    cfg.rebroadcastMode = static_cast<uint8_t>(_rebroadcastMode);
    cfg.buzzerMode = static_cast<uint8_t>(BSP::getInstance().getBuzzerMode());
    cfg.pkiEnabled = _pkiEnabled;
    cfg.gpsEnabled = _gpsEnabled;
    cfg.timezoneOffset = _timezoneOffset;
    cfg.nodeIdSalt = _nodeIdSalt;
    cfg.positionIntervalSec = _positionIntervalSec;
    cfg.meshTimeSync = _meshTimeSync;
    strncpy(cfg.primaryChannelName, _primaryChannelName, sizeof(cfg.primaryChannelName) - 1);
    cfg.primaryPskLen = _primaryPskLen;
    memcpy(cfg.primaryPsk, _primaryPsk, sizeof(cfg.primaryPsk));
    cfg.secondaryChannelCount = _secondaryChannels.size();
    for (size_t i = 0; i < _secondaryChannels.size() && i < 7; i++) {
        cfg.secondaryChannels[i] = _secondaryChannels[i];
    }
    cfg.ledNotifications = BSP::getInstance().getLedNotifications();
    cfg.buzzerVolume = BSP::getInstance().getBuzzerVolume();
    StorageManager::getInstance().saveSettings(cfg);

    // NodeDB persistence is heavy (the whole CSV is rewritten). The settings screen saves with
    // withNodes=false on every debounced change; the periodic save and the low-power/shutdown paths
    // call it with the default true, so the node list still reaches the card.
    if (!withNodes) return;

    std::vector<NodeRecord> records;
    for (const auto& n : _nodes) {
        NodeRecord r = {};
        r.nodeNum = n.nodeNum;
        strncpy(r.idStr, n.idStr, sizeof(r.idStr) - 1);
        strncpy(r.longName, n.longName, sizeof(r.longName) - 1);
        strncpy(r.shortName, n.shortName, sizeof(r.shortName) - 1);
        r.lat = n.lat;
        r.lon = n.lon;
        r.alt = n.alt;
        r.snr = n.snr;
        r.rssi = n.rssi;
        r.lastHeard = n.lastHeard;
        r.battery = n.battery;
        r.voltage = n.voltage;
        r.role = n.role;
        r.hasPublicKey = n.hasPublicKey;
        memcpy(r.publicKey, n.publicKey, 32);
        r.hasMac = n.hasMac;
        memcpy(r.mac, n.mac, 6);
        r.lastHeardEpoch = n.lastHeardEpoch;
        records.push_back(r);
    }
    StorageManager::getInstance().saveNodes(records);
}

const char* MeshService::getPresetName(ModemPreset preset) {
    switch (preset) {
        case ModemPreset::LongFast: return "LongFast";
        case ModemPreset::LongModerate: return "LongMod";
        case ModemPreset::LongSlow: return "LongSlow";
        case ModemPreset::MediumSlow: return "MedSlow";
        case ModemPreset::MediumFast: return "MedFast";
        case ModemPreset::ShortFast: return "ShortFast";
        case ModemPreset::ShortSlow: return "ShortSlow";
        default: return "Custom";
    }
}

const char* MeshService::getFrequencyRegionName(float freq) {
    if (abs(freq - 869.525f) < 0.05f) return "EU_868";
    if (abs(freq - 906.875f) < 0.05f) return "US_915";
    if (abs(freq - 433.175f) < 0.05f) return "EU_433";
    if (abs(freq - 920.800f) < 0.05f) return "JP_920";
    if (abs(freq - 915.000f) < 0.05f) return "ANZ_915";
    static char customBuf[32];
    snprintf(customBuf, sizeof(customBuf), "%.3f MHz", freq);
    return customBuf;
}

void MeshService::sendAck(uint32_t toNode, uint32_t originalPacketId) {
    uint8_t routingBuf[16];
    size_t rLen = 0;
    // Routing { error_reason = NONE }: field 3, wiretype 0
    routingBuf[rLen++] = (3 << 3) | 0;
    rLen += pbWriteVarint(routingBuf + rLen, 0);

    uint8_t raw[300];
    size_t rawLen = sizeof(raw);
    uint32_t pktId = 0;
    bool usedPki = false;
    // bypassDutyCycle: an ACK is a few bytes and is protocol-critical (without it the peer marks the
    // DM as failed after 3 retries), so it must not be swallowed by the hourly budget.
    if (!sendDataPacket(toNode, 5, routingBuf, rLen, false, false, false, raw, &rawLen, &pktId, &usedPki, originalPacketId, 0,
                        /*bypassDutyCycle=*/true)) {
        return;
    }

    // sendDataPacket() has already transmitted the frame. Queueing it again here sent every ACK twice
    // (double airtime on the busiest portnum of the mesh) and bypassed the relay queue bound.
    ESP_LOGI(TAG, "Meshtastic ACK sent to !%08x for packet 0x%08x", (unsigned)toNode, (unsigned)originalPacketId);
}

void MeshService::markMessageAcked(uint32_t id) {
    bool found = false;
    for (auto& m : _directMessages) {
        if (m.id == id && m.isOutgoing) {
            m.isAcked = true;
            m.isFailed = false;
            found = true;
        }
    }
    for (auto& m : _broadcastMessages) {
        if (m.id == id && m.isOutgoing) {
            m.isAcked = true;
            m.isFailed = false;
            found = true;
        }
    }
    for (auto it = _pendingAcks.begin(); it != _pendingAcks.end(); ) {
        if (it->packetId == id) {
            it = _pendingAcks.erase(it);
        } else {
            ++it;
        }
    }
    if (_pendingTracerouteId == id) {
        _pendingTracerouteId = 0;
    }
    if (found) {
        StorageManager::getInstance().updateChatMessageAck(id);
        UIEngine::getInstance().requestFullRefresh();
    }
}

void MeshService::markMessageFailed(uint32_t id, const char* reason) {
    bool found = false;
    for (auto& m : _directMessages) {
        if (m.id == id && m.isOutgoing) {
            m.isFailed = true;
            m.isAcked = false;
            found = true;
        }
    }
    for (auto& m : _broadcastMessages) {
        if (m.id == id && m.isOutgoing) {
            m.isFailed = true;
            m.isAcked = false;
            found = true;
        }
    }
    for (auto it = _pendingAcks.begin(); it != _pendingAcks.end(); ) {
        if (it->packetId == id) {
            it = _pendingAcks.erase(it);
        } else {
            ++it;
        }
    }
    if (_pendingTracerouteId == id) {
        _pendingTracerouteId = 0;
        _lastTraceroute.pending = false;
        snprintf(_lastTraceroute.text, sizeof(_lastTraceroute.text), "Traceroute failed (%s)", reason ? reason : "error");
    }
    ESP_LOGW(TAG, "Message 0x%08x marked as failed (%s)", (unsigned)id, reason ? reason : "unknown");
    if (found) {
        UIEngine::getInstance().requestFullRefresh();
    }
}

} // namespace MonoMesh
