#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "wifi_station.h"
#include "websocket_client.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "AQUANEXIS_BLE";

/* ============================================================
 * BLE state
 * ============================================================ */

static uint8_t own_addr_type = 0;

static uint16_t status_conn_handle =
    BLE_HS_CONN_HANDLE_NONE;

static uint16_t status_value_handle = 0;


/* ============================================================
 * Forward declaration
 * ============================================================ */

static void ble_start_advertising(void);

static bool
json_get_string(
    const char *json,
    const char *key,
    char *value,
    size_t value_size)
{
    char key_pattern[64];
    int key_length =
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);

    if (key_length < 0 ||
        (size_t)key_length >= sizeof(key_pattern)) {
        return false;
    }

    const char *key_start = strstr(json, key_pattern);

    if (key_start == NULL) {
        return false;
    }

    const char *cursor = key_start + key_length;

    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }

    if (*cursor != ':') {
        return false;
    }

    cursor++;

    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }

    if (*cursor != '"' || value_size == 0) {
        return false;
    }

    cursor++;

    size_t value_length = 0;

    while (*cursor != '\0' && *cursor != '"') {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
        }

        if (value_length + 1 >= value_size) {
            return false;
        }

        value[value_length++] = *cursor++;
    }

    if (*cursor != '"') {
        return false;
    }

    value[value_length] = '\0';
    return true;
}


/* ============================================================
 * AquaNexis Service UUID
 *
 * 4fafc201-1fb5-459e-8fcc-c5c9c331914b
 * ============================================================ */

static const ble_uuid128_t aquanexis_service_uuid =
    BLE_UUID128_INIT(
           0x4b, 0x91, 0x31, 0xc3,
           0xc9, 0xc5,
           0xcc, 0x8f,
           0x9e, 0x45,
           0xb5, 0x1f,
           0x01, 0xc2,
           0xaf, 0x4f
    );


/* ============================================================
 * Provision Characteristic UUID
 *
 * Flutter -> ESP32
 *
 * beb54850-36e1-4688-b7f5-ea07361b26b0
 * ============================================================ */

static const ble_uuid128_t provision_uuid =
    BLE_UUID128_INIT(
           0xb0, 0x26, 0x1b, 0x36,
           0x07, 0xea,
           0xf5, 0xb7,
           0x88, 0x46,
           0xe1, 0x36,
           0x50, 0x48,
           0xb5, 0xbe
    );


/* ============================================================
 * Status Characteristic UUID
 *
 * ESP32 -> Flutter
 *
 * beb54841-36e1-4688-b7f5-ea07361b26a4
 * ============================================================ */

static const ble_uuid128_t status_uuid =
    BLE_UUID128_INIT(
           0xa4, 0x26, 0x1b, 0x36,
           0x07, 0xea,
           0xf5, 0xb7,
           0x88, 0x46,
           0xe1, 0x36,
           0x41, 0x48,
           0xb5, 0xbe
    );


/* ============================================================
 * Provision characteristic callback
 *
 * Flutter writes JSON here.
 * ============================================================ */

static int
provision_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {

        ESP_LOGW(
            TAG,
            "Provision callback received unexpected operation: %d",
            ctxt->op
        );

        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t length =
        OS_MBUF_PKTLEN(ctxt->om);

    ESP_LOGI(
        TAG,
        "Provision write received. Length: %u",
        length
    );

    if (length == 0) {

        ESP_LOGW(
            TAG,
            "Empty provision payload"
        );

        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }


    /*
     * Maximum JSON payload.
     */

    char buffer[512];

    if (length >= sizeof(buffer)) {

        ESP_LOGE(
            TAG,
            "Provision payload too large: %u",
            length
        );

        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }


    /*
     * Copy BLE mbuf into normal buffer.
     */

    int rc =
        ble_hs_mbuf_to_flat(
            ctxt->om,
            buffer,
            sizeof(buffer) - 1,
            NULL
        );

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "Failed to copy BLE payload: %d",
            rc
        );

        return BLE_ATT_ERR_UNLIKELY;
    }


    buffer[length] = '\0';


    /* ========================================================
     * Print received JSON
     * ======================================================== */

    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "WIFI PROVISION PAYLOAD RECEIVED"
    );

    ESP_LOGI(
        TAG,
        "Length: %u",
        length
    );

    ESP_LOGI(
        TAG,
        "Payload:"
    );

    ESP_LOGI(
        TAG,
        "%s",
        buffer
    );

    ESP_LOGI(
        TAG,
        "================================"
    );


    char ssid[33];
    char password[65];
    char device_websocket_url[256];

    if (!json_get_string(buffer, "ssid", ssid, sizeof(ssid)) ||
        !json_get_string(buffer, "password", password, sizeof(password)) ||
        !json_get_string(
            buffer,
            "deviceWebSocketUrl",
            device_websocket_url,
            sizeof(device_websocket_url)
        )) {
        ESP_LOGE(
            TAG,
            "Provisioning JSON requires valid ssid, password, and deviceWebSocketUrl strings"
        );

        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    esp_err_t websocket_url_result =
        websocket_client_set_url(device_websocket_url);

    if (websocket_url_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Invalid deviceWebSocketUrl: %s",
            esp_err_to_name(websocket_url_result)
        );

        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    esp_err_t wifi_result =
        wifi_station_connect(ssid, password);

    if (wifi_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start Wi-Fi connection: %s",
            esp_err_to_name(wifi_result)
        );

        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi connection started from BLE provisioning"
    );


    return 0;
}


/* ============================================================
 * Status characteristic callback
 *
 * This characteristic is NOT readable.
 * It is notification-only.
 * ============================================================ */

static int
status_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}


/* ============================================================
 * GATT service table
 * ============================================================ */

static const struct ble_gatt_svc_def gatt_services[] = {

    {
        .type =
            BLE_GATT_SVC_TYPE_PRIMARY,

        .uuid =
            &aquanexis_service_uuid.u,

        .characteristics =
            (struct ble_gatt_chr_def[]) {

                /* --------------------------------------------
                 * Provision characteristic
                 * -------------------------------------------- */

                {
                    .uuid =
                        &provision_uuid.u,

                    .access_cb =
                        provision_access_cb,

                    .flags =
                        BLE_GATT_CHR_F_WRITE |
                        BLE_GATT_CHR_F_WRITE_NO_RSP,
                },


                /* --------------------------------------------
                 * Status characteristic
                 * -------------------------------------------- */

                {
                    .uuid =
                        &status_uuid.u,

                    .access_cb =
                        status_access_cb,

                    .val_handle =
                        &status_value_handle,

                    .flags =
                        BLE_GATT_CHR_F_NOTIFY,
                },


                /* End characteristics */
                {
                    0
                }
            },
    },


    /* End services */
    {
        0
    }
};


/* ============================================================
 * GAP event callback
 * ============================================================ */

static int
ble_gap_event(
    struct ble_gap_event *event,
    void *arg)
{
    switch (event->type) {

        /* ----------------------------------------------------
         * CONNECT
         * ---------------------------------------------------- */

        case BLE_GAP_EVENT_CONNECT:

            if (event->connect.status == 0) {

                status_conn_handle =
                    event->connect.conn_handle;

                ESP_LOGI(
                    TAG,
                    "================================"
                );

                ESP_LOGI(
                    TAG,
                    "BLE CLIENT CONNECTED"
                );

                ESP_LOGI(
                    TAG,
                    "Connection handle: %d",
                    status_conn_handle
                );

                ESP_LOGI(
                    TAG,
                    "================================"
                );

            } else {

                ESP_LOGW(
                    TAG,
                    "BLE connection failed: %d",
                    event->connect.status
                );

                status_conn_handle =
                    BLE_HS_CONN_HANDLE_NONE;

                ble_start_advertising();
            }

            break;


        /* ----------------------------------------------------
         * DISCONNECT
         * ---------------------------------------------------- */

        case BLE_GAP_EVENT_DISCONNECT:

            ESP_LOGI(
                TAG,
                "BLE client disconnected"
            );

            status_conn_handle =
                BLE_HS_CONN_HANDLE_NONE;

            ble_start_advertising();

            break;


        /* ----------------------------------------------------
         * SUBSCRIBE
         * ---------------------------------------------------- */

        case BLE_GAP_EVENT_SUBSCRIBE:

            ESP_LOGI(
                TAG,
                "================================"
            );

            ESP_LOGI(
                TAG,
                "BLE SUBSCRIPTION EVENT"
            );

            ESP_LOGI(
                TAG,
                "Connection handle: %d",
                event->subscribe.conn_handle
            );

            ESP_LOGI(
                TAG,
                "Attribute handle: %d",
                event->subscribe.attr_handle
            );

            ESP_LOGI(
                TAG,
                "Notify enabled: %d",
                event->subscribe.cur_notify
            );

            ESP_LOGI(
                TAG,
                "Indicate enabled: %d",
                event->subscribe.cur_indicate
            );

            ESP_LOGI(
                TAG,
                "================================"
            );

            break;


        default:

            break;
    }

    return 0;
}


/* ============================================================
 * BLE Advertising
 * ============================================================ */

static void
ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;


    memset(
        &fields,
        0,
        sizeof(fields)
    );


    /* --------------------------------------------------------
     * Advertising flags
     * -------------------------------------------------------- */

    fields.flags =
        BLE_HS_ADV_F_DISC_GEN |
        BLE_HS_ADV_F_BREDR_UNSUP;


    /* --------------------------------------------------------
     * Device name
     * -------------------------------------------------------- */

    const char *device_name =
        "AquaNexis-P4";

    fields.name =
        (const uint8_t *)device_name;

    fields.name_len =
        strlen(device_name);

    fields.name_is_complete =
        1;


    /*
     * IMPORTANT:
     *
     * We intentionally advertise ONLY the device name here.
     *
     * We will NOT depend on the service UUID being present
     * in the advertisement packet.
     *
     * Flutter will discover the GATT service after connection.
     */


    int rc =
        ble_gap_adv_set_fields(
            &fields
        );

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "ble_gap_adv_set_fields failed: %d",
            rc
        );

        return;
    }


    /* --------------------------------------------------------
     * Advertising parameters
     * -------------------------------------------------------- */

    memset(
        &adv_params,
        0,
        sizeof(adv_params)
    );

    adv_params.conn_mode =
        BLE_GAP_CONN_MODE_UND;

    adv_params.disc_mode =
        BLE_GAP_DISC_MODE_GEN;


    /* --------------------------------------------------------
     * Start advertising
     * -------------------------------------------------------- */

    rc =
        ble_gap_adv_start(
            own_addr_type,
            NULL,
            BLE_HS_FOREVER,
            &adv_params,
            ble_gap_event,
            NULL
        );

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "ble_gap_adv_start failed: %d",
            rc
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "BLE ADVERTISING STARTED"
    );

    ESP_LOGI(
        TAG,
        "Device name: %s",
        device_name
    );

    ESP_LOGI(
        TAG,
        "================================"
    );
}


/* ============================================================
 * BLE synchronized
 * ============================================================ */

static void
ble_on_sync(void)
{
    int rc =
        ble_hs_id_infer_auto(
            0,
            &own_addr_type
        );

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "ble_hs_id_infer_auto failed: %d",
            rc
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "BLE HOST SYNCHRONIZED"
    );

    ESP_LOGI(
        TAG,
        "Own address type: %d",
        own_addr_type
    );

    ESP_LOGI(
        TAG,
        "================================"
    );


    ble_start_advertising();
}


/* ============================================================
 * BLE host task
 * ============================================================ */

static void
ble_host_task(void *param)
{
    ESP_LOGI(
        TAG,
        "BLE host task started"
    );

    nimble_port_run();

    nimble_port_freertos_deinit();
}


/* ============================================================
 * Public BLE initialization
 * ============================================================ */

void
ble_test_start(void)
{
    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "Initializing AquaNexis BLE"
    );

    ESP_LOGI(
        TAG,
        "================================"
    );


    /* --------------------------------------------------------
     * Initialize NimBLE
     * -------------------------------------------------------- */

    nimble_port_init();


    /* --------------------------------------------------------
     * Standard GAP/GATT services
     * -------------------------------------------------------- */

    ble_svc_gap_init();

    ble_svc_gatt_init();


    /* --------------------------------------------------------
     * Device name
     * -------------------------------------------------------- */

    int rc =
        ble_svc_gap_device_name_set(
            "AquaNexis-P4"
        );

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "Failed to set device name: %d",
            rc
        );

        return;
    }


    /* --------------------------------------------------------
     * Count GATT resources
     * -------------------------------------------------------- */

    rc =
        ble_gatts_count_cfg(
            gatt_services
        );

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "ble_gatts_count_cfg failed: %d",
            rc
        );

        return;
    }


    /* --------------------------------------------------------
     * Add GATT services
     * -------------------------------------------------------- */

    rc =
        ble_gatts_add_svcs(
            gatt_services
        );

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "ble_gatts_add_svcs failed: %d",
            rc
        );

        return;
    }


    /* --------------------------------------------------------
     * Print expected UUIDs
     * -------------------------------------------------------- */

    ESP_LOGI(
        TAG,
        "GATT Service:"
    );

    ESP_LOGI(
        TAG,
        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
    );

    ESP_LOGI(
        TAG,
        "Provision Characteristic:"
    );

    ESP_LOGI(
        TAG,
        "beb54850-36e1-4688-b7f5-ea07361b26b0"
    );

    ESP_LOGI(
        TAG,
        "Status Characteristic:"
    );

    ESP_LOGI(
        TAG,
        "beb54841-36e1-4688-b7f5-ea07361b26a4"
    );


    /* --------------------------------------------------------
     * Synchronization callback
     * -------------------------------------------------------- */

    ble_hs_cfg.sync_cb =
        ble_on_sync;


    /* --------------------------------------------------------
     * Start NimBLE host
     * -------------------------------------------------------- */

    nimble_port_freertos_init(
        ble_host_task
    );


    ESP_LOGI(
        TAG,
        "BLE initialization complete"
    );
}