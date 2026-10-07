#include <stdio.h>

#include "app_config.h"
#include "version.h"
#include "log/app_log.h"
#include "storage/config_store.h"
#include "led/led_indicator.h"
#include "audio/audio_pipeline.h"
#include "keymap/key_state_machine.h"
#include "keymap/key_config_storage.h"
#include "remote/remote_profile.h"
#include "usb/usb_composite.h"
#include "usb/hid_bridge.h"
#include "ble/ble_remote_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

key_mapper_engine_t g_key_engine;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Core 0: NimBLE host supervision + key engine timing.
static void bridge_task(void *arg)
{
    (void)arg;
    app_log("SYSTEM", "BLE/key task running on core %d", xPortGetCoreID());
    while (1) {
        ble_remote_task();
        key_engine_tick(&g_key_engine, now_ms());
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    // 1. Persistent storage (NVS with auto-recovery).
    ESP_ERROR_CHECK(config_store_init());

    // Select the remote profile (model-independent key normalization) before
    // the key engine starts consuming canonical key codes.
    remote_profile_init();

    // 2. Logging + on-board RGB LED.
    app_log_init();
    led_indicator_init();

    app_log("SYSTEM", "==================================================");
    app_log("SYSTEM", " %s v%s (%s)", FIRMWARE_NAME, FIRMWARE_VERSION, HARDWARE_TARGET);
    app_log("SYSTEM", " Build: %s", FIRMWARE_BUILD);
    app_log("SYSTEM", " Xiaomi Remote 2 Pro (RC003) -> USB bridge + WebUSB");
    app_log("SYSTEM", "==================================================");

    // 3. Audio pipeline (must exist before USB starts streaming).
    audio_pipeline_init(&g_audio_pipeline);
    app_log("INIT", "Audio pipeline ready (16 kHz, 16-bit mono, UAC 1.0)");

    // 4. Key engine with the USB HID dispatcher, then restore the keymap.
    key_engine_init(&g_key_engine, usb_hid_dispatch_action);
    key_config_storage_init(&g_key_engine);
    app_log("INIT", "Key engine active (%u layers, layer 0 has %u bindings)",
            (unsigned)g_key_engine.layer_count, (unsigned)g_key_engine.layers[0].binding_count);

    // 5. USB composite device: UAC microphone + HID + WebUSB configuration channel.
    if (!usb_composite_init()) {
        app_log("USB", "FATAL: failed to start USB composite device");
    }

    // 6. BLE central.
    ble_remote_init();

    // 7. Background task for BLE supervision + key timing.
    xTaskCreatePinnedToCore(bridge_task, "bridge", 8192, NULL, PRIO_TASK_BLE, NULL, TASK_CORE_BLE);

    app_log("SYSTEM", "Initialization complete. Open the WebUSB config site and click Connect.");
}
