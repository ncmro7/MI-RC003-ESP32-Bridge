#include "usb_composite.h"
#include "usb_descriptors.h"
#include "uac_microphone.h"
#include "hid_bridge.h"
#include "keymap/key_state_machine.h"
#include "webusb_transport.h"
#include "app_config.h"
#include "app_log.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_usb_mounted = false;
static TaskHandle_t s_usb_recovery_task = NULL;
static volatile bool s_usb_recovering = false;
extern key_mapper_engine_t g_key_engine;

static void usb_restore_hid_transport(void)
{
    s_usb_mounted = true;
    hid_bridge_set_transport_enabled(true);
    usb_hid_keyboard_release();
    usb_hid_consumer_release();
    usb_hid_mouse_buttons_release();
}

static void usb_recovery_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_usb_recovering) {
            continue;
        }
        s_usb_recovering = true;

        // Let the host controller begin its resume sequence before toggling D+.
        vTaskDelay(pdMS_TO_TICKS(100));

        // A resume event can arrive before TinyUSB's mounted/suspended flags
        // settle. Never abandon recovery here: doing so leaves HID transport
        // disabled forever while WebUSB remains visible.
        for (int i = 0; i < 50 && tud_suspended(); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        app_log("USB", "Resume recovery: forcing re-enumeration (mounted=%d suspended=%d)",
                tud_mounted(), tud_suspended());
        s_usb_mounted = false;
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(250));
        tud_connect();

        bool active = false;
        for (int i = 0; i < 300; i++) {
            if (tud_mounted() && !tud_suspended()) {
                active = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (active) {
            usb_restore_hid_transport();
            app_log("USB", "Resume recovery complete: HID transport restored");
        } else {
            app_log("USB", "Resume recovery waiting for host enumeration");
        }

        // Drop duplicate resume notifications raised during recovery.
        ulTaskNotifyTake(pdTRUE, 0);
        s_usb_recovering = false;
    }
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            // Queue neutral reports for all HID collections. Failed reports
            // remain pending in hid_bridge and are retried after enumeration.
            usb_restore_hid_transport();
            app_log("USB", "Device mounted (enumerated by host)");
            break;
        case TINYUSB_EVENT_DETACHED:
            s_usb_mounted = false;
            hid_bridge_set_transport_enabled(false);
            uac_microphone_stop_usb_stream();
            app_log("USB", "Device unmounted");
            break;
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
        case TINYUSB_EVENT_SUSPENDED:
            // Clear the BLE/key-engine held state as the bus enters suspend.
            // Otherwise a key pressed immediately before sleep can remain
            // latched in Windows until the device is physically removed.
            key_engine_release_all(&g_key_engine,
                                   (uint32_t)(esp_timer_get_time() / 1000));
            usb_hid_keyboard_release();
            usb_hid_consumer_release();
            usb_hid_mouse_buttons_release();
            hid_bridge_set_transport_enabled(false);
            uac_microphone_stop_usb_stream();
            app_log("USB", "Host suspended the bus");
            break;
#endif
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
        case TINYUSB_EVENT_RESUMED:
            app_log("USB", "Host resumed the bus (PC wake)");
            hid_bridge_set_transport_enabled(false);
            if (s_usb_recovery_task && !s_usb_recovering) {
                xTaskNotifyGive(s_usb_recovery_task);
            }
            break;
#endif
        default:
            break;
    }
}

bool usb_composite_init(void)
{
    hid_bridge_init();
    webusb_transport_init();
    uac_microphone_init();
    usb_descriptors_init();

    if (xTaskCreatePinnedToCore(usb_recovery_task, "usb_recover", 3072, NULL,
                                PRIO_TASK_USB + 1, &s_usb_recovery_task,
                                TASK_CORE_USB) != pdPASS) {
        app_log("USB", "Failed to create USB recovery task");
        return false;
    }

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb, NULL);
    // WebUSB keymap JSON parsing runs in the TinyUSB task context; the default
    // 4 KB stack is not enough for a full multi-layer keymap.
    cfg.task.size = 10240;
    cfg.descriptor.device = &usb_device_descriptor;
    cfg.descriptor.string = usb_string_descriptors;
    cfg.descriptor.string_count = usb_string_descriptor_count;
    cfg.descriptor.full_speed_config = usb_config_descriptor;

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        app_log("USB", "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    app_log("USB", "Composite device ready: UAC mic + HID + WebUSB");
    return true;
}

void usb_composite_task(void)
{
    uac_microphone_task();
}

bool usb_composite_is_mounted(void)
{
    return s_usb_mounted;
}
