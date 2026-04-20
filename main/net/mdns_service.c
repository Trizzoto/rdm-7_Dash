/* mdns_service.c — see header for rationale.
 *
 * Uses the ESP-IDF mdns component. Host name is fixed as "rdm7" so devices
 * are always discoverable at rdm7.local regardless of SSID configuration.
 * TXT metadata lets the desktop app filter real RDM-7 dashes from other
 * mDNS-advertising appliances on the same LAN.
 */

#include "mdns_service.h"

#include "mdns.h"
#include "esp_log.h"
#include "esp_err.h"

#include "../system/device_id.h"
#include "../include/version.h"

static const char *TAG = "mdns_service";
static bool s_initialised = false;

/* mDNS is disabled — the espressif__mdns managed component was failing
 * to allocate memory from internal RAM (errors like `mdns_priv_alloc_packet:
 * Cannot allocate memory`) even with 3.8MB heap free, because its Kconfig
 * defaults pin it to `MDNS_MEMORY_ALLOC_INTERNAL`. Rather than fight the
 * component, we've removed mDNS from the runtime path — users reach the
 * dash via the IP shown in Device Settings, or by scanning the QR code.
 * Flip this to 0 and re-enable the code below if mDNS is ever desired. */
#define RDM7_MDNS_DISABLED 1

esp_err_t rdm7_mdns_init(void) {
    if (RDM7_MDNS_DISABLED) {
        ESP_LOGI(TAG, "mDNS disabled — use IP or QR code to reach the dash");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_initialised) return ESP_OK;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set("rdm7");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        /* non-fatal — we still try to advertise services */
    }

    err = mdns_instance_name_set("RDM-7 Dash");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
    }

    s_initialised = true;
    ESP_LOGI(TAG, "mDNS initialised — hostname rdm7.local");
    return ESP_OK;
}

esp_err_t rdm7_mdns_refresh(void) {
    if (!s_initialised) {
        esp_err_t err = rdm7_mdns_init();
        if (err != ESP_OK) return err;
    }

    char serial[MAX_SERIAL_LENGTH] = "";
    (void) get_device_serial(serial);

    /* TXT records for _rdm7._tcp — let the desktop app identify and version-gate us */
    const mdns_txt_item_t rdm7_txt[] = {
        { .key = "serial",  .value = serial },
        { .key = "version", .value = FIRMWARE_VERSION },
        { .key = "schema",  .value = "13" },
    };

    /* Remove any previous instance (harmless if absent) so we always advertise the latest metadata */
    (void) mdns_service_remove("_http", "_tcp");
    (void) mdns_service_remove("_rdm7", "_tcp");

    esp_err_t err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "mdns_service_add _http: %s", esp_err_to_name(err));

    err = mdns_service_add(NULL, "_rdm7", "_tcp", 80,
                           (mdns_txt_item_t *) rdm7_txt,
                           sizeof(rdm7_txt) / sizeof(rdm7_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add _rdm7: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS services refreshed (serial=%s, version=%s)", serial, FIRMWARE_VERSION);
    return ESP_OK;
}

void rdm7_mdns_stop(void) {
    if (!s_initialised) return;
    mdns_free();
    s_initialised = false;
}
