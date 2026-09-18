#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include <string>

namespace MonoMesh {

struct ChannelConfig {
    char name[16];
    uint8_t psk[32];
    uint8_t pskLen;      // 0 = cleartext, 16 = AES-128, 32 = AES-256
    uint8_t channelHash;
    bool enabled;
};

struct NodeRecord {
    uint32_t nodeNum;
    char idStr[12];
    char longName[32];
    char shortName[8];
    double lat;
    double lon;
    int32_t alt;
    float snr;
    float rssi;
    uint32_t lastHeard;
    uint8_t publicKey[32];
    bool hasPublicKey;
    uint8_t battery;
    float voltage;
    uint8_t role;
    uint8_t mac[6];
    bool hasMac;
    uint32_t lastHeardEpoch;   // unix time (0 = unknown, e.g. entries saved by older firmware)
};

struct SettingsConfig {
    char longName[32];
    char shortName[8];
    float frequency;
    int8_t txPower;
    uint8_t modemPreset; // 0=LongFast, 1=LongModerate, 2=LongSlow, 3=MediumSlow, 4=MediumFast, 5=ShortFast, 6=ShortSlow
    uint8_t hopLimit;
    uint8_t frontlight;
    // 0=PowerOff, 1=Standby, 2=Clock (radio off), 3=Standby+Clock (radio listening)
    uint8_t powerBtnAction;
    uint16_t antiGhostThreshold;
    bool clockLandscape = true;   // clock face orientation: landscape by default
    bool clockFlip180 = false;    // clock face turned 180° (upside-down wall mount)
    uint8_t nodeRole;             // 0=CLIENT, 1=CLIENT_MUTE
    uint32_t nodeInfoIntervalSec; // default 10800 (3h)
    uint32_t telemetryIntervalSec;// default 1800 (30m)
    // Defaults are member initializers so a config.json written by an older firmware (which did not
    // carry these keys) cannot silently disable the periodic broadcasts in the recovery path.
    uint32_t neighborInfoIntervalSec = 3600; // 0 = disabled
    bool pkiEnabled;              // default true
    bool gpsEnabled;              // default true
    int8_t timezoneOffset;        // default +1
    // Radio duty-cycle limit (percentage of the rolling hour spent transmitting).
    // 0 = region default (EU868/EU433 = 10%), 1..100 = explicit limit, 100 = override (no limit).
    uint8_t dutyCyclePct;
    // Managed-flooding participation: 0 = ALL, 1 = KNOWN ONLY, 2 = OFF
    uint8_t rebroadcastMode;
    // Buzzer policy: 0 = ALL, 1 = NOTIFICATIONS, 2 = DIRECT MSG, 3 = SYSTEM ONLY, 4 = DISABLED
    uint8_t buzzerMode;
    uint8_t secondaryChannelCount;// 0..7
    ChannelConfig secondaryChannels[7];
    // Primary channel (Ch 0): empty name = derive from modem preset (Meshtastic semantics)
    char primaryChannelName[16];
    uint8_t primaryPsk[32];
    uint8_t primaryPskLen;        // 0 = cleartext, 16 = AES-128, 32 = AES-256
    uint32_t nodeIdSalt;          // XORed with the MAC-derived node number when != 0
    uint32_t positionIntervalSec = 900; // periodic POSITION_APP broadcast (0 = disabled)
    bool ledNotifications = true; // default true
    uint8_t buzzerVolume = 2;     // 0=Mute, 1=Low, 2=Med, 3=High (default 2)
    // Channel auto-adapt: (hash,key) pair learned from peers when our own channel hash is unused
    bool learnedChanValid = false;
    uint8_t learnedChanHash = 0;
    uint8_t learnedChanKey[32] = {};
    uint8_t learnedChanKeyLen = 0;
    uint16_t learnedChanHits = 0;
    bool meshTimeSync = true;      // accept RTC updates carried by mesh packets
};

struct SavedMessage {
    uint32_t id;
    uint32_t fromNode;
    uint32_t toNode;
    char senderShort[8];
    // Must hold the longest text the radio can carry (223 bytes): a 160 B field silently truncated
    // every longer message when it was written to the CSV/NVS history.
    char text[224];
    uint32_t timestamp;
    bool isOutgoing;
    bool isAcked;
    char timeStr[8];
    char channelPreset[16];
};

class StorageManager {
public:
    static StorageManager& getInstance() {
        static StorageManager instance;
        return instance;
    }

    bool init();

    static uint8_t getQuickFrontlight();

    bool isSdMounted() const { return _sdMounted; }
    // Unmount / remount the card around standby: cutting the SD rail with a mounted FAT is unsafe,
    // and when the slot is empty both calls are no-ops that just report the state.
    bool unmountSd();
    bool remountSd();
    // The map view keeps its .mmap container open for the whole session; every other
    // SD user opens and closes per operation. Closing it before SD_MMC.end() avoids
    // dangling FATFS handles after a standby/power-off, so the holder registers here.
    void setSdReleaseHook(void (*fn)(void*), void* arg) { _sdReleaseHook = fn; _sdReleaseArg = arg; }
    void clearSdReleaseHook(void* arg) { if (_sdReleaseArg == arg) { _sdReleaseHook = nullptr; _sdReleaseArg = nullptr; } }
    uint64_t getSdTotalBytes() const { return _sdTotalBytes; }
    uint64_t getSdFreeBytes() const { return _sdFreeBytes; }

    // Settings
    bool loadSettings(SettingsConfig& cfg);
    bool saveSettings(const SettingsConfig& cfg);

    // PKI Keypair (X25519)
    bool getPkiKeyPair(uint8_t pubKey[32], uint8_t privKey[32]);
    bool savePkiKeyPair(const uint8_t pubKey[32], const uint8_t privKey[32]);

    // Nodes
    bool saveNodes(const std::vector<NodeRecord>& nodes);
    bool loadNodes(std::vector<NodeRecord>& nodes);
    bool clearNodes();

    // Channel auto-adapt pair (written once when the pair becomes usable for TX)
    bool saveLearnedChannel(bool valid, uint8_t hash, const uint8_t* key, uint8_t keyLen, uint16_t hits);

    // Last map view (centre + zoom) so the map reopens where the user left it, and so a user whose
    // tiles are not near the built-in default never starts on an empty area.
    bool saveMapView(int32_t latE7, int32_t lonE7, uint8_t zoom);
    bool loadMapView(int32_t& latE7, int32_t& lonE7, uint8_t& zoom);

    // Chat History
    bool appendChatMessage(const SavedMessage& msg);
    bool updateChatMessageAck(uint32_t id);
    bool loadChatMessages(std::vector<SavedMessage>& messages);
    bool clearChatHistory();

private:
    void (*_sdReleaseHook)(void*) = nullptr;
    void* _sdReleaseArg = nullptr;
    StorageManager() = default;
    ~StorageManager() = default;

    // Node list backing stores: CSV on the card when present, NVS blobs (chunked, bounded) when the
    // slot is empty so names/keys survive a reboot anyway.
    bool saveNodesCsv(const std::vector<NodeRecord>& nodes);
    bool loadNodesCsv(std::vector<NodeRecord>& nodes);
    bool saveNodesNvs(const std::vector<NodeRecord>& nodes);
    bool loadNodesNvs(std::vector<NodeRecord>& nodes);
    // Message ring helpers: the ring is written while the SD card is unmounted (standby), so its
    // entries must be flushed into the CSV as soon as the card is mounted again.
    void readNvsMessages(std::vector<SavedMessage>& messages);
    bool writeMessageToCsv(const SavedMessage& msg);
    bool flushNvsMessagesToSd();
    // Recovery path: the settings mirror on the SD card is read back if NVS has no settings at all
    // (wiped partition, failed writes...), so the node name and the radio config are not lost.
    bool loadSettingsFromConfigJson(SettingsConfig& cfg);

    // Compact node record for the NVS mirror: only what cannot be relearned cheaply (name, key, MAC,
    // last contact). Keeps the mirror to ~1.3 KB instead of ~5.6 KB so the 24 KB NVS partition that
    // also holds settings, keys and the message ring never runs out of space.
    struct __attribute__((packed)) NvsNodeRecord {
        uint32_t nodeNum;
        char longName[24];
        char shortName[5];
        uint32_t lastHeardEpoch;
        uint8_t mac[6];
        uint8_t flags;          // bit0 = hasPublicKey, bit1 = hasMac
        uint8_t publicKey[32];
    };

    Preferences _pref;
    bool _sdMounted = false;
    uint64_t _sdTotalBytes = 0;
    uint64_t _sdFreeBytes = 0;

    bool initSdCard();
};

} // namespace MonoMesh
