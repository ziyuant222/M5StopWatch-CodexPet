/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_codex_buddy.h"

#include <assets/assets.h>
#include <assets/gif_anim/seedy_gif_asset.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <hal/hal.h>
#include <hal/utils/settings/settings.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
#include <host/ble_store.h>
#include <host/util/util.h>
#include <mooncake_log.h>
#include <nimble/ble.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <smooth_lvgl.hpp>
#include <store/config/ble_store_config.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <string>
#include <vector>

using namespace mooncake;

extern "C" void ble_store_config_init(void);

namespace {

constexpr const char* TAG = "CodexBuddy";
constexpr const char* CODEX_BUDDY_SETTINGS_NS = "codex_buddy";
constexpr const char* USAGE_PRIMARY_KEY = "usage_primary";
constexpr const char* USAGE_SECONDARY_KEY = "usage_secondary";
constexpr const char* TOKENS_TODAY_KEY = "tokens_today";
constexpr uint32_t kAutoScreenOffMs = 60 * 1000;
constexpr uint32_t kFortuneRevealDelayMs = 2200;
constexpr uint32_t kShakeCooldownMs = 6500;
constexpr uint32_t kImuPollIntervalMs = 70;
constexpr uint32_t kShakeRequiredMs = 2200;
constexpr uint32_t kShakeMaxActiveMs = 3300;
constexpr uint32_t kShakeContinuityMs = 620;
constexpr uint32_t kShakeFeedbackIntervalMs = 700;
constexpr uint8_t kShakeRequiredStrongSamples = 12;
constexpr uint32_t kBreathInhaleMs = 4000;
constexpr uint32_t kBreathHoldMs = 7000;
constexpr uint32_t kBreathExhaleMs = 8000;
constexpr uint32_t kBreathCycleMs = kBreathInhaleMs + kBreathHoldMs + kBreathExhaleMs;
constexpr uint8_t kPetStateCount = 9;
constexpr const char* kBoardingPassQrPayload =
    "M1TANGZIYUAN          E       PEKHGHGJ 8988 153I022J0005 "
    "12B>1030MM0E89148358504760NI4****************4";
using codex_buddy_assets::GifAnimation;
using codex_buddy_assets::PetGifState;
using SerialReplyFn = void (*)(const char*);

// Nordic UART Service, little-endian byte order as required by BLE_UUID128_INIT.
static const ble_uuid128_t NUS_SERVICE_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_RX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_TX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

constexpr const char* kDynamicPetDir = "/spiflash/codex_pet";
constexpr uint8_t kDynamicPetMaxFrames = 16;
constexpr uint8_t kDynamicPetMaxSizeEntries = kDynamicPetMaxFrames + 1;
constexpr size_t kUploadChunkMaxBytes = 1024;

struct DynamicPetAnim {
    bool valid = false;
    bool rle = false;
    char path[96] = "";
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t frame_count = 0;
    uint16_t delays_ms[kDynamicPetMaxFrames] = {};
    uint32_t frame_offsets[kDynamicPetMaxSizeEntries] = {};
};

uint8_t stateIndex(PetGifState state)
{
    switch (state) {
        case PetGifState::Sleep: return 0;
        case PetGifState::Idle: return 1;
        case PetGifState::Busy: return 2;
        case PetGifState::Attention: return 3;
        case PetGifState::Celebrate: return 4;
        case PetGifState::Dizzy: return 5;
        case PetGifState::Heart: return 6;
        case PetGifState::Wave: return 7;
        case PetGifState::Sparkle: return 8;
        default: return 1;
    }
}

const char* stateName(PetGifState state)
{
static constexpr const char* names[] = {
        "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart", "wave", "sparkle",
    };
    return names[stateIndex(state)];
}

struct PetOption {
    const char* selector;
    const char* label;
};

struct RuntimePetOption {
    char selector[48] = "";
    char label[32] = "";
};

static constexpr PetOption kPetOptions[] = {
    {"codex", "Codex"},
    {"dewey", "Dewey"},
    {"fireball", "Fireball"},
    {"rocky", "Rocky"},
    {"seedy", "Seedy"},
    {"stacky", "Stacky"},
    {"bsod", "BSOD"},
    {"null-signal", "Null Signal"},
    {"custom:twinkle-twinkle", "Twinkle"},
    {"custom:labubu", "Labubu"},
    {"custom:bytedcli-pet", "BytedCLI"},
};

bool safeUploadName(const char* name)
{
    if (name == nullptr || name[0] == 0) {
        return false;
    }
    for (const char* p = name; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return std::strstr(name, "..") == nullptr;
}

bool safePetSelector(const char* value)
{
    if (value == nullptr || value[0] == 0) {
        return false;
    }
    for (const char* p = value; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.' || c == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool safePetLabel(const char* value)
{
    if (value == nullptr || value[0] == 0) {
        return false;
    }
    for (const char* p = value; *p; ++p) {
        const char c = *p;
        if (static_cast<unsigned char>(c) < 32 || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

void wipeDynamicPetDir()
{
    DIR* dir = opendir(kDynamicPetDir);
    if (dir == nullptr) {
        mkdir(kDynamicPetDir, 0775);
        return;
    }
    while (dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char path[320];
        std::snprintf(path, sizeof(path), "%s/%s", kDynamicPetDir, ent->d_name);
        unlink(path);
    }
    closedir(dir);
}

class DynamicPetPack {
public:
    void unload()
    {
        _loaded = false;
        _name[0] = '\0';
        for (auto& anim : _anims) {
            anim = DynamicPetAnim {};
        }
        ++_generation;
    }

    bool load()
    {
        char manifest_path[128];
        std::snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", kDynamicPetDir);
        FILE* fp = std::fopen(manifest_path, "rb");
        if (fp == nullptr) {
            unload();
            return false;
        }
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (size <= 0 || size > 8192) {
            std::fclose(fp);
            unload();
            return false;
        }
        std::string text;
        text.resize(static_cast<size_t>(size));
        const size_t read = std::fread(text.data(), 1, text.size(), fp);
        std::fclose(fp);
        if (read != text.size()) {
            unload();
            return false;
        }

        cJSON* doc = cJSON_ParseWithLength(text.data(), text.size());
        if (doc == nullptr) {
            unload();
            return false;
        }

        const cJSON* format = cJSON_GetObjectItem(doc, "format");
        const bool pack_rle = cJSON_IsString(format) && std::strcmp(format->valuestring, "rgb565rle") == 0;
        char next_name[48] = "";
        const cJSON* name = cJSON_GetObjectItem(doc, "name");
        if (cJSON_IsString(name) && safeUploadName(name->valuestring)) {
            std::snprintf(next_name, sizeof(next_name), "%s", name->valuestring);
        }
        DynamicPetAnim next[kPetStateCount] = {};
        const cJSON* states = cJSON_GetObjectItem(doc, "states");
        for (uint8_t i = 0; i < kPetStateCount; ++i) {
            const cJSON* item = cJSON_GetObjectItem(states, stateName(static_cast<PetGifState>(i)));
            if (!cJSON_IsObject(item)) {
                continue;
            }
            const cJSON* file = cJSON_GetObjectItem(item, "file");
            const cJSON* frames = cJSON_GetObjectItem(item, "frames");
            const cJSON* width = cJSON_GetObjectItem(item, "width");
            const cJSON* height = cJSON_GetObjectItem(item, "height");
            if (!cJSON_IsString(file) || !safeUploadName(file->valuestring) || !cJSON_IsNumber(frames) ||
                !cJSON_IsNumber(width) || !cJSON_IsNumber(height)) {
                continue;
            }
            auto& anim = next[i];
            anim.valid = true;
            anim.rle = pack_rle;
            anim.frame_count = std::min<int>(std::max<int>(frames->valueint, 1), kDynamicPetMaxFrames);
            anim.width = std::min<int>(std::max<int>(width->valueint, 1), 192);
            anim.height = std::min<int>(std::max<int>(height->valueint, 1), 208);
            std::snprintf(anim.path, sizeof(anim.path), "%s/%s", kDynamicPetDir, file->valuestring);
            const cJSON* delays = cJSON_GetObjectItem(item, "delays");
            for (uint8_t j = 0; j < anim.frame_count; ++j) {
                const cJSON* delay = cJSON_IsArray(delays) ? cJSON_GetArrayItem(delays, j) : nullptr;
                anim.delays_ms[j] = cJSON_IsNumber(delay) ? std::max<int>(delay->valueint, 20) : 120;
            }
            if (anim.rle) {
                const cJSON* sizes = cJSON_GetObjectItem(item, "sizes");
                if (!cJSON_IsArray(sizes)) {
                    anim.valid = false;
                    continue;
                }
                uint32_t offset = 0;
                anim.frame_offsets[0] = 0;
                bool sizes_ok = true;
                for (uint8_t j = 0; j < anim.frame_count; ++j) {
                    const cJSON* size_item = cJSON_GetArrayItem(sizes, j);
                    if (!cJSON_IsNumber(size_item) || size_item->valueint <= 0) {
                        sizes_ok = false;
                        break;
                    }
                    offset += static_cast<uint32_t>(size_item->valueint);
                    anim.frame_offsets[j + 1] = offset;
                }
                if (!sizes_ok) {
                    anim.valid = false;
                    continue;
                }
            } else {
                const uint32_t frame_bytes = static_cast<uint32_t>(anim.width) * anim.height * sizeof(uint16_t);
                for (uint8_t j = 0; j <= anim.frame_count; ++j) {
                    anim.frame_offsets[j] = frame_bytes * j;
                }
            }

            struct stat st = {};
            if (stat(anim.path, &st) != 0 || static_cast<uint32_t>(st.st_size) < anim.frame_offsets[anim.frame_count]) {
                anim.valid = false;
                continue;
            }
        }
        cJSON_Delete(doc);

        bool any = false;
        for (auto& anim : next) {
            any = any || anim.valid;
        }
        if (!any) {
            unload();
            return false;
        }
        for (uint8_t i = 0; i < kPetStateCount; ++i) {
            _anims[i] = next[i];
        }
        std::snprintf(_name, sizeof(_name), "%s", next_name);
        _loaded = true;
        ++_generation;
        return true;
    }

    const DynamicPetAnim* animation(PetGifState state) const
    {
        if (!_loaded) {
            return nullptr;
        }
        const uint8_t idx = stateIndex(state);
        if (_anims[idx].valid) {
            return &_anims[idx];
        }
        return _anims[1].valid ? &_anims[1] : nullptr;
    }

    uint32_t generation() const
    {
        return _generation;
    }

    const char* name() const
    {
        return _name;
    }

private:
    bool _loaded = false;
    char _name[48] = "";
    uint32_t _generation = 0;
    DynamicPetAnim _anims[kPetStateCount] = {};
};

DynamicPetPack& dynamicPetPack()
{
    static DynamicPetPack pack;
    return pack;
}

volatile uint8_t g_debug_page = 255;
volatile uint8_t g_debug_action = 0;
volatile uint8_t g_debug_current_page = 0;

struct CodexBuddyState {
    uint8_t total = 0;
    uint8_t running = 0;
    uint8_t waiting = 0;
    uint8_t pending = 0;
    uint32_t tokens_today = 0;
    bool completed = false;
    bool connected = false;
    bool subscribed = false;
    bool repair_pairing = false;
    uint32_t passkey = 0;
    uint32_t last_live_ms = 0;
    uint32_t repair_until_ms = 0;
    char msg[64] = "No Codex connected";
    char usage_primary[32] = "";
    char usage_secondary[32] = "";
    char prompt_id[48] = "";
    char prompt_tool[32] = "";
    char prompt_hint[80] = "";
};

void safe_copy(char* dst, size_t dst_size, const char* src)
{
    if (dst_size == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = 0;
        return;
    }
    std::strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

void ascii_sanitize(char* s)
{
    static constexpr char pool[] = "01[]{}<>/*+-_:#@ABCDEFXYZ";
    for (; *s; ++s) {
        if (static_cast<uint8_t>(*s) > 127) {
            *s = pool[esp_random() % (sizeof(pool) - 1)];
        }
    }
}

class CodexBuddyBle {
public:
    static CodexBuddyBle& instance()
    {
        static CodexBuddyBle bridge;
        return bridge;
    }

    void start()
    {
        if (_started) {
            return;
        }

        loadPersistedUsage();
        _device_name = makeDeviceName();

        const esp_err_t rc = nimble_port_init();
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "nimble init failed: %d", rc);
            return;
        }

        ble_hs_cfg.reset_cb = onReset;
        ble_hs_cfg.sync_cb = onSync;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

        ble_svc_gap_init();
        ble_svc_gatt_init();

        int err = ble_gatts_count_cfg(gattSvcs());
        if (err != 0) {
            ESP_LOGE(TAG, "gatt count failed: %d", err);
            return;
        }
        err = ble_gatts_add_svcs(gattSvcs());
        if (err != 0) {
            ESP_LOGE(TAG, "gatt add failed: %d", err);
            return;
        }

        err = ble_svc_gap_device_name_set(_device_name.c_str());
        if (err != 0) {
            ESP_LOGE(TAG, "set name failed: %d", err);
            return;
        }

        ble_store_config_init();
        nimble_port_freertos_init(hostTask);
        _started = true;
        ESP_LOGI(TAG, "starting BLE as %s", _device_name.c_str());
    }

    void loadPersistedUsage()
    {
        Settings settings(CODEX_BUDDY_SETTINGS_NS, false);
        const auto usage_primary = settings.GetString(USAGE_PRIMARY_KEY, "");
        const auto usage_secondary = settings.GetString(USAGE_SECONDARY_KEY, "");
        const int32_t tokens_today = settings.GetInt(TOKENS_TODAY_KEY, 0);

        std::lock_guard<std::mutex> lock(_mutex);
        safe_copy(_state.usage_primary, sizeof(_state.usage_primary), usage_primary.c_str());
        safe_copy(_state.usage_secondary, sizeof(_state.usage_secondary), usage_secondary.c_str());
        _state.tokens_today = std::max<int32_t>(tokens_today, 0);
    }

    CodexBuddyState snapshot()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        CodexBuddyState copy = _state;
        if (copy.repair_pairing && GetHAL().millis() > copy.repair_until_ms) {
            copy.repair_pairing = false;
        }
        if (copy.last_live_ms == 0 || GetHAL().millis() - copy.last_live_ms > 30000) {
            copy.connected = false;
            copy.total = 0;
            copy.running = 0;
            copy.waiting = 0;
            copy.pending = 0;
            copy.completed = false;
            safe_copy(copy.msg, sizeof(copy.msg), "No Codex connected");
        }
        return copy;
    }

    const std::string& deviceName() const
    {
        return _device_name;
    }

    uint8_t petOptionCount()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pet_catalog_count > 0) {
            return _pet_catalog_count;
        }
        return sizeof(kPetOptions) / sizeof(kPetOptions[0]);
    }

    const char* petOptionSelector(uint8_t index)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pet_catalog_count > 0) {
            return _pet_catalog[index % _pet_catalog_count].selector;
        }
        return kPetOptions[index % (sizeof(kPetOptions) / sizeof(kPetOptions[0]))].selector;
    }

    const char* petOptionLabel(uint8_t index)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pet_catalog_count > 0) {
            return _pet_catalog[index % _pet_catalog_count].label;
        }
        return kPetOptions[index % (sizeof(kPetOptions) / sizeof(kPetOptions[0]))].label;
    }

    void clearBondsForRepair()
    {
        ble_store_clear();
        if (_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            advertise();
        }
        markRepairNeeded("Bond cleared; forget device on Mac");
    }

    void sendPermission(const char* prompt_id, const char* decision)
    {
        if (prompt_id == nullptr || prompt_id[0] == 0 || decision == nullptr) {
            return;
        }
        char line[128];
        std::snprintf(line, sizeof(line), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}\n", prompt_id, decision);
        notify(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _state.prompt_id[0] = 0;
            _state.prompt_tool[0] = 0;
            _state.prompt_hint[0] = 0;
        }
    }

    void requestPet(const char* selector, const char* label)
    {
        if (selector == nullptr || selector[0] == 0) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            safe_copy(_pending_pet_selector, sizeof(_pending_pet_selector), selector);
            _pending_pet_notified = false;
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Sync %s from Mac", label != nullptr ? label : selector);
            safe_copy(_state.msg, sizeof(_state.msg), msg);
        }
        sendPendingPetRequest();
    }

    void flushPendingPetRequest()
    {
        sendPendingPetRequest();
    }

    bool handleSerialJson(const char* line, SerialReplyFn reply)
    {
        cJSON* doc = cJSON_Parse(line);
        if (doc == nullptr) {
            return false;
        }
        const cJSON* cmd = cJSON_GetObjectItem(doc, "cmd");
        const bool handled = cJSON_IsString(cmd) && handleCommand(doc, cmd->valuestring, reply);
        cJSON_Delete(doc);
        return handled;
    }

private:
    bool _started = false;
    uint8_t _own_addr_type = 0;
    uint16_t _conn_handle = BLE_HS_CONN_HANDLE_NONE;
    uint16_t _tx_handle = 0;
    uint16_t _rx_handle = 0;
    uint16_t _mtu = 23;
    std::string _device_name = "Codex-StopWatch";
    std::string _line_buffer;
    CodexBuddyState _state;
    std::mutex _mutex;
    FILE* _upload_file = nullptr;
    bool _upload_active = false;
    uint32_t _upload_expected = 0;
    uint32_t _upload_written = 0;
    RuntimePetOption _pet_catalog[16] = {};
    uint8_t _pet_catalog_count = 0;
    char _pending_pet_selector[48] = "";
    bool _pending_pet_notified = false;

    static void hostTask(void*)
    {
        nimble_port_run();
        nimble_port_freertos_deinit();
    }

    static void onReset(int reason)
    {
        ESP_LOGW(TAG, "nimble reset: %d", reason);
    }

    static void onSync()
    {
        auto& self = instance();
        int rc = ble_hs_util_ensure_addr(0);
        if (rc == 0) {
            rc = ble_hs_id_infer_auto(0, &self._own_addr_type);
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "addr init failed: %d", rc);
            return;
        }
        self.advertise();
    }

    static int gapEvent(struct ble_gap_event* event, void*)
    {
        auto& self = instance();
        switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self._conn_handle = event->connect.conn_handle;
                std::lock_guard<std::mutex> lock(self._mutex);
                self._state.connected = true;
                self._state.repair_pairing = false;
            } else {
                self.advertise();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            self._conn_handle = BLE_HS_CONN_HANDLE_NONE;
            {
                std::lock_guard<std::mutex> lock(self._mutex);
                self._state.subscribed = false;
                self._state.connected = false;
                self._state.passkey = 0;
            }
            self.advertise();
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            // Avoid logging from the NimBLE host callback. On this target a
            // reconnect-time encryption failure can enter vprintf through this
            // path and trip the newlib stdout lock assertion.
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == self._tx_handle) {
                std::lock_guard<std::mutex> lock(self._mutex);
                self._state.subscribed = event->subscribe.cur_notify != 0;
                if (self._state.subscribed) {
                    self._state.repair_pairing = false;
                    self._state.passkey = 0;
                }
            }
            if (event->subscribe.attr_handle == self._tx_handle && event->subscribe.cur_notify != 0) {
                self.sendPendingPetRequest();
            }
            return 0;
        case BLE_GAP_EVENT_MTU:
            self._mtu = event->mtu.value;
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            self.advertise();
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                struct ble_sm_io pkey = {};
                pkey.action = BLE_SM_IOACT_DISP;
                pkey.passkey = 100000 + (esp_random() % 900000);
                {
                    std::lock_guard<std::mutex> lock(self._mutex);
                    self._state.passkey = pkey.passkey;
                    self._state.repair_pairing = false;
                }
                ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            }
            return 0;
        default:
            return 0;
        }
    }

    static int accessCb(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void*)
    {
        auto& self = instance();
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == self._rx_handle) {
            const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            char buf[256];
            uint16_t copied = 0;
            const int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, std::min<uint16_t>(len, sizeof(buf)), &copied);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            self.feedBytes(reinterpret_cast<uint8_t*>(buf), copied);
            return 0;
        }

        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == self._tx_handle) {
            return self.appendReadResponse(ctxt->om);
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    static const ble_gatt_svc_def* gattSvcs()
    {
        static ble_gatt_chr_def chrs[] = {
            {
                .uuid = &NUS_TX_UUID.u,
                .access_cb = accessCb,
                .arg = nullptr,
                .descriptors = nullptr,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
                .min_key_size = 0,
                .val_handle = &instance()._tx_handle,
                .cpfd = nullptr,
            },
            {
                .uuid = &NUS_RX_UUID.u,
                .access_cb = accessCb,
                .arg = nullptr,
                .descriptors = nullptr,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
                .min_key_size = 0,
                .val_handle = &instance()._rx_handle,
                .cpfd = nullptr,
            },
            {
                .uuid = nullptr,
                .access_cb = nullptr,
                .arg = nullptr,
                .descriptors = nullptr,
                .flags = 0,
                .min_key_size = 0,
                .val_handle = nullptr,
                .cpfd = nullptr,
            },
        };

        static ble_gatt_svc_def svcs[] = {
            {
                .type = BLE_GATT_SVC_TYPE_PRIMARY,
                .uuid = &NUS_SERVICE_UUID.u,
                .includes = nullptr,
                .characteristics = chrs,
            },
            {
                .type = 0,
                .uuid = nullptr,
                .includes = nullptr,
                .characteristics = nullptr,
            },
        };
        return svcs;
    }

    std::string makeDeviceName()
    {
        auto mac = GetHAL().getFactoryMac();
        char name[20];
        std::snprintf(name, sizeof(name), "Codex-%02X%02X", mac[4], mac[5]);
        return name;
    }

    void advertise()
    {
        ble_gap_adv_stop();

        ble_hs_adv_fields fields = {};
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.name = reinterpret_cast<uint8_t*>(const_cast<char*>(_device_name.c_str()));
        fields.name_len = _device_name.size();
        fields.name_is_complete = 1;
        int rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "adv fields failed: %d", rc);
            return;
        }

        ble_hs_adv_fields rsp = {};
        rsp.uuids128 = const_cast<ble_uuid128_t*>(&NUS_SERVICE_UUID);
        rsp.num_uuids128 = 1;
        rsp.uuids128_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&rsp);
        if (rc != 0) {
            ESP_LOGW(TAG, "scan response failed: %d", rc);
        }

        ble_gap_adv_params params = {};
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        params.itvl_min = 0x20;
        params.itvl_max = 0x40;
        rc = ble_gap_adv_start(_own_addr_type, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
        if (rc != 0) {
            ESP_LOGE(TAG, "advertise failed: %d", rc);
        } else {
            ESP_LOGI(TAG, "advertising as %s", _device_name.c_str());
        }
    }

    void persistUsage(const CodexBuddyState& state)
    {
        Settings settings(CODEX_BUDDY_SETTINGS_NS, true);
        settings.SetString(USAGE_PRIMARY_KEY, state.usage_primary);
        settings.SetString(USAGE_SECONDARY_KEY, state.usage_secondary);
        settings.SetInt(TOKENS_TODAY_KEY, static_cast<int32_t>(state.tokens_today));
    }

    void sendPendingPetRequest()
    {
        char selector[48];
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_pending_pet_selector[0] == 0 || !_state.subscribed) {
                return;
            }
            if (_pending_pet_notified) {
                return;
            }
            safe_copy(selector, sizeof(selector), _pending_pet_selector);
            _pending_pet_notified = true;
            safe_copy(_state.msg, sizeof(_state.msg), "Pet sync requested");
        }

        char line[128];
        std::snprintf(line, sizeof(line), "{\"cmd\":\"pet_select\",\"selector\":\"%s\"}\n", selector);
        notify(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    }

    int appendReadResponse(os_mbuf* om)
    {
        char line[128];
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_pending_pet_selector[0] != 0) {
                std::snprintf(line, sizeof(line), "{\"cmd\":\"pet_select\",\"selector\":\"%s\"}\n", _pending_pet_selector);
                _pending_pet_selector[0] = 0;
                _pending_pet_notified = false;
                safe_copy(_state.msg, sizeof(_state.msg), "Pet sync requested");
            } else {
                std::snprintf(line, sizeof(line), "Codex Buddy ready\n");
            }
        }
        return os_mbuf_append(om, line, std::strlen(line)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    void feedBytes(const uint8_t* data, size_t len)
    {
        for (size_t i = 0; i < len; ++i) {
            const char c = static_cast<char>(data[i]);
            if (c == '\n' || c == '\r') {
                if (!_line_buffer.empty()) {
                    parseLine(_line_buffer.c_str());
                    _line_buffer.clear();
                }
            } else if (_line_buffer.size() < 1023) {
                _line_buffer.push_back(c);
            }
        }
    }

    void parseLine(const char* line)
    {
        cJSON* doc = cJSON_Parse(line);
        if (doc == nullptr) {
            return;
        }

        const cJSON* cmd = cJSON_GetObjectItem(doc, "cmd");
        if (cJSON_IsString(cmd) && handleCommand(doc, cmd->valuestring)) {
            cJSON_Delete(doc);
            return;
        }

        CodexBuddyState next;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            next = _state;
        }

        next.connected = true;
        next.repair_pairing = false;
        next.last_live_ms = GetHAL().millis();

        const cJSON* total = cJSON_GetObjectItem(doc, "total");
        const cJSON* running = cJSON_GetObjectItem(doc, "running");
        const cJSON* waiting = cJSON_GetObjectItem(doc, "waiting");
        const cJSON* pending = cJSON_GetObjectItem(doc, "pending");
        const cJSON* completed = cJSON_GetObjectItem(doc, "completed");
        const cJSON* tokens = cJSON_GetObjectItem(doc, "tokens_today");
        bool should_persist_usage = false;
        if (cJSON_IsNumber(total)) next.total = total->valueint;
        if (cJSON_IsNumber(running)) next.running = running->valueint;
        if (cJSON_IsNumber(waiting)) next.waiting = waiting->valueint;
        if (cJSON_IsNumber(pending)) next.pending = pending->valueint;
        if (cJSON_IsBool(completed)) next.completed = cJSON_IsTrue(completed);
        if (cJSON_IsNumber(tokens)) {
            next.tokens_today = tokens->valuedouble;
            should_persist_usage = true;
        }

        const cJSON* msg = cJSON_GetObjectItem(doc, "msg");
        if (cJSON_IsString(msg)) {
            safe_copy(next.msg, sizeof(next.msg), msg->valuestring);
            ascii_sanitize(next.msg);
        }
        const cJSON* usage_primary = cJSON_GetObjectItem(doc, "usage_primary");
        if (cJSON_IsString(usage_primary)) {
            safe_copy(next.usage_primary, sizeof(next.usage_primary), usage_primary->valuestring);
            should_persist_usage = true;
        }
        const cJSON* usage_secondary = cJSON_GetObjectItem(doc, "usage_secondary");
        if (cJSON_IsString(usage_secondary)) {
            safe_copy(next.usage_secondary, sizeof(next.usage_secondary), usage_secondary->valuestring);
            should_persist_usage = true;
        }

        const cJSON* prompt = cJSON_GetObjectItem(doc, "prompt");
        if (cJSON_IsObject(prompt)) {
            const cJSON* id = cJSON_GetObjectItem(prompt, "id");
            const cJSON* tool = cJSON_GetObjectItem(prompt, "tool");
            const cJSON* hint = cJSON_GetObjectItem(prompt, "hint");
            safe_copy(next.prompt_id, sizeof(next.prompt_id), cJSON_IsString(id) ? id->valuestring : "");
            safe_copy(next.prompt_tool, sizeof(next.prompt_tool), cJSON_IsString(tool) ? tool->valuestring : "");
            safe_copy(next.prompt_hint, sizeof(next.prompt_hint), cJSON_IsString(hint) ? hint->valuestring : "");
            ascii_sanitize(next.prompt_tool);
            ascii_sanitize(next.prompt_hint);

            char ack[128];
            std::snprintf(ack, sizeof(ack), "{\"ack\":\"prompt\",\"ok\":true,\"id\":\"%s\"}\n", next.prompt_id);
            notify(reinterpret_cast<const uint8_t*>(ack), std::strlen(ack));
        } else if (total != nullptr || running != nullptr || waiting != nullptr || pending != nullptr || msg != nullptr) {
            next.prompt_id[0] = 0;
            next.prompt_tool[0] = 0;
            next.prompt_hint[0] = 0;
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _state = next;
        }
        if (should_persist_usage) {
            persistUsage(next);
        }
        cJSON_Delete(doc);
    }

    bool handleCommand(cJSON* doc, const char* cmd, SerialReplyFn reply = nullptr)
    {
        if (std::strcmp(cmd, "pet_catalog") == 0) {
            RuntimePetOption next[16] = {};
            uint8_t count = 0;
            const cJSON* pets = cJSON_GetObjectItem(doc, "pets");
            if (cJSON_IsArray(pets)) {
                const int items = std::min<int>(cJSON_GetArraySize(pets), sizeof(next) / sizeof(next[0]));
                for (int i = 0; i < items; ++i) {
                    const cJSON* item = cJSON_GetArrayItem(pets, i);
                    const cJSON* selector = cJSON_GetObjectItem(item, "selector");
                    const cJSON* label = cJSON_GetObjectItem(item, "label");
                    if (!cJSON_IsString(selector) || !safePetSelector(selector->valuestring)) {
                        continue;
                    }
                    const char* label_value = cJSON_IsString(label) && safePetLabel(label->valuestring)
                                                  ? label->valuestring
                                                  : selector->valuestring;
                    safe_copy(next[count].selector, sizeof(next[count].selector), selector->valuestring);
                    safe_copy(next[count].label, sizeof(next[count].label), label_value);
                    ascii_sanitize(next[count].label);
                    ++count;
                }
            }
            if (count > 0) {
                std::lock_guard<std::mutex> lock(_mutex);
                for (uint8_t i = 0; i < count; ++i) {
                    _pet_catalog[i] = next[i];
                }
                _pet_catalog_count = count;
            }
            ack("pet_catalog", count > 0, count, reply);
            if (reply == nullptr && count > 0) {
                sendPendingPetRequest();
            }
            return true;
        }

        if (std::strcmp(cmd, "usage") == 0) {
            CodexBuddyState next;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                next = _state;
            }

            next.connected = true;
            next.repair_pairing = false;
            next.last_live_ms = GetHAL().millis();

            const cJSON* tokens = cJSON_GetObjectItem(doc, "tokens_today");
            bool should_persist_usage = false;
            if (cJSON_IsNumber(tokens)) {
                next.tokens_today = tokens->valuedouble;
                should_persist_usage = true;
            }
            const cJSON* primary = cJSON_GetObjectItem(doc, "primary");
            if (!cJSON_IsString(primary)) {
                primary = cJSON_GetObjectItem(doc, "usage_primary");
            }
            if (cJSON_IsString(primary)) {
                safe_copy(next.usage_primary, sizeof(next.usage_primary), primary->valuestring);
                should_persist_usage = true;
            }
            const cJSON* secondary = cJSON_GetObjectItem(doc, "secondary");
            if (!cJSON_IsString(secondary)) {
                secondary = cJSON_GetObjectItem(doc, "usage_secondary");
            }
            if (cJSON_IsString(secondary)) {
                safe_copy(next.usage_secondary, sizeof(next.usage_secondary), secondary->valuestring);
                should_persist_usage = true;
            }

            {
                std::lock_guard<std::mutex> lock(_mutex);
                _state = next;
            }
            if (should_persist_usage) {
                persistUsage(next);
            }
            return true;
        }

        if (std::strcmp(cmd, "status") == 0) {
            ack("status", true, 0, reply);
            return true;
        }

        if (std::strcmp(cmd, "char_begin") == 0) {
            closeUploadFile();
            dynamicPetPack().unload();
            wipeDynamicPetDir();
            _upload_active = true;
            _upload_expected = 0;
            _upload_written = 0;
            ack("char_begin", true, 0, reply);
            return true;
        }

        if (std::strcmp(cmd, "file") == 0) {
            closeUploadFile();
            if (!_upload_active) {
                ack("file", false, 0, reply);
                return true;
            }
            const cJSON* path = cJSON_GetObjectItem(doc, "path");
            const cJSON* size = cJSON_GetObjectItem(doc, "size");
            if (!cJSON_IsString(path) || !safeUploadName(path->valuestring)) {
                ack("file", false, 0, reply);
                return true;
            }
            _upload_expected = cJSON_IsNumber(size) ? std::max<int>(size->valueint, 0) : 0;
            _upload_written = 0;
            char full_path[320];
            std::snprintf(full_path, sizeof(full_path), "%s/%s", kDynamicPetDir, path->valuestring);
            _upload_file = std::fopen(full_path, "wb");
            ack("file", _upload_file != nullptr, 0, reply);
            return true;
        }

        if (std::strcmp(cmd, "chunk") == 0) {
            const cJSON* data = cJSON_GetObjectItem(doc, "d");
            if (!_upload_active || _upload_file == nullptr || !cJSON_IsString(data)) {
                ack("chunk", false, _upload_written, reply);
                return true;
            }
            uint8_t decoded[kUploadChunkMaxBytes];
            size_t decoded_len = 0;
            const int rc = mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_len,
                                                 reinterpret_cast<const uint8_t*>(data->valuestring),
                                                 std::strlen(data->valuestring));
            if (rc != 0) {
                ack("chunk", false, _upload_written, reply);
                return true;
            }
            const size_t written = std::fwrite(decoded, 1, decoded_len, _upload_file);
            _upload_written += written;
            ack("chunk", written == decoded_len, _upload_written, reply);
            return true;
        }

        if (std::strcmp(cmd, "file_end") == 0) {
            const bool ok = _upload_file != nullptr && (_upload_expected == 0 || _upload_written == _upload_expected);
            closeUploadFile();
            ack("file_end", ok, _upload_written, reply);
            return true;
        }

        if (std::strcmp(cmd, "char_end") == 0) {
            closeUploadFile();
            _upload_active = false;
            const bool ok = dynamicPetPack().load();
            ack("char_end", ok, 0, reply);
            if (ok) {
                std::lock_guard<std::mutex> lock(_mutex);
                safe_copy(_state.msg, sizeof(_state.msg), "Pet updated");
            }
            return true;
        }

        if (std::strcmp(cmd, "owner") == 0 || std::strcmp(cmd, "name") == 0 || std::strcmp(cmd, "time") == 0) {
            return true;
        }

        return false;
    }

    void closeUploadFile()
    {
        if (_upload_file != nullptr) {
            std::fclose(_upload_file);
            _upload_file = nullptr;
        }
    }

    void ack(const char* what, bool ok, uint32_t n = 0, SerialReplyFn reply = nullptr)
    {
        char line[96];
        std::snprintf(line, sizeof(line), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n", what, ok ? "true" : "false",
                      static_cast<unsigned long>(n));
        if (reply != nullptr) {
            reply(line);
            return;
        }
        notify(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    }

    void notify(const uint8_t* data, size_t len)
    {
        if (_conn_handle == BLE_HS_CONN_HANDLE_NONE || _tx_handle == 0) {
            return;
        }
        const size_t max_chunk = std::min<size_t>(_mtu > 3 ? _mtu - 3 : 20, 180);
        for (size_t sent = 0; sent < len;) {
            const size_t chunk = std::min(max_chunk, len - sent);
            os_mbuf* om = os_msys_get_pkthdr(chunk, 0);
            if (om == nullptr) {
                return;
            }
            if (os_mbuf_append(om, data + sent, chunk) != 0) {
                os_mbuf_free_chain(om);
                return;
            }
            ble_gatts_notify_custom(_conn_handle, _tx_handle, om);
            sent += chunk;
        }
    }

    void markRepairNeeded(const char* msg)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _state.repair_pairing = true;
        _state.repair_until_ms = GetHAL().millis() + 60000;
        _state.passkey = 0;
        safe_copy(_state.msg, sizeof(_state.msg), msg);
    }
};

class PetGifRenderer {
public:
    void playTemporary(PetGifState state, uint32_t duration_ms)
    {
        _forced_state = state;
        _forced_until_ms = GetHAL().millis() + duration_ms;
        _anim = nullptr;
        _dyn_anim = nullptr;
        _frame = 0;
        _next_frame_ms = 0;
    }

    void playRandomInteraction()
    {
        static constexpr PetGifState states[] = {
            PetGifState::Heart,
            PetGifState::Wave,
            PetGifState::Sparkle,
            PetGifState::Celebrate,
            PetGifState::Attention,
            PetGifState::Dizzy,
        };
        const uint32_t now = GetHAL().millis();
        _ambient_seed = _ambient_seed * 1664525u + 1013904223u + now;
        playTemporary(states[(_ambient_seed >> 16) % (sizeof(states) / sizeof(states[0]))], 5200);
        _next_ambient_state_ms = now + 16000 + ((_ambient_seed >> 8) % 12000);
    }

    void render(const CodexBuddyState& state, uint8_t page)
    {
        const bool fullscreen = page == 1;
        const bool visible = (page == 0 || fullscreen) && state.passkey == 0 && state.prompt_id[0] == 0 &&
                             !state.repair_pairing;
        if (!visible) {
            _visible = false;
            _anim = nullptr;
            _frame = 0;
            _next_frame_ms = 0;
            return;
        }

        const PetGifState next_state = chooseRenderState(state, fullscreen);
        const DynamicPetAnim* dyn = dynamicPetPack().animation(next_state);
        const uint32_t dyn_generation = dynamicPetPack().generation();
        if (dyn != nullptr) {
            if (_dyn_anim != dyn || _dyn_generation != dyn_generation || _fullscreen != fullscreen) {
                _dyn_anim = dyn;
                _dyn_generation = dyn_generation;
                _anim = nullptr;
                _frame = 0;
                _next_frame_ms = 0;
                _fullscreen = fullscreen;
            }

            const uint32_t now = GetHAL().millis();
            if (_visible && now < _next_frame_ms) {
                return;
            }

            if (drawDynamicFrame(*dyn, fullscreen)) {
                _visible = true;
                const auto delay = dyn->delays_ms[_frame];
                _frame = (_frame + 1) % dyn->frame_count;
                _next_frame_ms = now + scaledDelay(delay);
                return;
            }

            _dyn_anim = nullptr;
            _anim = nullptr;
            _frame = 0;
            _next_frame_ms = 0;
        }

        const GifAnimation& anim = codex_buddy_assets::seedyAnimation(next_state);
        if (_anim != &anim || _fullscreen != fullscreen) {
            _anim = &anim;
            _dyn_anim = nullptr;
            _frame = 0;
            _next_frame_ms = 0;
            _fullscreen = fullscreen;
        }

        const uint32_t now = GetHAL().millis();
        if (_visible && now < _next_frame_ms) {
            return;
        }

        drawFrame(anim, fullscreen);
        _visible = true;
        const auto delay = anim.frames[_frame].delay_ms;
        _frame = (_frame + 1) % anim.frame_count;
        _next_frame_ms = now + scaledDelay(delay);
    }

    void renderPreview()
    {
        static constexpr int kPreviewW = 132;
        static constexpr int kPreviewH = 142;
        static constexpr int kPreviewX = (466 - kPreviewW) / 2;
        static constexpr int kPreviewY = 100;

        const DynamicPetAnim* dyn = dynamicPetPack().animation(PetGifState::Idle);
        if (dyn != nullptr) {
            const uint8_t old_frame = _frame;
            _frame = 0;
            const bool ok = loadDynamicFrame(*dyn);
            _frame = old_frame;
            if (ok) {
                drawScaledRgb565(_frame_buffer.data(), dyn->width, dyn->height, kPreviewX, kPreviewY, kPreviewW, kPreviewH);
                return;
            }
        }

        const GifAnimation& anim = codex_buddy_assets::seedyAnimation(PetGifState::Idle);
        if (anim.frame_count > 0) {
            drawScaledRgb565(anim.frames[0].pixels, anim.width, anim.height, kPreviewX, kPreviewY, kPreviewW, kPreviewH);
        }
    }

    void renderFortuneMascot()
    {
        static constexpr int kMascotW = 154;
        static constexpr int kMascotH = 168;
        static constexpr int kMascotX = (466 - kMascotW) / 2;
        static constexpr int kMascotY = 74;
        static constexpr PetGifState kFortuneState = PetGifState::Sparkle;

        const uint32_t generation = dynamicPetPack().generation();
        if (_fortune_asset_drawn && _fortune_asset_generation == generation) {
            return;
        }
        _fortune_asset_drawn = true;
        _fortune_asset_generation = generation;

        const DynamicPetAnim* dyn = dynamicPetPack().animation(kFortuneState);
        if (dyn == nullptr) {
            dyn = dynamicPetPack().animation(PetGifState::Idle);
        }
        if (dyn != nullptr) {
            const uint8_t old_frame = _frame;
            _frame = 0;
            const bool ok = loadDynamicFrame(*dyn);
            _frame = old_frame;
            if (ok) {
                drawScaledRgb565(_frame_buffer.data(), dyn->width, dyn->height, kMascotX, kMascotY, kMascotW, kMascotH, false);
                return;
            }
        }

        const GifAnimation& anim = codex_buddy_assets::seedyAnimation(kFortuneState);
        if (anim.frame_count > 0) {
            drawScaledRgb565(anim.frames[0].pixels, anim.width, anim.height, kMascotX, kMascotY, kMascotW, kMascotH, false);
        }
    }

    void invalidateFortuneMascot()
    {
        _fortune_asset_drawn = false;
    }

private:
    static constexpr int kAssetW = 96;
    static constexpr int kAssetH = 104;
    static constexpr int kRenderW = 168;
    static constexpr int kRenderH = 182;
    static constexpr int kRenderY = 34;
    static constexpr int kFullRenderW = 310;
    static constexpr int kFullRenderH = 336;
    static constexpr int kFullRenderY = 60;

    const GifAnimation* _anim = nullptr;
    const DynamicPetAnim* _dyn_anim = nullptr;
    uint32_t _dyn_generation = 0;
    uint8_t _frame = 0;
    uint32_t _next_frame_ms = 0;
    uint32_t _next_ambient_state_ms = 0;
    uint32_t _ambient_effect_until_ms = 0;
    uint32_t _ambient_seed = 0x5A17C0DE;
    uint32_t _forced_until_ms = 0;
    uint32_t _fortune_asset_generation = 0;
    PetGifState _ambient_state = PetGifState::Idle;
    PetGifState _forced_state = PetGifState::Idle;
    std::vector<uint16_t> _frame_buffer;
    std::vector<uint8_t> _encoded_buffer;
    bool _visible = false;
    bool _fullscreen = false;
    bool _fortune_asset_drawn = false;

    static PetGifState chooseState(const CodexBuddyState& state)
    {
        if (state.completed) {
            return PetGifState::Celebrate;
        }
        if (state.pending > 0) {
            return PetGifState::Attention;
        }
        if (state.running > 0) {
            return PetGifState::Busy;
        }
        if (state.waiting > 0) {
            return PetGifState::Heart;
        }
        if (!state.connected && state.total == 0) {
            return PetGifState::Sleep;
        }
        return PetGifState::Idle;
    }

    PetGifState chooseRenderState(const CodexBuddyState& state, bool fullscreen)
    {
        const uint32_t now = GetHAL().millis();
        if (_forced_until_ms != 0 && now < _forced_until_ms) {
            return _forced_state;
        }
        _forced_until_ms = 0;
        return fullscreen ? chooseAmbientState() : chooseState(state);
    }

    PetGifState chooseAmbientState()
    {
        const uint32_t now = GetHAL().millis();
        if (_ambient_effect_until_ms != 0 && now < _ambient_effect_until_ms) {
            return _ambient_state;
        }
        _ambient_effect_until_ms = 0;
        _ambient_state = PetGifState::Idle;

        if (_next_ambient_state_ms == 0) {
            _ambient_seed = _ambient_seed * 1664525u + 1013904223u + now;
            _next_ambient_state_ms = now + 9000 + ((_ambient_seed >> 8) % 9000);
            return _ambient_state;
        }

        if (now >= _next_ambient_state_ms) {
            static constexpr PetGifState states[] = {
                PetGifState::Heart,
                PetGifState::Attention,
                PetGifState::Celebrate,
                PetGifState::Busy,
                PetGifState::Wave,
                PetGifState::Sparkle,
            };
            _ambient_seed = _ambient_seed * 1664525u + 1013904223u + now;
            _ambient_state = states[(_ambient_seed >> 16) % (sizeof(states) / sizeof(states[0]))];
            _ambient_effect_until_ms = now + 2600 + ((_ambient_seed >> 4) % 1800);
            _next_ambient_state_ms = _ambient_effect_until_ms + 12000 + ((_ambient_seed >> 8) % 12000);
        }
        return _ambient_state;
    }

    static uint32_t scaledDelay(uint32_t delay)
    {
        const uint32_t base = delay > 0 ? delay : 100;
        if (base < 500) {
            return std::max<uint32_t>(600, base * 4);
        }
        return base;
    }

    void drawFrame(const GifAnimation& anim, bool fullscreen)
    {
        if (anim.frame_count == 0 || anim.width != kAssetW || anim.height != kAssetH) {
            return;
        }

        const uint16_t* src = anim.frames[_frame].pixels;
        auto& gfx = GetHAL().getDisplay();
        const int render_w = fullscreen ? kFullRenderW : kRenderW;
        const int render_h = fullscreen ? kFullRenderH : kRenderH;
        const int render_y = fullscreen ? kFullRenderY : kRenderY;
        const int render_x = (gfx.width() - render_w) / 2;
        std::array<lgfx::rgb565_t, kFullRenderW> line;

        gfx.startWrite();
        if (fullscreen) {
            gfx.fillRect(0, 0, gfx.width(), gfx.height(), 0);
        }
        for (int dy = 0; dy < render_h; ++dy) {
            const int sy = std::min((dy * kAssetH) / render_h, kAssetH - 1);
            const uint16_t* src_row = src + (sy * kAssetW);
            for (int dx = 0; dx < render_w; ++dx) {
                const int sx = std::min((dx * kAssetW) / render_w, kAssetW - 1);
                line[dx] = lgfx::rgb565_t(src_row[sx]);
            }
            gfx.pushImage(render_x, render_y + dy, render_w, 1, line.data());
        }
        gfx.endWrite();
    }

    bool drawDynamicFrame(const DynamicPetAnim& anim, bool fullscreen)
    {
        if (!anim.valid || anim.frame_count == 0 || anim.width == 0 || anim.height == 0) {
            return false;
        }
        if (!loadDynamicFrame(anim)) {
            return false;
        }

        auto& gfx = GetHAL().getDisplay();
        const int render_w = fullscreen ? kFullRenderW : kRenderW;
        const int render_h = fullscreen ? kFullRenderH : kRenderH;
        const int render_y = fullscreen ? kFullRenderY : kRenderY;
        const int render_x = (gfx.width() - render_w) / 2;
        std::array<lgfx::rgb565_t, kFullRenderW> line;

        gfx.startWrite();
        if (fullscreen) {
            gfx.fillRect(0, 0, gfx.width(), gfx.height(), 0);
        }
        for (int dy = 0; dy < render_h; ++dy) {
            const int sy = std::min((dy * anim.height) / render_h, static_cast<int>(anim.height) - 1);
            const uint16_t* src_row = _frame_buffer.data() + (static_cast<size_t>(sy) * anim.width);
            for (int dx = 0; dx < render_w; ++dx) {
                const int sx = std::min((dx * anim.width) / render_w, static_cast<int>(anim.width) - 1);
                line[dx] = lgfx::rgb565_t(src_row[sx]);
            }
            gfx.pushImage(render_x, render_y + dy, render_w, 1, line.data());
        }
        gfx.endWrite();
        return true;
    }

    static void drawScaledRgb565(const uint16_t* src, int src_w, int src_h, int x, int y, int w, int h,
                                 bool clear_background = true)
    {
        if (src == nullptr || src_w <= 0 || src_h <= 0 || w <= 0 || h <= 0) {
            return;
        }
        auto& gfx = GetHAL().getDisplay();
        std::array<lgfx::rgb565_t, kFullRenderW> line;
        const int clipped_w = std::min<int>(w, static_cast<int>(line.size()));
        gfx.startWrite();
        if (clear_background) {
            gfx.fillRect(x - 8, y - 8, w + 16, h + 16, 0);
        }
        for (int dy = 0; dy < h; ++dy) {
            const int sy = std::min((dy * src_h) / h, src_h - 1);
            const uint16_t* src_row = src + (sy * src_w);
            for (int dx = 0; dx < clipped_w; ++dx) {
                const int sx = std::min((dx * src_w) / clipped_w, src_w - 1);
                line[dx] = lgfx::rgb565_t(src_row[sx]);
            }
            gfx.pushImage(x, y + dy, clipped_w, 1, line.data());
        }
        gfx.endWrite();
    }

    bool loadDynamicFrame(const DynamicPetAnim& anim)
    {
        const size_t pixel_count = static_cast<size_t>(anim.width) * anim.height;
        _frame_buffer.resize(pixel_count);

        FILE* fp = std::fopen(anim.path, "rb");
        if (fp == nullptr) {
            return false;
        }

        bool ok = false;
        if (anim.rle) {
            const uint32_t offset = anim.frame_offsets[_frame];
            const uint32_t next_offset = anim.frame_offsets[_frame + 1];
            const uint32_t encoded_size = next_offset > offset ? next_offset - offset : 0;
            _encoded_buffer.resize(encoded_size);
            if (encoded_size > 0 && std::fseek(fp, offset, SEEK_SET) == 0 &&
                std::fread(_encoded_buffer.data(), 1, encoded_size, fp) == encoded_size) {
                ok = decodeRgb565Rle(_encoded_buffer.data(), encoded_size, _frame_buffer.data(), pixel_count);
            }
        } else {
            const uint32_t offset = anim.frame_offsets[_frame];
            if (std::fseek(fp, offset, SEEK_SET) == 0) {
                ok = std::fread(_frame_buffer.data(), sizeof(uint16_t), pixel_count, fp) == pixel_count;
            }
        }
        std::fclose(fp);
        return ok;
    }

    static bool decodeRgb565Rle(const uint8_t* src, size_t len, uint16_t* dst, size_t pixel_count)
    {
        size_t si = 0;
        size_t di = 0;
        while (si < len && di < pixel_count) {
            const uint8_t ctrl = src[si++];
            const size_t count = (ctrl & 0x7F) + 1;
            if ((ctrl & 0x80) != 0) {
                if (si + 2 > len || di + count > pixel_count) {
                    return false;
                }
                const uint16_t value = static_cast<uint16_t>(src[si] | (src[si + 1] << 8));
                si += 2;
                for (size_t i = 0; i < count; ++i) {
                    dst[di++] = value;
                }
            } else {
                const size_t bytes = count * sizeof(uint16_t);
                if (si + bytes > len || di + count > pixel_count) {
                    return false;
                }
                for (size_t i = 0; i < count; ++i) {
                    dst[di++] = static_cast<uint16_t>(src[si] | (src[si + 1] << 8));
                    si += 2;
                }
            }
        }
        return si == len && di == pixel_count;
    }
};

class CodexBuddyView {
public:
    void init(lv_obj_t* parent)
    {
        _root = lv_obj_create(parent);
        lv_obj_set_size(_root, 466, 466);
        lv_obj_center(_root);
        lv_obj_set_style_radius(_root, 0, 0);
        lv_obj_set_style_border_width(_root, 0, 0);
        lv_obj_set_style_pad_all(_root, 0, 0);
        lv_obj_set_style_bg_color(_root, lv_color_hex(0x000000), 0);
        lv_obj_remove_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

        _usage_title = makeLabel(_root, "Usage", &lv_font_montserrat_20, 0xAEB8D8, LV_ALIGN_TOP_LEFT, 92, 238);
        createUsageRow(_usage_primary, 92, 284, 0xC95B69, 0x211C3F);
        createUsageRow(_usage_secondary, 92, 322, 0x5F7FCA, 0x151B40);

        _dot_active = makeDot(_root, 0x54C982, 164, 373);
        _dot_pending = makeDot(_root, 0xD7A545, 164, 406);
        _active_label = makeLabel(_root, "Active", &lv_font_montserrat_20, 0xD9E1FF, LV_ALIGN_TOP_LEFT, 184, 366);
        _pending_label = makeLabel(_root, "Pending", &lv_font_montserrat_20, 0xD9E1FF, LV_ALIGN_TOP_LEFT, 184, 399);
        _active_count_label = makeLabel(_root, "", &lv_font_montserrat_20, 0xD9E1FF, LV_ALIGN_TOP_LEFT, 286, 366);
        _pending_count_label = makeLabel(_root, "", &lv_font_montserrat_20, 0xD9E1FF, LV_ALIGN_TOP_LEFT, 286, 399);
        lv_obj_set_width(_active_count_label, 32);
        lv_obj_set_width(_pending_count_label, 32);
        lv_obj_set_style_text_align(_active_count_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_align(_pending_count_label, LV_TEXT_ALIGN_RIGHT, 0);

        _detail_title = makeLabel(_root, "Codex Buddy", &lv_font_montserrat_28, 0xE8EEFF, LV_ALIGN_TOP_MID, 0, 56);
        _detail_status = makeLabel(_root, "", &lv_font_montserrat_18, 0x7DFFB6, LV_ALIGN_TOP_MID, 0, 102);
        _detail_body = makeLabel(_root, "", &lv_font_montserrat_22, 0xD9E1FF, LV_ALIGN_TOP_LEFT, 86, 150);
        lv_obj_set_width(_detail_body, 304);
        lv_label_set_long_mode(_detail_body, LV_LABEL_LONG_WRAP);
        _detail_hint = makeLabel(_root, "A clear BLE   B page   A+B home", &lv_font_montserrat_16, 0x68738B, LV_ALIGN_BOTTOM_MID, 0, -32);

        _prompt_title = makeLabel(_root, "Approval", &lv_font_montserrat_28, 0xFFE56C, LV_ALIGN_TOP_MID, 0, 58);
        _prompt_tool = makeLabel(_root, "", &lv_font_montserrat_28, 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 112);
        _prompt_hint = makeLabel(_root, "", &lv_font_montserrat_22, 0xC9D3EA, LV_ALIGN_TOP_MID, 0, 170);
        lv_obj_set_width(_prompt_hint, 320);
        lv_label_set_long_mode(_prompt_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(_prompt_hint, LV_TEXT_ALIGN_CENTER, 0);
        _prompt_footer = makeLabel(_root, "A approve   B deny", &lv_font_montserrat_16, 0x68738B, LV_ALIGN_BOTTOM_MID, 0, -28);

        _approve = makeButton(_root, "APPROVE", 0x0FBF6A, LV_ALIGN_BOTTOM_MID, -92, -82);
        _deny = makeButton(_root, "DENY", 0xD84E4E, LV_ALIGN_BOTTOM_MID, 92, -82);
        lv_obj_add_event_cb(_approve, onApproveClicked, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(_deny, onDenyClicked, LV_EVENT_CLICKED, this);

        _fortune_header = makeLabel(_root, "星星答案機", &lv_font_source_han_sans_sc_16_cjk, 0x8D99BF, LV_ALIGN_TOP_MID, 0, 43);
        _fortune_badge = makeBadge(_root, "", LV_ALIGN_TOP_MID, 0, 82);
        _fortune_title = makeLabel(_root, "", &lv_font_source_han_sans_sc_16_cjk, 0xF1E7D0, LV_ALIGN_TOP_MID, 0, 122);
        lv_obj_set_style_text_letter_space(_fortune_title, 1, 0);

        _fortune_signal_box = lv_obj_create(_root);
        lv_obj_set_size(_fortune_signal_box, 382, 124);
        lv_obj_align(_fortune_signal_box, LV_ALIGN_TOP_MID, 0, 272);
        lv_obj_set_style_radius(_fortune_signal_box, 14, 0);
        lv_obj_set_style_border_width(_fortune_signal_box, 1, 0);
        lv_obj_set_style_border_color(_fortune_signal_box, lv_color_hex(0x3A4358), 0);
        lv_obj_set_style_bg_color(_fortune_signal_box, lv_color_hex(0x080D18), 0);
        lv_obj_set_style_bg_opa(_fortune_signal_box, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(_fortune_signal_box, 12, 0);
        lv_obj_remove_flag(_fortune_signal_box, LV_OBJ_FLAG_SCROLLABLE);
        _fortune_signal = makeLabel(_fortune_signal_box, "", &lv_font_source_han_sans_sc_16_cjk, 0xE7ECF8, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_width(_fortune_signal, 300);
        lv_obj_set_style_text_align(_fortune_signal, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(_fortune_signal, 6, 0);
        lv_obj_set_style_transform_scale(_fortune_signal, 320, 0);
        lv_obj_set_style_transform_pivot_x(_fortune_signal, 150, 0);
        lv_obj_set_style_transform_pivot_y(_fortune_signal, 30, 0);
        lv_label_set_long_mode(_fortune_signal, LV_LABEL_LONG_WRAP);

        _fortune_read_title = makeLabel(_root, "小提示", &lv_font_source_han_sans_sc_16_cjk, 0x9AA7C2, LV_ALIGN_TOP_LEFT, 88, 318);
        _fortune_read = makeLabel(_root, "", &lv_font_source_han_sans_sc_14_cjk, 0xDDE5F3, LV_ALIGN_TOP_LEFT, 88, 306);
        lv_obj_set_width(_fortune_read, 300);
        lv_label_set_long_mode(_fortune_read, LV_LABEL_LONG_WRAP);
        _fortune_plan_title = makeLabel(_root, "今天可以", &lv_font_source_han_sans_sc_16_cjk, 0x9AA7C2, LV_ALIGN_TOP_LEFT, 88, 356);
        _fortune_plan = makeLabel(_root, "", &lv_font_source_han_sans_sc_14_cjk, 0xDDE5F3, LV_ALIGN_TOP_LEFT, 88, 384);
        lv_obj_set_width(_fortune_plan, 300);
        lv_label_set_long_mode(_fortune_plan, LV_LABEL_LONG_WRAP);
        _fortune_hint = makeLabel(_root, "B 返回", &lv_font_source_han_sans_sc_14_cjk, 0x68738B, LV_ALIGN_BOTTOM_MID, 0, -24);

        _pet_menu_title = makeLabel(_root, "Settings", &lv_font_montserrat_28, 0xE8EEFF, LV_ALIGN_TOP_MID, 0, 44);
        _pet_preview_frame = lv_obj_create(_root);
        lv_obj_set_size(_pet_preview_frame, 154, 164);
        lv_obj_align(_pet_preview_frame, LV_ALIGN_TOP_MID, 0, 88);
        lv_obj_set_style_radius(_pet_preview_frame, 12, 0);
        lv_obj_set_style_border_width(_pet_preview_frame, 1, 0);
        lv_obj_set_style_border_color(_pet_preview_frame, lv_color_hex(0x263047), 0);
        lv_obj_set_style_bg_opa(_pet_preview_frame, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(_pet_preview_frame, 0, 0);
        lv_obj_remove_flag(_pet_preview_frame, LV_OBJ_FLAG_SCROLLABLE);
        _pet_prev_button = makeNavButton(_root, "<", LV_ALIGN_TOP_MID, -126, 142);
        _pet_next_button = makeNavButton(_root, ">", LV_ALIGN_TOP_MID, 126, 142);
        lv_obj_add_event_cb(_pet_prev_button, onPetPrevClicked, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(_pet_next_button, onPetNextClicked, LV_EVENT_CLICKED, this);
        _pet_menu_name = makeLabel(_root, "", &lv_font_montserrat_24, 0x7DFFB6, LV_ALIGN_TOP_MID, 0, 266);
        lv_obj_set_width(_pet_menu_name, 340);
        lv_obj_set_style_text_align(_pet_menu_name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(_pet_menu_name, LV_LABEL_LONG_DOT);
        _pet_menu_desc = makeLabel(_root, "", &lv_font_montserrat_18, 0xAEB8D8, LV_ALIGN_TOP_MID, 0, 306);
        _pet_menu_hint = makeLabel(_root, "", &lv_font_montserrat_16, 0x68738B, LV_ALIGN_BOTTOM_MID, 0, -38);

        _wellness_triangle = makeBreathTriangle(_root, _wellness_triangle_points);
        _wellness_title = makeLabel(_root, "", &lv_font_montserrat_28, 0xE8EEFF, LV_ALIGN_TOP_MID, 0, 40);
        _wellness_body = makeLabel(_root, "", &lv_font_source_han_sans_sc_16_cjk, 0xBFC6D2, LV_ALIGN_CENTER, 0, 42);
        lv_obj_set_width(_wellness_body, 300);
        lv_obj_set_style_text_align(_wellness_body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_transform_scale(_wellness_body, 448, 0);
        lv_obj_set_style_transform_pivot_x(_wellness_body, 150, 0);
        lv_obj_set_style_transform_pivot_y(_wellness_body, 18, 0);
        lv_label_set_long_mode(_wellness_body, LV_LABEL_LONG_WRAP);
        _wellness_sub = makeLabel(_root, "", &lv_font_montserrat_28, 0xC7CDD9, LV_ALIGN_CENTER, 0, -34);
        lv_obj_set_width(_wellness_sub, 300);
        lv_obj_set_style_text_align(_wellness_sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_transform_scale(_wellness_sub, 500, 0);
        lv_obj_set_style_transform_pivot_x(_wellness_sub, 150, 0);
        lv_obj_set_style_transform_pivot_y(_wellness_sub, 34, 0);
        lv_label_set_long_mode(_wellness_sub, LV_LABEL_LONG_CLIP);
        _wellness_hint = makeLabel(_root, "A start/restart   B next", &lv_font_montserrat_16, 0x68738B, LV_ALIGN_BOTTOM_MID, 0, -30);
        _wellness_dot = makeDot(_root, 0xE2B84F, 0, 0);
        lv_obj_set_size(_wellness_dot, 48, 48);
        lv_obj_set_style_radius(_wellness_dot, LV_RADIUS_CIRCLE, 0);

        _boarding_title = makeLabel(_root, "Boarding Pass", &lv_font_montserrat_28, 0xF4F6FB, LV_ALIGN_TOP_MID, 0, 32);
        _boarding_route = makeLabel(_root, "PEK  >  HGH", &lv_font_montserrat_36, 0xF4F6FB, LV_ALIGN_TOP_MID, 0, 74);
        _boarding_meta = makeLabel(_root, "GJ8988   2026-06-02   22J", &lv_font_montserrat_18, 0xAEB8D8, LV_ALIGN_TOP_MID, 0, 118);
        _boarding_qr_frame = lv_obj_create(_root);
        lv_obj_set_size(_boarding_qr_frame, 280, 280);
        lv_obj_align(_boarding_qr_frame, LV_ALIGN_TOP_MID, 0, 146);
        lv_obj_set_style_radius(_boarding_qr_frame, 8, 0);
        lv_obj_set_style_border_width(_boarding_qr_frame, 0, 0);
        lv_obj_set_style_bg_color(_boarding_qr_frame, lv_color_white(), 0);
        lv_obj_set_style_pad_all(_boarding_qr_frame, 10, 0);
        lv_obj_remove_flag(_boarding_qr_frame, LV_OBJ_FLAG_SCROLLABLE);

        _boarding_qr = lv_qrcode_create(_boarding_qr_frame);
        lv_qrcode_set_size(_boarding_qr, 260);
        lv_qrcode_set_dark_color(_boarding_qr, lv_color_black());
        lv_qrcode_set_light_color(_boarding_qr, lv_color_white());
        lv_qrcode_set_quiet_zone(_boarding_qr, true);
        lv_obj_center(_boarding_qr);
        _boarding_qr_ready = lv_qrcode_update(_boarding_qr, kBoardingPassQrPayload,
                                              std::strlen(kBoardingPassQrPayload)) == LV_RESULT_OK;

        _boarding_hint = makeLabel(_root, "B next   A+B home", &lv_font_montserrat_16, 0x68738B, LV_ALIGN_BOTTOM_MID, 0, -18);
        _boarding_status = makeLabel(_root, "", &lv_font_montserrat_16, 0xD84E4E, LV_ALIGN_BOTTOM_MID, 0, -44);

        _pet_touch_zone = lv_obj_create(_root);
        lv_obj_set_size(_pet_touch_zone, 246, 220);
        lv_obj_align(_pet_touch_zone, LV_ALIGN_TOP_MID, 0, 18);
        lv_obj_set_style_bg_opa(_pet_touch_zone, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_pet_touch_zone, 0, 0);
        lv_obj_set_style_pad_all(_pet_touch_zone, 0, 0);
        lv_obj_remove_flag(_pet_touch_zone, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_pet_touch_zone, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(_pet_touch_zone, onPetClicked, LV_EVENT_CLICKED, this);

        setPromptMode(false);
        setDetailMode(false);
        setFullscreenMode(false);
        setPetMenuMode(false);
        setWellnessMode(false);
        setBoardingPassMode(false);
        setFortuneMode(false);
    }

    ~CodexBuddyView()
    {
        if (_breath_saved_speaker_volume >= 0) {
            GetHAL().setSpeakerVolume(_breath_saved_speaker_volume, false);
            _breath_saved_speaker_volume = -1;
        }
        if (_root != nullptr) {
            lv_obj_delete(_root);
        }
    }

    void update(const CodexBuddyState& state, uint8_t page)
    {
        char buf[160];

        _last_state = state;
        _page = page % kPageCount;

        if (state.repair_pairing || state.prompt_id[0] != 0 || state.passkey != 0) {
            _fortune_pending = false;
            setFortuneMode(false);
            setDetailMode(false);
            setFullscreenMode(false);
            setPetMenuMode(false);
            setWellnessMode(false);
            setBoardingPassMode(false);
            setPromptMode(true);
            if (state.repair_pairing) {
                lv_label_set_text(_prompt_title, "Re-pair BLE");
                lv_label_set_text(_prompt_tool, CodexBuddyBle::instance().deviceName().c_str());
                lv_label_set_text(_prompt_hint, "Forget this device on Mac, then pair again");
                setDecisionButtons(false);
            } else if (state.passkey != 0) {
                lv_label_set_text(_prompt_title, "Pair Code");
                std::snprintf(buf, sizeof(buf), "%06" PRIu32, state.passkey);
                lv_label_set_text(_prompt_tool, buf);
                lv_label_set_text(_prompt_hint, "Enter this code on your Mac");
                setDecisionButtons(false);
            } else {
                lv_label_set_text(_prompt_title, "Approval");
                lv_label_set_text(_prompt_tool, state.prompt_tool[0] ? state.prompt_tool : "Tool");
                lv_label_set_text(_prompt_hint, state.prompt_hint[0] ? state.prompt_hint : "Permission requested");
                setDecisionButtons(true);
            }
            return;
        }

        const uint32_t now = GetHAL().millis();
        if (_fortune_pending && now >= _fortune_reveal_ms) {
            _fortune_pending = false;
            setFortuneMode(true);
        }
        if (_fortune_result_visible) {
            setPromptMode(false);
            setDetailMode(false);
            setFullscreenMode(false);
            setFortuneMode(true);
            return;
        }

        setPromptMode(false);
        setDetailMode(false);
        setFullscreenMode(_page == 1);
        setPetMenuMode(_page == 2);
        setWellnessMode(_page == 3);
        setBoardingPassMode(_page == 4);

        if (_page == 2) {
            updateSettingsPage();
            return;
        }
        if (_page == 3) {
            updateWellnessPage(now);
            return;
        }
        if (_page == 4) {
            updateBoardingPassPage();
            return;
        }

        if (_page == 0) {
            updateUsageRow(_usage_primary, state.usage_primary, "5H", false);
            updateUsageRow(_usage_secondary, state.usage_secondary, "1W", true);
            std::snprintf(buf, sizeof(buf), "%u", state.running);
            lv_label_set_text(_active_count_label, buf);
            std::snprintf(buf, sizeof(buf), "%u", state.pending);
            lv_label_set_text(_pending_count_label, buf);
        }
    }

    void nextPage()
    {
        _page = (_page + 1) % kPageCount;
        if (_page != 2) {
            _settings_pet_picker = false;
        }
        if (_page != 3) {
            _breath_running = false;
        }
    }

    void previousPage()
    {
        _page = (_page + kPageCount - 1) % kPageCount;
        if (_page != 2) {
            _settings_pet_picker = false;
        }
        if (_page != 3) {
            _breath_running = false;
        }
    }

    void setPage(uint8_t page)
    {
        _page = page % kPageCount;
        if (_page != 2) {
            _settings_pet_picker = false;
        }
        if (_page != 3) {
            _breath_running = false;
        }
    }

    bool settingsActive() const
    {
        return _page == 2;
    }

    bool petPickerActive() const
    {
        return _page == 2 && _settings_pet_picker;
    }

    bool wellnessActive() const
    {
        return _page == 3;
    }

    void nextPetOption()
    {
        const uint8_t count = std::max<uint8_t>(CodexBuddyBle::instance().petOptionCount(), 1);
        _selected_pet = (_selected_pet + 1) % count;
        updateSettingsPage();
        GetHAL().vibrate(25, 45);
    }

    void previousPetOption()
    {
        const uint8_t count = std::max<uint8_t>(CodexBuddyBle::instance().petOptionCount(), 1);
        _selected_pet = (_selected_pet + count - 1) % count;
        updateSettingsPage();
        GetHAL().vibrate(25, 45);
    }

    void openSettingsOption()
    {
        _settings_pet_picker = true;
        syncSelectedPetToInstalled();
        updateSettingsPage();
        GetHAL().vibrate(25, 45);
    }

    void requestSelectedPet()
    {
        CodexBuddyBle::instance().requestPet(CodexBuddyBle::instance().petOptionSelector(_selected_pet),
                                             CodexBuddyBle::instance().petOptionLabel(_selected_pet));
        lv_label_set_text(_pet_menu_desc, "Waiting for bridge");
        GetHAL().vibrate(55, 70);
    }

    void closePetPicker()
    {
        _settings_pet_picker = false;
        updateSettingsPage();
        GetHAL().vibrate(25, 45);
    }

    void activateWellnessAction()
    {
        if (_page == 3) {
            startOrRestartBreathing();
        }
    }

    void startOrRestartBreathing()
    {
        _breath_running = true;
        _breath_started_ms = GetHAL().millis();
        _wellness_last_beep_phase_id = 0xFF;
        resetWellnessUiCache();
        GetHAL().vibrate(35, 55);
    }

    bool breathingRunning() const
    {
        return _page == 3 && _breath_running;
    }

    uint8_t page() const
    {
        return _page;
    }

    void renderPetAsset(const CodexBuddyState& state, uint8_t page)
    {
        if (_fortune_result_visible) {
            _pet_renderer.renderFortuneMascot();
            return;
        }
        if (petPickerActive()) {
            _pet_renderer.renderPreview();
            return;
        }
        _pet_renderer.render(state, page);
    }

    void beginShakeAnimation()
    {
        _fortune_pending = false;
        setFortuneMode(false);
        setPromptMode(false);
        setDetailMode(false);
        setPetMenuMode(false);
        setWellnessMode(false);
        setBoardingPassMode(false);
        setFullscreenMode(true);
        _pet_renderer.playTemporary(PetGifState::Sparkle, kShakeMaxActiveMs + 900);
    }

    void endShakeAnimation()
    {
        setFullscreenMode(false);
    }

    void renderShakeAnimation(const CodexBuddyState& state)
    {
        _pet_renderer.render(state, 1);
    }

    void invalidateFortuneMascot()
    {
        _pet_renderer.invalidateFortuneMascot();
    }

    void approve()
    {
        auto state = CodexBuddyBle::instance().snapshot();
        CodexBuddyBle::instance().sendPermission(state.prompt_id, "once");
    }

    void deny()
    {
        auto state = CodexBuddyBle::instance().snapshot();
        CodexBuddyBle::instance().sendPermission(state.prompt_id, "deny");
    }

    void petClicked()
    {
        GetHAL().vibrate(45, 70);
        _pet_renderer.playRandomInteraction();
    }

    void triggerFortune(uint32_t seed)
    {
        _fortune_index = static_cast<uint8_t>((seed ^ (seed >> 11) ^ esp_random()) %
                                             (sizeof(kFortunes) / sizeof(kFortunes[0])));
        _fortune_pending = true;
        _fortune_result_visible = false;
        _fortune_reveal_ms = GetHAL().millis() + kFortuneRevealDelayMs;
        setFortuneMode(false);
        GetHAL().vibrate(420, 100);
        _pet_renderer.playTemporary(PetGifState::Dizzy, kFortuneRevealDelayMs + 400);
    }

    void showFortuneResult(uint32_t seed)
    {
        _fortune_index = static_cast<uint8_t>((seed ^ (seed >> 11) ^ esp_random()) %
                                             (sizeof(kFortunes) / sizeof(kFortunes[0])));
        _fortune_pending = false;
        _fortune_result_visible = false;
        _fortune_reveal_ms = 0;
        setFullscreenMode(false);
        setFortuneMode(true);
        GetHAL().vibrate(420, 100);
    }

    bool fortuneResultVisible() const
    {
        return _fortune_result_visible;
    }

    bool fortunePending() const
    {
        return _fortune_pending;
    }

    void dismissFortune()
    {
        _fortune_pending = false;
        if (_fortune_result_visible) {
            _fortune_dismissed = true;
        }
        setFortuneMode(false);
    }

    bool consumeFortuneDismissed()
    {
        const bool dismissed = _fortune_dismissed;
        _fortune_dismissed = false;
        return dismissed;
    }

private:
    static constexpr uint8_t kPageCount = 5;

    struct UsageRow {
        lv_obj_t* scope = nullptr;
        lv_obj_t* reset = nullptr;
        lv_obj_t* bar = nullptr;
        lv_obj_t* percent = nullptr;
    };

    struct ParsedUsage {
        char scope[4] = "";
        char reset[8] = "";
        int percent = 0;
    };

    struct Fortune {
        const char* badge;
        const char* title;
        const char* signal;
        const char* read;
        const char* plan;
        uint32_t accent;
    };

    static constexpr Fortune kFortunes[] = {
        {"來自星星", "答案在發光", "今天也要開心，答案會自己發光。", "", "", 0x86A8FF},
        {"低電模式", "可以緩一點", "不用急著回答，先把電量留給自己。", "", "", 0x78B6A3},
        {"星星提示", "你也是星星", "不要忘了，你也是會發光的星星。", "", "", 0xD2B45F},
        {"輕一點", "選輕的路", "先選讓心變輕的那條路就好。", "", "", 0xA7C27A},
        {"不急", "明天回答", "不是所有問題都要在今天解開。", "", "", 0x9A8BD1},
        {"補給站", "先吃快樂", "先吃一口快樂，再處理困難。", "", "", 0xE0A15D},
        {"道路正常", "還在路上", "看不見終點時，就先看下一步。", "", "", 0x6FA8D8},
        {"小確幸", "可以試試", "小幸運在旁邊等你，去試試看。", "", "", 0xD58DBC},
        {"清空一點", "別放太滿", "腦內太滿時，答案會找不到座位。", "", "", 0x82B8C0},
        {"繼續發光", "今天夠了", "今天做到這裡，就已經在發光了。", "", "", 0xC8A66C},
        {"星雲通過", "會變清楚", "現在看不清楚，不代表一直看不清楚。", "", "", 0x7F9FD4},
        {"小小答案", "別太用力", "放輕一點，答案可能會更近一點。", "", "", 0xB08BD0},
    };

    lv_obj_t* _root = nullptr;
    lv_obj_t* _usage_title = nullptr;
    UsageRow _usage_primary;
    UsageRow _usage_secondary;
    lv_obj_t* _dot_active = nullptr;
    lv_obj_t* _dot_pending = nullptr;
    lv_obj_t* _active_label = nullptr;
    lv_obj_t* _pending_label = nullptr;
    lv_obj_t* _active_count_label = nullptr;
    lv_obj_t* _pending_count_label = nullptr;
    lv_obj_t* _page_mark = nullptr;
    lv_obj_t* _detail_title = nullptr;
    lv_obj_t* _detail_status = nullptr;
    lv_obj_t* _detail_body = nullptr;
    lv_obj_t* _detail_hint = nullptr;
    lv_obj_t* _prompt_title = nullptr;
    lv_obj_t* _prompt_tool = nullptr;
    lv_obj_t* _prompt_hint = nullptr;
    lv_obj_t* _prompt_footer = nullptr;
    lv_obj_t* _approve = nullptr;
    lv_obj_t* _deny = nullptr;
    lv_obj_t* _pet_touch_zone = nullptr;
    lv_obj_t* _fortune_glow = nullptr;
    lv_obj_t* _fortune_orbit = nullptr;
    lv_obj_t* _fortune_orbit_inner = nullptr;
    lv_obj_t* _fortune_left_person = nullptr;
    lv_obj_t* _fortune_right_person = nullptr;
    lv_obj_t* _fortune_rule_left = nullptr;
    lv_obj_t* _fortune_rule_right = nullptr;
    std::array<lv_obj_t*, 12> _fortune_stars = {};
    lv_obj_t* _fortune_header = nullptr;
    lv_obj_t* _fortune_badge = nullptr;
    lv_obj_t* _fortune_title = nullptr;
    lv_obj_t* _fortune_signal_box = nullptr;
    lv_obj_t* _fortune_signal = nullptr;
    lv_obj_t* _fortune_read_title = nullptr;
    lv_obj_t* _fortune_read = nullptr;
    lv_obj_t* _fortune_plan_title = nullptr;
    lv_obj_t* _fortune_plan = nullptr;
    lv_obj_t* _fortune_hint = nullptr;
    lv_obj_t* _pet_menu_title = nullptr;
    lv_obj_t* _pet_preview_frame = nullptr;
    lv_obj_t* _pet_prev_button = nullptr;
    lv_obj_t* _pet_next_button = nullptr;
    lv_obj_t* _pet_menu_name = nullptr;
    lv_obj_t* _pet_menu_desc = nullptr;
    lv_obj_t* _pet_menu_hint = nullptr;
    lv_obj_t* _wellness_triangle = nullptr;
    lv_obj_t* _wellness_title = nullptr;
    lv_obj_t* _wellness_body = nullptr;
    lv_obj_t* _wellness_sub = nullptr;
    lv_obj_t* _wellness_hint = nullptr;
    lv_obj_t* _wellness_dot = nullptr;
    lv_obj_t* _boarding_title = nullptr;
    lv_obj_t* _boarding_route = nullptr;
    lv_obj_t* _boarding_meta = nullptr;
    lv_obj_t* _boarding_qr_frame = nullptr;
    lv_obj_t* _boarding_qr = nullptr;
    lv_obj_t* _boarding_hint = nullptr;
    lv_obj_t* _boarding_status = nullptr;
    lv_point_precise_t _wellness_triangle_points[4] = {};
    CodexBuddyState _last_state;
    PetGifRenderer _pet_renderer;
    uint32_t _fortune_reveal_ms = 0;
    uint32_t _breath_started_ms = 0;
    int _wellness_last_dot_x = -10000;
    int _wellness_last_dot_y = -10000;
    int _wellness_last_countdown = -1;
    uint8_t _wellness_last_phase_id = 0xFF;
    uint8_t _wellness_last_beep_phase_id = 0xFF;
    bool _breath_running = false;
    int _breath_saved_speaker_volume = -1;
    std::vector<int16_t> _breath_cue_buffer;
    uint8_t _fortune_index = 0;
    uint8_t _selected_pet = 0;
    uint8_t _page = 0;
    bool _prompt_mode_active = false;
    bool _detail_mode_active = false;
    bool _fullscreen_mode_active = false;
    bool _pet_menu_mode_active = false;
    bool _wellness_mode_active = false;
    bool _boarding_pass_mode_active = false;
    bool _boarding_qr_ready = false;
    bool _settings_pet_picker = false;
    bool _fortune_pending = false;
    bool _fortune_result_visible = false;
    bool _fortune_dismissed = false;

    static lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, lv_align_t align, int x, int y)
    {
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_align(label, align, x, y);
        return label;
    }

    static lv_obj_t* makeDot(lv_obj_t* parent, uint32_t color, int x, int y)
    {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        return dot;
    }

    static lv_obj_t* makeBreathTriangle(lv_obj_t* parent, lv_point_precise_t* points)
    {
        points[0] = {68, 382};
        points[1] = {233, 96};
        points[2] = {398, 382};
        points[3] = {68, 382};

        lv_obj_t* line = lv_line_create(parent);
        lv_obj_set_size(line, 466, 466);
        lv_obj_set_pos(line, 0, 0);
        lv_line_set_points(line, points, 4);
        lv_obj_set_style_line_width(line, 6, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(0xF4F5F0), 0);
        lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        return line;
    }

    static lv_obj_t* makeDecorDot(lv_obj_t* parent, int size, uint32_t color, lv_opa_t opa, int x, int y)
    {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_set_size(dot, size, size);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(dot, opa, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    }

    static lv_obj_t* makeFortuneRule(lv_obj_t* parent, int x, int y, int w, uint32_t color)
    {
        lv_obj_t* rule = lv_obj_create(parent);
        lv_obj_set_size(rule, w, 2);
        lv_obj_align(rule, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_set_style_radius(rule, 1, 0);
        lv_obj_set_style_bg_color(rule, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_60, 0);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_pad_all(rule, 0, 0);
        lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
        return rule;
    }

    static lv_obj_t* makeFortuneGlow(lv_obj_t* parent)
    {
        lv_obj_t* glow = lv_obj_create(parent);
        lv_obj_set_size(glow, 328, 328);
        lv_obj_align(glow, LV_ALIGN_TOP_MID, 0, 70);
        lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(glow, lv_color_hex(0x071225), 0);
        lv_obj_set_style_bg_opa(glow, LV_OPA_20, 0);
        lv_obj_set_style_border_width(glow, 1, 0);
        lv_obj_set_style_border_color(glow, lv_color_hex(0x1E2A55), 0);
        lv_obj_set_style_border_opa(glow, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(glow, 0, 0);
        lv_obj_remove_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(glow, LV_OBJ_FLAG_CLICKABLE);
        return glow;
    }

    static lv_obj_t* makeFortuneArc(lv_obj_t* parent, int w, int h, lv_align_t align, int x, int y, int start, int end,
                                    uint32_t color)
    {
        lv_obj_t* arc = lv_arc_create(parent);
        lv_obj_set_size(arc, w, h);
        lv_obj_align(arc, align, x, y);
        lv_arc_set_angles(arc, start, end);
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_60, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_0, LV_PART_MAIN);
        lv_obj_set_style_opa(arc, LV_OPA_0, LV_PART_KNOB);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        return arc;
    }

    static lv_obj_t* makeStarPersonPart(lv_obj_t* parent, int w, int h, int radius, uint32_t color, lv_opa_t opa,
                                        lv_align_t align, int x, int y)
    {
        lv_obj_t* part = lv_obj_create(parent);
        lv_obj_set_size(part, w, h);
        lv_obj_align(part, align, x, y);
        lv_obj_set_style_radius(part, radius, 0);
        lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(part, opa, 0);
        lv_obj_set_style_border_width(part, 0, 0);
        lv_obj_set_style_pad_all(part, 0, 0);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE);
        return part;
    }

    static lv_obj_t* makeStarPerson(lv_obj_t* parent, lv_align_t align, int x, int y, uint32_t accent)
    {
        lv_obj_t* body = lv_obj_create(parent);
        lv_obj_set_size(body, 48, 72);
        lv_obj_align(body, align, x, y);
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(body, 0, 0);
        lv_obj_set_style_pad_all(body, 0, 0);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

        auto pixel = [body](uint8_t gx, uint8_t gy, uint32_t color, lv_opa_t opa) {
            makeStarPersonPart(body, 3, 3, 0, color, opa, LV_ALIGN_TOP_LEFT, gx * 4, gy * 4);
        };

        static constexpr uint8_t outline[][2] = {
            {5, 0},  {4, 1},  {5, 1},  {6, 1},  {3, 2},  {4, 2},  {5, 2},  {6, 2},  {7, 2},
            {4, 3},  {5, 3},  {6, 3},  {5, 4},  {5, 5},  {4, 6},  {6, 6},  {3, 7},  {7, 7},
            {2, 8},  {8, 8},  {1, 9},  {9, 9},  {1, 10}, {9, 10}, {0, 11}, {10, 11}, {0, 12},
            {10, 12}, {1, 13}, {9, 13}, {1, 14}, {9, 14}, {2, 15}, {8, 15}, {3, 16}, {4, 16},
            {5, 16}, {6, 16}, {7, 16},
        };
        static constexpr uint8_t face[][2] = {
            {4, 8}, {5, 8}, {6, 8}, {4, 9}, {5, 9}, {6, 9}, {4, 10}, {5, 10}, {6, 10},
        };
        static constexpr uint8_t eyes[][2] = {
            {4, 9}, {6, 9}, {5, 11}, {4, 14}, {5, 14}, {6, 14},
        };

        for (const auto& p : outline) {
            pixel(p[0], p[1], accent, LV_OPA_80);
        }
        for (const auto& p : face) {
            pixel(p[0], p[1], 0xF2A06C, LV_OPA_70);
        }
        for (const auto& p : eyes) {
            pixel(p[0], p[1], 0xF6F8FF, LV_OPA_COVER);
        }
        return body;
    }

    void createFortuneStarfield()
    {
        struct StarSpec {
            int x;
            int y;
            int size;
            uint32_t color;
            lv_opa_t opa;
        };
        static constexpr StarSpec kStars[] = {
            {116, 58, 5, 0x8AA5FF, LV_OPA_60}, {345, 64, 4, 0xFFE18A, LV_OPA_70},
            {86, 128, 4, 0xC299FF, LV_OPA_60}, {372, 126, 6, 0x6FAFEA, LV_OPA_60},
            {58, 258, 4, 0xFFE18A, LV_OPA_50}, {404, 262, 5, 0x8AA5FF, LV_OPA_50},
            {86, 338, 3, 0x6FAFEA, LV_OPA_50}, {382, 342, 4, 0xC299FF, LV_OPA_50},
            {136, 424, 3, 0xFFE18A, LV_OPA_40}, {322, 424, 3, 0x8AA5FF, LV_OPA_40},
            {226, 75, 6, 0x8AA5FF, LV_OPA_80}, {232, 436, 3, 0xFFE18A, LV_OPA_50},
        };

        for (size_t i = 0; i < _fortune_stars.size(); ++i) {
            _fortune_stars[i] = makeDecorDot(_root, kStars[i].size, kStars[i].color, kStars[i].opa, kStars[i].x, kStars[i].y);
        }
    }

    static lv_obj_t* makeBadge(lv_obj_t* parent, const char* text, lv_align_t align, int x, int y)
    {
        lv_obj_t* badge = lv_label_create(parent);
        lv_label_set_text(badge, text);
        lv_obj_set_style_text_font(badge, &lv_font_source_han_sans_sc_16_cjk, 0);
        lv_obj_set_style_text_color(badge, lv_color_hex(0xE8E0CD), 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x111720), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(0x445066), 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_set_style_pad_left(badge, 14, 0);
        lv_obj_set_style_pad_right(badge, 14, 0);
        lv_obj_set_style_pad_top(badge, 5, 0);
        lv_obj_set_style_pad_bottom(badge, 5, 0);
        lv_obj_align(badge, align, x, y);
        return badge;
    }

    void createUsageRow(UsageRow& row, int x, int y, uint32_t accent, uint32_t track)
    {
        row.scope = makeLabel(_root, "", &lv_font_montserrat_20, 0xD9E1FF, LV_ALIGN_TOP_LEFT, x, y - 5);
        lv_obj_set_width(row.scope, 42);

        row.reset = makeLabel(_root, "", &lv_font_montserrat_18, 0xD9E1FF, LV_ALIGN_TOP_LEFT, x + 52, y - 3);
        lv_obj_set_width(row.reset, 66);

        row.bar = lv_bar_create(_root);
        lv_obj_set_size(row.bar, 118, 14);
        lv_obj_align(row.bar, LV_ALIGN_TOP_LEFT, x + 134, y + 3);
        lv_bar_set_range(row.bar, 0, 100);
        lv_bar_set_value(row.bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(row.bar, 7, LV_PART_MAIN);
        lv_obj_set_style_radius(row.bar, 7, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(row.bar, lv_color_hex(track), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row.bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.bar, lv_color_hex(accent), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(row.bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(row.bar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row.bar, 2, LV_PART_MAIN);
        lv_obj_remove_flag(row.bar, LV_OBJ_FLAG_CLICKABLE);

        row.percent = makeLabel(_root, "", &lv_font_montserrat_18, accent, LV_ALIGN_TOP_LEFT, x + 268, y - 3);
        lv_obj_set_width(row.percent, 58);
        lv_obj_set_style_text_align(row.percent, LV_TEXT_ALIGN_LEFT, 0);
    }

    static lv_obj_t* makeButton(lv_obj_t* parent, const char* text, uint32_t color, lv_align_t align, int x, int y)
    {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, 150, 58);
        lv_obj_set_style_radius(btn, 29, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_align(btn, align, x, y);
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        return btn;
    }

    static lv_obj_t* makeNavButton(lv_obj_t* parent, const char* text, lv_align_t align, int x, int y)
    {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, 54, 54);
        lv_obj_set_style_radius(btn, 27, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x121827), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x2D3954), 0);
        lv_obj_align(btn, align, x, y);
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xD9E1FF), 0);
        lv_obj_center(label);
        return btn;
    }

    static void setHidden(lv_obj_t* obj, bool hidden)
    {
        if (obj == nullptr) {
            return;
        }
        hidden ? lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    static void setHidden(UsageRow& row, bool hidden)
    {
        setHidden(row.scope, hidden);
        setHidden(row.reset, hidden);
        setHidden(row.bar, hidden);
        setHidden(row.percent, hidden);
    }

    static void updateUsageRow(UsageRow& row, const char* line, const char* fallbackScope, bool weekly)
    {
        const ParsedUsage parsed = parseUsage(line, fallbackScope, weekly);
        char pct[8];
        std::snprintf(pct, sizeof(pct), "%d%%", parsed.percent);
        lv_label_set_text(row.scope, parsed.scope);
        lv_label_set_text(row.reset, parsed.reset);
        lv_bar_set_value(row.bar, parsed.percent, LV_ANIM_OFF);
        lv_label_set_text(row.percent, pct);
    }

    static ParsedUsage parseUsage(const char* line, const char* fallbackScope, bool weekly)
    {
        ParsedUsage parsed;
        safeCopy(parsed.scope, sizeof(parsed.scope), fallbackScope);
        safeCopy(parsed.reset, sizeof(parsed.reset), weekly ? "--/--" : "--:--");
        parsed.percent = 0;

        if (line == nullptr || line[0] == '\0') {
            return parsed;
        }

        const char* cursor = line;
        while (*cursor == ' ') {
            ++cursor;
        }
        char scope[4] = "";
        size_t scope_len = 0;
        while (*cursor != '\0' && *cursor != ' ' && scope_len + 1 < sizeof(scope)) {
            scope[scope_len++] = static_cast<char>(std::toupper(static_cast<unsigned char>(*cursor++)));
        }
        scope[scope_len] = '\0';
        if (scope[0] != '\0') {
            safeCopy(parsed.scope, sizeof(parsed.scope), scope);
        }

        const char* percent_marker = std::strchr(line, '%');
        if (percent_marker != nullptr) {
            const char* start = percent_marker;
            while (start > line && std::isdigit(static_cast<unsigned char>(*(start - 1)))) {
                --start;
            }
            if (start != percent_marker) {
                char number[4] = "";
                const size_t len = std::min<size_t>(sizeof(number) - 1, percent_marker - start);
                std::memcpy(number, start, len);
                parsed.percent = std::clamp(std::atoi(number), 0, 100);
            }
        }

        const char* reset_marker = std::strchr(line, '|');
        if (reset_marker != nullptr) {
            formatReset(reset_marker + 1, weekly, parsed.reset, sizeof(parsed.reset));
        }
        return parsed;
    }

    static void formatReset(const char* raw, bool weekly, char* out, size_t out_len)
    {
        while (raw != nullptr && *raw == ' ') {
            ++raw;
        }
        if (raw == nullptr || raw[0] == '\0') {
            safeCopy(out, out_len, weekly ? "--/--" : "--:--");
            return;
        }

        if (!weekly) {
            const char* colon = std::strchr(raw, ':');
            if (colon != nullptr && colon - raw >= 1) {
                char value[8] = "";
                size_t len = 0;
                while (raw[len] != '\0' && raw[len] != ' ' && len + 1 < sizeof(value)) {
                    value[len] = raw[len];
                    ++len;
                }
                value[len] = '\0';
                safeCopy(out, out_len, value);
            } else {
                safeCopy(out, out_len, "--:--");
            }
            return;
        }

        char month[4] = "";
        int day = 0;
        if (std::isalpha(static_cast<unsigned char>(raw[0])) && std::sscanf(raw, "%3s %d", month, &day) == 2) {
            const int month_number = monthNumber(month);
            if (month_number > 0 && day > 0 && day <= 31) {
                std::snprintf(out, out_len, "%02d/%02d", month_number, day);
                return;
            }
        }

        int month_number = 0;
        if (std::sscanf(raw, "%d/%d", &month_number, &day) == 2 && month_number > 0 && month_number <= 12 &&
            day > 0 && day <= 31) {
            std::snprintf(out, out_len, "%02d/%02d", month_number, day);
            return;
        }
        safeCopy(out, out_len, "--/--");
    }

    static int monthNumber(const char* value)
    {
        static constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        for (int i = 0; i < 12; ++i) {
            if (std::strncmp(value, months[i], 3) == 0) {
                return i + 1;
            }
        }
        return 0;
    }

    static bool petMatchesInstalled(const char* selector, const char* label, const char* installed)
    {
        if (installed == nullptr || installed[0] == '\0') {
            return false;
        }
        if (selector != nullptr) {
            if (std::strcmp(selector, installed) == 0) {
                return true;
            }
            const char* suffix = std::strchr(selector, ':');
            if (suffix != nullptr && std::strcmp(suffix + 1, installed) == 0) {
                return true;
            }
        }
        if (label == nullptr) {
            return false;
        }
        char normalized[48] = "";
        size_t pos = 0;
        for (const char* p = label; *p != '\0' && pos + 1 < sizeof(normalized); ++p) {
            const unsigned char ch = static_cast<unsigned char>(*p);
            if (std::isalnum(ch)) {
                normalized[pos++] = static_cast<char>(std::tolower(ch));
            } else if ((ch == ' ' || ch == '_' || ch == '-') && pos > 0 && normalized[pos - 1] != '-') {
                normalized[pos++] = '-';
            }
        }
        normalized[pos] = '\0';
        return std::strcmp(normalized, installed) == 0;
    }

    void syncSelectedPetToInstalled()
    {
        const char* installed = dynamicPetPack().name();
        const uint8_t count = CodexBuddyBle::instance().petOptionCount();
        if (installed == nullptr || installed[0] == '\0' || count == 0) {
            return;
        }
        for (uint8_t i = 0; i < count; ++i) {
            if (petMatchesInstalled(CodexBuddyBle::instance().petOptionSelector(i),
                                    CodexBuddyBle::instance().petOptionLabel(i), installed)) {
                _selected_pet = i;
                return;
            }
        }
    }

    static void safeCopy(char* dst, size_t dst_len, const char* src)
    {
        if (dst_len == 0) {
            return;
        }
        if (src == nullptr) {
            dst[0] = '\0';
            return;
        }
        std::snprintf(dst, dst_len, "%s", src);
    }

    void setDecisionButtons(bool show)
    {
        setHidden(_approve, !show);
        setHidden(_deny, !show);
        setHidden(_prompt_footer, !show);
    }

    void setFortuneMode(bool show)
    {
        if (show && !_fortune_result_visible) {
            clearDirectDrawLayer();
            _pet_renderer.invalidateFortuneMascot();
            const Fortune& fortune = kFortunes[_fortune_index % (sizeof(kFortunes) / sizeof(kFortunes[0]))];
            char header[32];
            std::snprintf(header, sizeof(header), "星星答案機 #%03u", static_cast<unsigned>(_fortune_index + 1));
            lv_label_set_text(_fortune_header, header);
            lv_label_set_text(_fortune_badge, fortune.badge);
            lv_label_set_text(_fortune_title, fortune.title);
            lv_label_set_text(_fortune_signal, fortune.signal);
            lv_obj_set_style_text_color(_fortune_badge, lv_color_hex(fortune.accent), 0);
            lv_obj_set_style_border_color(_fortune_badge, lv_color_hex(fortune.accent), 0);
            lv_obj_set_style_border_color(_fortune_signal_box, lv_color_hex(fortune.accent), 0);
            lv_obj_set_style_text_color(_fortune_title, lv_color_hex(fortune.accent), 0);
        } else if (!show && _fortune_result_visible) {
            clearDirectDrawLayer();
            _pet_renderer.invalidateFortuneMascot();
        }

        _fortune_result_visible = show;
        setHidden(_fortune_glow, !show);
        setHidden(_fortune_orbit, !show);
        setHidden(_fortune_orbit_inner, !show);
        setHidden(_fortune_left_person, !show);
        setHidden(_fortune_right_person, !show);
        setHidden(_fortune_rule_left, !show);
        setHidden(_fortune_rule_right, !show);
        for (lv_obj_t* star : _fortune_stars) {
            setHidden(star, !show);
        }
        setHidden(_fortune_header, !show);
        setHidden(_fortune_badge, true);
        setHidden(_fortune_title, true);
        setHidden(_fortune_signal_box, !show);
        setHidden(_fortune_signal, !show);
        setHidden(_fortune_read_title, true);
        setHidden(_fortune_read, true);
        setHidden(_fortune_plan_title, true);
        setHidden(_fortune_plan, true);
        setHidden(_fortune_hint, !show);
        setHidden(_pet_touch_zone, show || _prompt_mode_active || _page != 0);
        if (show) {
            setPetMenuMode(false);
            setWellnessMode(false);
            setBoardingPassMode(false);
        }

        if (show) {
            setHidden(_usage_title, true);
            setHidden(_usage_primary, true);
            setHidden(_usage_secondary, true);
            setHidden(_dot_active, true);
            setHidden(_dot_pending, true);
            setHidden(_active_label, true);
            setHidden(_pending_label, true);
            setHidden(_active_count_label, true);
            setHidden(_pending_count_label, true);
        }
    }

    void setPromptMode(bool prompt)
    {
        if (prompt && !_prompt_mode_active) {
            clearDirectDrawLayer();
        }
        _prompt_mode_active = prompt;
        setHidden(_usage_title, prompt || _page != 0);
        setHidden(_usage_primary, prompt || _page != 0);
        setHidden(_usage_secondary, prompt || _page != 0);
        setHidden(_dot_active, prompt || _page != 0);
        setHidden(_dot_pending, prompt || _page != 0);
        setHidden(_active_label, prompt || _page != 0);
        setHidden(_pending_label, prompt || _page != 0);
        setHidden(_active_count_label, prompt || _page != 0);
        setHidden(_pending_count_label, prompt || _page != 0);
        setHidden(_page_mark, prompt);
        setHidden(_pet_touch_zone, prompt || _page != 0);
        setHidden(_prompt_title, !prompt);
        setHidden(_prompt_tool, !prompt);
        setHidden(_prompt_hint, !prompt);
        if (prompt) {
            setPetMenuMode(false);
            setWellnessMode(false);
            setBoardingPassMode(false);
        }
        setHidden(_detail_title, true);
        setHidden(_detail_status, true);
        setHidden(_detail_body, true);
        setHidden(_detail_hint, true);
        if (!prompt) {
            setDecisionButtons(false);
        }
    }

    void setDetailMode(bool detail)
    {
        if (detail && !_detail_mode_active) {
            clearDirectDrawLayer();
        }
        _detail_mode_active = detail;
        setHidden(_detail_title, true);
        setHidden(_detail_status, true);
        setHidden(_detail_body, true);
        setHidden(_detail_hint, true);
    }

    void setPetMenuMode(bool show)
    {
        if (show && !_pet_menu_mode_active) {
            clearDirectDrawLayer();
        }
        _pet_menu_mode_active = show;
        setHidden(_pet_menu_title, !show);
        setHidden(_pet_preview_frame, true);
        setHidden(_pet_prev_button, true);
        setHidden(_pet_next_button, true);
        setHidden(_pet_menu_name, !show);
        setHidden(_pet_menu_desc, !show);
        setHidden(_pet_menu_hint, !show);
        if (show) {
            setHidden(_usage_title, true);
            setHidden(_usage_primary, true);
            setHidden(_usage_secondary, true);
            setHidden(_dot_active, true);
            setHidden(_dot_pending, true);
            setHidden(_active_label, true);
            setHidden(_pending_label, true);
            setHidden(_active_count_label, true);
            setHidden(_pending_count_label, true);
            setHidden(_pet_touch_zone, true);
            setBoardingPassMode(false);
        }
    }

    void setWellnessMode(bool show)
    {
        if (show && !_wellness_mode_active) {
            clearDirectDrawLayer();
            resetWellnessUiCache();
        }
        _wellness_mode_active = show;
        setHidden(_wellness_triangle, !show);
        setHidden(_wellness_title, !show);
        setHidden(_wellness_body, !show);
        setHidden(_wellness_sub, !show);
        setHidden(_wellness_hint, !show);
        setHidden(_wellness_dot, !show);
        if (show) {
            setHidden(_usage_title, true);
            setHidden(_usage_primary, true);
            setHidden(_usage_secondary, true);
            setHidden(_dot_active, true);
            setHidden(_dot_pending, true);
            setHidden(_active_label, true);
            setHidden(_pending_label, true);
            setHidden(_active_count_label, true);
            setHidden(_pending_count_label, true);
            setHidden(_pet_touch_zone, true);
            setBoardingPassMode(false);
        }
    }

    void setBoardingPassMode(bool show)
    {
        if (show && !_boarding_pass_mode_active) {
            clearDirectDrawLayer();
        }
        _boarding_pass_mode_active = show;
        setHidden(_boarding_title, !show);
        setHidden(_boarding_route, !show);
        setHidden(_boarding_meta, !show);
        setHidden(_boarding_qr_frame, !show);
        setHidden(_boarding_qr, !show);
        setHidden(_boarding_hint, !show);
        setHidden(_boarding_status, !show || _boarding_qr_ready);
        if (show) {
            setHidden(_usage_title, true);
            setHidden(_usage_primary, true);
            setHidden(_usage_secondary, true);
            setHidden(_dot_active, true);
            setHidden(_dot_pending, true);
            setHidden(_active_label, true);
            setHidden(_pending_label, true);
            setHidden(_active_count_label, true);
            setHidden(_pending_count_label, true);
            setHidden(_pet_touch_zone, true);
        }
    }

    void resetWellnessUiCache()
    {
        _wellness_last_dot_x = -10000;
        _wellness_last_dot_y = -10000;
        _wellness_last_countdown = -1;
        _wellness_last_phase_id = 0xFF;
    }

    void playBreathPhaseCue(uint8_t phase_id)
    {
        if (_wellness_last_beep_phase_id == phase_id) {
            return;
        }
        _wellness_last_beep_phase_id = phase_id;
        if (_breath_saved_speaker_volume < 0) {
            _breath_saved_speaker_volume = GetHAL().getSpeakerVolume(false);
        }
        if (GetHAL().getSpeakerVolume(false) <= 0) {
            GetHAL().setSpeakerVolume(22, false);
        }

        const int frequencies[] = {784, 988, 659};
        const int sample_rate = GetHAL().getAudioSampleRate();
        const int sample_count = std::max(1, sample_rate * 55 / 1000);
        if (static_cast<int>(_breath_cue_buffer.size()) != sample_count) {
            _breath_cue_buffer.resize(sample_count);
        }

        const int frequency = frequencies[std::min<uint8_t>(phase_id, 2)];
        const float step = 2.0f * static_cast<float>(M_PI) * static_cast<float>(frequency) /
                           static_cast<float>(sample_rate);
        const int fade_len = std::min(sample_count / 2, 200);
        float phase = 0.0f;
        for (int i = 0; i < sample_count; ++i) {
            float envelope = 1.0f;
            if (fade_len > 0 && i >= sample_count - fade_len) {
                envelope = static_cast<float>(sample_count - i) / static_cast<float>(fade_len);
            }
            _breath_cue_buffer[i] = static_cast<int16_t>(sinf(phase) * 2100.0f * envelope);
            phase += step;
            if (phase > 2.0f * static_cast<float>(M_PI)) {
                phase -= 2.0f * static_cast<float>(M_PI);
            }
        }
        GetHAL().audioPlay(_breath_cue_buffer, false);
    }

    void updateSettingsPage()
    {
        if (!_settings_pet_picker) {
            lv_label_set_text(_pet_menu_title, "Settings");
            lv_label_set_text(_pet_menu_name, "Pet");
            lv_label_set_text(_pet_menu_desc, "A select pet");
            lv_label_set_text(_pet_menu_hint, "A open   B next   A+B home");
            setHidden(_pet_preview_frame, true);
            setHidden(_pet_prev_button, true);
            setHidden(_pet_next_button, true);
            return;
        }

        const uint8_t count = std::max<uint8_t>(CodexBuddyBle::instance().petOptionCount(), 1);
        _selected_pet %= count;
        char title[48];
        std::snprintf(title, sizeof(title), "%u/%u  %s", static_cast<unsigned>(_selected_pet + 1),
                      static_cast<unsigned>(count), CodexBuddyBle::instance().petOptionLabel(_selected_pet));
        lv_label_set_text(_pet_menu_title, "Pet");
        lv_label_set_text(_pet_menu_name, title);
        lv_label_set_text(_pet_menu_desc, "Current preview updates after sync");
        lv_label_set_text(_pet_menu_hint, "A sync   B back");
        setHidden(_pet_preview_frame, false);
        setHidden(_pet_prev_button, false);
        setHidden(_pet_next_button, false);
        _pet_renderer.renderPreview();
    }

    void updateWellnessPage(uint32_t now)
    {
        if (_page == 3) {
            if (!_breath_running) {
                if (_wellness_last_phase_id != 0xFE) {
                    lv_label_set_text(_wellness_title, "4-7-8");
                    lv_label_set_text(_wellness_body, "A START");
                    lv_label_set_text(_wellness_sub, "4");
                    _wellness_last_phase_id = 0xFE;
                    _wellness_last_countdown = 4;
                }
                const int dot_x = static_cast<int>(_wellness_triangle_points[0].x);
                const int dot_y = static_cast<int>(_wellness_triangle_points[0].y);
                if (dot_x != _wellness_last_dot_x || dot_y != _wellness_last_dot_y) {
                    lv_obj_set_pos(_wellness_dot, dot_x - 24, dot_y - 24);
                    _wellness_last_dot_x = dot_x;
                    _wellness_last_dot_y = dot_y;
                }
                return;
            }

            const uint32_t elapsed = now - _breath_started_ms;
            const uint32_t cycle = elapsed % kBreathCycleMs;
            const lv_point_precise_t inhale_start = _wellness_triangle_points[0];
            const lv_point_precise_t hold_start = _wellness_triangle_points[1];
            const lv_point_precise_t exhale_start = _wellness_triangle_points[2];
            const lv_point_precise_t exhale_end = _wellness_triangle_points[3];
            const char* phase = "吸气";
            uint8_t phase_id = 0;
            uint32_t phase_elapsed = cycle;
            uint32_t phase_total = kBreathInhaleMs;
            lv_point_precise_t from = inhale_start;
            lv_point_precise_t to = hold_start;

            if (cycle >= kBreathInhaleMs + kBreathHoldMs) {
                phase = "呼气";
                phase_id = 2;
                phase_elapsed = cycle - kBreathInhaleMs - kBreathHoldMs;
                phase_total = kBreathExhaleMs;
                from = exhale_start;
                to = exhale_end;
            } else if (cycle >= kBreathInhaleMs) {
                phase = "屏气";
                phase_id = 1;
                phase_elapsed = cycle - kBreathInhaleMs;
                phase_total = kBreathHoldMs;
                from = hold_start;
                to = exhale_start;
            }

            const int remaining = static_cast<int>((phase_total - phase_elapsed + 999) / 1000);
            const float t = std::min(1.0f, static_cast<float>(phase_elapsed) / static_cast<float>(phase_total));
            const int dot_x = static_cast<int>(std::lround(from.x + (to.x - from.x) * t));
            const int dot_y = static_cast<int>(std::lround(from.y + (to.y - from.y) * t));
            char countdown[8];
            std::snprintf(countdown, sizeof(countdown), "%d", std::max(1, remaining));

            if (_wellness_last_phase_id == 0xFF) {
                lv_label_set_text(_wellness_title, "4-7-8");
            }
            if (_wellness_last_phase_id != phase_id) {
                lv_label_set_text(_wellness_body, phase);
                _wellness_last_phase_id = phase_id;
                playBreathPhaseCue(phase_id);
            }
            if (_wellness_last_countdown != std::max(1, remaining)) {
                lv_label_set_text(_wellness_sub, countdown);
                _wellness_last_countdown = std::max(1, remaining);
            }
            if (std::abs(dot_x - _wellness_last_dot_x) >= 2 || std::abs(dot_y - _wellness_last_dot_y) >= 2) {
                lv_obj_set_pos(_wellness_dot, dot_x - 24, dot_y - 24);
                _wellness_last_dot_x = dot_x;
                _wellness_last_dot_y = dot_y;
            }
            return;
        }

        if (_wellness_last_phase_id != 0) {
            lv_label_set_text(_wellness_title, "4-7-8");
            lv_label_set_text(_wellness_body, "吸气");
            _wellness_last_phase_id = 0;
        }
        if (_wellness_last_countdown != 4) {
            lv_label_set_text(_wellness_sub, "4");
            _wellness_last_countdown = 4;
        }
    }

    void updateBoardingPassPage()
    {
        if (!_boarding_qr_ready) {
            lv_label_set_text(_boarding_status, "QR unavailable");
        }
    }

    void setFullscreenMode(bool fullscreen)
    {
        if (fullscreen && !_fullscreen_mode_active) {
            clearDirectDrawLayer();
            lv_obj_set_size(_pet_touch_zone, 466, 466);
            lv_obj_align(_pet_touch_zone, LV_ALIGN_CENTER, 0, 0);
        } else if (!fullscreen && _fullscreen_mode_active) {
            clearDirectDrawLayer();
            lv_obj_set_size(_pet_touch_zone, 246, 220);
            lv_obj_align(_pet_touch_zone, LV_ALIGN_TOP_MID, 0, 18);
        }
        _fullscreen_mode_active = fullscreen;
        setHidden(_usage_title, fullscreen || _page != 0);
        setHidden(_usage_primary, fullscreen || _page != 0);
        setHidden(_usage_secondary, fullscreen || _page != 0);
        setHidden(_dot_active, fullscreen || _page != 0);
        setHidden(_dot_pending, fullscreen || _page != 0);
        setHidden(_active_label, fullscreen || _page != 0);
        setHidden(_pending_label, fullscreen || _page != 0);
        setHidden(_active_count_label, fullscreen || _page != 0);
        setHidden(_pending_count_label, fullscreen || _page != 0);
        setHidden(_detail_title, true);
        setHidden(_detail_status, true);
        setHidden(_detail_body, true);
        setHidden(_detail_hint, true);
        setHidden(_pet_touch_zone, _prompt_mode_active || _page != 0);
        if (fullscreen) {
            setPetMenuMode(false);
            setWellnessMode(false);
            setBoardingPassMode(false);
        }
    }

    static void onApproveClicked(lv_event_t* e)
    {
        static_cast<CodexBuddyView*>(lv_event_get_user_data(e))->approve();
    }

    static void onDenyClicked(lv_event_t* e)
    {
        static_cast<CodexBuddyView*>(lv_event_get_user_data(e))->deny();
    }

    static void onPetClicked(lv_event_t* e)
    {
        static_cast<CodexBuddyView*>(lv_event_get_user_data(e))->petClicked();
    }

    static void onPetPrevClicked(lv_event_t* e)
    {
        static_cast<CodexBuddyView*>(lv_event_get_user_data(e))->previousPetOption();
    }

    static void onPetNextClicked(lv_event_t* e)
    {
        static_cast<CodexBuddyView*>(lv_event_get_user_data(e))->nextPetOption();
    }

    void clearDirectDrawLayer()
    {
        auto& gfx = GetHAL().getDisplay();
        gfx.startWrite();
        gfx.fillRect(0, 0, gfx.width(), gfx.height(), 0);
        gfx.endWrite();
        lv_obj_invalidate(_root);
    }
};

}  // namespace

extern "C" bool codex_buddy_handle_serial_json(const char* line, void (*reply)(const char*))
{
    cJSON* doc = cJSON_Parse(line);
    if (doc != nullptr) {
        const cJSON* cmd = cJSON_GetObjectItem(doc, "cmd");
        if (cJSON_IsString(cmd) && std::strcmp(cmd->valuestring, "ui_page") == 0) {
            const cJSON* page = cJSON_GetObjectItem(doc, "page");
            if (cJSON_IsNumber(page)) {
                g_debug_page = static_cast<uint8_t>(std::max<int>(0, page->valueint));
                if (reply != nullptr) {
                    reply("{\"ack\":\"ui_page\",\"ok\":true}\n");
                }
                cJSON_Delete(doc);
                return true;
            }
        }
        if (cJSON_IsString(cmd) && std::strcmp(cmd->valuestring, "ui_action") == 0) {
            const cJSON* action = cJSON_GetObjectItem(doc, "action");
            g_debug_action = cJSON_IsNumber(action) ? static_cast<uint8_t>(std::max<int>(1, action->valueint)) : 1;
            if (reply != nullptr) {
                reply("{\"ack\":\"ui_action\",\"ok\":true}\n");
            }
            cJSON_Delete(doc);
            return true;
        }
        if (cJSON_IsString(cmd) && std::strcmp(cmd->valuestring, "ui_status") == 0) {
            if (reply != nullptr) {
                char out[64];
                std::snprintf(out, sizeof(out), "{\"ack\":\"ui_status\",\"page\":%u}\n",
                              static_cast<unsigned>(g_debug_current_page));
                reply(out);
            }
            cJSON_Delete(doc);
            return true;
        }
        cJSON_Delete(doc);
    }
    return CodexBuddyBle::instance().handleSerialJson(line, reply);
}

AppCodexBuddy::AppCodexBuddy()
{
    setAppInfo().name = "CodexBuddy";
    setAppInfo().icon = (void*)&icon_codex_buddy;
    static uint32_t color = 0x37D67A;
    setAppInfo().userData = &color;
}

void AppCodexBuddy::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
    dynamicPetPack().load();
    CodexBuddyBle::instance().start();
}

void AppCodexBuddy::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    _key_manager = std::make_unique<input::KeyManager>();
    _page = 0;
    _fullscreen_lvgl_paused = false;
    _touch_was_down = false;
    _screen_dimmed = false;
    _ignore_next_power_click = false;
    _last_screen_activity_ms = GetHAL().millis();
    _last_imu_poll_ms = 0;
    _last_shake_ms = 0;
    _shake_window_start_ms = 0;
    _last_strong_shake_ms = 0;
    _last_shake_feedback_ms = 0;
    _shake_strong_samples = 0;
    _has_accel_sample = false;
    _screen_brightness_before_dim = std::max(10, GetHAL().getBackLightBrightness(false));
    auto button_config = GetHAL().getButtonConfig(false);
    button_config.sfxEnabled = false;
    GetHAL().setButtonConfig(button_config, false);

    LvglLockGuard lock;
    auto* view = new CodexBuddyView();
    view->init(lv_screen_active());
    _view = view;
}

void AppCodexBuddy::onRunning()
{
    const uint32_t now = GetHAL().millis();
    const auto state = CodexBuddyBle::instance().snapshot();
    auto* view = static_cast<CodexBuddyView*>(_view);
    input::KeyEvent event = input::KeyEvent::None;

    if (_key_manager) {
        event = _key_manager->update();
    }

    const auto touch = GetHAL().getTouchPoint();
    const bool touch_down = touch.num > 0;
    const bool power_clicked = GetHAL().btnPwr.wasClicked();
    const bool power_pressed = GetHAL().btnPwr.wasPressed();
    const bool power_hold = GetHAL().btnPwr.wasHold() || GetHAL().btnPwr.isHolding();
    const bool power_released = GetHAL().btnPwr.wasReleased() || GetHAL().btnPwr.wasReleasedAfterHold();
    const bool power_active = power_clicked || power_pressed || power_hold || power_released || GetHAL().btnPwr.isPressed();
    const bool input_active = event != input::KeyEvent::None || touch_down || power_active;
    const bool attention_required = state.repair_pairing || state.passkey != 0 || state.prompt_id[0] != 0;
    const bool breathing_blocks_auto_dim = view != nullptr && view->breathingRunning();

    CodexBuddyBle::instance().flushPendingPetRequest();

    auto wakeForDebug = [&]() {
        if (_screen_dimmed) {
            GetHAL().setBackLightBrightness(_screen_brightness_before_dim, false);
            _screen_dimmed = false;
            _touch_was_down = touch_down;
        }
    };

    auto resumePausedDirectDraw = [&]() {
        _shake_animation_active = false;
        if (_fullscreen_lvgl_paused) {
            GetHAL().startLvglUpdate();
            _fullscreen_lvgl_paused = false;
            _touch_was_down = false;
        }
    };

    if (view != nullptr && g_debug_page != 255) {
        wakeForDebug();
        view->setPage(g_debug_page);
        _page = view->page();
        g_debug_current_page = _page;
        g_debug_page = 255;
        _last_screen_activity_ms = now;
    }

    if (view != nullptr && g_debug_action != 0) {
        wakeForDebug();
        const uint8_t action = g_debug_action;
        g_debug_action = 0;
        if (action == 2 && view->petPickerActive()) {
            view->requestSelectedPet();
        } else if (action == 2 && view->settingsActive()) {
            view->openSettingsOption();
        } else if (action == 3 && view->petPickerActive()) {
            view->nextPetOption();
        } else if (action == 4 && view->petPickerActive()) {
            view->previousPetOption();
        } else if (action == 5) {
            view->triggerFortune(now);
        } else if (view->wellnessActive()) {
            view->activateWellnessAction();
        }
        _last_screen_activity_ms = now;
    }

    if (_screen_dimmed) {
        if (input_active || attention_required) {
            GetHAL().setBackLightBrightness(_screen_brightness_before_dim, false);
            _screen_dimmed = false;
            _last_screen_activity_ms = now;
            _touch_was_down = touch_down;
            if (power_active) {
                _ignore_next_power_click = true;
            }
            mclog::tagInfo(getAppInfo().name, "screen restored");
        }
        return;
    }

    if (_ignore_next_power_click) {
        if (power_clicked || power_released) {
            _ignore_next_power_click = false;
            _last_screen_activity_ms = now;
            return;
        }
        if (!GetHAL().btnPwr.isPressed()) {
            _ignore_next_power_click = false;
        }
    }

    if (power_clicked && !attention_required) {
        _screen_brightness_before_dim = std::max(10, GetHAL().getBackLightBrightness(false));
        GetHAL().setBackLightBrightness(0, false);
        _screen_dimmed = true;
        mclog::tagInfo(getAppInfo().name, "screen dimmed by power button");
        return;
    }

    if (input_active || attention_required) {
        _last_screen_activity_ms = now;
    } else if (!breathing_blocks_auto_dim && now - _last_screen_activity_ms >= kAutoScreenOffMs) {
        _screen_brightness_before_dim = std::max(10, GetHAL().getBackLightBrightness(false));
        GetHAL().setBackLightBrightness(0, false);
        _screen_dimmed = true;
        mclog::tagInfo(getAppInfo().name, "screen dimmed after idle");
        return;
    }

    if (event == input::KeyEvent::GoHome && view != nullptr) {
        if (view->fortuneResultVisible()) {
            view->dismissFortune();
            resumePausedDirectDraw();
            resetShakeDetector(now);
        }
        view->setPage(0);
        _page = view->page();
        _last_screen_activity_ms = now;
        return;
    }

    if (view != nullptr && view->fortuneResultVisible()) {
        if (event == input::KeyEvent::GoNext) {
            view->dismissFortune();
            resumePausedDirectDraw();
            resetShakeDetector(now);
            _last_screen_activity_ms = now;
            return;
        }
    }

    if (_key_manager) {
        if (state.prompt_id[0] != 0 && view != nullptr) {
            if (event == input::KeyEvent::GoPrevious) {
                view->approve();
            } else if (event == input::KeyEvent::GoNext) {
                view->deny();
            }
        } else if (state.repair_pairing) {
            // Keep the re-pair prompt stable while the user fixes the Mac bond.
        } else if (view != nullptr) {
            if (view->settingsActive()) {
                if (event == input::KeyEvent::GoPrevious) {
                    if (view->petPickerActive()) {
                        view->requestSelectedPet();
                    } else {
                        view->openSettingsOption();
                    }
                } else if (event == input::KeyEvent::GoNext) {
                    if (view->petPickerActive()) {
                        view->closePetPicker();
                    } else {
                        view->nextPage();
                        _page = view->page();
                    }
                }
            } else if (view->wellnessActive()) {
                if (event == input::KeyEvent::GoPrevious) {
                    view->activateWellnessAction();
                } else if (event == input::KeyEvent::GoNext) {
                    view->nextPage();
                    _page = view->page();
                }
            } else if (event == input::KeyEvent::GoPrevious) {
                view->previousPage();
                _page = view->page();
            } else if (event == input::KeyEvent::GoNext) {
                view->nextPage();
                _page = view->page();
            }
        }
    }

    if (view != nullptr && _page == 0 && !attention_required && !view->fortunePending() &&
        !view->fortuneResultVisible()) {
        const ShakeDetectorState shake_state = updateShakeDetector(now);
        if (shake_state == ShakeDetectorState::Active) {
            if (!_shake_animation_active) {
                _shake_animation_active = true;
                view->beginShakeAnimation();
            }
            _last_screen_activity_ms = now;
        } else if (shake_state == ShakeDetectorState::Completed) {
            _last_shake_ms = now;
            _last_screen_activity_ms = now;
            _shake_animation_active = false;
            if (_fullscreen_lvgl_paused) {
                GetHAL().startLvglUpdate();
                _fullscreen_lvgl_paused = false;
            }
            view->endShakeAnimation();
            view->showFortuneResult(now);
            mclog::tagInfo(getAppInfo().name, "shake completed");
        } else if (shake_state == ShakeDetectorState::Cancelled) {
            _shake_animation_active = false;
            view->endShakeAnimation();
            if (_fullscreen_lvgl_paused) {
                GetHAL().startLvglUpdate();
                _fullscreen_lvgl_paused = false;
            }
            mclog::tagInfo(getAppInfo().name, "shake cancelled");
        }
    } else if (_shake_animation_active && view != nullptr) {
        _shake_animation_active = false;
        view->endShakeAnimation();
        resetShakeDetector(now);
    }

    const bool fullscreen = view != nullptr && _page == 1 && !state.repair_pairing && state.passkey == 0 &&
                            state.prompt_id[0] == 0;
    const bool fortune_direct = view != nullptr && view->fortuneResultVisible();
    const bool direct_draw = fullscreen || fortune_direct || _shake_animation_active;
    if (direct_draw && !_fullscreen_lvgl_paused) {
        {
            LvglLockGuard lock;
            if (_shake_animation_active) {
                view->beginShakeAnimation();
            } else {
                view->update(state, _page);
            }
            lv_refr_now(nullptr);
        }
        GetHAL().stopLvglUpdate();
        if (fortune_direct) {
            view->invalidateFortuneMascot();
        }
        _fullscreen_lvgl_paused = true;
    } else if (!direct_draw && _fullscreen_lvgl_paused) {
        GetHAL().startLvglUpdate();
        _fullscreen_lvgl_paused = false;
        _touch_was_down = false;
    }

    if (fullscreen && view != nullptr) {
        if (touch_down && !_touch_was_down) {
            view->petClicked();
        }
        _touch_was_down = touch_down;
    }

    if (view != nullptr) {
        if (_shake_animation_active) {
            view->renderShakeAnimation(state);
        } else if (direct_draw) {
            view->renderPetAsset(state, _page);
        } else {
            LvglLockGuard lock;
            view->update(state, _page);
            lv_refr_now(nullptr);
            view->renderPetAsset(state, _page);
        }
        if (view->consumeFortuneDismissed()) {
            _shake_animation_active = false;
            resetShakeDetector(now);
        }
    }
}

AppCodexBuddy::ShakeDetectorState AppCodexBuddy::updateShakeDetector(uint32_t now)
{
    if (now - _last_shake_ms < kShakeCooldownMs || now - _last_imu_poll_ms < kImuPollIntervalMs) {
        return _shake_window_start_ms != 0 ? ShakeDetectorState::Active : ShakeDetectorState::Idle;
    }
    _last_imu_poll_ms = now;
    GetHAL().updateImuData();
    const auto& imu = GetHAL().getImuData();
    const float dx = imu.accelX - _last_accel_x;
    const float dy = imu.accelY - _last_accel_y;
    const float dz = imu.accelZ - _last_accel_z;
    const float jerk = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float magnitude = std::sqrt(imu.accelX * imu.accelX + imu.accelY * imu.accelY + imu.accelZ * imu.accelZ);

    _last_accel_x = imu.accelX;
    _last_accel_y = imu.accelY;
    _last_accel_z = imu.accelZ;

    if (!_has_accel_sample) {
        _has_accel_sample = true;
        return ShakeDetectorState::Idle;
    }

    const bool strong = jerk > 1.75f || magnitude > 2.75f || magnitude < 0.22f;
    if (!strong) {
        if (_shake_window_start_ms == 0) {
            return ShakeDetectorState::Idle;
        }
        if (now - _last_strong_shake_ms <= kShakeContinuityMs) {
            return ShakeDetectorState::Active;
        }

        const bool long_enough = _last_strong_shake_ms - _shake_window_start_ms >= kShakeRequiredMs;
        const bool enough_motion = _shake_strong_samples >= kShakeRequiredStrongSamples;
        const ShakeDetectorState result = long_enough && enough_motion ? ShakeDetectorState::Completed
                                                                       : ShakeDetectorState::Cancelled;
        _shake_window_start_ms = 0;
        _last_strong_shake_ms = 0;
        _last_shake_feedback_ms = 0;
        _shake_strong_samples = 0;
        return result;
    }

    if (_shake_window_start_ms == 0 || now - _last_strong_shake_ms > kShakeContinuityMs) {
        if (_shake_window_start_ms != 0) {
            _shake_window_start_ms = 0;
            _last_strong_shake_ms = 0;
            _last_shake_feedback_ms = 0;
            _shake_strong_samples = 0;
        }
        _shake_window_start_ms = now;
        _shake_strong_samples = 0;
        _last_shake_feedback_ms = 0;
    }

    _last_strong_shake_ms = now;
    if (_shake_strong_samples < 255) {
        ++_shake_strong_samples;
    }

    const bool long_enough = now - _shake_window_start_ms >= kShakeRequiredMs;
    const bool enough_motion = _shake_strong_samples >= kShakeRequiredStrongSamples;
    if (long_enough && enough_motion) {
        _shake_window_start_ms = 0;
        _last_strong_shake_ms = 0;
        _last_shake_feedback_ms = 0;
        _shake_strong_samples = 0;
        return ShakeDetectorState::Completed;
    }

    if (_last_shake_feedback_ms == 0 || now - _last_shake_feedback_ms >= kShakeFeedbackIntervalMs) {
        GetHAL().vibrate(55, 75);
        _last_shake_feedback_ms = now;
    }

    return ShakeDetectorState::Active;
}

void AppCodexBuddy::resetShakeDetector(uint32_t now)
{
    _shake_animation_active = false;
    _last_shake_ms = now;
    _last_imu_poll_ms = now;
    _shake_window_start_ms = 0;
    _last_strong_shake_ms = 0;
    _last_shake_feedback_ms = 0;
    _shake_strong_samples = 0;
    GetHAL().updateImuData();
    const auto& imu = GetHAL().getImuData();
    _last_accel_x = imu.accelX;
    _last_accel_y = imu.accelY;
    _last_accel_z = imu.accelZ;
    _has_accel_sample = true;
}

void AppCodexBuddy::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    _key_manager.reset();
    if (_screen_dimmed) {
        GetHAL().setBackLightBrightness(_screen_brightness_before_dim, false);
        _screen_dimmed = false;
    }
    if (_fullscreen_lvgl_paused) {
        GetHAL().startLvglUpdate();
        _fullscreen_lvgl_paused = false;
    }

    LvglLockGuard lock;
    delete static_cast<CodexBuddyView*>(_view);
    _view = nullptr;
}
