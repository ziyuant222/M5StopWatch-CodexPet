file(GLOB_RECURSE MY_SRCS
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "assets/*.c"
    "assets/*.cc"
    "assets/*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
)

if(DEFINED ENV{CODEX_BUDDY_ONLY} AND "$ENV{CODEX_BUDDY_ONLY}" STREQUAL "1")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_alarm_clock/.*")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_badge/.*")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_fft/.*")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_imu/.*")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_lucky_wheel/.*")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_stopwatch/.*")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_template/.*")
    list(FILTER MY_SRCS EXCLUDE REGEX ".*/apps/app_watch_face/.*")
    set(CODEX_BUDDY_ONLY_BUILD ON)
else()
    set(CODEX_BUDDY_ONLY_BUILD OFF)
endif()

set(MY_INCS
    "."
)

idf_component_register(
    SRCS 
        "main.cpp"
        ${MY_SRCS}
    INCLUDE_DIRS 
        ${MY_INCS}
    REQUIRES
        ArduinoJson
        BMI270_BMM150_Sensor
        M5GFX
        M5IOE1
        M5PM1
        bt
        cjson
        fatfs
        lvgl
        mbedtls
        mooncake
        mooncake_log
        nvs_flash
        smooth_ui_toolkit
    EMBED_FILES
        "assets/sfx/boot_sfx.bin"
    EMBED_TXTFILES
        "hal/utils/config_ap/assets/badge_config_ap.html"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=deprecated-enum-enum-conversion
    -Wno-error=missing-field-initializers
)

if(CODEX_BUDDY_ONLY_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CODEX_BUDDY_ONLY=1)
endif()
