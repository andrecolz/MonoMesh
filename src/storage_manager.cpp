#include "storage_manager.h"
#include "bsp_papermono.h"
#include <SD_MMC.h>
#include <FS.h>
#include <esp_log.h>
#include <sodium.h>

static constexpr const char* TAG = "MonoMesh-Storage";

namespace MonoMesh {

bool StorageManager::init() {
    ESP_LOGI(TAG, "Initializing Storage Subsystem (NVS + MicroSD)...");

    // libsodium must be initialised before any other call: the PKI path uses
    // crypto_scalarmult_curve25519 / crypto_box_keypair, and getPkiKeyPair() returns early when the
    // keypair already exists in NVS (which was skipping the old lazy init). Idempotent and cheap.
    if (sodium_init() < 0) {
        ESP_LOGE(TAG, "libsodium init failed");
        return false;
    }

    _pref.begin("monomesh", false);
    ESP_LOGI(TAG, "NVS namespace 'monomesh' opened (free entries: %u)", (unsigned)_pref.freeEntries());

    // SavedMessage grew (text 160 -> 224 B) so blobs written by older builds cannot be read back:
    // drop the old ring once instead of leaving a size mismatch and "wrong length" errors behind.
    if (_pref.getUChar("msgVer", 1) < 2) {
        const uint32_t legacyCnt = _pref.getUInt("m_cnt", 0);
        for (uint32_t i = 0; i < 24; ++i) {
            char k[16];
            snprintf(k, sizeof(k), "msg_%u", (unsigned)i);
            _pref.remove(k);
        }
        _pref.putUInt("m_head", 0);
        _pref.putUInt("m_cnt", 0);
        _pref.putUChar("msgVer", 2);
        if (legacyCnt > 0) {
            ESP_LOGW(TAG, "Dropped %u message(s) from the old NVS ring (record layout changed)",
                     (unsigned)(legacyCnt > 24 ? 24 : legacyCnt));
        }
    }

    initSdCard();
    // Messages received while the card was unmounted (standby) only exist in the NVS ring: write
    // them into the CSV now, before loadChatMessages() prefers the CSV and hides them.
    flushNvsMessagesToSd();

    return true;
}

bool StorageManager::initSdCard() {
    auto& ioe1 = BSP::getInstance().getIOE1();

    ioe1.pinMode(IOEPins::TF_EN, OUTPUT);
    ioe1.setDriveMode(IOEPins::TF_EN, M5IOE1_DRIVE_PUSHPULL);

    ioe1.pinMode(IOEPins::TF_DET, INPUT);
    ioe1.setPullMode(IOEPins::TF_DET, M5IOE1_PULL_UP);

    int detVal = ioe1.digitalRead(IOEPins::TF_DET);
    if (detVal != 0) {
        ESP_LOGI(TAG, "MicroSD slot empty (TF_DET HIGH)");
        _sdMounted = false;
        return false;
    }

    ESP_LOGI(TAG, "MicroSD card detected in slot. Powering rail on...");
    ioe1.digitalWrite(IOEPins::TF_EN, HIGH);
    delay(300); // 300ms rail stabilization

    SD_MMC.setPins(13, 12, 11, 10, 9, 8); // CLK=13, CMD=12, D0=11, D1=10, D2=9, D3=8
    if (!SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5)) {
        ESP_LOGW(TAG, "SD_MMC 4-bit mount failed, trying 1-bit mode...");
        if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
            ESP_LOGE(TAG, "SD_MMC mount failed entirely!");
            _sdMounted = false;
            return false;
        }
    }

    _sdMounted = true;
    _sdTotalBytes = SD_MMC.totalBytes();
    _sdFreeBytes = _sdTotalBytes - SD_MMC.usedBytes();

    ESP_LOGI(TAG, "MicroSD mounted successfully! Total: %llu MB, Free: %llu MB",
             _sdTotalBytes / (1024 * 1024), _sdFreeBytes / (1024 * 1024));

    if (!SD_MMC.exists("/monomesh")) {
        SD_MMC.mkdir("/monomesh");
    }
    // NOTE: the firmware used to create an empty "/tiles" folder here on every mount, but it never
    // writes PNG tiles (they are produced on the PC by tools/mmap_converter.py): the folder was pure
    // clutter and made the legacy tree look "used". Remove a stray empty one, but never touch a real
    // legacy tree (the rmdir only succeeds on an empty directory; a card with /tiles/<z>/... stays).
    if (SD_MMC.exists("/tiles")) {
        File dir = SD_MMC.open("/tiles");
        if (dir && dir.isDirectory()) {
            File entry = dir.openNextFile();
            const bool empty = !entry;
            if (entry) entry.close();
            dir.close();
            if (empty && SD_MMC.rmdir("/tiles")) {
                ESP_LOGI(TAG, "Removed the stray empty /tiles folder (the firmware no longer creates it)");
            }
        } else if (dir) {
            dir.close();
        }
    }

    return true;
}

bool StorageManager::unmountSd() {
    if (!_sdMounted) return true;
    // Whoever holds an open handle (the map container) must let go first: after
    // SD_MMC.end() the FATFS object is gone and the handle would dangle.
    if (_sdReleaseHook) _sdReleaseHook(_sdReleaseArg);
    SD_MMC.end();
    _sdMounted = false;
    _sdTotalBytes = 0;
    _sdFreeBytes = 0;
    ESP_LOGI(TAG, "MicroSD unmounted (standby)");
    return true;
}

bool StorageManager::remountSd() {
    if (_sdMounted) return true;
    ESP_LOGI(TAG, "Remounting MicroSD after standby...");
    if (!initSdCard()) return false;
    flushNvsMessagesToSd();
    return true;
}

uint8_t StorageManager::getQuickFrontlight() {
    Preferences p;
    if (p.begin("monomesh", true)) {
        uint8_t fl = p.getUChar("frontlight", 30);
        p.end();
        return fl;
    }
    return 30;
}

bool StorageManager::getPkiKeyPair(uint8_t pubKey[32], uint8_t privKey[32]) {
    if (_pref.isKey("pki_priv") && _pref.isKey("pki_pub")) {
        size_t lenPriv = _pref.getBytes("pki_priv", privKey, 32);
        size_t lenPub = _pref.getBytes("pki_pub", pubKey, 32);
        if (lenPriv == 32 && lenPub == 32) {
            ESP_LOGI(TAG, "Loaded existing PKI keypair from NVS");
            return true;
        }
    }

    if (sodium_init() < 0) {
        ESP_LOGE(TAG, "libsodium init failed");
        return false;
    }

    // A new keypair means our public key changes: peers that already store a key for this node will
    // reject our NodeInfo ("Public Key mismatch, drop NodeInfo") and their direct messages will stop
    // decrypting here. Make it loud, it is the usual root cause of "others cannot DM me anymore".
    ESP_LOGW(TAG, "No usable PKI keypair in NVS -> generating a NEW keypair (peers must relearn our key)");
    crypto_box_keypair(pubKey, privKey);
    size_t wPriv = _pref.putBytes("pki_priv", privKey, 32);
    size_t wPub = _pref.putBytes("pki_pub", pubKey, 32);
    if (wPriv != 32 || wPub != 32) {
        ESP_LOGE(TAG, "Could not persist the keypair (priv %u/32, pub %u/32 bytes): NVS full?",
                 (unsigned)wPriv, (unsigned)wPub);
        ESP_LOGE(TAG, "The key will change again at the next boot until NVS has room!");
    } else {
        ESP_LOGI(TAG, "Generated and saved new X25519 PKI keypair into NVS");
    }
    return true;
}

bool StorageManager::savePkiKeyPair(const uint8_t pubKey[32], const uint8_t privKey[32]) {
    size_t wPriv = _pref.putBytes("pki_priv", privKey, 32);
    size_t wPub = _pref.putBytes("pki_pub", pubKey, 32);
    if (wPriv != 32 || wPub != 32) {
        ESP_LOGE(TAG, "savePkiKeyPair failed (priv %u/32, pub %u/32): NVS full?",
                 (unsigned)wPriv, (unsigned)wPub);
        return false;
    }
    return true;
}

bool StorageManager::loadSettings(SettingsConfig& cfg) {
    // The key must exist AND be true: saveSettings() clears it before rewriting the block, so a
    // partial write (NVS full) is not loaded as valid on the next boot.
    if (_pref.isKey("cfg_init") && _pref.getBool("cfg_init", false)) {
        String lName = _pref.getString("longName", cfg.longName);
        String sName = _pref.getString("shortName", cfg.shortName);
        strncpy(cfg.longName, lName.c_str(), sizeof(cfg.longName) - 1);
        strncpy(cfg.shortName, sName.c_str(), sizeof(cfg.shortName) - 1);

        cfg.frequency = _pref.getFloat("freq", cfg.frequency);
        cfg.txPower = _pref.getChar("txPower", cfg.txPower);
        cfg.modemPreset = _pref.getUChar("preset", cfg.modemPreset);
        cfg.hopLimit = _pref.getUChar("hopLimit", cfg.hopLimit);
        cfg.frontlight = _pref.getUChar("frontlight", cfg.frontlight);
        cfg.powerBtnAction = _pref.getUChar("pwrBtnAct", cfg.powerBtnAction);
        cfg.antiGhostThreshold = _pref.getUShort("antiGhost", cfg.antiGhostThreshold);
        cfg.clockLandscape = _pref.getBool("clockLand", true);
        cfg.clockFlip180 = _pref.getBool("clockFlip", false);

        cfg.nodeRole = _pref.getUChar("nodeRole", 0);
        cfg.nodeInfoIntervalSec = _pref.getUInt("niIntSec", 10800);
        cfg.telemetryIntervalSec = _pref.getUInt("telIntSec", 1800);
        cfg.neighborInfoIntervalSec = _pref.getUInt("nbIntSec", 3600);
        cfg.dutyCyclePct = _pref.getUChar("dutyPct", 0);
        cfg.rebroadcastMode = _pref.getUChar("rbcMode", 0);
        cfg.buzzerMode = _pref.getUChar("buzMode", 0);
        cfg.pkiEnabled = _pref.getBool("pkiEn", true);
        cfg.gpsEnabled = _pref.getBool("gpsEn", true);
        cfg.timezoneOffset = _pref.getChar("tzOff", 1);
        // Secondary channels are only restored when they were written with the current layout
        cfg.secondaryChannelCount = 0;
        if (_pref.getUChar("chVer", 0) == 2) {
            uint8_t cnt = _pref.getUChar("secChCnt", 0);
            for (uint8_t i = 0; i < cnt && i < 7; ++i) {
                char k[16];
                snprintf(k, sizeof(k), "ch_%u", (unsigned)i);
                ChannelConfig tmp = {};
                if (_pref.getBytes(k, &tmp, sizeof(ChannelConfig)) == sizeof(ChannelConfig)) {
                    cfg.secondaryChannels[cfg.secondaryChannelCount++] = tmp;
                }
            }
        } else {
            ESP_LOGW(TAG, "Secondary channel layout changed -> resetting secondary channels");
        }

        // Primary channel (Ch 0)
        String ch0 = _pref.getString("ch0Name", "");
        strncpy(cfg.primaryChannelName, ch0.c_str(), sizeof(cfg.primaryChannelName) - 1);
        cfg.primaryChannelName[sizeof(cfg.primaryChannelName) - 1] = '\0';
        cfg.primaryPskLen = _pref.getUChar("ch0PskLen", 16);
        if (cfg.primaryPskLen > 32) cfg.primaryPskLen = 16;
        memset(cfg.primaryPsk, 0, sizeof(cfg.primaryPsk));
        if (cfg.primaryPskLen > 0) {
            if (_pref.getBytes("ch0Psk", cfg.primaryPsk, cfg.primaryPskLen) != cfg.primaryPskLen) {
                const uint8_t defKey[16] = {0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01};
                memcpy(cfg.primaryPsk, defKey, 16);
                cfg.primaryPskLen = 16;
            }
        }
        cfg.nodeIdSalt = _pref.getUInt("idSalt", 0);
        cfg.positionIntervalSec = _pref.getUInt("posIntSec", 900);

        cfg.ledNotifications = _pref.getBool("ledNotif", true);
        cfg.buzzerVolume = _pref.getUChar("buzVol", 2);

        // Channel auto-adapt: restore the (hash,key) pair learned from peers before the last reboot
        cfg.learnedChanValid = _pref.getBool("lcValid", false);
        cfg.learnedChanHash = _pref.getUChar("lcHash", 0);
        cfg.learnedChanKeyLen = _pref.getUChar("lcKeyLen", 0);
        if (cfg.learnedChanKeyLen > 32) {
            cfg.learnedChanValid = false;
            cfg.learnedChanKeyLen = 0;
        }
        memset(cfg.learnedChanKey, 0, sizeof(cfg.learnedChanKey));
        if (cfg.learnedChanKeyLen > 0) {
            if (_pref.getBytes("lcKey", cfg.learnedChanKey, cfg.learnedChanKeyLen) != cfg.learnedChanKeyLen) {
                memset(cfg.learnedChanKey, 0, sizeof(cfg.learnedChanKey));
                cfg.learnedChanKeyLen = 0;
                cfg.learnedChanValid = false;
            }
        }
        cfg.learnedChanHits = _pref.getUShort("lcHits", 0);
        cfg.meshTimeSync = _pref.getBool("meshTsync", true);

        ESP_LOGI(TAG, "Loaded settings from NVS (Node: %s / %s, Freq: %.3f MHz, Pwr: %d dBm, FL: %u%%, PwrBtn: %u, Clock: %s%s, Role: %u, Led: %d, Vol: %u, Duty: %u%%, RBC: %u)",
                 cfg.shortName, cfg.longName, cfg.frequency, cfg.txPower, cfg.frontlight, cfg.powerBtnAction,
                 cfg.clockLandscape ? "landscape" : "portrait", cfg.clockFlip180 ? " 180" : "", cfg.nodeRole,
                 cfg.ledNotifications ? 1 : 0, cfg.buzzerVolume, (unsigned)cfg.dutyCyclePct,
                 (unsigned)cfg.rebroadcastMode);
        return true;
    }

    ESP_LOGI(TAG, "No NVS settings found, using defaults");
    // NVS lost the settings: try the mirror we keep on the SD card before falling back to defaults
    if (loadSettingsFromConfigJson(cfg)) {
        ESP_LOGW(TAG, "Recovered settings from /monomesh/config.json (NVS was empty)");
        saveSettings(cfg); // re-seed NVS so the next boot is normal again
        return true;
    }
    ESP_LOGI(TAG, "No settings on the SD either, using built-in defaults");
    return false;
}

namespace {
// Extract the value of `"key": value` from one line of our own flat config.json
bool jsonLineValue(const String& line, const char* key, String& value) {
    String token = String("\"") + key + "\"";
    int k = line.indexOf(token);
    if (k < 0) return false;
    int colon = line.indexOf(':', k + token.length());
    if (colon < 0) return false;
    String v = line.substring(colon + 1);
    v.trim();
    if (v.endsWith(",")) v.remove(v.length() - 1);
    v.trim();
    if (v.length() >= 2 && v.startsWith("\"") && v.endsWith("\"")) {
        v = v.substring(1, v.length() - 1);
    }
    value = v;
    return true;
}
} // namespace

bool StorageManager::loadSettingsFromConfigJson(SettingsConfig& cfg) {
    if (!_sdMounted || !SD_MMC.exists("/monomesh/config.json")) return false;

    File f = SD_MMC.open("/monomesh/config.json", FILE_READ);
    if (!f) return false;

    bool gotIdentity = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        String v;
        if (jsonLineValue(line, "longName", v)) {
            if (v.length() > 0) {
                strncpy(cfg.longName, v.c_str(), sizeof(cfg.longName) - 1);
                gotIdentity = true;
            }
        } else if (jsonLineValue(line, "shortName", v)) {
            if (v.length() > 0) strncpy(cfg.shortName, v.c_str(), sizeof(cfg.shortName) - 1);
        } else if (jsonLineValue(line, "frequency", v)) {
            float fq = v.toFloat();
            if (fq > 100.0f && fq < 1000.0f) cfg.frequency = fq;
        } else if (jsonLineValue(line, "txPower", v)) {
            cfg.txPower = (int8_t)v.toInt();
        } else if (jsonLineValue(line, "modemPreset", v)) {
            cfg.modemPreset = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "hopLimit", v)) {
            cfg.hopLimit = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "frontlight", v)) {
            cfg.frontlight = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "powerBtnAction", v)) {
            cfg.powerBtnAction = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "clockLandscape", v)) {
            cfg.clockLandscape = (v == "true");
        } else if (jsonLineValue(line, "clockFlip180", v)) {
            cfg.clockFlip180 = (v == "true");
        } else if (jsonLineValue(line, "nodeRole", v)) {
            cfg.nodeRole = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "antiGhostThreshold", v)) {
            cfg.antiGhostThreshold = (uint16_t)v.toInt();
        } else if (jsonLineValue(line, "nodeInfoIntervalSec", v)) {
            cfg.nodeInfoIntervalSec = (uint32_t)v.toInt();
        } else if (jsonLineValue(line, "telemetryIntervalSec", v)) {
            cfg.telemetryIntervalSec = (uint32_t)v.toInt();
        } else if (jsonLineValue(line, "positionIntervalSec", v)) {
            cfg.positionIntervalSec = (uint32_t)v.toInt();
        } else if (jsonLineValue(line, "neighborInfoIntervalSec", v)) {
            cfg.neighborInfoIntervalSec = (uint32_t)v.toInt();
        } else if (jsonLineValue(line, "dutyCyclePct", v)) {
            cfg.dutyCyclePct = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "rebroadcastMode", v)) {
            cfg.rebroadcastMode = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "buzzerMode", v)) {
            cfg.buzzerMode = (uint8_t)v.toInt();
        } else if (jsonLineValue(line, "pkiEnabled", v)) {
            cfg.pkiEnabled = (v == "true");
        } else if (jsonLineValue(line, "gpsEnabled", v)) {
            cfg.gpsEnabled = (v == "true");
        } else if (jsonLineValue(line, "timezoneOffset", v)) {
            cfg.timezoneOffset = (int8_t)v.toInt();
        } else if (jsonLineValue(line, "primaryChannelName", v)) {
            strncpy(cfg.primaryChannelName, v.c_str(), sizeof(cfg.primaryChannelName) - 1);
        } else if (jsonLineValue(line, "primaryPskHex", v)) {
            if (v.length() == 32 || v.length() == 64) {
                uint8_t key[32] = {};
                uint8_t keyLen = 0;
                for (size_t i = 0; i + 1 < v.length(); i += 2) {
                    key[keyLen++] = (uint8_t)strtoul(v.substring(i, i + 2).c_str(), nullptr, 16);
                }
                memcpy(cfg.primaryPsk, key, keyLen);
                cfg.primaryPskLen = keyLen;
            }
        }
    }
    f.close();
    return gotIdentity;
}

bool StorageManager::saveSettings(const SettingsConfig& cfg) {
    // Every write is checked: the block is marked valid again only if all of them succeeded.
    bool ok = true;
    auto chk = [&ok](bool success) { if (!success) ok = false; };

    // Invalidate the block before touching anything: if a write fails below (NVS full) the next boot
    // must not read a half-new/half-old configuration just because cfg_init was left true by the
    // previous save.
    chk(_pref.putBool("cfg_init", false) == 1);

    chk(_pref.putString("longName", cfg.longName) != 0 || cfg.longName[0] == '\0');
    chk(_pref.putString("shortName", cfg.shortName) != 0 || cfg.shortName[0] == '\0');
    chk(_pref.putFloat("freq", cfg.frequency) == 4);
    chk(_pref.putChar("txPower", cfg.txPower) == 1);
    chk(_pref.putUChar("preset", cfg.modemPreset) == 1);
    chk(_pref.putUChar("hopLimit", cfg.hopLimit) == 1);
    chk(_pref.putUChar("frontlight", cfg.frontlight) == 1);
    chk(_pref.putUChar("pwrBtnAct", cfg.powerBtnAction) == 1);
    chk(_pref.putUShort("antiGhost", cfg.antiGhostThreshold) == 2);
    chk(_pref.putBool("clockLand", cfg.clockLandscape) == 1);
    chk(_pref.putBool("clockFlip", cfg.clockFlip180) == 1);

    chk(_pref.putUChar("nodeRole", cfg.nodeRole) == 1);
    chk(_pref.putUInt("niIntSec", cfg.nodeInfoIntervalSec) == 4);
    chk(_pref.putUInt("telIntSec", cfg.telemetryIntervalSec) == 4);
    chk(_pref.putUInt("nbIntSec", cfg.neighborInfoIntervalSec) == 4);
    chk(_pref.putUChar("dutyPct", cfg.dutyCyclePct) == 1);
    chk(_pref.putUChar("rbcMode", cfg.rebroadcastMode) == 1);
    chk(_pref.putUChar("buzMode", cfg.buzzerMode) == 1);
    chk(_pref.putBool("pkiEn", cfg.pkiEnabled) == 1);
    chk(_pref.putBool("gpsEn", cfg.gpsEnabled) == 1);
    chk(_pref.putChar("tzOff", cfg.timezoneOffset) == 1);
    chk(_pref.putUChar("secChCnt", cfg.secondaryChannelCount) == 1);
    chk(_pref.putUChar("chVer", 2) == 1);
    for (uint8_t i = 0; i < cfg.secondaryChannelCount && i < 7; ++i) {
        char k[16];
        snprintf(k, sizeof(k), "ch_%u", (unsigned)i);
        chk(_pref.putBytes(k, &cfg.secondaryChannels[i], sizeof(ChannelConfig)) == sizeof(ChannelConfig));
    }

    // Primary channel (Ch 0). putString returns strlen(): an intentionally empty channel name is a
    // valid write and must not mark the whole block as failed.
    chk(_pref.putString("ch0Name", cfg.primaryChannelName) != 0 || cfg.primaryChannelName[0] == '\0');
    chk(_pref.putUChar("ch0PskLen", cfg.primaryPskLen) == 1);
    if (cfg.primaryPskLen > 0) {
        chk(_pref.putBytes("ch0Psk", cfg.primaryPsk, cfg.primaryPskLen) == cfg.primaryPskLen);
    }
    chk(_pref.putUInt("idSalt", cfg.nodeIdSalt) == 4);
    chk(_pref.putUInt("posIntSec", cfg.positionIntervalSec) == 4);
    chk(_pref.putBool("ledNotif", cfg.ledNotifications) == 1);
    chk(_pref.putUChar("buzVol", cfg.buzzerVolume) == 1);

    chk(_pref.putBool("lcValid", cfg.learnedChanValid) == 1);
    chk(_pref.putUChar("lcHash", cfg.learnedChanHash) == 1);
    chk(_pref.putUChar("lcKeyLen", cfg.learnedChanKeyLen) == 1);
    if (cfg.learnedChanKeyLen > 0) {
        chk(_pref.putBytes("lcKey", cfg.learnedChanKey, cfg.learnedChanKeyLen) == cfg.learnedChanKeyLen);
    }
    chk(_pref.putUShort("lcHits", cfg.learnedChanHits) == 2);
    chk(_pref.putBool("meshTsync", cfg.meshTimeSync) == 1);

    // Valid only if every single write above succeeded: otherwise the block stays invalid and the
    // boot path falls back to config.json / defaults instead of mixing new and old fields.
    if (ok) {
        chk(_pref.putBool("cfg_init", true) == 1);
    } else {
        ESP_LOGE(TAG, "Settings NVS write incomplete: cfg_init left false (the block will not be loaded)");
    }

    ESP_LOGI(TAG, "Saved settings to NVS (%u free entries left)", (unsigned)_pref.freeEntries());

    if (_sdMounted) {
        File f = SD_MMC.open("/monomesh/config.json", FILE_WRITE);
        if (f) {
            f.printf("{\n");
            f.printf("  \"longName\": \"%s\",\n", cfg.longName);
            f.printf("  \"shortName\": \"%s\",\n", cfg.shortName);
            f.printf("  \"frequency\": %.3f,\n", cfg.frequency);
            f.printf("  \"txPower\": %d,\n", cfg.txPower);
            f.printf("  \"modemPreset\": %u,\n", cfg.modemPreset);
            f.printf("  \"hopLimit\": %u,\n", cfg.hopLimit);
            f.printf("  \"frontlight\": %u,\n", cfg.frontlight);
            f.printf("  \"powerBtnAction\": %u,\n", cfg.powerBtnAction);
            f.printf("  \"antiGhostThreshold\": %u,\n", cfg.antiGhostThreshold);
            f.printf("  \"clockLandscape\": %s,\n", cfg.clockLandscape ? "true" : "false");
            f.printf("  \"clockFlip180\": %s,\n", cfg.clockFlip180 ? "true" : "false");
            f.printf("  \"nodeRole\": %u,\n", cfg.nodeRole);
            f.printf("  \"nodeInfoIntervalSec\": %u,\n", (unsigned)cfg.nodeInfoIntervalSec);
            f.printf("  \"telemetryIntervalSec\": %u,\n", (unsigned)cfg.telemetryIntervalSec);
            f.printf("  \"positionIntervalSec\": %u,\n", (unsigned)cfg.positionIntervalSec);
            f.printf("  \"neighborInfoIntervalSec\": %u,\n", (unsigned)cfg.neighborInfoIntervalSec);
            f.printf("  \"dutyCyclePct\": %u,\n", (unsigned)cfg.dutyCyclePct);
            f.printf("  \"rebroadcastMode\": %u,\n", (unsigned)cfg.rebroadcastMode);
            f.printf("  \"buzzerMode\": %u,\n", (unsigned)cfg.buzzerMode);
            f.printf("  \"pkiEnabled\": %s,\n", cfg.pkiEnabled ? "true" : "false");
            f.printf("  \"gpsEnabled\": %s,\n", cfg.gpsEnabled ? "true" : "false");
            f.printf("  \"timezoneOffset\": %d,\n", (int)cfg.timezoneOffset);
            // Channel identity too, so a recovery from this file restores the full configuration
            f.printf("  \"primaryChannelName\": \"%s\",\n", cfg.primaryChannelName);
            char pskHex[65] = {0};
            for (uint8_t i = 0; i < cfg.primaryPskLen && i < 32; ++i) {
                snprintf(pskHex + (i * 2), 3, "%02x", cfg.primaryPsk[i]);
            }
            f.printf("  \"primaryPskHex\": \"%s\"\n", pskHex);
            f.printf("}\n");
            f.close();
            ESP_LOGI(TAG, "Saved settings to /sdcard/monomesh/config.json");
        }
    }

    return true;
}

bool StorageManager::saveMapView(int32_t latE7, int32_t lonE7, uint8_t zoom) {
    _pref.putInt("mapLatI", latE7);
    _pref.putInt("mapLonI", lonE7);
    _pref.putUChar("mapZoom", zoom);
    return true;
}

bool StorageManager::loadMapView(int32_t& latE7, int32_t& lonE7, uint8_t& zoom) {
    if (!_pref.isKey("mapZoom")) return false;
    latE7 = _pref.getInt("mapLatI", 0);
    lonE7 = _pref.getInt("mapLonI", 0);
    zoom = _pref.getUChar("mapZoom", 13);
    return (latE7 != 0 || lonE7 != 0);
}

bool StorageManager::saveLearnedChannel(bool valid, uint8_t hash, const uint8_t* key, uint8_t keyLen, uint16_t hits) {
    if (!valid || !key || keyLen == 0 || keyLen > 32) {
        _pref.putBool("lcValid", false);
        _pref.putUChar("lcKeyLen", 0);
        _pref.putUShort("lcHits", 0);
        return true;
    }
    _pref.putBool("lcValid", true);
    _pref.putUChar("lcHash", hash);
    _pref.putUChar("lcKeyLen", keyLen);
    _pref.putBytes("lcKey", key, keyLen);
    _pref.putUShort("lcHits", hits);
    ESP_LOGI(TAG, "Persisted learned channel hash 0x%02x (%u byte key, %u hits)", (unsigned)hash,
             (unsigned)keyLen, (unsigned)hits);
    return true;
}

bool StorageManager::saveNodes(const std::vector<NodeRecord>& nodes) {
    if (_sdMounted) return saveNodesCsv(nodes);
    return saveNodesNvs(nodes);
}

bool StorageManager::saveNodesCsv(const std::vector<NodeRecord>& nodes) {
    if (!_sdMounted) return false;

    File f = SD_MMC.open("/monomesh/nodes.csv", FILE_WRITE);
    if (!f) {
        ESP_LOGE(TAG, "Failed to open /monomesh/nodes.csv for write");
        return false;
    }

    f.println("nodeNum,idStr,longName,shortName,lat,lon,alt,snr,rssi,lastHeard,battery,voltage,role,pubKey,mac,lastEpoch");
    for (const auto& n : nodes) {
        // Absent fields must not be written as empty: sscanf() with "%64[^,]" stops there and would
        // skip every following column (the MAC and the last-heard epoch used to be lost this way).
        char hexKey[65] = "-";
        if (n.hasPublicKey) {
            for (int b = 0; b < 32; ++b) {
                snprintf(hexKey + (b * 2), 3, "%02x", n.publicKey[b]);
            }
        }
        char hexMac[13] = "-";
        if (n.hasMac) {
            for (int b = 0; b < 6; ++b) {
                snprintf(hexMac + (b * 2), 3, "%02x", n.mac[b]);
            }
        }
        f.printf("%u,%s,\"%s\",\"%s\",%.6f,%.6f,%d,%.1f,%.1f,%u,%u,%.2f,%u,%s,%s,%u\n",
                 (unsigned)n.nodeNum, n.idStr, n.longName, n.shortName,
                 n.lat, n.lon, (int)n.alt, n.snr, n.rssi, (unsigned)n.lastHeard,
                 (unsigned)n.battery, n.voltage, (unsigned)n.role, hexKey, hexMac,
                 (unsigned)n.lastHeardEpoch);
    }
    f.close();
    ESP_LOGI(TAG, "Persisted %u nodes to /sdcard/monomesh/nodes.csv", (unsigned)nodes.size());
    return true;
}

bool StorageManager::loadNodes(std::vector<NodeRecord>& nodes) {
    if (_sdMounted && SD_MMC.exists("/monomesh/nodes.csv")) return loadNodesCsv(nodes);
    return loadNodesNvs(nodes);
}

bool StorageManager::loadNodesCsv(std::vector<NodeRecord>& nodes) {
    if (!_sdMounted || !SD_MMC.exists("/monomesh/nodes.csv")) return false;

    File f = SD_MMC.open("/monomesh/nodes.csv", FILE_READ);
    if (!f) return false;

    nodes.clear();
    String header = f.readStringUntil('\n'); // skip header
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        NodeRecord nr = {};
        char lBuf[32] = "";
        char sBuf[8] = "";
        char hexKey[65] = "";
        char hexMac[13] = "";
        unsigned int bat = 0, role = 0;
        unsigned int lastEpoch = 0;
        float volt = 0.0f;
        int matched = sscanf(line.c_str(),
                             "%u,%11[^,],\"%31[^\"]\",\"%7[^\"]\",%lf,%lf,%d,%f,%f,%u,%u,%f,%u,%64[^,],%12[^,],%u",
                             &nr.nodeNum, nr.idStr, lBuf, sBuf,
                             &nr.lat, &nr.lon, &nr.alt, &nr.snr, &nr.rssi, &nr.lastHeard,
                             &bat, &volt, &role, hexKey, hexMac, &lastEpoch);
        if (matched < 10) {
            // Fallback to legacy 10-field CSV format
            matched = sscanf(line.c_str(), "%u,%11[^,],\"%31[^\"]\",\"%7[^\"]\",%lf,%lf,%d,%f,%f,%u",
                             &nr.nodeNum, nr.idStr, lBuf, sBuf,
                             &nr.lat, &nr.lon, &nr.alt, &nr.snr, &nr.rssi, &nr.lastHeard);
        } else {
            nr.battery = (uint8_t)bat;
            nr.voltage = volt;
            nr.role = (uint8_t)role;
            if (strlen(hexKey) == 64) {
                for (int b = 0; b < 32; ++b) {
                    unsigned int byteVal = 0;
                    sscanf(hexKey + (b * 2), "%02x", &byteVal);
                    nr.publicKey[b] = (uint8_t)byteVal;
                }
                nr.hasPublicKey = true;
            }
            if (strlen(hexMac) == 12) {
                for (int b = 0; b < 6; ++b) {
                    unsigned int byteVal = 0;
                    sscanf(hexMac + (b * 2), "%02x", &byteVal);
                    nr.mac[b] = (uint8_t)byteVal;
                }
                nr.hasMac = true;
            }
            if (lastEpoch > 1700000000UL) {
                nr.lastHeardEpoch = lastEpoch;
            }
        }
        if (matched >= 4) {
            strncpy(nr.longName, lBuf, sizeof(nr.longName) - 1);
            strncpy(nr.shortName, sBuf, sizeof(nr.shortName) - 1);
            nodes.push_back(nr);
        }
    }
    f.close();
    ESP_LOGI(TAG, "Loaded %u nodes from /sdcard/monomesh/nodes.csv", (unsigned)nodes.size());
    return true;
}

// ---- NVS node mirror: keeps the discovered nodes alive when no card is inserted ----
// 8 records per blob (~1.1 KB each, NVS entries hold up to ~4 KB) and a hard cap of 40 nodes so the
// mirror never eats the 24 KB NVS partition shared with settings, keys and the message ring.
// Compact mirror: 16 nodes at ~80 bytes each (2 blobs of ~640 B) instead of the full records, so the
// 24 KB NVS partition shared with settings, keys and the message ring never fills up.
static constexpr uint8_t NVS_NODE_PER_CHUNK = 8;
static constexpr uint8_t NVS_NODE_MAX = 16;

bool StorageManager::saveNodesNvs(const std::vector<NodeRecord>& nodes) {
    // Refuse to write when the partition is tight: a failed write would corrupt nothing but would
    // silently lose the update (and, in the worst case, starve the settings/keys writes).
    const size_t neededEntries = 32;
    if (_pref.freeEntries() < neededEntries) {
        ESP_LOGE(TAG, "NVS almost full (%u free entries), skipping the node mirror to protect settings",
                 (unsigned)_pref.freeEntries());
        return false;
    }

    uint8_t count = (uint8_t)(nodes.size() > NVS_NODE_MAX ? NVS_NODE_MAX : nodes.size());
    // Invalidate the mirror before overwriting the chunks: if a write fails (NVS full) the next boot
    // must not rebuild a mixed/truncated list from the old count and the new chunks.
    _pref.putUChar("ndCnt", 0);

    NvsNodeRecord buf[NVS_NODE_PER_CHUNK];
    for (uint8_t i = 0; i < count; i += NVS_NODE_PER_CHUNK) {
        uint8_t n = (uint8_t)((count - i) > NVS_NODE_PER_CHUNK ? NVS_NODE_PER_CHUNK : (count - i));
        memset(buf, 0, sizeof(buf));
        for (uint8_t k = 0; k < n; ++k) {
            const NodeRecord& src = nodes[i + k];
            buf[k].nodeNum = src.nodeNum;
            strncpy(buf[k].longName, src.longName, sizeof(buf[k].longName) - 1);
            strncpy(buf[k].shortName, src.shortName, sizeof(buf[k].shortName) - 1);
            buf[k].lastHeardEpoch = src.lastHeardEpoch;
            memcpy(buf[k].mac, src.mac, 6);
            if (src.hasPublicKey) {
                buf[k].flags |= 0x01;
                memcpy(buf[k].publicKey, src.publicKey, 32);
            }
            if (src.hasMac) buf[k].flags |= 0x02;
        }
        char key[10];
        snprintf(key, sizeof(key), "ndB_%u", (unsigned)(i / NVS_NODE_PER_CHUNK));
        size_t wrote = _pref.putBytes(key, buf, n * sizeof(NvsNodeRecord));
        if (wrote != n * sizeof(NvsNodeRecord)) {
            ESP_LOGE(TAG, "NVS node mirror write failed (%u/%u bytes): NVS full?",
                     (unsigned)wrote, (unsigned)(n * sizeof(NvsNodeRecord)));
            return false;
        }
    }
    // Drop chunks left over from a previously longer list
    for (uint8_t c = (uint8_t)((count + NVS_NODE_PER_CHUNK - 1) / NVS_NODE_PER_CHUNK);
         c < (NVS_NODE_MAX / NVS_NODE_PER_CHUNK); ++c) {
        char key[10];
        snprintf(key, sizeof(key), "ndB_%u", (unsigned)c);
        _pref.remove(key);
    }
    if (_pref.putUChar("ndCnt", count) != 1) {
        ESP_LOGE(TAG, "NVS node mirror count write failed: mirror left invalid");
        return false;
    }
    ESP_LOGI(TAG, "Persisted %u nodes to NVS (no SD card)", (unsigned)count);
    return true;
}

bool StorageManager::loadNodesNvs(std::vector<NodeRecord>& nodes) {
    nodes.clear();
    uint8_t count = _pref.getUChar("ndCnt", 0);
    if (count == 0) return false;
    if (count > NVS_NODE_MAX) count = NVS_NODE_MAX;

    NvsNodeRecord buf[NVS_NODE_PER_CHUNK];
    for (uint8_t i = 0; i < count; i += NVS_NODE_PER_CHUNK) {
        char key[10];
        snprintf(key, sizeof(key), "ndB_%u", (unsigned)(i / NVS_NODE_PER_CHUNK));
        uint8_t n = (uint8_t)((count - i) > NVS_NODE_PER_CHUNK ? NVS_NODE_PER_CHUNK : (count - i));
        size_t want = (size_t)n * sizeof(NvsNodeRecord);
        memset(buf, 0, sizeof(buf));
        if (_pref.getBytes(key, buf, want) != want) break;
        for (uint8_t k = 0; k < n; ++k) {
            NodeRecord rec = {};
            rec.nodeNum = buf[k].nodeNum;
            strncpy(rec.longName, buf[k].longName, sizeof(rec.longName) - 1);
            strncpy(rec.shortName, buf[k].shortName, sizeof(rec.shortName) - 1);
            snprintf(rec.idStr, sizeof(rec.idStr), "!%08x", (unsigned)buf[k].nodeNum);
            rec.lastHeardEpoch = buf[k].lastHeardEpoch;
            memcpy(rec.mac, buf[k].mac, 6);
            rec.hasMac = (buf[k].flags & 0x02) != 0;
            rec.hasPublicKey = (buf[k].flags & 0x01) != 0;
            if (rec.hasPublicKey) memcpy(rec.publicKey, buf[k].publicKey, 32);
            nodes.push_back(rec);
        }
    }
    ESP_LOGI(TAG, "Loaded %u nodes from NVS (no SD card)", (unsigned)nodes.size());
    return !nodes.empty();
}

bool StorageManager::clearNodes() {
    if (_sdMounted && SD_MMC.exists("/monomesh/nodes.csv")) {
        SD_MMC.remove("/monomesh/nodes.csv");
        ESP_LOGI(TAG, "Removed /sdcard/monomesh/nodes.csv");
    }
    // Clear the NVS mirror too, otherwise the deleted list would come back when no card is present
    _pref.putUChar("ndCnt", 0);
    for (uint8_t c = 0; c < (NVS_NODE_MAX / NVS_NODE_PER_CHUNK); ++c) {
        char key[10];
        snprintf(key, sizeof(key), "ndB_%u", (unsigned)c);
        _pref.remove(key);
    }
    return true;
}

bool StorageManager::writeMessageToCsv(const SavedMessage& msg) {
    if (!_sdMounted) return false;

    const bool exists = SD_MMC.exists("/monomesh/messages.csv");
    File f = SD_MMC.open("/monomesh/messages.csv", FILE_APPEND);
    if (!f) return false;
    if (!exists) {
        f.println("id,from,to,sender,isOut,timestamp,isAck,time,preset,text");
    }
    // Replace newlines in text to preserve CSV integrity
    char cleanText[sizeof(msg.text)];
    strncpy(cleanText, msg.text, sizeof(cleanText) - 1);
    cleanText[sizeof(cleanText) - 1] = '\0';
    for (char* p = cleanText; *p; ++p) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }

    f.printf("%u,%u,%u,%s,%d,%u,%d,%s,%s,%s\n",
             (unsigned)msg.id,
             (unsigned)msg.fromNode,
             (unsigned)msg.toNode,
             msg.senderShort,
             msg.isOutgoing ? 1 : 0,
             (unsigned)msg.timestamp,
             msg.isAcked ? 1 : 0,
             msg.timeStr[0] ? msg.timeStr : "--:--",
             msg.channelPreset[0] ? msg.channelPreset : "MedFast",
             cleanText);
    f.close();
    return true;
}

bool StorageManager::appendChatMessage(const SavedMessage& msg) {
    // 1. Persist to SD card if mounted (otherwise the ring below is the only copy)
    writeMessageToCsv(msg);

    // 2. Mirror into NVS ring buffer (up to 24 most recent messages). Only advance the ring when the
    // blob was actually stored: counting failed writes made m_cnt point at keys that never existed.
    uint32_t head = _pref.getUInt("m_head", 0);
    uint32_t cnt = _pref.getUInt("m_cnt", 0);
    char key[16];
    snprintf(key, sizeof(key), "msg_%u", (unsigned)(head % 24));
    if (_pref.putBytes(key, &msg, sizeof(SavedMessage)) != sizeof(SavedMessage)) {
        ESP_LOGE(TAG, "NVS message ring write failed (key %s): NVS full?", key);
        return _sdMounted; // on SD the message is safe, without SD it only lives in RAM
    }
    _pref.putUInt("m_head", (head + 1) % 24);
    if (cnt < 24) {
        _pref.putUInt("m_cnt", cnt + 1);
    }

    return true;
}

// Chronological contents of the NVS ring (oldest first). Used both by the boot fallback and by the
// flush that moves standby-received messages into the CSV.
void StorageManager::readNvsMessages(std::vector<SavedMessage>& messages) {
    uint32_t cnt = _pref.getUInt("m_cnt", 0);
    if (cnt == 0) return;
    if (cnt > 24) cnt = 24;
    const uint32_t head = _pref.getUInt("m_head", 0);
    const uint32_t start = (head >= cnt) ? (head - cnt) : (24 + head - cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        const uint32_t idx = (start + i) % 24;
        char key[16];
        snprintf(key, sizeof(key), "msg_%u", (unsigned)idx);
        SavedMessage sm = {};
        if (_pref.getBytes(key, &sm, sizeof(SavedMessage)) == sizeof(SavedMessage)) {
            if (sm.timeStr[0] == '\0') strncpy(sm.timeStr, "--:--", sizeof(sm.timeStr) - 1);
            messages.push_back(sm);
        }
    }
}

// The device receives messages while the SD card is unmounted (standby) and can only put them in the
// NVS ring. Without this flush they would be invisible on the next boot, where the CSV wins.
bool StorageManager::flushNvsMessagesToSd() {
    if (!_sdMounted) return false;

    std::vector<SavedMessage> ring;
    readNvsMessages(ring);
    if (ring.empty()) return true;

    // Collect the ids present in the tail of the CSV. The ring holds the newest 24 appends, so a
    // 16 KB window always covers the 24 matching lines (a line is ~300 B at most) and keeps the scan
    // cheap even when the history has grown large.
    std::vector<uint32_t> knownIds;
    if (SD_MMC.exists("/monomesh/messages.csv")) {
        File f = SD_MMC.open("/monomesh/messages.csv", FILE_READ);
        if (f) {
            const size_t tailSpan = 16 * 1024;
            if (f.size() > tailSpan) {
                f.seek(f.size() - tailSpan);
                // drop the (possibly partial) first line of the window
                f.readStringUntil('\n');
            }
            while (f.available()) {
                String line = f.readStringUntil('\n');
                uint32_t lineId = 0;
                if (sscanf(line.c_str(), "%u,", &lineId) == 1) knownIds.push_back(lineId);
            }
            f.close();
        }
    }

    uint32_t written = 0;
    for (const auto& sm : ring) {
        bool present = false;
        for (uint32_t id : knownIds) {
            if (id == sm.id) { present = true; break; }
        }
        if (present) continue;
        if (writeMessageToCsv(sm)) {
            knownIds.push_back(sm.id);
            ++written;
        }
    }
    if (written > 0) {
        ESP_LOGW(TAG, "Moved %u message(s) received while the SD was unmounted into /sdcard/monomesh/messages.csv",
                 (unsigned)written);
    }
    return true;
}

bool StorageManager::updateChatMessageAck(uint32_t id) {
    bool updated = false;

    // 1. Update SD card if mounted
    if (_sdMounted && SD_MMC.exists("/monomesh/messages.csv")) {
        File fIn = SD_MMC.open("/monomesh/messages.csv", FILE_READ);
        if (fIn) {
            File fOut = SD_MMC.open("/monomesh/messages.tmp", FILE_WRITE);
            if (fOut) {
                while (fIn.available()) {
                    String line = fIn.readStringUntil('\n');
                    String trimmed = line;
                    trimmed.trim();
                    if (trimmed.length() == 0) continue;

                    if (trimmed.startsWith("id,")) {
                        fOut.println(line);
                        continue;
                    }

                    // Check if line matches packet ID
                    uint32_t lineId = 0;
                    if (sscanf(trimmed.c_str(), "%u,", &lineId) == 1 && lineId == id) {
                        // Match found! Re-parse and rewrite with isAck = 1
                        SavedMessage sm = {};
                        int isOut = 0, isAck = 0;
                        char timeBuf[16] = "--:--", presetBuf[32] = "MedFast", textBuf[224] = "";
                        int matched = sscanf(trimmed.c_str(), "%u,%u,%u,%7[^,],%d,%u,%d,%15[^,],%31[^,],%223[^\r\n]",
                                             &sm.id, &sm.fromNode, &sm.toNode,
                                             sm.senderShort, &isOut, &sm.timestamp, &isAck,
                                             timeBuf, presetBuf, textBuf);
                        if (matched >= 10) {
                            fOut.printf("%u,%u,%u,%s,%d,%u,1,%s,%s,%s\n",
                                        (unsigned)sm.id, (unsigned)sm.fromNode, (unsigned)sm.toNode,
                                        sm.senderShort, isOut, (unsigned)sm.timestamp,
                                        timeBuf, presetBuf, textBuf);
                        } else {
                            matched = sscanf(trimmed.c_str(), "%u,%u,%u,%7[^,],%d,%u,%d,%223[^\r\n]",
                                             &sm.id, &sm.fromNode, &sm.toNode,
                                             sm.senderShort, &isOut, &sm.timestamp, &isAck, textBuf);
                            if (matched >= 8) {
                                fOut.printf("%u,%u,%u,%s,%d,%u,1,--:--,MedFast,%s\n",
                                            (unsigned)sm.id, (unsigned)sm.fromNode, (unsigned)sm.toNode,
                                            sm.senderShort, isOut, (unsigned)sm.timestamp, textBuf);
                            } else {
                                fOut.println(trimmed);
                            }
                        }
                        updated = true;
                    } else {
                        fOut.println(trimmed);
                    }
                }
                fOut.close();
            }
            fIn.close();

            if (updated) {
                SD_MMC.remove("/monomesh/messages.csv");
                SD_MMC.rename("/monomesh/messages.tmp", "/monomesh/messages.csv");
                ESP_LOGI(TAG, "Persisted ACK for packet 0x%08x in /sdcard/monomesh/messages.csv", (unsigned)id);
            } else {
                SD_MMC.remove("/monomesh/messages.tmp");
            }
        }
    }

    // 2. Update matching message in NVS ring buffer
    uint32_t cnt = _pref.getUInt("m_cnt", 0);
    if (cnt > 24) cnt = 24;
    for (uint32_t i = 0; i < cnt; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "msg_%u", (unsigned)i);
        if (!_pref.isKey(key)) continue; // ring hole left by an older build/partial write
        SavedMessage sm = {};
        if (_pref.getBytes(key, &sm, sizeof(SavedMessage)) == sizeof(SavedMessage)) {
            if (sm.id == id && sm.isOutgoing) {
                sm.isAcked = true;
                _pref.putBytes(key, &sm, sizeof(SavedMessage));
                updated = true;
                ESP_LOGI(TAG, "Persisted ACK for packet 0x%08x in NVS key %s", (unsigned)id, key);
            }
        }
    }
    return updated;
}

bool StorageManager::loadChatMessages(std::vector<SavedMessage>& messages) {
    messages.clear();

    // 1. Try loading from SD card first
    if (_sdMounted && SD_MMC.exists("/monomesh/messages.csv")) {
        File f = SD_MMC.open("/monomesh/messages.csv", FILE_READ);
        if (f) {
            String header = f.readStringUntil('\n'); // skip header
            while (f.available()) {
                String line = f.readStringUntil('\n');
                line.trim();
                if (line.length() == 0) continue;

                SavedMessage sm = {};
                int isOut = 0;
                int isAck = 0;
                char timeBuf[16] = "";
                char presetBuf[32] = "";
                char textBuf[224] = "";

                // New 10-column format: id,from,to,sender,isOut,timestamp,isAck,time,preset,text
                int matched = sscanf(line.c_str(), "%u,%u,%u,%7[^,],%d,%u,%d,%15[^,],%31[^,],%223[^\r\n]",
                                     &sm.id, &sm.fromNode, &sm.toNode,
                                     sm.senderShort, &isOut, &sm.timestamp, &isAck,
                                     timeBuf, presetBuf, textBuf);
                if (matched >= 10) {
                    sm.isOutgoing = (isOut != 0);
                    sm.isAcked = (isAck != 0);
                    strncpy(sm.timeStr, timeBuf, sizeof(sm.timeStr) - 1);
                    strncpy(sm.channelPreset, presetBuf, sizeof(sm.channelPreset) - 1);
                    strncpy(sm.text, textBuf, sizeof(sm.text) - 1);
                    messages.push_back(sm);
                } else {
                    // Fallback to legacy 8-column format: id,from,to,sender,isOut,timestamp,isAck,text
                    matched = sscanf(line.c_str(), "%u,%u,%u,%7[^,],%d,%u,%d,%223[^\r\n]",
                                     &sm.id, &sm.fromNode, &sm.toNode,
                                     sm.senderShort, &isOut, &sm.timestamp, &isAck, textBuf);
                    if (matched >= 8) {
                        sm.isOutgoing = (isOut != 0);
                        sm.isAcked = (isAck != 0);
                        strncpy(sm.timeStr, "--:--", sizeof(sm.timeStr) - 1);
                        strncpy(sm.channelPreset, "MedFast", sizeof(sm.channelPreset) - 1);
                        strncpy(sm.text, textBuf, sizeof(sm.text) - 1);
                        messages.push_back(sm);
                    } else {
                        // Fallback to legacy 7-column format: id,from,to,sender,isOut,timestamp,text
                        matched = sscanf(line.c_str(), "%u,%u,%u,%7[^,],%d,%u,%223[^\r\n]",
                                         &sm.id, &sm.fromNode, &sm.toNode,
                                         sm.senderShort, &isOut, &sm.timestamp, textBuf);
                        if (matched >= 7) {
                            sm.isOutgoing = (isOut != 0);
                            sm.isAcked = false;
                            strncpy(sm.timeStr, "--:--", sizeof(sm.timeStr) - 1);
                            strncpy(sm.channelPreset, "MedFast", sizeof(sm.channelPreset) - 1);
                            strncpy(sm.text, textBuf, sizeof(sm.text) - 1);
                            messages.push_back(sm);
                        }
                    }
                }
            }
            f.close();
            ESP_LOGI(TAG, "Loaded %u messages from /sdcard/monomesh/messages.csv", (unsigned)messages.size());
        }
    }

    // 2. Merge the NVS ring. It is the only store when no card is inserted, but it also holds the
    // messages received while the card was unmounted (standby): those must not be discarded just
    // because the CSV exists.
    std::vector<SavedMessage> ring;
    readNvsMessages(ring);
    uint32_t added = 0;
    for (const auto& sm : ring) {
        bool present = false;
        for (const auto& existing : messages) {
            if (existing.id == sm.id) { present = true; break; }
        }
        if (present) continue;
        messages.push_back(sm);
        ++added;
    }
    if (added > 0) {
        ESP_LOGI(TAG, "Recovered %u message(s) from the NVS ring (not yet in the CSV)", (unsigned)added);
    }

    return !messages.empty();
}

bool StorageManager::clearChatHistory() {
    if (_sdMounted && SD_MMC.exists("/monomesh/messages.csv")) {
        SD_MMC.remove("/monomesh/messages.csv");
        ESP_LOGI(TAG, "Removed /sdcard/monomesh/messages.csv");
    }
    if (_sdMounted && SD_MMC.exists("/monomesh/messages.txt")) {
        SD_MMC.remove("/monomesh/messages.txt");
    }
    _pref.putUInt("m_head", 0);
    _pref.putUInt("m_cnt", 0);
    return true;
}

} // namespace MonoMesh
