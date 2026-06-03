/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <hal/utils/settings/settings.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lv_demos.h>
#include <apps/common/audio/audio.h>
#include <driver/usb_serial_jtag.h>
#include <esp_err.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" bool codex_buddy_handle_serial_json(const char* line, void (*reply)(const char*));

namespace {

constexpr const char* STARTUP_APP_KEY = "startup_app";
constexpr const char* STARTUP_APP_DEFAULT = "CodexBuddy";
constexpr const char* STARTUP_APP_LAUNCHER = "Launcher";
constexpr const char* SYSTEM_SETTINGS_NS = "system";

int find_app_id_by_name(const std::string& app_name)
{
    for (const auto& props : GetMooncake().getAllAppProps()) {
        if (props.info.name == app_name) {
            return props.appID;
        }
    }
    return -1;
}

constexpr char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < len ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < len ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(BASE64_TABLE[(triple >> 18) & 0x3F]);
        out.push_back(BASE64_TABLE[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? BASE64_TABLE[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? BASE64_TABLE[triple & 0x3F] : '=');
    }
    return out;
}

void debug_serial_write(const char* data)
{
    if (data == nullptr || !usb_serial_jtag_is_driver_installed()) {
        return;
    }
    const size_t len = std::strlen(data);
    size_t written = 0;
    while (written < len) {
        const int rc = usb_serial_jtag_write_bytes(data + written, len - written, pdMS_TO_TICKS(1000));
        if (rc <= 0) {
            break;
        }
        written += static_cast<size_t>(rc);
    }
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
}

void dump_screen_rgb565()
{
    auto& gfx = GetHAL().getDisplay();
    const int width = gfx.width();
    const int height = gfx.height();
    std::vector<uint16_t> row(width);
    char line[1536];

    GetHAL().stopLvglUpdate();
    GetHAL().lvglLock();

    std::snprintf(line, sizeof(line), "__CODEX_SCREENSHOT_BEGIN__ width=%d height=%d format=swap565le\n", width, height);
    debug_serial_write(line);
    for (int y = 0; y < height; ++y) {
        gfx.readRect(0, y, width, 1, row.data());
        const auto encoded = base64_encode(reinterpret_cast<const uint8_t*>(row.data()), row.size() * sizeof(uint16_t));
        std::snprintf(line, sizeof(line), "__CODEX_SCREENSHOT_ROW__ y=%d data=%s\n", y, encoded.c_str());
        debug_serial_write(line);
    }
    debug_serial_write("__CODEX_SCREENSHOT_END__\n");

    GetHAL().lvglUnlock();
    GetHAL().startLvglUpdate();
}

bool install_debug_serial()
{
    if (usb_serial_jtag_is_driver_installed()) {
        return true;
    }

    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = 4096;
    config.tx_buffer_size = 4096;
    const esp_err_t rc = usb_serial_jtag_driver_install(&config);
    if (rc != ESP_OK) {
        std::printf("usb serial jtag debug install failed: %s\n", esp_err_to_name(rc));
        return false;
    }
    return true;
}

void serial_debug_task(void*)
{
    if (!install_debug_serial()) {
        vTaskDelete(nullptr);
        return;
    }

    char line[2048];
    size_t len = 0;
    while (true) {
        char ch = 0;
        const int rc = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (rc <= 0) {
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            line[len] = 0;
            if (std::strcmp(line, "codex:screenshot") == 0) {
                dump_screen_rgb565();
            } else if (std::strncmp(line, "{\"cmd\":", 7) == 0) {
                codex_buddy_handle_serial_json(line, debug_serial_write);
            }
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = ch;
        } else {
            len = 0;
        }
    }
}

}  // namespace

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Install apps
    auto launcher = std::make_unique<AppLauncher>();
    auto* launcher_ptr = launcher.get();
    GetMooncake().installApp(std::move(launcher));
#ifndef CODEX_BUDDY_ONLY
    GetMooncake().installApp(std::make_unique<AppAlarmClock>());
    GetMooncake().installApp(std::make_unique<AppWatchFace>());
    GetMooncake().installApp(std::make_unique<AppStopWatch>());
    GetMooncake().installApp(std::make_unique<AppBadge>());
    GetMooncake().installApp(std::make_unique<AppImu>());
    GetMooncake().installApp(std::make_unique<AppFft>());
    GetMooncake().installApp(std::make_unique<AppLuckyWheel>());
#endif
    GetMooncake().installApp(std::make_unique<AppCodexBuddy>());
    GetMooncake().installApp(std::make_unique<AppSetup>());
    // GetMooncake().installApp(std::make_unique<AppTemplate>());

    Settings settings(SYSTEM_SETTINGS_NS, false);
    const auto startup_app = settings.GetString(STARTUP_APP_KEY, STARTUP_APP_DEFAULT);
    if (startup_app != STARTUP_APP_LAUNCHER) {
        const int startup_app_id = find_app_id_by_name(startup_app);
        if (startup_app_id >= 0) {
            launcher_ptr->setStartupAppId(startup_app_id);
        }
    }

    xTaskCreate(serial_debug_task, "serial_debug_task", 12288, nullptr, 1, nullptr);

    // Main loop
    while (1) {
        GetHAL().feedTheDog();
        GetMooncake().update();
    }
}
