/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <memory>

class AppCodexBuddy : public mooncake::AppAbility {
public:
    AppCodexBuddy();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class ShakeDetectorState : uint8_t {
        Idle,
        Active,
        Completed,
        Cancelled,
    };

    ShakeDetectorState updateShakeDetector(uint32_t now);
    void resetShakeDetector(uint32_t now);

    std::unique_ptr<input::KeyManager> _key_manager;
    void* _view = nullptr;
    uint8_t _page = 0;
    bool _fullscreen_lvgl_paused = false;
    bool _shake_animation_active = false;
    bool _touch_was_down = false;
    bool _screen_dimmed = false;
    bool _ignore_next_power_click = false;
    uint32_t _last_screen_activity_ms = 0;
    uint32_t _last_imu_poll_ms = 0;
    uint32_t _last_shake_ms = 0;
    uint32_t _shake_window_start_ms = 0;
    uint32_t _last_strong_shake_ms = 0;
    uint32_t _last_shake_feedback_ms = 0;
    float _last_accel_x = 0.0f;
    float _last_accel_y = 0.0f;
    float _last_accel_z = 0.0f;
    uint8_t _shake_strong_samples = 0;
    int _screen_brightness_before_dim = 80;
    bool _has_accel_sample = false;
};
