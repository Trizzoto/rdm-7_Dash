#include "ota_handler.h"
#include "version.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_log.h"
#include "esp_image_format.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_sntp.h"

// Add these definitions from main.c
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     1

// Add external declarations
extern TaskHandle_t lvglTaskHandle;
extern void rdm_lvgl_port_task(void *pvParameter);
#include "can/can_manager.h"
#include "net/dns_hijack.h"
#include "system/crash_log.h"

// GitHub Releases API for OTA updates
#define GITHUB_API_URL "https://api.github.com/repos/Trizzoto/potato-jubilee/releases/latest"

// Increase the task stack and adjust priority as needed.
#define OTA_TASK_STACK_SIZE 8192
#define OTA_TASK_PRIORITY     5

// Add new defines after existing defines
#define OTA_RETRY_ATTEMPTS 3
#define OTA_CHUNK_SIZE 2048           // Smaller chunks for weak connections
#define OTA_ADAPTIVE_TIMEOUT_BASE 30000  // Base timeout 30 seconds (increased from 15s)
#define OTA_WEAK_SIGNAL_THRESHOLD -70    // RSSI threshold for weak signal
#define OTA_MAX_TIMEOUT 180000          // Maximum timeout 3 minutes (increased from 2 minutes)

static const char *TAG = "ota_handler";

static volatile ota_status_t ota_status = OTA_IDLE;
static char latest_version[16] = {0};
static char download_url[512] = {0};
static char *response_buffer = NULL;
static int response_buffer_size = 0;
static volatile int ota_progress = 0;

// Add new static variables for update information
static int wifi_rssi = 0;
static int download_retry_count = 0;
static float update_file_size_mb = 0.0f;
static char release_notes[256] = {0};

// Add version comparison structures and functions after the forward declarations
typedef struct {
    int major;
    int minor;
    int patch;
} version_t;

// Forward declarations
static bool check_wifi_signal_strength(void);
static esp_err_t optimize_wifi_for_ota(void);
static esp_err_t restore_wifi_settings(void);
static int calculate_adaptive_timeout(void);
static version_t parse_version(const char* version_string);
static int compare_versions(const char* current_ver, const char* newer_ver);

static esp_err_t __attribute__((unused)) validate_image_header(const esp_app_desc_t *new_app_info) {
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;
    esp_ota_get_partition_description(running, &running_app_info);

    ESP_LOGI(TAG, "Current version: %s", running_app_info.version);
    ESP_LOGI(TAG, "New version: %s", new_app_info->version);

    // Skip version comparison - we want to allow the update regardless of version
    // This is temporary until you update the version string in your firmware
    return ESP_OK;

    /* Original version check - uncomment this when version strings are correct
    if (strcmp(new_app_info->version, running_app_info.version) == 0) {
        ESP_LOGW(TAG, "Current running version is the same as the new version");
        return ESP_FAIL;
    }
    return ESP_OK;
    */
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (response_buffer_size + evt->data_len > 64 * 1024) {
                    ESP_LOGE(TAG, "Response buffer exceeds 64KB limit");
                    free(response_buffer);
                    response_buffer = NULL;
                    response_buffer_size = 0;
                    return ESP_FAIL;
                }
                char *temp = realloc(response_buffer, response_buffer_size + evt->data_len + 1);
                if (temp == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory");
                    free(response_buffer);
                    response_buffer = NULL;
                    response_buffer_size = 0;
                    return ESP_FAIL;
                }
                response_buffer = temp;
                memcpy(response_buffer + response_buffer_size, evt->data, evt->data_len);
                response_buffer_size += evt->data_len;
                response_buffer[response_buffer_size] = '\0';
            }
            break;
        // Handle other events if necessary
        default:
            break;
    }
    return ESP_OK;
}

// Parse GitHub Releases API JSON response
static void compare_and_set_ota_status(void) {
    cJSON *root = cJSON_Parse(response_buffer);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse GitHub API JSON response");
        ota_status = OTA_UPDATE_FAILED;
        return;
    }

    cJSON *tag_name = cJSON_GetObjectItem(root, "tag_name");
    cJSON *body = cJSON_GetObjectItem(root, "body");
    cJSON *assets = cJSON_GetObjectItem(root, "assets");

    if (!tag_name || !cJSON_IsString(tag_name) || !assets || !cJSON_IsArray(assets)) {
        ESP_LOGE(TAG, "Required fields (tag_name, assets) not found in GitHub response");
        ota_status = OTA_UPDATE_FAILED;
        cJSON_Delete(root);
        return;
    }

    /* Find the .bin asset */
    cJSON *bin_asset = NULL;
    int asset_count = cJSON_GetArraySize(assets);
    for (int i = 0; i < asset_count; i++) {
        cJSON *asset = cJSON_GetArrayItem(assets, i);
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        if (name && cJSON_IsString(name)) {
            const char *fname = name->valuestring;
            size_t len = strlen(fname);
            if (len > 4 && strcmp(fname + len - 4, ".bin") == 0) {
                bin_asset = asset;
                break;
            }
        }
    }

    if (!bin_asset) {
        ESP_LOGE(TAG, "No .bin asset found in GitHub release");
        ota_status = OTA_UPDATE_FAILED;
        cJSON_Delete(root);
        return;
    }

    cJSON *browser_download_url = cJSON_GetObjectItem(bin_asset, "browser_download_url");
    cJSON *asset_size = cJSON_GetObjectItem(bin_asset, "size");

    if (!browser_download_url || !cJSON_IsString(browser_download_url)) {
        ESP_LOGE(TAG, "No download URL in GitHub release asset");
        ota_status = OTA_UPDATE_FAILED;
        cJSON_Delete(root);
        return;
    }

    /* Strip leading 'v' from tag_name for version comparison */
    const char *latest_version_str = tag_name->valuestring;
    if (latest_version_str[0] == 'v' || latest_version_str[0] == 'V') {
        latest_version_str++;
    }
    const char *current_version_str = FIRMWARE_VERSION;

    ESP_LOGI(TAG, "Firmware Update Check Results:");
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "Current Version:  %s", current_version_str);
    ESP_LOGI(TAG, "Latest Version:   %s", latest_version_str);

    if (asset_size && cJSON_IsNumber(asset_size)) {
        ESP_LOGI(TAG, "File Size:        %d bytes (%.2f MB)",
                 (int)asset_size->valuedouble,
                 asset_size->valuedouble / (1024.0 * 1024.0));
    }

    ESP_LOGI(TAG, "=====================================");

    if (body && cJSON_IsString(body)) {
        ESP_LOGI(TAG, "Release notes: %s", body->valuestring);
    }

    int version_comparison = compare_versions(current_version_str, latest_version_str);

    if (version_comparison > 0) {
        ESP_LOGI(TAG, "NEW FIRMWARE AVAILABLE!");

        if (asset_size && cJSON_IsNumber(asset_size)) {
            update_file_size_mb = (float)(asset_size->valuedouble / (1024.0 * 1024.0));
        }

        if (body && cJSON_IsString(body)) {
            strncpy(release_notes, body->valuestring, sizeof(release_notes) - 1);
            release_notes[sizeof(release_notes) - 1] = '\0';
        } else {
            strcpy(release_notes, "No release notes available");
        }

        ota_status = OTA_UPDATE_AVAILABLE;
        strncpy(latest_version, latest_version_str, sizeof(latest_version) - 1);
        latest_version[sizeof(latest_version) - 1] = '\0';

        strncpy(download_url, browser_download_url->valuestring, sizeof(download_url) - 1);
        download_url[sizeof(download_url) - 1] = '\0';
        ESP_LOGI(TAG, "GitHub download URL: %s", download_url);
        ESP_LOGI(TAG, "Firmware will be fetched via HTTP proxy (plain HTTP)");

    } else if (version_comparison == 0) {
        ESP_LOGI(TAG, "Firmware is up to date!");
        ota_status = OTA_NO_UPDATE_AVAILABLE;

    } else {
        ESP_LOGI(TAG, "Local version is newer than release version (dev/beta)");
        ota_status = OTA_NO_UPDATE_AVAILABLE;
    }

    cJSON_Delete(root);
}

/* Free internal RAM before HTTPS / OTA. Post-WiFi-init the internal heap is
   often <10 KB free, which is below what mbedTLS needs for an SSL handshake
   (AES context + DRBG seed) and what's needed to allocate an 8 KB task stack.
   Safe — STA is the path used for OTA traffic. */
static void ota_free_internal_ram(void) {
    /* Drop SoftAP if STA is connected — saves several KB of WiFi buffers. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP)) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "STA connected — dropping AP to free internal RAM");
            esp_wifi_set_mode(WIFI_MODE_STA);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    /* Stop captive-portal DNS hijack — frees lwIP socket + small internal buffers.
       It only matters before STA is configured; we're past that now. */
    dns_hijack_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
}

void check_for_update(void) {
    ESP_LOGI(TAG, "Checking for updates from GitHub Releases...");
    ESP_LOGI(TAG, "Current version: %s", FIRMWARE_VERSION);

    ota_free_internal_ram();

    /* Reset response buffer */
    if (response_buffer != NULL) {
        free(response_buffer);
        response_buffer = NULL;
    }
    response_buffer_size = 0;

    /* Check WiFi */
    bool strong_signal = check_wifi_signal_strength();
    if (wifi_rssi == 0) {
        ESP_LOGE(TAG, "WiFi not connected");
        ota_status = OTA_UPDATE_FAILED;
        return;
    }

    int api_timeout = strong_signal ? 30000 : 45000;
    ESP_LOGI(TAG, "Using API timeout: %d ms (%s signal, %d dBm)",
             api_timeout, strong_signal ? "strong" : "weak", wifi_rssi);

    /* Temporarily disable power save */
    wifi_ps_type_t original_ps_mode;
    esp_wifi_get_ps(&original_ps_mode);
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = api_timeout,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        esp_wifi_set_ps(original_ps_mode);
        ota_status = OTA_UPDATE_FAILED;
        return;
    }

    /* GitHub API requires User-Agent and accepts versioned JSON */
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    esp_http_client_set_header(client, "User-Agent", "ESP32-RDM7");
    esp_http_client_set_header(client, "Connection", "keep-alive");

    /* Retry logic */
    esp_err_t err = ESP_FAIL;
    int api_retry_count = strong_signal ? 2 : 3;

    for (int i = 0; i < api_retry_count; i++) {
        if (i > 0) {
            ESP_LOGI(TAG, "API retry attempt %d/%d", i + 1, api_retry_count);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        ESP_LOGI(TAG, "Connecting to GitHub API...");
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GitHub API connection successful!");
            break;
        } else {
            ESP_LOGW(TAG, "API request attempt %d failed: %s", i + 1, esp_err_to_name(err));
        }
    }

    /* Restore power save */
    esp_wifi_set_ps(original_ps_mode);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "GitHub API Status Code: %d", status_code);

        if (status_code == 200 && response_buffer != NULL) {
            compare_and_set_ota_status();
        } else if (status_code == 404) {
            ESP_LOGI(TAG, "No releases found on GitHub");
            ota_status = OTA_NO_UPDATE_AVAILABLE;
        } else if (status_code == 403) {
            ESP_LOGW(TAG, "GitHub API rate limit exceeded (60 req/hr unauthenticated)");
            ota_status = OTA_UPDATE_FAILED;
        } else {
            ESP_LOGE(TAG, "Unexpected status code or empty response: %d", status_code);
            ota_status = OTA_UPDATE_FAILED;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed after %d attempts: %s", api_retry_count, esp_err_to_name(err));
        ota_status = OTA_UPDATE_FAILED;
    }

    /* Cleanup */
    esp_http_client_cleanup(client);
    if (response_buffer != NULL) {
        free(response_buffer);
        response_buffer = NULL;
    }
    response_buffer_size = 0;
}

// Add new function before start_ota_update
static bool check_wifi_signal_strength(void) {
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK) {
        wifi_rssi = ap_info.rssi;
        ESP_LOGI(TAG, "WiFi RSSI: %d dBm", wifi_rssi);
        return wifi_rssi > OTA_WEAK_SIGNAL_THRESHOLD;
    }
    ESP_LOGW(TAG, "Failed to get WiFi signal info");
    return false;
}

// Add new function to optimize WiFi for OTA
static esp_err_t optimize_wifi_for_ota(void) {
    ESP_LOGI(TAG, "Optimizing WiFi for OTA update...");
    
    // Disable WiFi power save mode during OTA
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable WiFi power save: %s", esp_err_to_name(ret));
    }
    
    // Set WiFi to maximum performance mode
    wifi_config_t wifi_config;
    ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK) {
        // Force 11n mode for better throughput if supported
        ret = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi protocol: %s", esp_err_to_name(ret));
        }
    }
    
    return ESP_OK;
}

// Add new function to restore WiFi settings
static esp_err_t restore_wifi_settings(void) {
    ESP_LOGI(TAG, "Restoring WiFi settings...");
    
    // Re-enable power save mode
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore WiFi power save: %s", esp_err_to_name(ret));
    }
    
    return ESP_OK;
}

// Add new function for adaptive timeout calculation
static int calculate_adaptive_timeout(void) {
    int timeout = OTA_ADAPTIVE_TIMEOUT_BASE;
    
    // Increase timeout for weak signals
    if (wifi_rssi < OTA_WEAK_SIGNAL_THRESHOLD) {
        timeout *= 2;  // Double timeout for weak signals
        ESP_LOGI(TAG, "Weak WiFi signal detected (%d dBm), using extended timeout", wifi_rssi);
    }
    
    // Cap the maximum timeout
    if (timeout > OTA_MAX_TIMEOUT) {
        timeout = OTA_MAX_TIMEOUT;
    }
    
    ESP_LOGI(TAG, "Using adaptive timeout: %d ms", timeout);
    return timeout;
}

// Smart version parsing function
static version_t parse_version(const char* version_string) {
    version_t v = {0, 0, 0};
    
    if (version_string == NULL) {
        return v;
    }
    
    // Parse version format: MAJOR.MINOR.PATCH
    int parsed = sscanf(version_string, "%d.%d.%d", &v.major, &v.minor, &v.patch);
    
    if (parsed < 3) {
        ESP_LOGW(TAG, "Version parsing incomplete: '%s' -> %d.%d.%d", 
                 version_string, v.major, v.minor, v.patch);
    }
    
    return v;
}

// Smart version comparison: Returns 1 if newer > current, 0 if equal, -1 if older
static int compare_versions(const char* current_ver, const char* newer_ver) {
    version_t current = parse_version(current_ver);
    version_t newer = parse_version(newer_ver);
    
    ESP_LOGI(TAG, "Version comparison: %d.%d.%d vs %d.%d.%d", 
             current.major, current.minor, current.patch,
             newer.major, newer.minor, newer.patch);
    
    // Compare major version
    if (newer.major > current.major) return 1;
    if (newer.major < current.major) return -1;
    
    // Major versions equal, compare minor
    if (newer.minor > current.minor) return 1;
    if (newer.minor < current.minor) return -1;
    
    // Major and minor equal, compare patch
    if (newer.patch > current.patch) return 1;
    if (newer.patch < current.patch) return -1;
    
    // Versions are identical
    return 0;
}


/* ── OTA download over plain HTTP (no TLS — avoids internal RAM exhaustion) ── */

/* Firmware download base URL — plain HTTP server hosting .bin files.
 * Override in NVS key "ota_base_url" or change this default. */
/* Cloudflare Worker proxy URL — serves firmware over plain HTTP.
 * Deploy tools/cloudflare-ota-proxy/ and set this to your worker URL.
 * Pattern: <base>/<version>/esp32-firmware.bin */
#define OTA_DEFAULT_BASE_URL "https://rdm7-ota-proxy.rdm7-ota-proxy.workers.dev"

static char ota_firmware_url[512] = {0};

void ota_set_firmware_url(const char *url) {
    strncpy(ota_firmware_url, url, sizeof(ota_firmware_url) - 1);
    ota_firmware_url[sizeof(ota_firmware_url) - 1] = '\0';
}

const char *ota_get_firmware_url(void) {
    return ota_firmware_url[0] ? ota_firmware_url : NULL;
}

esp_err_t start_ota_update(void) {
    /* Build the download URL */
    char url[512];
    if (ota_firmware_url[0]) {
        strncpy(url, ota_firmware_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else if (download_url[0]) {
        strncpy(url, download_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else {
        snprintf(url, sizeof(url), "%s/%s/esp32-firmware.bin",
                 OTA_DEFAULT_BASE_URL, latest_version);
    }

    ESP_LOGI(TAG, "Starting OTA download from: %s", url);

    if (strlen(url) == 0) {
        ESP_LOGE(TAG, "Download URL is empty");
        ota_status = OTA_UPDATE_FAILED;
        return ESP_FAIL;
    }

    optimize_wifi_for_ota();
    check_wifi_signal_strength();
    int adaptive_timeout = calculate_adaptive_timeout();

    ESP_LOGI(TAG, "Free heap: %lu, free internal: %lu, free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t final_result = ESP_FAIL;

    for (download_retry_count = 0; download_retry_count < OTA_RETRY_ATTEMPTS; download_retry_count++) {
        if (download_retry_count > 0) {
            ESP_LOGI(TAG, "OTA retry attempt %d/%d", download_retry_count + 1, OTA_RETRY_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(3000));
            check_wifi_signal_strength();
            adaptive_timeout = calculate_adaptive_timeout();
        }

        esp_http_client_config_t http_config = {
            .url = url,
            .timeout_ms = adaptive_timeout,
            .buffer_size = 8192,
            .buffer_size_tx = 1024,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
            .keep_alive_idle = 15,
            .keep_alive_interval = 5,
            .keep_alive_count = 3,
        };

        esp_https_ota_config_t ota_config = {
            .http_config = &http_config,
        };

        ESP_LOGI(TAG, "Connecting (attempt %d)...", download_retry_count + 1);

        esp_https_ota_handle_t ota_handle = NULL;
        esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
            continue;
        }

        int image_size = esp_https_ota_get_image_size(ota_handle);
        if (image_size > 0) {
            ESP_LOGI(TAG, "Firmware size: %d bytes (%.2f MB)",
                     image_size, image_size / (1024.0 * 1024.0));
        }

        ESP_LOGI(TAG, "Downloading...");
        int last_progress = -1;
        uint32_t last_time = (uint32_t)(esp_timer_get_time() / 1000);
        bool success = true;

        while (1) {
            err = esp_https_ota_perform(ota_handle);
            if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                /* Still downloading — report progress */
                int read_so_far = esp_https_ota_get_image_len_read(ota_handle);
                int progress = (image_size > 0) ? (read_so_far * 100) / image_size : 0;
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

                if (progress != last_progress || (now - last_time) > 3000) {
                    ESP_LOGI(TAG, "Progress: %d%% (%d/%d bytes) RSSI: %d dBm",
                             progress, read_so_far, image_size, wifi_rssi);
                    last_progress = progress;
                    last_time = now;
                    ota_progress = progress;

                    if (progress > 0 && progress % 20 == 0) {
                        check_wifi_signal_strength();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            } else if (err == ESP_OK) {
                /* Download complete */
                break;
            } else {
                ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
                success = false;
                break;
            }
        }

        if (success) {
            /* Image verification — esp_https_ota_finish() calls esp_ota_end()
               which disables cache. Safe to call inline because THIS task's
               stack is in internal RAM. */
            err = esp_https_ota_finish(ota_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "OTA successful after %d attempt(s)!", download_retry_count + 1);
                ota_status = OTA_UPDATE_COMPLETED;
                ota_progress = 100;
                final_result = ESP_OK;
                break;
            } else {
                ESP_LOGE(TAG, "OTA validation failed: %s", esp_err_to_name(err));
            }
        } else {
            esp_https_ota_abort(ota_handle);
            ESP_LOGE(TAG, "Download failed at %d/%d bytes",
                     esp_https_ota_get_image_len_read(ota_handle), image_size);
        }
    }

    restore_wifi_settings();

    if (final_result != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed after %d attempts", OTA_RETRY_ATTEMPTS);
        ota_status = OTA_UPDATE_FAILED;
        ota_progress = -1;
    }

    return final_result;
}

ota_status_t get_ota_status(void) {
    return ota_status;
}

const char* get_latest_version(void) {
    return latest_version;
}

int get_ota_progress(void) {
    if (ota_status == OTA_UPDATE_FAILED) return -1;
    if (ota_status == OTA_UPDATE_COMPLETED) return 100;
    return ota_progress;
}

// Replace the ota_update_task function with this safer version:

static void ota_update_task(void *pvParameter) {
    // Remove task from watchdog but don't delete other tasks
    esp_task_wdt_delete(NULL);
    
    ESP_LOGI(TAG, "Starting OTA update task");
    
    // Print memory state
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Free internal memory: %d bytes", free_internal);
    
    /* Temporarily reduce CAN task priority to free up CPU for smoother UI */
    UBaseType_t original_can_priority = can_task_get_priority();
    can_task_set_priority(1);
    ESP_LOGI(TAG, "Temporarily reduced CAN task priority for smoother OTA");
    
    // Set status to in progress
    ota_status = OTA_UPDATE_IN_PROGRESS;
    ota_progress = 0;
    
    // Small delay to let UI update
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Attempt OTA update
    esp_err_t ret = start_ota_update();
    
    /* Restore CAN task priority */
    can_task_set_priority(original_can_priority);
    ESP_LOGI(TAG, "Restored CAN task priority");
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA update completed successfully");
        ota_status = OTA_UPDATE_COMPLETED;
        ota_progress = 100;

        // Wait a bit for UI to show completion, then reboot
        vTaskDelay(pdMS_TO_TICKS(3000));
        crash_log_mark_clean_shutdown();
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ret));
        ota_status = OTA_UPDATE_FAILED;
        ota_progress = -1;
    }
    
    vTaskDelete(NULL);
}

void start_ota_update_task(void) {
    ESP_LOGI(TAG, "Creating OTA update task");

    ESP_LOGI(TAG, "Free internal (before prep): %lu, free PSRAM: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ota_free_internal_ram();

    ESP_LOGI(TAG, "Free internal (after prep): %lu, largest block: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* 6 KB is enough for esp_https_ota begin/perform/finish — the deep paths
       are TLS handshake (~4-5 KB peak) and HTTP parsing. Was 8 KB; reduced
       because internal heap can be tightly fragmented at OTA time. */
    const uint32_t OTA_STACK = 6 * 1024;
    const uint32_t OTA_PRIORITY = 2;

    BaseType_t ret = xTaskCreatePinnedToCore(
        ota_update_task,
        "ota_update",
        OTA_STACK,
        NULL,
        OTA_PRIORITY,
        NULL,
        0  /* core 0 — opposite of LVGL on core 1 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        ota_status = OTA_UPDATE_FAILED;
    } else {
        ESP_LOGI(TAG, "OTA task created successfully");
        ota_status = OTA_UPDATE_IN_PROGRESS;
    }
}

// Getter functions for additional update information
float get_update_file_size_mb(void) {
    return update_file_size_mb;
}

const char* get_release_notes(void) {
    return release_notes;
}

/* SNTP — set the wall clock from a public NTP pool once WiFi STA has an
 * IP. Required so anything that builds an HMAC over a unix timestamp
 * (e.g. Share Raw CAN cloud upload, see storage/can_upload.c) lands inside
 * the worker's ±10 min replay-protection window. Without this, time() stays
 * at the post-boot epoch and the worker rejects every upload with 401.
 * Idempotent — safe to call on every reconnect. */
void initialize_sntp(void) {
    static bool s_sntp_started = false;
    if (s_sntp_started) return;
    ESP_LOGI(TAG, "Starting SNTP sync (pool.ntp.org)");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
}

// Add this function to test the download URL and network connectivity:

void debug_ota_connectivity(void) {
    ESP_LOGI(TAG, "=== OTA Connectivity Debug ===");
    
    // Check WiFi status
    wifi_ap_record_t ap_info;
    esp_err_t wifi_status = esp_wifi_sta_get_ap_info(&ap_info);
    if (wifi_status == ESP_OK) {
        ESP_LOGI(TAG, "WiFi: Connected to %s, RSSI: %d", ap_info.ssid, ap_info.rssi);
    } else {
        ESP_LOGE(TAG, "WiFi: Not connected (%s)", esp_err_to_name(wifi_status));
        return;
    }
    
    // Check if we have a download URL
    if (strlen(download_url) == 0) {
        ESP_LOGE(TAG, "Download URL is empty - run check_for_update() first");
        return;
    }
    
    ESP_LOGI(TAG, "Download URL: %s", download_url);
    
    /* Test basic HTTP connection */
    esp_http_client_config_t config = {
        .url = download_url,
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for test");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "Connection test: Status %d, Content-Length: %d", status_code, content_length);
        
        if (status_code == 200) {
            ESP_LOGI(TAG, "✓ Connection successful!");
        } else {
            ESP_LOGE(TAG, "✗ HTTP error: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "✗ Connection failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "=== End Debug ===");
} 
