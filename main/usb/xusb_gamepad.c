#include "xusb_gamepad.h"
#include "app_config.h"
#include "app_log.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

// Provided by uac_microphone.c; registered together with the XUSB driver.
extern usbd_class_driver_t const *uac_microphone_class_driver(void);

static uint8_t s_ep_in = USB_EP_XUSB_IN;
static uint8_t s_ep_out = USB_EP_XUSB_OUT;
static volatile bool s_mounted = false;

static xusb_gamepad_report_t s_last;
static volatile bool s_last_valid = false;

// Stable TX buffer for the in-flight IN report. Passing &s_last directly would
// let a later report (keepalive / tap release) overwrite the bytes the host is
// still reading, dropping short taps.
static uint8_t s_tx_buf[XUSB_REPORT_LEN];
static xusb_gamepad_report_t s_pending;
static volatile bool s_pending_valid = false;

static uint8_t s_out_buf[64];
static uint8_t s_ctrl_buf[64];

// Serializes report submission across the key task, the release/keepalive task
// and the TinyUSB task (transfer-complete callback).
static SemaphoreHandle_t s_xusb_mutex = NULL;

static inline void xusb_lock(void)
{
    if (s_xusb_mutex) {
        xSemaphoreTake(s_xusb_mutex, portMAX_DELAY);
    }
}

static inline void xusb_unlock(void)
{
    if (s_xusb_mutex) {
        xSemaphoreGive(s_xusb_mutex);
    }
}

static bool xusb_send_locked(const xusb_gamepad_report_t *report);

static void xusb_neutral_report(xusb_gamepad_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->report_id = 0x00;
    report->size = XUSB_REPORT_LEN;
}

// ===========================================================================
// Custom XUSB class driver
// ===========================================================================

static void xusbd_init(void)
{
    xusb_neutral_report(&s_last);
    s_last_valid = false;
}

static bool xusbd_deinit(void)
{
    s_mounted = false;
    return true;
}

static void xusbd_reset(uint8_t rhport)
{
    (void)rhport;
    s_ep_in = USB_EP_XUSB_IN;
    s_ep_out = USB_EP_XUSB_OUT;
    s_mounted = false;
    xusb_neutral_report(&s_last);
    s_last_valid = false;
}

static uint16_t xusbd_open(uint8_t rhport, tusb_desc_interface_t const *desc_itf,
                           uint16_t max_len)
{
    TU_VERIFY(desc_itf->bInterfaceNumber == USB_ITF_XUSB, 0);
    TU_VERIFY(desc_itf->bInterfaceClass == 0xFF &&
              desc_itf->bInterfaceSubClass == 0x5D, 0);

    uint8_t const *p_desc = tu_desc_next(desc_itf);
    uint8_t const *const itf_end = ((uint8_t const *)desc_itf) + max_len;
    uint16_t drv_len = desc_itf->bLength;
    uint8_t opened = 0;

    // Walk the class-specific XUSB descriptor (type 0x21) and the endpoints.
    while ((p_desc + 2) <= itf_end && opened < desc_itf->bNumEndpoints) {
        uint8_t const len = p_desc[0];
        uint8_t const type = p_desc[1];
        if (len < 2) break;

        if (type == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p_desc;
            TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                s_ep_in = ep->bEndpointAddress;
            } else {
                s_ep_out = ep->bEndpointAddress;
            }
            opened++;
        }

        drv_len += len;
        p_desc += len;
    }

    if (opened < desc_itf->bNumEndpoints) {
        return 0;
    }

    // Arm the interrupt OUT endpoint (rumble / device control).
    if (s_ep_out) {
        usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf));
    }

    s_mounted = true;
    app_log("XUSB", "interface open: IN=%02X OUT=%02X", s_ep_in, s_ep_out);
    return drv_len;
}

static bool xusbd_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                  tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        request->wIndex != USB_ITF_XUSB ||
        request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) {
        return false;
    }

    // Get_Report (Input): 0xA1, bRequest 0x01, wValue 0x01xx.
    if (request->bRequest == 0x01 &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        uint16_t len = request->wLength;
        if (len > XUSB_REPORT_LEN) len = XUSB_REPORT_LEN;
        return tud_control_xfer(rhport, request, (void *)&s_last, len);
    }

    // Set_Report (Output device control): 0x21, bRequest 0x09, wValue 0x02xx.
    if (request->bRequest == 0x09 &&
        request->bmRequestType_bit.direction == TUSB_DIR_OUT) {
        uint16_t len = request->wLength;
        if (len > sizeof(s_ctrl_buf)) len = sizeof(s_ctrl_buf);
        if (len == 0) {
            return tud_control_status(rhport, request);
        }
        return tud_control_xfer(rhport, request, s_ctrl_buf, len);
    }

    return false;
}

static bool xusbd_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                          xfer_result_t result, uint32_t xferred_bytes)
{
    (void)result;
    (void)xferred_bytes;

    if (ep_addr == s_ep_out) {
        // TODO(Phase 3): decode game pad rumble motor control (report 0x00).
        if (s_mounted) {
            usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf));
        }
    } else if (ep_addr == s_ep_in) {
        // Flush the state that arrived while the previous report was in flight.
        xusb_lock();
        if (s_pending_valid) {
            xusb_gamepad_report_t p = s_pending;
            s_pending_valid = false;
            xusb_send_locked(&p);
        }
        xusb_unlock();
    }
    return true;
}

static usbd_class_driver_t const s_xusb_driver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    .name = "XUSB",
#endif
    .init = xusbd_init,
    .deinit = xusbd_deinit,
    .reset = xusbd_reset,
    .open = xusbd_open,
    .control_xfer_cb = xusbd_control_xfer_cb,
    .xfer_cb = xusbd_xfer_cb,
    .xfer_isr = NULL,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    static usbd_class_driver_t s_app_drivers[2];
    s_app_drivers[0] = *uac_microphone_class_driver();
    s_app_drivers[1] = s_xusb_driver;
    *driver_count = 2;
    return s_app_drivers;
}

// ===========================================================================
// Public API
// ===========================================================================

void xusb_gamepad_init(void)
{
    if (!s_xusb_mutex) {
        s_xusb_mutex = xSemaphoreCreateMutex();
    }
    xusbd_reset(0);
}

bool xusb_gamepad_mounted(void)
{
    return s_mounted;
}

// Caller must hold s_xusb_mutex.
static bool xusb_send_locked(const xusb_gamepad_report_t *report)
{
    s_last = *report;
    s_last_valid = true;

    uint8_t const rhport = 0;
    if (usbd_edpt_busy(rhport, s_ep_in)) {
        s_pending = *report;
        s_pending_valid = true;
        return false;
    }
    if (!usbd_edpt_claim(rhport, s_ep_in)) {
        s_pending = *report;
        s_pending_valid = true;
        return false;
    }
    memcpy(s_tx_buf, report, XUSB_REPORT_LEN);
    if (!usbd_edpt_xfer(rhport, s_ep_in, s_tx_buf, XUSB_REPORT_LEN)) {
        usbd_edpt_release(rhport, s_ep_in);
        s_pending = *report;
        s_pending_valid = true;
        return false;
    }
    return true;
}

bool xusb_gamepad_send(const xusb_gamepad_report_t *report)
{
    if (!report || !s_mounted) {
        return false;
    }
    xusb_lock();
    bool ok = xusb_send_locked(report);
    xusb_unlock();
    return ok;
}

void xusb_gamepad_keepalive(void)
{
    if (s_mounted && s_last_valid) {
        xusb_gamepad_send(&s_last);
    }
}

bool xusb_gamepad_handle_vendor_request(uint8_t rhport,
                                        tusb_control_request_t const *request)
{
    // Get_Device_ID: 0xC0, bRequest 0x01, device level, wLength 0x0004.
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->bRequest == 0x01 && request->wValue == 0x0000 &&
        request->wIndex == 0x0000 && request->wLength == 0x0004) {
        static const uint8_t device_id[4] = { 0x01, 0x23, 0x45, 0x67 };
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)device_id,
                                sizeof(device_id));
    }

    // Interface-level vendor requests targeting the XUSB interface.
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE &&
        request->wIndex == USB_ITF_XUSB) {

        // Get_Report / Get_Capabilities: return the current input report.
        if (request->bmRequestType_bit.direction == TUSB_DIR_IN &&
            request->bRequest == 0x01) {
            uint16_t len = request->wLength;
            if (len > XUSB_REPORT_LEN) len = XUSB_REPORT_LEN;
            return tud_control_xfer(rhport, request, (void *)&s_last, len);
        }

        // Set_Report (Output device control): consume the data stage.
        if (request->bmRequestType_bit.direction == TUSB_DIR_OUT &&
            request->bRequest == 0x09) {
            uint16_t len = request->wLength;
            if (len > sizeof(s_ctrl_buf)) len = sizeof(s_ctrl_buf);
            if (len == 0) {
                return tud_control_status(rhport, request);
            }
            return tud_control_xfer(rhport, request, s_ctrl_buf, len);
        }

        // Set_Control: 0x41, bRequest 0x00, no data stage.
        if (request->bmRequestType_bit.direction == TUSB_DIR_OUT &&
            request->bRequest == 0x00 && request->wLength == 0x0000) {
            return tud_control_status(rhport, request);
        }
    }

    return false;
}
