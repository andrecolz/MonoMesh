#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <vector>
#include <string>
#include <deque>
#include <sodium.h>
#include "storage_manager.h"

namespace MonoMesh {

// Firmware version shown on the boot banner and in the settings Info page.
constexpr const char* MONOMESH_VERSION = "1.0b";

constexpr uint32_t NODENUM_BROADCAST = 0xFFFFFFFF;
// NodeDB in RAM. sizeof(MeshNode) is 248 B on the ESP32-S3, so the vector costs ~31 KB here: the
// internal heap has ~190 KB free, and the SD card cannot help (the runtime paths - routing by node
// number, public keys for PKI, positions for the map - all need the entries in RAM; the CSV/NVS are
// only a boot-time snapshot). At capacity the least recently heard node is replaced.
constexpr size_t MAX_NODES = 128;
constexpr size_t MAX_MESSAGES = 50;

enum class NodeRole : uint8_t {
    Client = 0,
    ClientMute = 1,
    Router = 2
};

enum class ModemPreset {
    LongFast = 0,
    LongModerate = 1,
    LongSlow = 2,
    MediumSlow = 3,
    MediumFast = 4,
    ShortFast = 5,
    ShortSlow = 6
};

// Managed-flooding participation. ALL relays everything relayable (decodable and opaque),
// KNOWN_ONLY only packets whose sender is already in the NodeDB, OFF never relays other nodes.
enum class RebroadcastMode : uint8_t {
    All = 0,
    KnownOnly = 1,
    Off = 2
};

struct MeshNode {
    uint32_t nodeNum = 0;
    char idStr[12] = "";      // "!xxxxxxxx"
    char longName[32] = "";
    char shortName[8] = "";
    float snr = 0.0f;
    float rssi = 0.0f;
    uint8_t battery = 0;      // 0..100%
    float voltage = 0.0f;     // Battery voltage in Volts (e.g. 4.12V)
    uint8_t role = 0;         // NodeRole (0=Client, 1=ClientMute, 2=Router)
    uint32_t lastHeard = 0;   // millis()
    bool hasPosition = false;
    double lat = 0.0;
    double lon = 0.0;
    int32_t alt = 0;
    double distanceKm = 0.0;  // Relative to local node
    double bearingDeg = 0.0;
    uint8_t publicKey[32] = {0};
    bool hasPublicKey = false;
    uint8_t hwModel = 0;
    bool isLicensed = false;
    bool isUnmessagable = false;
    uint8_t hopsAway = 0;
    uint8_t channelIndex = 0;
    bool keyVerified = false;   // received a PKI-encrypted packet we could authenticate
    uint8_t mac[6] = {0};
    bool hasMac = false;
    bool hasEnvMetrics = false;
    float temperature = 0.0f;   // °C (EnvironmentMetrics)
    float humidity = 0.0f;      // % (EnvironmentMetrics)
    float pressure = 0.0f;      // hPa (EnvironmentMetrics)
    float iaq = 0.0f;           // IAQ index (EnvironmentMetrics.iaq)
    float pm25 = 0.0f;          // PM2.5 ug/m3 (AirQualityMetrics)
    char statusText[41] = "";   // NODE_STATUS_APP string set by the peer
    uint32_t lastNodeInfoReply = 0; // millis() of last unicast NodeInfo reply to this peer
    uint32_t lastHeardEpoch = 0;    // unix time of the last packet (0 = unknown, or millis-only)
};

// Waypoint received over the mesh (WAYPOINT_APP). Drawn on the map view.
struct MeshWaypoint {
    uint32_t id = 0;
    uint32_t fromNode = 0;
    double lat = 0.0;
    double lon = 0.0;
    char name[31] = "";
    uint32_t expire = 0;
    uint32_t receivedAt = 0;
};

struct TracerouteHop {
    uint32_t nodeNum = 0;
    float snr = 0.0f;
    bool snrKnown = false;
};

struct TracerouteResult {
    bool valid = false;
    bool pending = false;
    uint32_t target = 0;
    uint32_t startMillis = 0;
    std::vector<TracerouteHop> forward;
    std::vector<TracerouteHop> back;
    char text[192] = "";
};

struct ChatMessage {
    uint32_t id = 0;
    uint32_t fromNode = 0;
    uint32_t toNode = NODENUM_BROADCAST;
    char senderShort[8] = "";
    // Holds the longest text the radio can carry (223 B); the old 160 B cut longer messages both in
    // the bubble and in the CSV/NVS history.
    char text[224] = "";
    uint32_t timestamp = 0;
    float snr = 0.0f;
    float rssi = 0.0f;
    bool isOutgoing = false;
    bool isRead = false;
    bool isAcked = false;
    bool isFailed = false;
    char timeStr[8] = "";
    char channelPreset[16] = "";
    uint32_t seq = 0;
};

struct PendingAck {
    uint32_t packetId = 0;
    uint32_t toNode = 0;
    char text[224] = "";
    uint8_t retriesLeft = 3;
    uint32_t timeoutMs = 8000;
    uint32_t lastSentMillis = 0;
    // Max on-air frame is 255 B (SX1262/RadioLib): 16B header + 239B encrypted payload.
    uint8_t rawPacket[256] = {0};
    size_t rawPacketLen = 0;
};

struct PendingRelay {
    uint8_t packet[256] = {0};
    size_t len = 0;
    uint32_t id = 0;
    uint32_t transmitAtMillis = 0;
};

// One frame read out of the radio FIFO by the core-0 radio task and handed to the protocol code
// running in the main loop. Reading the FIFO into this queue is what keeps packets from being lost
// while the e-paper is refreshing (the SX1262 has a single 256 byte buffer).
struct RadioRxFrame {
    uint8_t data[256] = {0};
    uint16_t len = 0;
    float snr = 0.0f;
    float rssi = 0.0f;
    uint32_t toaUs = 0;
};

class MeshService {
public:
    static MeshService& getInstance() {
        static MeshService instance;
        return instance;
    }

    bool init();
    void update();

    void sendAck(uint32_t toNode, uint32_t originalPacketId);
    void markMessageAcked(uint32_t id);
    void markMessageFailed(uint32_t id, const char* reason);

    // Node Information
    uint32_t getLocalNodeNum() const { return _localNodeNum; }
    const char* getLocalIdStr() const { return _localIdStr; }
    const char* getLocalLongName() const { return _localLongName; }
    const char* getLocalShortName() const { return _localShortName; }
    void setLocalName(const char* longName, const char* shortName);
    const uint8_t* getLocalPublicKey() const { return _localPublicKey; }
    void setNodeRole(NodeRole role) { _nodeRole = role; }
    NodeRole getNodeRole() const { return _nodeRole; }
    void setNodeRoleAndPersist(NodeRole role);

    // Radio Metrics
    float getChannelUtilization() const { return _channelUtilization; }
    float getAirtimeTx() const { return _airtimeTx; }
    bool isRxActive() const { return _rxActive; }
    bool isTxActive() const { return _txActive; }
    uint32_t getUnreadCount() const { return _unreadCount; }

    // Messaging
    bool sendBroadcastMessage(const char* text, const char* channelName = nullptr);
    bool sendDirectMessage(uint32_t toNode, const char* text);
    const std::deque<ChatMessage>& getBroadcastMessages() const { return _broadcastMessages; }
    std::vector<ChatMessage> getBroadcastMessagesForPreset(const char* preset) const;
    std::vector<std::string> getActiveBroadcastPresets() const;
    std::vector<ChatMessage> getDirectMessages(uint32_t nodeNum) const;
    // Cheap counters for the UI: the views only need to know whether the history grew, and copying
    // the whole deque every loop iteration wasted heap (50 messages x ~230 B every 15 ms).
    uint32_t getDirectMessageCount(uint32_t nodeNum) const;
    uint32_t getBroadcastMessageCountForPreset(const char* preset) const;
    uint32_t getConversationMessageCount() const;
    uint32_t getUnreadCountForChannel(const char* preset) const;
    uint32_t getUnreadCountForNode(uint32_t nodeNum) const;
    void markChannelMessagesRead(const char* preset);
    void markDirectMessagesRead(uint32_t nodeNum);
    void markMessagesRead();

    // Reset in-RAM history / node DB (the settings screen clears both RAM and storage)
    void clearChatHistory();
    void clearDiscoveredNodes();

    // Waypoints received over the air
    const std::vector<MeshWaypoint>& getWaypoints() const { return _waypoints; }

    // Age of the last packet from a node. Returns UINT32_MAX when unknown. The list is kept sorted
    // by recency (most recent first, local node always on top).
    uint32_t getNodeAgeSeconds(const MeshNode& node) const;
    static void formatAgeLabel(uint32_t seconds, char* out, size_t outLen);

    // Clock
    int8_t getTimezoneOffset() const { return _timezoneOffset; }
    void setTimezoneOffset(int8_t hours);
    bool getMeshTimeSync() const { return _meshTimeSync; }
    void setMeshTimeSync(bool en);

    // Radio ownership (core-0 task): the loop takes the mutex before light sleep so no SPI
    // transaction can be interrupted, and asks whether the radio task still has work pending.
    bool lockRadio(uint32_t timeoutMs);
    void unlockRadio();
    bool hasPendingRadioWork();   // a frame is still sitting in the radio FIFO
    bool hasQueuedFrames();       // frames already handed over and waiting to be processed
    bool waitRadioIdle(uint32_t timeoutMs);

    // CLOCK power mode: park the SX1262 in sleep so the radio stops drawing RX current, keeping the
    // rail on (the chip keeps its configuration, so resuming does not need a full begin()). While
    // suspended the core-0 task and every radio query must not touch the SPI bus.
    bool suspendRadio();
    void resumeRadio();
    bool isRadioSuspended() const { return _radioSuspended; }

    // PKI
    bool hasPkiKeys() const { return _hasLocalKeys; }
    bool isPkiEnabled() const { return _pkiEnabled; }
    void setPkiEnabled(bool en) { _pkiEnabled = en; }

    // Primary channel (Ch 0). Empty name = derived from modem preset (Meshtastic default behaviour)
    const char* getPrimaryChannelName() const;
    void setPrimaryChannelName(const char* name);
    const char* getPrimaryChannelNameRaw() const { return _primaryChannelName; }
    uint8_t getPrimaryChannelPskLen() const { return _primaryPskLen; }
    const uint8_t* getPrimaryChannelPsk() const { return _primaryPsk; }
    void setPrimaryChannelPsk(const uint8_t* psk, uint8_t len);
    bool isPrimaryChannelDefaultPsk() const;

    // Identity
    void regenerateIdentity(bool changeNodeNumber);

    // Traceroute
    bool sendTraceroute(uint32_t dest);
    const TracerouteResult& getLastTraceroute() const { return _lastTraceroute; }
    uint32_t getPkiKeyFingerprint() const;

    // Channel auto-adapt: if nobody in range ever uses our own channel hash, transmit with the
    // (hash, key) pair actually seen on air so peers can decrypt our broadcasts.
    uint8_t getTxChannelHash() const;
    const uint8_t* getTxChannelKey() const;
    uint8_t getTxChannelKeyLen() const;
    bool isTxChannelAdapted() const;
    bool isLocalChannelHashConfirmed() const { return _localHashConfirmed; }
    uint8_t getLearnedChannelHash() const { return _learnedHash; }
    uint32_t getLearnedHashHits() const { return _learnedCount; }
    uint32_t getLearnedHashAgeSec() const
    {
        return (_learnedLastMillis == 0) ? 0 : (uint32_t)((millis() - _learnedLastMillis) / 1000);
    }

    // Diagnostics (shown in the Storage & Info page, useful for on-field troubleshooting)
    uint32_t getRxTotal() const { return _rxTotal; }
    uint32_t getRxDecoded() const { return _rxDecoded; }
    uint32_t getRxUndecodable() const { return _rxUndecodable; }
    uint32_t getTxTotal() const { return _txTotal; }
    uint32_t getLastRxMillis() const { return _lastRxMillis; }
    uint32_t getLastRxFrom() const { return _lastRxFrom; }
    uint32_t getLastRxPortnum() const { return _lastRxPortnum; }
    uint8_t getLastRxHash() const { return _lastRxHash; }
    bool getLastRxWasPki() const { return _lastRxPki; }
    uint32_t getLastTxMillis() const { return _lastTxMillis; }
    uint32_t getLastTxErrorCode() const { return _lastTxErrorCode; }
    uint32_t getLastTxErrorAgeSec() const
    {
        return (_lastTxErrorMillis == 0) ? 0 : (uint32_t)((millis() - _lastTxErrorMillis) / 1000);
    }
    uint32_t getLocalHashConfirmedBy() const { return _localHashConfirmedBy; }
    uint32_t getLocalHashConfirmedAgeSec() const
    {
        return (_localHashConfirmedMillis == 0) ? 0 : (uint32_t)((millis() - _localHashConfirmedMillis) / 1000);
    }
    uint32_t getOpaqueRelayCount() const { return _opaqueRelays; }

    // NodeDB
    const std::vector<MeshNode>& getNodes() const { return _nodes; }
    const MeshNode* findNode(uint32_t nodeNum) const;
    MeshNode* findNode(uint32_t nodeNum);
    size_t getNodeCount() const { return _nodes.size(); }
    void updateLocalPosition(double lat, double lon, int32_t alt);

    // Configuration
    void setTxPower(int8_t powerDbm);
    int8_t getTxPower() const { return _txPower; }
    void setModemPreset(ModemPreset preset);
    ModemPreset getModemPreset() const { return _modemPreset; }
    float getFrequency() const { return _frequency; }
    void setFrequency(float freqMhz);
    uint8_t getHopLimit() const { return _hopLimit; }
    void setHopLimit(uint8_t hops);
    uint8_t getFrontlight() const { return _frontlight; }
    void setFrontlight(uint8_t fl) { _frontlight = (fl > 100) ? 100 : fl; }
    uint16_t getAntiGhostThreshold() const { return _antiGhostThreshold; }
    uint8_t getPowerBtnAction() const { return _powerBtnAction; }
    void setPowerBtnAction(uint8_t act) { _powerBtnAction = (act <= 3) ? act : 1; }
    bool getClockLandscape() const { return _clockLandscape; }
    void setClockLandscape(bool en) { _clockLandscape = en; }
    bool getClockFlip180() const { return _clockFlip180; }
    void setClockFlip180(bool en) { _clockFlip180 = en; }
    // Broadcast intervals (seconds). 0 = disabled where the periodic tick honours it.
    uint32_t getNodeInfoIntervalSec() const { return _nodeInfoIntervalSec; }
    void setNodeInfoIntervalSec(uint32_t sec) { _nodeInfoIntervalSec = (sec == 0) ? 10800 : sec; }
    uint32_t getTelemetryIntervalSec() const { return _telemetryIntervalSec; }
    void setTelemetryIntervalSec(uint32_t sec) { _telemetryIntervalSec = (sec == 0) ? 1800 : sec; }
    uint32_t getPositionIntervalSec() const { return _positionIntervalSec; }
    void setPositionIntervalSec(uint32_t sec) { _positionIntervalSec = sec; }
    uint32_t getNeighborInfoIntervalSec() const { return _neighborInfoIntervalSec; }
    void setNeighborInfoIntervalSec(uint32_t sec) { _neighborInfoIntervalSec = sec; }

    // ---- Radio duty cycle ----
    // The limit is the maximum share of a rolling hour spent transmitting. 0 = region default
    // (EU868/EU433 = 10%, other regions have no duty-cycle limit), 100 = override (no limit).
    uint8_t getDutyCyclePct() const { return _dutyCyclePct; }
    void setDutyCyclePct(uint8_t pct) { _dutyCyclePct = (pct <= 100) ? pct : 0; }
    uint8_t getRegionDutyCyclePct() const;
    uint8_t getEffectiveDutyCyclePct() const;
    // TX airtime accumulated in the rolling hour (percent of the hour) and the hard limiter state.
    float getHourlyAirtimePct() const;
    bool canTransmit() const;
    bool isTxLimited() const { return !canTransmit(); }

    // Rebroadcast (managed flooding) participation
    RebroadcastMode getRebroadcastMode() const { return _rebroadcastMode; }
    void setRebroadcastMode(RebroadcastMode mode) { _rebroadcastMode = mode; }

    void saveConfig(bool withNodes = true);
    // Force the next periodic NodeDB save to run soon (used after the SD card is remounted: node
    // updates collected while the card was unmounted only live in the NVS mirror otherwise).
    void requestNodePersist();

    static const char* getPresetName(ModemPreset preset);
    static const char* getFrequencyRegionName(float freq);

    // Callbacks / Periodic
    void broadcastNodeInfo(bool wantResponse = false);
    void broadcastTelemetry();
    void broadcastPosition();
    void broadcastNeighborInfo();
    void queueOpaqueRelay(const uint8_t* payload, size_t len, uint32_t id, uint8_t hopLimit, float snr);
    uint32_t currentEpoch() const;           // cached unix time (1s), 0 when the RTC is unusable
    void maybeSyncRtcFromMesh(uint32_t unixUtc); // honour the "sync time from mesh" setting
    void markNodeHeard(MeshNode& node);      // refresh millis + unix recency stamps
    void sortNodesByRecency();               // local node stays first, the rest by last heard
    static void radioTaskEntry(void* arg);   // core-0 RX pump (FIFO -> _rxQueue)
    void radioTaskLoop();
    void requestNodeInfo(uint32_t nodeNum);
    uint8_t getChannelHash() const;
    uint8_t getChannelHashFor(const char* name, const uint8_t* psk, uint8_t pskLen) const;
    // True when the given on-air hash belongs to one of our configured secondary channels, or to the
    // PKI pseudo-channel (0). Never adapt our primary TX to those.
    bool isForeignChannelHash(uint8_t hash) const;
    // True when our primary TX should use the (hash,key) pair learned from peers instead of our own.
    bool shouldUseLearnedChannel() const;
    static const char* getPresetCanonicalName(ModemPreset preset);

private:
    MeshService();
    ~MeshService() = default;

    uint32_t _macNodeNum = 0;      // MAC-derived node number (before salt)
    uint32_t _localNodeNum = 0;
    char _localIdStr[12] = "";
    char _localLongName[32] = "MonoMesh-Node";
    char _localShortName[8] = "MM";
    int8_t _txPower = 22; // dBm
    float _frequency = 869.525f; // EU868 Meshtastic default
    ModemPreset _modemPreset = ModemPreset::MediumFast;
    uint8_t _hopLimit = 3;
    uint8_t _frontlight = 30;
    uint16_t _antiGhostThreshold = 0; // 0 = disabled (partial updates only), see EPDDriver
    uint8_t _powerBtnAction = 1; // 0=PowerOff, 1=Standby, 2=Clock, 3=Standby+Clock (default Standby)
    bool _clockLandscape = true; // clock face orientation (default landscape)
    bool _clockFlip180 = false;  // clock face upside down (180°, for an inverted wall mount)
    uint8_t _dutyCyclePct = 0;   // 0 = region default, 100 = override (no limit)
    RebroadcastMode _rebroadcastMode = RebroadcastMode::All;
    // True while the SX1262 is parked in sleep by the CLOCK power mode: no radio register may be
    // touched until resumeRadio() (the chip answers 0x00/garbage with its rail still on).
    bool _radioSuspended = false;
    // Set when a radio reconfiguration could not take _radioMutex (a TX holds it for the whole
    // airtime): update() retries, so the chip cannot stay on a different config than _frequency/etc.
    bool _radioConfigDirty = false;

    // Backend features
    NodeRole _nodeRole = NodeRole::Client;
    uint32_t _nodeInfoIntervalSec = 10800; // 3 hours
    uint32_t _telemetryIntervalSec = 1800;  // 30 minutes
    uint32_t _positionIntervalSec = 900;    // 15 minutes
    uint32_t _lastTelemetryTime = 0;
    uint32_t _lastNodeSaveTime = 0;
    bool _nodeDbDirty = false;
    uint32_t _lastPositionTime = 0;
    uint32_t _lastPositionReplyMillis = 0; // throttle for unicast position replies (official: 3 min)
    uint32_t _bootMillis = 0;
    bool _bootNodeInfoSent = false;
    uint32_t _lastNodeInfoTx = 0;
    uint32_t _lastNodeInfoRequestReply = 0;
    uint32_t _nodeIdSalt = 0;
    // Primary channel (idx 0) configuration
    char _primaryChannelName[16] = "";
    uint8_t _primaryPsk[32] = {0};
    uint8_t _primaryPskLen = 16;
    int8_t _timezoneOffset = 1;
    bool _pkiEnabled = true;
    bool _gpsEnabled = true;
    uint8_t _localPublicKey[32] = {0};
    uint8_t _localPrivateKey[32] = {0};
    bool _hasLocalKeys = false;
    std::vector<ChannelConfig> _secondaryChannels;

    // Channel auto-adapt + diagnostics counters
    bool _localHashConfirmed = false;
    bool _learnedHashValid = false;
    uint8_t _learnedHash = 0;
    uint8_t _learnedKey[32] = {0};
    uint8_t _learnedKeyLen = 0;
    uint32_t _learnedCount = 0;
    uint32_t _learnedLastMillis = 0;         // when the learned pair was last heard on air
    uint32_t _localHashConfirmedMillis = 0;  // when a peer last used our own channel hash
    uint32_t _localHashConfirmedBy = 0;      // peer that last confirmed our own channel hash
    uint32_t _lastTxErrorCode = 0;
    uint32_t _lastTxErrorMillis = 0;
    uint32_t _lastIrqPollMs = 0;   // rate-limits the RX IRQ register poll
    uint32_t _lastUndecodableLog = 0;
    uint32_t _lastRxSummaryLog = 0;

    // NodeInfo request bookkeeping. The official firmware suppresses NodeInfo replies for 12h after
    // any want_response NodeInfo from the same sender, and every new request refreshes that window,
    // so asking repeatedly makes us invisible to the peer's reply path indefinitely.
    struct NodeInfoRequest {
        uint32_t nodeNum = 0;
        uint32_t lastMillis = 0;
    };
    static constexpr uint32_t NODEINFO_REQUEST_COOLDOWN_MS = 12UL * 3600UL * 1000UL;
    NodeInfoRequest _nodeInfoRequests[16];
    TaskHandle_t _radioTask = nullptr;
    QueueHandle_t _rxQueue = nullptr;
    SemaphoreHandle_t _radioMutex = nullptr;
    uint32_t _opaqueRelays = 0;
    uint32_t _neighborInfoIntervalSec = 3600;
    uint32_t _lastNeighborInfoTime = 0;
    bool _bootTelemetrySent = false;
    bool _meshTimeSync = true;
    bool _nodeOrderDirty = false;
    bool _epochBackfilled = false;
    mutable uint32_t _epochCacheMillis = 0;
    mutable uint32_t _epochCacheValue = 0;
    std::vector<MeshWaypoint> _waypoints;
    uint32_t _rxTotal = 0;
    uint32_t _rxDecoded = 0;
    uint32_t _rxUndecodable = 0;
    uint32_t _txTotal = 0;
    uint32_t _lastRxMillis = 0;
    uint32_t _lastRxFrom = 0;
    uint32_t _lastRxPortnum = 0;
    uint8_t _lastRxHash = 0;
    bool _lastRxPki = false;
    uint32_t _lastTxMillis = 0;

    // ---- Rolling-hour TX airtime budget (duty cycle) ----
    // 60 one-minute buckets of accumulated time-on-air (µs). Cheap, fixed size, no heap. The sum is
    // kept incrementally so canTransmit() is O(1) on the TX path.
    static constexpr size_t AIRTIME_BUCKETS = 60;
    static constexpr uint32_t AIRTIME_BUCKET_MS = 60000;
    uint32_t _txAirtimeUs[AIRTIME_BUCKETS] = {0};
    uint64_t _hourlyAirtimeUs = 0;
    uint32_t _airtimeBucketIndex = 0;
    uint32_t _airtimeBucketStartMs = 0;
    void rotateAirtimeBuckets(uint32_t now);

    float _channelUtilization = 0.0f;
    float _airtimeTx = 0.0f;
    bool _rxActive = false;
    bool _txActive = false;
    uint32_t _rxActivityTimer = 0;
    uint32_t _txActivityTimer = 0;
    uint32_t _unreadCount = 0;
    uint32_t _lastBroadcastTime = 0;
    uint32_t _globalMsgSeq = 0;
    uint32_t _lastDiscoveryRequestTime = 0;
    uint32_t _lastNodeInfoRequestFrom = 0;
    TracerouteResult _lastTraceroute;
    uint32_t _pendingTracerouteId = 0;

    // Real airtime calculation
    uint32_t _accumulatedTxUs = 0;
    uint32_t _accumulatedRxUs = 0;
    uint32_t _airtimeWindowStart = 0;
    void recordAirtimeTx(size_t packetLen);
    void updateAirtimeMetrics();

    // Queues
    std::vector<PendingAck> _pendingAcks;
    std::vector<PendingRelay> _pendingRelays;

    // Deferred outbound actions (unicast NodeInfo replies, telemetry/position responses)
    enum class DeferredKind : uint8_t { NodeInfoUnicast = 0, TelemetryUnicast = 1, PositionUnicast = 2, TracerouteReply = 3, TracerouteRequest = 4, DirectMessagePkiRetry = 5 };
    struct DeferredSend {
        DeferredKind kind = DeferredKind::NodeInfoUnicast;
        uint32_t dest = 0;
        uint32_t atMillis = 0;
        uint32_t arg = 0;          // traceroute reply id / DM packet id
        float snr = 0.0f;
        uint8_t attemptsLeft = 0;
        char text[224] = "";       // full DM text: a 160 B field truncated the PKI retry copy
    };
    std::vector<DeferredSend> _deferredSends;
    void queueDeferred(DeferredKind kind, uint32_t dest, uint32_t delayMs, uint32_t arg = 0, float snr = 0.0f);
    void queueDeferredDm(uint32_t dest, const char* text, uint32_t packetId, uint32_t delayMs);
    void processDeferredSends();
    void processAckQueue();
    void processRelayQueue();
    // Drain the radio FIFO into _rxQueue. Called by the core-0 radio task, and directly from the
    // main loop when the task could not be created (real fallback, not just a log line).
    void moveRadioFramesToQueue(size_t maxFrames);

    // PKI (X25519 + SHA-256 + AES-256-CCM)
    bool decryptPki(uint32_t fromNode, uint32_t packetId, const uint8_t* in, size_t inLen, uint8_t* out, size_t& outLen);
    bool encryptPki(uint32_t toNode, uint32_t packetId, const uint8_t* plain, size_t plainLen, uint8_t* out, size_t& outLen);

    void sendNodeInfoTo(uint32_t dest, bool wantResponse, uint32_t requestId = 0);
    size_t buildNodeInfoPayload(uint8_t* out, size_t cap) const;
    bool sendDataPacket(uint32_t dest, uint32_t portnum, const uint8_t* payload, size_t payloadLen,
                        bool wantResponse, bool wantAck, bool allowPki, uint8_t* rawOut, size_t* rawLenOut,
                        uint32_t* packetIdOut, bool* usedPkiOut, uint32_t requestId = 0, uint32_t forcedId = 0,
                        bool bypassDutyCycle = false);
    void sendTelemetryTo(uint32_t dest, bool wantResponse = false, uint32_t requestId = 0);
    void handleNodeInfoRequest(uint32_t from, bool wantResponse, uint32_t requestId = 0);
    void handleTracerouteReceived(uint32_t from, uint32_t to, uint32_t id, const uint8_t* payload, size_t len,
                                  bool isResponse, const uint8_t* chanKey, uint8_t chanKeyLen, float snr);
    void sendTracerouteReply(uint32_t dest, uint32_t requestId, const uint8_t* payload, size_t len);
    void applyModemPresetLocked(ModemPreset preset);
    bool applyRadioConfig();    // (re)programs frequency/power/preset under the mutex; false on timeout
    // wantResponse must stay false for replies: a unicast position reply that asks for a reply in turn
    // makes two MonoMesh nodes answer each other forever (official firmware replies with it cleared).
    void sendPositionTo(uint32_t dest, uint8_t precisionBits, bool wantResponse, uint32_t requestId = 0);
    bool buildTraceroutePayload(uint8_t* out, size_t outCap, size_t& outLen, const uint8_t* inPayload, size_t inLen,
                                bool isResponse, bool appendId, uint32_t nodeId, float rxSnr);
    bool queueTracerouteRelay(uint8_t* rawPacket, size_t rawLen, uint32_t packetId, uint8_t hopLimit,
                              const uint8_t* key, uint8_t keyLen, const uint8_t* newPlain, size_t newPlainLen);
    void cancelRelayFor(uint32_t id);
    // Rebroadcast policy gate (rebroadcastMode): used by both the decoded and the opaque relay paths.
    bool relayAllowedFor(uint32_t from) const;
    bool isPacketForUs(uint32_t to) const;
    void updateNodeHops(uint32_t nodeNum, uint8_t hops);
    size_t buildPkiOverheadProbe(uint8_t* out) const;

    std::vector<MeshNode> _nodes;
    std::deque<ChatMessage> _broadcastMessages;
    std::deque<ChatMessage> _directMessages;

    // Packet Deduplication (Meshtastic mesh hop deduplication)
    struct SeenPacket {
        uint32_t from = 0;
        uint32_t id = 0;
        uint32_t timestamp = 0;
    };
    static constexpr size_t SEEN_PACKETS_MAX = 64;
    SeenPacket _seenPackets[SEEN_PACKETS_MAX] = {};
    size_t _seenPacketIndex = 0;
    bool isPacketDuplicate(uint32_t from, uint32_t id);
    void recordSeenPacket(uint32_t from, uint32_t id);

    // RadioLib instances
    SPIClass _spiBus;
    Module* _radioModule = nullptr;
    SX1262* _radio = nullptr;
    bool _radioReady = false;

    void processPacket(uint8_t* payload, size_t len, float snr, float rssi);
    void handleDecryptedData(uint32_t from, uint32_t to, uint32_t id, const uint8_t* data, size_t len,
                             float snr, float rssi, bool wantAck, bool pkiDecoded,
                             const uint8_t* chanKey, uint8_t chanKeyLen, uint8_t hopsTaken);
    bool transmitRaw(const uint8_t* buf, size_t len, bool bypassDutyCycle = false);
    void updateNode(uint32_t nodeNum, const char* longName, const char* shortName, float snr, float rssi,
                    const uint8_t* pubKey = nullptr, uint8_t battery = 0, float voltage = 0.0f, int8_t role = -1,
                    uint8_t hwModel = 0, int8_t isLicensed = -1, int8_t isUnmessagable = -1);
    void updateNodePosition(uint32_t nodeNum, double lat, double lon, int32_t alt);
};

} // namespace MonoMesh
