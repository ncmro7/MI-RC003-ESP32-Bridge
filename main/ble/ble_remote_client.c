#include "ble_remote_client.h"
#include "app_config.h"
#include "app_log.h"
#include "audio/audio_pipeline.h"
#include "keymap/key_state_machine.h"
#include "led/led_indicator.h"
#include "remote/remote_profile.h"
#include "storage/config_store.h"
#include "usb/hid_bridge.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

// Provided by the NimBLE store configuration but not declared in a public header.
void ble_store_config_init(void);

// ===========================================================================
// UUIDs
// ===========================================================================
static const ble_uuid128_t s_uuid_atvv_svc =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x01, 0x00, 0x5e, 0xab);
static const ble_uuid128_t s_uuid_atvv_cmd =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x02, 0x00, 0x5e, 0xab);
static const ble_uuid128_t s_uuid_atvv_aud =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x03, 0x00, 0x5e, 0xab);
static const ble_uuid128_t s_uuid_atvv_ctl =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x04, 0x00, 0x5e, 0xab);
static const ble_uuid16_t s_uuid_hogp_svc = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t s_uuid_hid_report = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t s_uuid_hid_proto_mode = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t s_uuid_hid_ctrl_point = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t s_uuid_batt_level = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t s_uuid_batt_status = BLE_UUID16_INIT(0x2BED);
static const ble_uuid16_t s_uuid_model_number = BLE_UUID16_INIT(0x2A24);

// ===========================================================================
// State
// ===========================================================================
static ble_remote_state_t s_state = BLE_STATE_DISCONNECTED;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type = 0;

// Discovered characteristic handles
static uint16_t s_atvv_cmd_chr = 0;
static uint16_t s_atvv_aud_chr = 0;
static uint16_t s_atvv_ctl_chr = 0;
#define MAX_REPORTS 6
static uint16_t s_hid_report_chrs[MAX_REPORTS];
static int s_hid_report_count = 0;
static uint16_t s_hid_proto_chr = 0;
static uint16_t s_hid_ctrl_chr = 0;
static uint16_t s_batt_level_chr = 0;
static uint16_t s_batt_status_chr = 0;
static uint16_t s_model_chr = 0;
static bool     s_model_read = false;
static int      s_battery_level = -1;
static uint32_t s_battery_last_ms = 0;

// How often to re-read the remote's battery level while connected. Kept slow
// to limit extra BLE traffic and power draw.
#define BATTERY_REFRESH_INTERVAL_MS 900000  // 15 minutes
static uint16_t s_atvv_start = 0, s_atvv_end = 0;
static uint16_t s_hid_start = 0, s_hid_end = 0;

static size_t s_frame_size = AUDIO_DEFAULT_FRAME_BYTES;
static uint8_t s_session_id = 0;
static uint32_t s_conn_start_ms = 0;
static bool s_discovery_started = false;

// Bound remote (persisted in NVS)
static char    s_bound_mac[18] = {0};
static char    s_bound_name[40] = {0};
static uint8_t s_bound_type = 1;

// Connected remote info
static char s_connected_mac[18] = {0};
static char s_connected_name[40] = {0};

// Asynchronous requests from other tasks
static volatile bool s_req_unpair = false;
static volatile bool s_req_reconnect = false;
static volatile bool s_do_connect = false;
static char     s_pending_mac[18] = {0};
static uint8_t  s_pending_type = 1;
static char     s_pending_name[40] = {0};

// HOGP pressed-usage tracking
static uint16_t s_pressed[8];
static int      s_pressed_count = 0;

// Discovered device cache
typedef struct {
    char     name[40];
    char     mac[18];
    int8_t   rssi;
    uint8_t  type;
    uint32_t last_seen_ms;
} discovered_t;
static discovered_t s_discovered[BLE_MAX_DISCOVERED];
static int s_discovered_count = 0;
static portMUX_TYPE s_disc_mux = portMUX_INITIALIZER_UNLOCKED;

extern key_mapper_engine_t g_key_engine;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool mac_equals(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'F') ca += 32;
        if (cb >= 'A' && cb <= 'F') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

// ===========================================================================
// Write operation queue (chained GATT procedures)
// ===========================================================================
typedef enum {
    PHASE_IDLE = 0,
    PHASE_SUBSCRIBE,
    PHASE_HID_INIT,
    PHASE_HANDSHAKE
} init_phase_t;

#define MAX_OPS 10
typedef struct {
    uint16_t handle;
    uint8_t  len;
    uint8_t  data[8];
} gatt_op_t;

static gatt_op_t s_ops[MAX_OPS];
static int s_op_count = 0;
static int s_op_idx = 0;
static init_phase_t s_phase = PHASE_IDLE;

static void run_next_op(void);
static uint16_t cccd_lookup(uint16_t chr_val_handle);
static void read_battery(void);
static void read_model_number(void);

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status != 0) {
        app_log("BLE", "GATT write handle 0x%04X failed: %d", s_ops[s_op_idx].handle, error->status);
    }
    s_op_idx++;
    run_next_op();
    return 0;
}

static void run_next_op(void)
{
    if (s_op_idx >= s_op_count) {
        // Phase complete -> advance to the next phase.
        if (s_phase == PHASE_HID_INIT) {
            s_phase = PHASE_SUBSCRIBE;
            s_op_count = 0;
            s_op_idx = 0;

            // ATVV audio + control (simple characteristics, CCCD = value + 1).
            uint16_t atvv[2] = { s_atvv_aud_chr, s_atvv_ctl_chr };
            for (int i = 0; i < 2; i++) {
                uint16_t chr = atvv[i];
                if (!chr) continue;
                uint16_t cccd = cccd_lookup(chr);
                if (!cccd) cccd = chr + 1;
                s_ops[s_op_count].handle = cccd;
                s_ops[s_op_count].data[0] = 0x01; // notifications enabled
                s_ops[s_op_count].data[1] = 0x00;
                s_ops[s_op_count].len = 2;
                s_op_count++;
                app_log("BLE", "Subscribe ATVV chr 0x%04X via CCCD 0x%04X", chr, cccd);
            }

            // HID input reports: use the discovered CCCD; output-only reports
            // have none and are skipped.
            for (int i = 0; i < s_hid_report_count && s_op_count < MAX_OPS; i++) {
                uint16_t chr = s_hid_report_chrs[i];
                uint16_t cccd = cccd_lookup(chr);
                if (!cccd) {
                    app_log("BLE", "Report chr 0x%04X has no CCCD, skipping", chr);
                    continue;
                }
                s_ops[s_op_count].handle = cccd;
                s_ops[s_op_count].data[0] = 0x01;
                s_ops[s_op_count].data[1] = 0x00;
                s_ops[s_op_count].len = 2;
                s_op_count++;
                app_log("BLE", "Subscribe HID report chr 0x%04X via CCCD 0x%04X", chr, cccd);
            }

            run_next_op();
        } else if (s_phase == PHASE_SUBSCRIBE) {
            s_phase = PHASE_HANDSHAKE;
            s_op_count = 0;
            s_op_idx = 0;
            if (s_atvv_cmd_chr) {
                static const uint8_t caps[6] = { 0x0A, 0x01, 0x00, 0x00, 0x03, 0x03 };
                s_ops[s_op_count].handle = s_atvv_cmd_chr;
                memcpy(s_ops[s_op_count].data, caps, sizeof(caps));
                s_ops[s_op_count].len = sizeof(caps);
                s_op_count++;
            }
            run_next_op();
        } else if (s_phase == PHASE_HANDSHAKE) {
            s_phase = PHASE_IDLE;
            s_state = BLE_STATE_CONNECTED;
            led_indicator_set(LED_STATE_CONNECTED);
            app_log("BLE", "Remote ready: HOGP + ATVV initialized");
            read_model_number();
        }
        return;
    }

    gatt_op_t *op = &s_ops[s_op_idx];
    int rc = ble_gattc_write_flat(s_conn_handle, op->handle, op->data, op->len, write_cb, NULL);
    if (rc != 0) {
        app_log("BLE", "ble_gattc_write_flat(0x%04X) rc=%d", op->handle, rc);
        s_op_idx++;
        run_next_op();
    }
}

// Attribute handles discovered from the peer's database.
#define MAX_CCCDS      32
#define MAX_ALL_CHRS   64
static uint16_t s_cccd_handles[MAX_CCCDS];
static int s_cccd_count = 0;
static uint8_t s_dsc_phase = 0;
static uint16_t s_all_chrs[MAX_ALL_CHRS];
static int s_all_chr_count = 0;

static uint16_t next_chr_handle(uint16_t chr)
{
    uint16_t best = 0;
    for (int i = 0; i < s_all_chr_count; i++) {
        uint16_t h = s_all_chrs[i];
        if (h > chr && (best == 0 || h < best)) {
            best = h;
        }
    }
    return best ? best : 0xFFFF;
}

// The CCCD of a characteristic is the first 0x2902 descriptor after its value
// handle and before the next characteristic. NimBLE reports chr_val_handle=0
// for a whole-range descriptor scan, so we cannot key the table by it; instead
// match by handle order. This handles characteristics (e.g. HOGP reports) that
// have a Report Reference descriptor before the CCCD.
static uint16_t cccd_lookup(uint16_t chr)
{
    uint16_t limit = next_chr_handle(chr);
    uint16_t best = 0;
    for (int i = 0; i < s_cccd_count; i++) {
        uint16_t h = s_cccd_handles[i];
        if (h > chr && h < limit && (best == 0 || h < best)) {
            best = h;
        }
    }
    return best;
}

// Start the post-discovery init sequence: HID Control Point / Protocol Mode
// first, then subscribe to notifications, then the ATVV GET_CAPS handshake.
static void begin_init_sequence(void)
{
    s_op_count = 0;
    s_op_idx = 0;
    s_phase = PHASE_HID_INIT;

    if (s_hid_ctrl_chr) {
        s_ops[s_op_count].handle = s_hid_ctrl_chr;
        s_ops[s_op_count].data[0] = 0x00; // exit suspend
        s_ops[s_op_count].len = 1;
        s_op_count++;
    }
    if (s_hid_proto_chr) {
        s_ops[s_op_count].handle = s_hid_proto_chr;
        s_ops[s_op_count].data[0] = 0x01; // report protocol mode
        s_ops[s_op_count].len = 1;
        s_op_count++;
    }

    run_next_op();
}

// Enumerate every descriptor so each notify characteristic can be mapped to
// its real Client Characteristic Configuration Descriptor (0x2902). Some
// characteristics (e.g. the HOGP report) have extra descriptors before the
// CCCD, so chr+1 is not reliable.
static int dsc_all_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (error->status == 0 && dsc) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
            if (s_cccd_count < MAX_CCCDS) {
                s_cccd_handles[s_cccd_count++] = dsc->handle;
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        // Chain the HOGP range after the ATVV range so we never scan all
        // 0xFFFF handles (which took ~11 s on a remote with high slave latency).
        if (s_dsc_phase == 0 && s_hid_start && s_hid_end >= s_hid_start) {
            s_dsc_phase = 1;
            int rc = ble_gattc_disc_all_dscs(conn_handle, s_hid_start, s_hid_end, dsc_all_cb, NULL);
            if (rc == 0) return 0;
            app_log("BLE", "disc HOGP descriptors rc=%d", rc);
        }
        app_log("BLE", "Discovered %d CCCD(s)", s_cccd_count);
        begin_init_sequence();
    }
    return 0;
}

// ===========================================================================
// Characteristic discovery
// ===========================================================================
static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr) {
        if (s_all_chr_count < MAX_ALL_CHRS) {
            s_all_chrs[s_all_chr_count++] = chr->val_handle;
        }
        const ble_uuid_t *u = &chr->uuid.u;
        if (u->type == BLE_UUID_TYPE_128) {
            if (ble_uuid_cmp(u, &s_uuid_atvv_cmd.u) == 0) s_atvv_cmd_chr = chr->val_handle;
            else if (ble_uuid_cmp(u, &s_uuid_atvv_aud.u) == 0) s_atvv_aud_chr = chr->val_handle;
            else if (ble_uuid_cmp(u, &s_uuid_atvv_ctl.u) == 0) s_atvv_ctl_chr = chr->val_handle;
        } else if (u->type == BLE_UUID_TYPE_16) {
            if (ble_uuid_cmp(u, &s_uuid_hid_report.u) == 0) {
                // Only input reports notify; output reports have no CCCD.
                if ((chr->properties & 0x30) && s_hid_report_count < MAX_REPORTS) {
                    s_hid_report_chrs[s_hid_report_count++] = chr->val_handle;
                }
            } else if (ble_uuid_cmp(u, &s_uuid_hid_proto_mode.u) == 0) {
                s_hid_proto_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_hid_ctrl_point.u) == 0) {
                s_hid_ctrl_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_batt_level.u) == 0) {
                s_batt_level_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_batt_status.u) == 0) {
                s_batt_status_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_model_number.u) == 0) {
                s_model_chr = chr->val_handle;
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        app_log("BLE", "Chars: cmd=0x%04X aud=0x%04X ctl=0x%04X reports=%d proto=0x%04X cpoint=0x%04X",
                s_atvv_cmd_chr, s_atvv_aud_chr, s_atvv_ctl_chr,
                s_hid_report_count, s_hid_proto_chr, s_hid_ctrl_chr);
        s_cccd_count = 0;
        s_dsc_phase = 0;
        int rc = -1;
        if (s_atvv_start && s_atvv_end >= s_atvv_start) {
            rc = ble_gattc_disc_all_dscs(conn_handle, s_atvv_start, s_atvv_end, dsc_all_cb, NULL);
        } else if (s_hid_start && s_hid_end >= s_hid_start) {
            s_dsc_phase = 1;
            rc = ble_gattc_disc_all_dscs(conn_handle, s_hid_start, s_hid_end, dsc_all_cb, NULL);
        }
        if (rc != 0) {
            app_log("BLE", "disc_all_dscs rc=%d, subscribing with fallback handles", rc);
            begin_init_sequence();
        }
    }
    return 0;
}

// ===========================================================================
// Service discovery
// ===========================================================================
static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error->status == 0 && svc) {
        const ble_uuid_t *u = &svc->uuid.u;
        if (u->type == BLE_UUID_TYPE_128 && ble_uuid_cmp(u, &s_uuid_atvv_svc.u) == 0) {
            s_atvv_start = svc->start_handle;
            s_atvv_end = svc->end_handle;
            app_log("BLE", "ATVV service: 0x%04X-0x%04X", s_atvv_start, s_atvv_end);
        } else if (u->type == BLE_UUID_TYPE_16 && ble_uuid_cmp(u, &s_uuid_hogp_svc.u) == 0) {
            s_hid_start = svc->start_handle;
            s_hid_end = svc->end_handle;
            app_log("BLE", "HOGP service: 0x%04X-0x%04X", s_hid_start, s_hid_end);
        }
    } else if (error->status == BLE_HS_EDONE) {
        ble_gattc_disc_all_chrs(conn_handle, 1, 0xFFFF, chr_disc_cb, NULL);
    }
    return 0;
}

static void start_discovery(void)
{
    if (s_discovery_started || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    s_discovery_started = true;
    s_atvv_cmd_chr = s_atvv_aud_chr = s_atvv_ctl_chr = 0;
    s_hid_proto_chr = s_hid_ctrl_chr = 0;
    s_hid_report_count = 0;
    s_model_chr = 0;
    s_model_read = false;
    s_atvv_start = s_atvv_end = s_hid_start = s_hid_end = 0;
    s_cccd_count = 0;
    s_all_chr_count = 0;
    app_log("BLE", "Discovering GATT services...");
    ble_gattc_disc_all_svcs(s_conn_handle, svc_disc_cb, NULL);
}

// ===========================================================================
// Notification handling
// ===========================================================================
static void handle_atvv_ctl(const uint8_t *data, size_t len)
{
    if (len < 1) return;
    uint8_t op = data[0];

    if (op == 0x04 && len >= 2 && data[1] == 0x03) {
        s_session_id = (len >= 4) ? data[3] : 0;
        uint8_t codec = (len >= 3) ? data[2] : 0;
        s_state = BLE_STATE_TALKING;
        key_engine_feed_key(&g_key_engine, MI_KEY_VOICE, true, now_ms());
        app_log("ATVV", "Voice start (session %u, codec %u)", s_session_id, codec);
    } else if (op == 0x00 || op == 0x08) {
        if (s_state == BLE_STATE_TALKING) {
            s_state = BLE_STATE_CONNECTED;
            key_engine_feed_key(&g_key_engine, MI_KEY_VOICE, false, now_ms());
            app_log("ATVV", "Voice stop");
        }
    } else if (op == 0x0B && len >= 7) {
        uint16_t ver = (uint16_t)((data[1] << 8) | data[2]);
        uint8_t codecs = (len >= 4) ? data[3] : 0;
        uint16_t fs = (uint16_t)((data[5] << 8) | data[6]);
        if (fs > 0) s_frame_size = fs;
        app_log("ATVV", "Capabilities: ver=0x%04X codecs=0x%02X frame=%u", ver, codecs, (unsigned)s_frame_size);
    } else if (op == 0x0A && len >= 7) {
        int16_t pred = (int16_t)((data[4] << 8) | data[5]);
        int8_t step = (int8_t)data[6];
        audio_pipeline_sync(&g_audio_pipeline, pred, step);
    }
}

static void handle_hid_report(const uint8_t *data, size_t len)
{
    if (len == 0) return;

    // RC003 HOGP input report: report ID 1 followed by an array of 16-bit
    // little-endian keyboard usages (6 or 7 bytes total).
    uint16_t usages[4];
    int usage_count = 0;

    if (len == 8) {
        // Standard 8-byte boot keyboard report: modifiers, reserved, key0..5
        if (data[2] != 0) {
            usages[usage_count++] = data[2];
        }
    } else {
        const uint8_t *p = data;
        size_t n = len;
        if ((n == 7) && (p[0] == 0x01)) {
            p++; n--;
        }
        if ((n % 2) == 0) {
            for (size_t i = 0; i + 1 < n && usage_count < 4; i += 2) {
                uint16_t u = (uint16_t)(p[i] | (p[i + 1] << 8));
                if (u != 0) usages[usage_count++] = u;
            }
        }
    }

    // Diff against the previously pressed set.
    for (int i = 0; i < s_pressed_count; i++) {
        bool still = false;
        for (int j = 0; j < usage_count; j++) {
            if (usages[j] == s_pressed[i]) { still = true; break; }
        }
        if (!still && s_pressed[i] <= 0xFF) {
            key_engine_feed_key(&g_key_engine, (uint8_t)s_pressed[i], false, now_ms());
        }
    }
    for (int j = 0; j < usage_count; j++) {
        bool was = false;
        for (int i = 0; i < s_pressed_count; i++) {
            if (usages[j] == s_pressed[i]) { was = true; break; }
        }
        if (!was && usages[j] <= 0xFF) {
            app_log("HOGP", "Key usage 0x%02X DOWN", usages[j]);
            if (usages[j] == MI_KEY_VOICE_ALT || usages[j] == MI_KEY_VOICE) {
                if (s_atvv_cmd_chr) {
                    static const uint8_t cmd_open[2] = { 0x0C, 0x00 };
                    ble_gattc_write_flat(s_conn_handle, s_atvv_cmd_chr, cmd_open, 2, NULL, NULL);
                }
            }
            key_engine_feed_key(&g_key_engine, (uint8_t)usages[j], true, now_ms());
        }
    }

    s_pressed_count = usage_count;
    for (int i = 0; i < usage_count; i++) {
        s_pressed[i] = usages[i];
    }
}

// ===========================================================================
// Battery level (Battery Service 0x180F, characteristic 0x2A19)
// ===========================================================================
static int battery_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error->status != 0) {
        app_log("BATTERY", "read failed: %d", error->status);
        return 0;
    }
    if (attr && attr->om) {
        uint8_t buf[8];
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(buf)) len = sizeof(buf);
        if (os_mbuf_copydata(attr->om, 0, len, buf) == 0 && len >= 1) {
            s_battery_level = buf[0];
            if (s_battery_level > 100) s_battery_level = 100;
            app_log("BATTERY", "Remote battery: %d%%", s_battery_level);
        }
    }
    return 0;
}

static void read_battery(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_batt_level_chr == 0) {
        return;
    }
    s_battery_last_ms = now_ms();
    ble_gattc_read(s_conn_handle, s_batt_level_chr, battery_read_cb, NULL);
}

// ===========================================================================
// Device model (Device Information Service 0x180A, characteristic 0x2A24)
//
// RC001/RC003 firmware 2671 packs IMA-ADPCM high-nibble-first, while the ARN9
// firmware used by the Bluetooth Remote 2 / 2 Pro packs low-nibble-first.
// Reading the model string lets us pick the correct decode order; otherwise
// the voice stream decodes into noise.
// ===========================================================================
static int model_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error->status != 0) {
        app_log("MODEL", "read failed: %d", error->status);
        read_battery();
        return 0;
    }
    if (attr && attr->om) {
        char buf[48];
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
        if (os_mbuf_copydata(attr->om, 0, len, buf) == 0) {
            buf[len] = '\0';
            app_log("MODEL", "Remote model: %s", buf);
            for (char *p = buf; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            }
            if (strstr(buf, "ARN9")) {
                audio_pipeline_set_nibble_order(&g_audio_pipeline, true);
                app_log("MODEL", "ARN9 detected -> ADPCM low-nibble-first");
            }
        }
    }
    read_battery();
    return 0;
}

static void read_model_number(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_model_chr == 0 || s_model_read) {
        read_battery();
        return;
    }
    s_model_read = true;
    int rc = ble_gattc_read(s_conn_handle, s_model_chr, model_read_cb, NULL);
    if (rc != 0) {
        app_log("MODEL", "ble_gattc_read rc=%d", rc);
        read_battery();
    }
}

// NimBLE (central) does not exchange the ATT MTU automatically. Without this
// the link stays at the 23-byte default and the remote can only send 20-byte
// audio notifications, which is far below the ~8 KB/s needed for 16 kHz ADPCM.
static int mtu_exchange_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           uint16_t mtu, void *arg)
{
    (void)conn_handle; (void)arg;
    app_log("BLE", "MTU exchange done: status=%d mtu=%u", error->status, mtu);
    return 0;
}

// ===========================================================================
// GAP event handler
// ===========================================================================
static void add_discovered(const ble_addr_t *addr, const char *name, int8_t rssi)
{
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr->val[5], addr->val[4], addr->val[3], addr->val[2], addr->val[1], addr->val[0]);

    portENTER_CRITICAL(&s_disc_mux);
    int found = -1;
    for (int i = 0; i < s_discovered_count; i++) {
        if (mac_equals(s_discovered[i].mac, mac)) { found = i; break; }
    }
    if (found < 0) {
        if (s_discovered_count >= BLE_MAX_DISCOVERED) {
            memmove(&s_discovered[0], &s_discovered[1], sizeof(discovered_t) * (BLE_MAX_DISCOVERED - 1));
            s_discovered_count = BLE_MAX_DISCOVERED - 1;
        }
        found = s_discovered_count++;
        memset(&s_discovered[found], 0, sizeof(discovered_t));
        strncpy(s_discovered[found].mac, mac, sizeof(s_discovered[found].mac) - 1);
    }
    if (name && name[0]) {
        strncpy(s_discovered[found].name, name, sizeof(s_discovered[found].name) - 1);
    }
    s_discovered[found].rssi = rssi;
    s_discovered[found].type = addr->type;
    s_discovered[found].last_seen_ms = now_ms();
    portEXIT_CRITICAL(&s_disc_mux);
}

static bool adv_is_target(const ble_addr_t *addr, const char *name, const uint8_t *data, uint8_t len)
{
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr->val[5], addr->val[4], addr->val[3], addr->val[2], addr->val[1], addr->val[0]);

    if (s_bound_mac[0]) {
        if (mac_equals(mac, s_bound_mac)) return true;
        if (s_bound_name[0] && name && name[0] && strcasecmp(name, s_bound_name) == 0) return true;
        return false;
    }

    if (name && (strstr(name, "MI RC") || strstr(name, "Xiaomi") ||
                 strstr(name, "Remote") || strstr(name, "小米") || strstr(name, "遥控"))) {
        return true;
    }

    // Look for the ATVV 128-bit service UUID (0xAB5E0001...) in the advertising data.
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t ad_len = data[i];
        if (ad_len == 0) break;
        uint8_t ad_type = data[i + 1];
        if ((ad_type == 0x06 || ad_type == 0x07) && ad_len >= 17) {
            // 128-bit UUID, little-endian encoded.
            static const uint8_t atvv_prefix[4] = { 0x64, 0xb6, 0x17, 0xf6 };
            if (memcmp(&data[i + 2], atvv_prefix, 4) == 0) return true;
        }
        i += ad_len + 1;
    }
    return false;
}

static void start_scan(void);
static void do_connect_addr(const ble_addr_t *addr);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_gap_disc_desc *d = &event->disc;
            char name[40] = {0};
            for (uint8_t i = 0; i + 1 < d->length_data; ) {
                uint8_t ad_len = d->data[i];
                if (ad_len == 0) break;
                uint8_t ad_type = d->data[i + 1];
                if ((ad_type == 0x08 || ad_type == 0x09) && ad_len >= 2) {
                    uint8_t n = ad_len - 1;
                    if (n > sizeof(name) - 1) n = sizeof(name) - 1;
                    memcpy(name, &d->data[i + 2], n);
                    name[n] = '\0';
                }
                i += ad_len + 1;
            }
            add_discovered(&d->addr, name, d->rssi);

            if (!s_do_connect && s_state <= BLE_STATE_SCANNING &&
                adv_is_target(&d->addr, name, d->data, d->length_data)) {
                app_log("BLE", "Target found: %s (%02x:%02x:%02x:%02x:%02x:%02x)",
                        name[0] ? name : "unknown",
                        d->addr.val[5], d->addr.val[4], d->addr.val[3],
                        d->addr.val[2], d->addr.val[1], d->addr.val[0]);
                ble_gap_disc_cancel();
                s_pending_type = d->addr.type;
                if (name[0]) {
                    strncpy(s_pending_name, name, sizeof(s_pending_name) - 1);
                }
                char mac[18];
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         d->addr.val[5], d->addr.val[4], d->addr.val[3],
                         d->addr.val[2], d->addr.val[1], d->addr.val[0]);
                strncpy(s_pending_mac, mac, sizeof(s_pending_mac) - 1);
                s_do_connect = true;
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (s_state == BLE_STATE_SCANNING) {
                s_state = BLE_STATE_DISCONNECTED;
            }
            break;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_state = BLE_STATE_CONNECTING;
                s_conn_start_ms = now_ms();
                s_discovery_started = false;
                app_log("BLE", "GATT connected (conn=%u)", s_conn_handle);
                struct ble_gap_conn_desc cdesc;
                if (ble_gap_conn_find(s_conn_handle, &cdesc) == 0) {
                    app_log("BLE", "Conn params: itvl=%u latency=%u timeout=%u",
                            cdesc.conn_itvl, cdesc.conn_latency, cdesc.supervision_timeout);
                }
                int mtu_rc = ble_gattc_exchange_mtu(s_conn_handle, mtu_exchange_cb, NULL);
                if (mtu_rc != 0) {
                    app_log("BLE", "MTU exchange failed to start: rc=%d", mtu_rc);
                }
                int rc = ble_gap_security_initiate(s_conn_handle);
                if (rc != 0) {
                    app_log("BLE", "security_initiate rc=%d, discovering anyway", rc);
                    start_discovery();
                }
            } else {
                app_log("BLE", "Connect failed: %d", event->connect.status);
                s_state = BLE_STATE_DISCONNECTED;
                start_scan();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            app_log("BLE", "Disconnected (reason=%d)", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_state = BLE_STATE_DISCONNECTED;
            s_discovery_started = false;
            s_pressed_count = 0;
            s_batt_level_chr = 0;
            s_batt_status_chr = 0;
            s_model_chr = 0;
            s_model_read = false;
            s_battery_level = -1;
            key_engine_release_all(&g_key_engine, now_ms());
            usb_hid_keyboard_release();
            usb_hid_consumer_release();
            usb_hid_mouse_buttons_release();
            audio_pipeline_stop_session(&g_audio_pipeline);
            led_indicator_set(LED_STATE_WAIT_CONNECTION);
            break;

        case BLE_GAP_EVENT_ENC_CHANGE: {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                app_log("BLE", "Encryption change: status=%d encrypted=%d bonded=%d authenticated=%d",
                        event->enc_change.status, desc.sec_state.encrypted,
                        desc.sec_state.bonded, desc.sec_state.authenticated);
            } else {
                app_log("BLE", "Encryption change: status=%d", event->enc_change.status);
            }
            if (event->enc_change.status == 0 || !s_discovery_started) {
                start_discovery();
            }
            break;
        }

        case BLE_GAP_EVENT_MTU:
            app_log("BLE", "MTU updated: %u", event->mtu.value);
            break;

        case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
            // The remote asks for a large slave latency (skips connection
            // events to save power), which delays HID notifications by up to
            // ~1 s. Refuse it so the link keeps the low-latency connect
            // parameters (latency = 0).
            const struct ble_gap_upd_params *pp = event->conn_update_req.peer_params;
            if (pp && pp->latency != 0) {
                app_log("BLE", "Reject peer conn params (latency=%u) to stay responsive",
                        pp->latency);
                return BLE_HS_EREJECT;
            }
            break;
        }

        case BLE_GAP_EVENT_CONN_UPDATE_REQ: {
            // Link-layer parameter request: counter-propose latency = 0.
            struct ble_gap_upd_params *sp = event->conn_update_req.self_params;
            if (sp) {
                sp->latency = 0;
                if (sp->itvl_min < 6) sp->itvl_min = 6;
                if (sp->itvl_max > 12) sp->itvl_max = 12;
                if (sp->supervision_timeout < 400) sp->supervision_timeout = 400;
                sp->min_ce_len = 0;
                sp->max_ce_len = 0;
            }
            app_log("BLE", "Clamp conn param req -> latency=0");
            break;
        }

        case BLE_GAP_EVENT_CONN_UPDATE: {
            struct ble_gap_conn_desc udesc;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &udesc) == 0) {
                app_log("BLE", "Conn updated: status=%d itvl=%u latency=%u timeout=%u",
                        event->conn_update.status, udesc.conn_itvl,
                        udesc.conn_latency, udesc.supervision_timeout);
            } else {
                app_log("BLE", "Conn updated: status=%d", event->conn_update.status);
            }
            break;
        }

        case BLE_GAP_EVENT_NOTIFY_RX: {
            struct os_mbuf *om = event->notify_rx.om;
            uint16_t handle = event->notify_rx.attr_handle;
            uint8_t buf[512];
            uint16_t len = OS_MBUF_PKTLEN(om);
            if (len > sizeof(buf)) len = sizeof(buf);
            if (os_mbuf_copydata(om, 0, len, buf) != 0) break;

            if (handle == s_atvv_aud_chr) {
                audio_pipeline_feed_adpcm(&g_audio_pipeline, buf, len);
            } else if (handle == s_atvv_ctl_chr) {
                handle_atvv_ctl(buf, len);
            } else {
                for (int i = 0; i < s_hid_report_count; i++) {
                    if (handle == s_hid_report_chrs[i]) {
                        handle_hid_report(buf, len);
                        break;
                    }
                }
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

// ===========================================================================
// Connect / scan control
// ===========================================================================
static struct ble_gap_conn_params s_conn_params = {
    .scan_itvl = 16,
    .scan_window = 16,
    .itvl_min = 6,       // 7.5 ms: enough events to carry 16 kHz ADPCM (~67 frames/s)
    .itvl_max = 12,      // 15 ms
    .latency = 0,
    .supervision_timeout = 400,
    .min_ce_len = 0,
    .max_ce_len = 0,
};

static void do_connect_addr(const ble_addr_t *addr)
{
    s_state = BLE_STATE_CONNECTING;
    led_indicator_set(LED_STATE_WAIT_CONNECTION);
    int rc = ble_gap_connect(s_own_addr_type, addr, 10000, &s_conn_params, gap_event_cb, NULL);
    if (rc != 0) {
        app_log("BLE", "ble_gap_connect rc=%d", rc);
        s_state = BLE_STATE_DISCONNECTED;
        start_scan();
    }
}

static void start_scan(void)
{
    if (s_state == BLE_STATE_CONNECTING || s_state >= BLE_STATE_CONNECTED) {
        return;
    }
    struct ble_gap_disc_params params = {0};
    params.itvl = (uint16_t)(BLE_SCAN_INTERVAL_MS * 1000 / 625);
    params.window = (uint16_t)(BLE_SCAN_WINDOW_MS * 1000 / 625);
    params.passive = 0;
    params.filter_duplicates = 0;
    params.limited = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == 0) {
        s_state = BLE_STATE_SCANNING;
        led_indicator_set(LED_STATE_WAIT_CONNECTION);
        app_log("BLE", "Scanning for Xiaomi remote...");
    } else if (rc != BLE_HS_EALREADY) {
        app_log("BLE", "ble_gap_disc rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        app_log("BLE", "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        app_log("BLE", "infer_auto rc=%d", rc);
        return;
    }
    start_scan();
}

static void on_reset(int reason)
{
    app_log("BLE", "Host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ===========================================================================
// Public API
// ===========================================================================
void ble_remote_init(void)
{
    size_t len = config_store_get_str("ble_conf", "bound_mac", s_bound_mac, sizeof(s_bound_mac));
    if (len > 0) {
        char typebuf[4] = {0};
        config_store_get_str("ble_conf", "bound_name", s_bound_name, sizeof(s_bound_name));
        config_store_get_str("ble_conf", "bound_type", typebuf, sizeof(typebuf));
        s_bound_type = (uint8_t)atoi(typebuf);
        app_log("BLE", "Loaded bound remote: %s (%s)", s_bound_name, s_bound_mac);
    }

    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();
    ble_att_set_preferred_mtu(512);

    nimble_port_freertos_init(host_task);
    app_log("BLE", "NimBLE host started");
}

void ble_remote_task(void)
{
    uint32_t now = now_ms();

    // Handle asynchronous requests from the WebUSB task.
    if (s_req_unpair || s_req_reconnect) {
        bool is_unpair = s_req_unpair;
        s_req_unpair = false;
        s_req_reconnect = false;
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (is_unpair) {
            ble_store_clear();
            s_connected_mac[0] = '\0';
            s_connected_name[0] = '\0';
        }
        s_state = BLE_STATE_DISCONNECTED;
        start_scan();
    }

    if (s_do_connect) {
        s_do_connect = false;
        ble_gap_disc_cancel();
        ble_addr_t addr = {0};
        addr.type = s_pending_type;
        unsigned int v[6];
        if (sscanf(s_pending_mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
            for (int i = 0; i < 6; i++) addr.val[5 - i] = (uint8_t)v[i];
            strncpy(s_connected_mac, s_pending_mac, sizeof(s_connected_mac) - 1);
            strncpy(s_connected_name, s_pending_name[0] ? s_pending_name : "Xiaomi Voice Remote",
                    sizeof(s_connected_name) - 1);
            // Pick the matching remote profile for this model, if any.
            remote_profile_autodetect(s_connected_name);
            do_connect_addr(&addr);
        }
    }

    // Fallback: if security never completes, still attempt discovery.
    if (s_state == BLE_STATE_CONNECTING && s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        !s_discovery_started && (now - s_conn_start_ms) > 3000) {
        app_log("BLE", "Security timeout -> discovering services anyway");
        start_discovery();
    }

    if (s_state == BLE_STATE_DISCONNECTED) {
        start_scan();
    }

    // Refresh the remote battery level periodically while connected.
    if ((s_state == BLE_STATE_CONNECTED || s_state == BLE_STATE_TALKING) &&
        s_batt_level_chr && (now - s_battery_last_ms) > BATTERY_REFRESH_INTERVAL_MS) {
        read_battery();
    }

    // Persist bound remote when connected.
    if ((s_state == BLE_STATE_CONNECTED || s_state == BLE_STATE_TALKING) && s_connected_mac[0] &&
        !mac_equals(s_bound_mac, s_connected_mac)) {
        strncpy(s_bound_mac, s_connected_mac, sizeof(s_bound_mac) - 1);
        strncpy(s_bound_name, s_connected_name, sizeof(s_bound_name) - 1);
        s_bound_type = s_pending_type;
        char typebuf[4];
        snprintf(typebuf, sizeof(typebuf), "%u", (unsigned)s_bound_type);
        config_store_set_str("ble_conf", "bound_mac", s_bound_mac);
        config_store_set_str("ble_conf", "bound_name", s_bound_name);
        config_store_set_str("ble_conf", "bound_type", typebuf);
    }
}

ble_remote_state_t ble_remote_get_state(void)
{
    return s_state;
}

void ble_remote_trigger_reconnect(void)
{
    s_req_reconnect = true;
}

bool ble_remote_connect_target(const char *mac_str, uint8_t addr_type, const char *dev_name)
{
    if (!mac_str || !mac_str[0]) return false;
    strncpy(s_pending_mac, mac_str, sizeof(s_pending_mac) - 1);
    s_pending_type = addr_type;
    strncpy(s_pending_name, (dev_name && dev_name[0]) ? dev_name : "Xiaomi Voice Remote",
            sizeof(s_pending_name) - 1);
    s_do_connect = true;
    app_log("BLE", "Manual connect queued: %s (%s)", s_pending_name, s_pending_mac);
    return true;
}

bool ble_remote_connect_mac(const char *mac_str)
{
    uint8_t type = 1;
    char name[40] = {0};
    portENTER_CRITICAL(&s_disc_mux);
    for (int i = 0; i < s_discovered_count; i++) {
        if (mac_equals(s_discovered[i].mac, mac_str)) {
            type = s_discovered[i].type;
            strncpy(name, s_discovered[i].name, sizeof(name) - 1);
            break;
        }
    }
    portEXIT_CRITICAL(&s_disc_mux);
    return ble_remote_connect_target(mac_str, type, name);
}

void ble_remote_unpair(void)
{
    s_bound_mac[0] = '\0';
    s_bound_name[0] = '\0';
    config_store_erase_key("ble_conf", "bound_mac");
    config_store_erase_key("ble_conf", "bound_name");
    config_store_erase_key("ble_conf", "bound_type");
    s_req_unpair = true;
    app_log("BLE", "Unpair requested");
}

size_t ble_remote_scan_devices_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    size_t w = 0;
    w += (size_t)snprintf(out + w, out_len - w, "{\"devices\":[");
    portENTER_CRITICAL(&s_disc_mux);
    for (int i = 0; i < s_discovered_count; i++) {
        w += (size_t)snprintf(out + w, out_len - w,
                              "%s{\"name\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"type\":%u}",
                              (i == 0) ? "" : ",",
                              s_discovered[i].name[0] ? s_discovered[i].name : "Unnamed",
                              s_discovered[i].mac, s_discovered[i].rssi, s_discovered[i].type);
        if (w >= out_len - 4) break;
    }
    portEXIT_CRITICAL(&s_disc_mux);
    w += (size_t)snprintf(out + w, out_len - w, "]}");
    return w;
}

size_t ble_remote_get_connected_info(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    return (size_t)snprintf(out, out_len,
                            "{\"connected\":%s,\"state\":%d,\"name\":\"%s\",\"mac\":\"%s\","
                            "\"bound_mac\":\"%s\",\"bound_name\":\"%s\",\"battery\":%d}",
                            (s_state >= BLE_STATE_CONNECTED) ? "true" : "false",
                            (int)s_state, s_connected_name, s_connected_mac,
                            s_bound_mac, s_bound_name, s_battery_level);
}

int ble_remote_get_battery(void)
{
    return s_battery_level;
}
