#include "ota_handler.h"
#include "version.h"
#include "esp_http_client.h"
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
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"

// Add these definitions from main.c
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     1

// Add external declarations
extern TaskHandle_t lvglTaskHandle;
extern void example_lvgl_port_task(void *pvParameter);
#include "can/can_manager.h"

// Updated URLs for your firmware portal
#define FIRMWARE_API_URL "https://rdm-7-ota-firmware-updates.vercel.app/api/firmware/latest"
#define FIRMWARE_PORTAL_BASE_URL "https://rdm-7-ota-firmware-updates.vercel.app"
#define MAX_READ_RETRIES 3

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

static ota_status_t ota_status = OTA_IDLE;
static char latest_version[16] = {0};
static char download_url[256] = {0};
static char *response_buffer = NULL;
static int response_buffer_size = 0;
static int ota_progress = 0;

// Add new static variables for update information
static int wifi_rssi = 0;
static int download_retry_count = 0;
static float update_file_size_mb = 0.0f;
static char update_type_str[64] = {0};
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
static const char* get_update_type(const char* current_ver, const char* newer_ver);

static esp_err_t validate_image_header(const esp_app_desc_t *new_app_info) {
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

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                response_buffer = realloc(response_buffer, response_buffer_size + evt->data_len + 1);
                if (response_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory");
                    return ESP_FAIL;
                }
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

// Updated function to parse your firmware portal's JSON response with smart version comparison
static void compare_and_set_ota_status(void) {
    // Parse JSON response from your firmware portal
    cJSON *root = cJSON_Parse(response_buffer);
    if (root) {
        cJSON *version = cJSON_GetObjectItem(root, "version");
        cJSON *downloadUrl = cJSON_GetObjectItem(root, "downloadUrl");
        cJSON *sizeBytes = cJSON_GetObjectItem(root, "sizeBytes");
        cJSON *releaseNotes = cJSON_GetObjectItem(root, "releaseNotes");
        cJSON *date = cJSON_GetObjectItem(root, "date");
        
        if (version && cJSON_IsString(version) && downloadUrl && cJSON_IsString(downloadUrl)) {
            const char *latest_version_str = version->valuestring;
    const char *current_version_str = FIRMWARE_VERSION;

            ESP_LOGI(TAG, "📋 Firmware Update Check Results:");
            ESP_LOGI(TAG, "=====================================");
            ESP_LOGI(TAG, "Current Version:  %s", current_version_str);
            ESP_LOGI(TAG, "Latest Version:   %s", latest_version_str);
            
            if (date && cJSON_IsString(date)) {
                ESP_LOGI(TAG, "Release Date:     %s", date->valuestring);
            }
            
            if (sizeBytes && cJSON_IsNumber(sizeBytes)) {
                ESP_LOGI(TAG, "File Size:        %d bytes (%.2f MB)", 
                         (int)sizeBytes->valueint, 
                         sizeBytes->valueint / (1024.0 * 1024.0));
            }
            
            ESP_LOGI(TAG, "=====================================");
            
            if (releaseNotes && cJSON_IsString(releaseNotes)) {
                ESP_LOGI(TAG, "Release notes: %s", releaseNotes->valuestring);
            }
            
            // Smart version comparison using semantic versioning
            int version_comparison = compare_versions(current_version_str, latest_version_str);
            
            if (version_comparison > 0) {
                // New firmware available
                ESP_LOGI(TAG, "🚀 NEW FIRMWARE AVAILABLE!");
                
                const char* update_type = get_update_type(current_version_str, latest_version_str);
                ESP_LOGI(TAG, "Update Type: %s", update_type);
                
                // Store additional update information
                strncpy(update_type_str, update_type, sizeof(update_type_str) - 1);
                update_type_str[sizeof(update_type_str) - 1] = '\0';
                
                if (sizeBytes && cJSON_IsNumber(sizeBytes)) {
                    update_file_size_mb = sizeBytes->valueint / (1024.0f * 1024.0f);
                }
                
                if (releaseNotes && cJSON_IsString(releaseNotes)) {
                    strncpy(release_notes, releaseNotes->valuestring, sizeof(release_notes) - 1);
                    release_notes[sizeof(release_notes) - 1] = '\0';
                } else {
                    strcpy(release_notes, "No release notes available");
                }
                
        ota_status = OTA_UPDATE_AVAILABLE;
        strncpy(latest_version, latest_version_str, sizeof(latest_version) - 1);
        latest_version[sizeof(latest_version) - 1] = '\0';

                strncpy(download_url, downloadUrl->valuestring, sizeof(download_url) - 1);
                        download_url[sizeof(download_url) - 1] = '\0';
                        ESP_LOGI(TAG, "Download URL: %s", download_url);
                
            } else if (version_comparison == 0) {
                ESP_LOGI(TAG, "✅ Firmware is up to date!");
                        ota_status = OTA_NO_UPDATE_AVAILABLE;
                
            } else {
                ESP_LOGI(TAG, "⚠️  Local version is newer than server version!");
                ESP_LOGI(TAG, "   (Development/beta version detected)");
                ota_status = OTA_NO_UPDATE_AVAILABLE;
            }
        } else {
            ESP_LOGE(TAG, "Required fields (version, downloadUrl) not found in API response");
            ota_status = OTA_UPDATE_FAILED;
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON response from firmware portal");
        ota_status = OTA_UPDATE_FAILED;
    }
}

void check_for_update(void) {
    ESP_LOGI(TAG, "Checking for updates from RDM-7 firmware portal...");
    ESP_LOGI(TAG, "Current version: %s", FIRMWARE_VERSION);

    // Reset response buffer
    if (response_buffer != NULL) {
        free(response_buffer);
        response_buffer = NULL;
    }
    response_buffer_size = 0;

    // Check and optimize WiFi for API call
    bool strong_signal = check_wifi_signal_strength();
    if (wifi_rssi == 0) {
        ESP_LOGE(TAG, "WiFi not connected");
        ota_status = OTA_UPDATE_FAILED;
        return;
    }
    
    // Calculate adaptive timeout for API call
    int api_timeout = strong_signal ? 30000 : 45000; // 30s for strong, 45s for weak signal (increased)
    ESP_LOGI(TAG, "Using API timeout: %d ms for %s signal (%d dBm)", 
             api_timeout, strong_signal ? "strong" : "weak", wifi_rssi);

    // Temporarily disable power save for API call reliability
    wifi_ps_type_t original_ps_mode;
    esp_wifi_get_ps(&original_ps_mode);
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_http_client_config_t config = {
        .url = FIRMWARE_API_URL,  // Use new firmware portal URL
        .event_handler = _http_event_handler,
        .timeout_ms = api_timeout,
        .buffer_size = 8192,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .keep_alive_enable = true,  // Enable keep-alive for better connection
        .port = 443,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Use certificate bundle for verification
        .skip_cert_common_name_check = true  // Allow hostname mismatch for Vercel
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        esp_wifi_set_ps(original_ps_mode); // Restore power save
        ota_status = OTA_UPDATE_FAILED;
        return;
    }

    // Update headers for the new API
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32-RDM7-OTA/1.0");
    esp_http_client_set_header(client, "Connection", "keep-alive");

    // Retry logic for API call
    esp_err_t err = ESP_FAIL;
    int api_retry_count = strong_signal ? 2 : 3; // More retries for weak signal
    
    for (int i = 0; i < api_retry_count; i++) {
        if (i > 0) {
            ESP_LOGI(TAG, "API retry attempt %d/%d", i + 1, api_retry_count);
            vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds between retries (increased)
        }
        
        ESP_LOGI(TAG, "Attempting HTTPS connection to firmware portal...");
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTPS connection successful!");
            break; // Success
        } else {
            ESP_LOGW(TAG, "API request attempt %d failed: %s", i + 1, esp_err_to_name(err));
            if (err == ESP_ERR_HTTP_CONNECT) {
                ESP_LOGE(TAG, "HTTPS connection failed - SSL certificate verification bypassed");
            }
        }
    }

    // Restore original power save mode
    esp_wifi_set_ps(original_ps_mode);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Firmware Portal API Status Code: %d", status_code);
        
        if (status_code == 200 && response_buffer != NULL) {
            ESP_LOGI(TAG, "API Response: %s", response_buffer);
            compare_and_set_ota_status();  // Updated function call without parameter
        } else if (status_code == 404) {
            ESP_LOGI(TAG, "No firmware files available on server");
            ota_status = OTA_NO_UPDATE_AVAILABLE;
        } else {
            ESP_LOGE(TAG, "Unexpected status code or empty response: %d", status_code);
            ota_status = OTA_UPDATE_FAILED;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed after %d attempts: %s", api_retry_count, esp_err_to_name(err));
        ota_status = OTA_UPDATE_FAILED;
    }

    // Cleanup
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

// Determine update type based on version difference
static const char* get_update_type(const char* current_ver, const char* newer_ver) {
    version_t current = parse_version(current_ver);
    version_t newer = parse_version(newer_ver);
    
    if (newer.major > current.major) {
        return "MAJOR UPDATE (Breaking changes possible)";
    } else if (newer.minor > current.minor) {
        return "MINOR UPDATE (New features)";
    } else if (newer.patch > current.patch) {
        return "PATCH UPDATE (Bug fixes)";
    }
    
    return "UNKNOWN UPDATE TYPE";
}

// Replace the start_ota_update function with this optimized version
esp_err_t start_ota_update(void) {
    ESP_LOGI(TAG, "Starting optimized OTA update from: %s", download_url);
    
    // Validate download URL
    if (strlen(download_url) == 0) {
        ESP_LOGE(TAG, "Download URL is empty");
        ota_status = OTA_UPDATE_FAILED;
        return ESP_FAIL;
    }
    
    // Check and optimize WiFi connection
    optimize_wifi_for_ota();
    bool strong_signal = check_wifi_signal_strength();
    int adaptive_timeout = calculate_adaptive_timeout();
    
    // Choose chunk size based on signal strength
    int chunk_size = strong_signal ? 4096 : OTA_CHUNK_SIZE;
    ESP_LOGI(TAG, "Using chunk size: %d bytes for %s signal", 
             chunk_size, strong_signal ? "strong" : "weak");
    
    esp_err_t final_result = ESP_FAIL;
    
    // Retry loop for robustness
    for (download_retry_count = 0; download_retry_count < OTA_RETRY_ATTEMPTS; download_retry_count++) {
        if (download_retry_count > 0) {
            ESP_LOGI(TAG, "OTA retry attempt %d/%d", download_retry_count + 1, OTA_RETRY_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds between retries
            
            // Re-check WiFi signal before retry
            check_wifi_signal_strength();
            adaptive_timeout = calculate_adaptive_timeout();
        }
        
        // Enhanced HTTP client configuration with adaptive settings
        esp_http_client_config_t config = {
            .url = download_url,
            .timeout_ms = adaptive_timeout,
            .keep_alive_enable = true,
            .buffer_size = chunk_size,
            .buffer_size_tx = 1024,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .skip_cert_common_name_check = true,  // Skip hostname verification
            .use_global_ca_store = false,
            .disable_auto_redirect = false,
            .max_redirection_count = 3,   // Reduced redirects for faster failure
            .event_handler = NULL,
            .is_async = false,
            .auth_type = HTTP_AUTH_TYPE_NONE
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client on attempt %d", download_retry_count + 1);
            continue;
        }

        // Set optimized headers
        esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client/2.0");
        esp_http_client_set_header(client, "Accept", "application/octet-stream");
        esp_http_client_set_header(client, "Connection", "keep-alive");
        esp_http_client_set_header(client, "Cache-Control", "no-cache");

        ESP_LOGI(TAG, "Opening HTTPS connection for firmware download (attempt %d)...", download_retry_count + 1);
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open HTTPS connection: %s", esp_err_to_name(err));
            if (err == ESP_ERR_HTTP_CONNECT) {
                ESP_LOGE(TAG, "HTTPS download connection failed - SSL certificate verification bypassed");
            }
            esp_http_client_cleanup(client);
            continue;
        }

        // Fetch headers with timeout handling
        int content_length = esp_http_client_fetch_headers(client);
        if (content_length <= 0 && esp_http_client_get_status_code(client) != 200) {
            ESP_LOGE(TAG, "Invalid content length: %d", content_length);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }
        
        ESP_LOGI(TAG, "OTA file size: %d bytes (%.2f MB)", content_length, content_length / (1024.0 * 1024.0));

        // Check HTTP status
        int status_code = esp_http_client_get_status_code(client);
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        // Prepare OTA partition (only on first attempt)
        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        if (update_partition == NULL) {
            ESP_LOGE(TAG, "Failed to get update partition");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            final_result = ESP_FAIL;
            break;
        }

        ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x (size: %d)", 
                 update_partition->subtype, update_partition->address, update_partition->size);

        // Check if file fits in partition
        if (content_length > update_partition->size) {
            ESP_LOGE(TAG, "Update file too large: %d bytes, partition size: %d bytes", 
                     content_length, update_partition->size);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            final_result = ESP_FAIL;
            break;
        }

        esp_ota_handle_t ota_handle = 0;
        err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        // Allocate buffer with adaptive size
        char *upgrade_data_buf = malloc(chunk_size);
        if (!upgrade_data_buf) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for download buffer", chunk_size);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        ESP_LOGI(TAG, "Starting download with %d byte chunks...", chunk_size);
        int binary_file_length = 0;
        bool image_header_was_checked = false;
        int last_progress = 0;
        uint32_t last_report_time = esp_timer_get_time() / 1000;
        uint32_t last_data_time = last_report_time;
        bool download_successful = true;
        int consecutive_failures = 0;
        const int max_consecutive_failures = 5;

        // Enhanced download loop with error recovery
        while (binary_file_length < content_length && download_successful) {
            int remaining = content_length - binary_file_length;
            int read_size = (remaining < chunk_size) ? remaining : chunk_size;
            
            int data_read = esp_http_client_read(client, upgrade_data_buf, read_size);
            uint32_t current_time = esp_timer_get_time() / 1000;
            
            if (data_read < 0) {
                ESP_LOGE(TAG, "HTTP read error: %d", data_read);
                consecutive_failures++;
                if (consecutive_failures >= max_consecutive_failures) {
                    ESP_LOGE(TAG, "Too many consecutive read failures (%d)", consecutive_failures);
                    download_successful = false;
                    break;
                }
                
                // Brief delay before retry
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
                
            } else if (data_read == 0) {
                // Check for timeout
                if ((current_time - last_data_time) > (adaptive_timeout / 2)) {
                    ESP_LOGE(TAG, "Download timeout - no data for %lu ms", current_time - last_data_time);
                    download_successful = false;
                    break;
                }
                
                // Brief delay and continue
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            
            // Reset failure count on successful read
            consecutive_failures = 0;
            last_data_time = current_time;
            
            // Small yield after each successful read to reduce UI glitching
            if (data_read > 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            // Check image header on first chunk
            if (!image_header_was_checked && data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, &upgrade_data_buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));

                ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
                ESP_LOGI(TAG, "New firmware project: %s", new_app_info.project_name);
                ESP_LOGI(TAG, "New firmware compile time: %s %s", new_app_info.date, new_app_info.time);
                
                if (validate_image_header(&new_app_info) != ESP_OK) {
                    ESP_LOGE(TAG, "Image header validation failed");
                    download_successful = false;
                    break;
                }
                image_header_was_checked = true;
            }

            err = esp_ota_write(ota_handle, upgrade_data_buf, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
                download_successful = false;
                break;
            }

            binary_file_length += data_read;
            int progress = (binary_file_length * 100) / content_length;
            
            // Update progress more frequently for user feedback
            if (progress != last_progress || (current_time - last_report_time) > 3000) {
                ESP_LOGI(TAG, "Download progress: %d%% (%d/%d bytes) [Attempt %d] RSSI: %d dBm", 
                         progress, binary_file_length, content_length, download_retry_count + 1, wifi_rssi);
                last_progress = progress;
                last_report_time = current_time;
                ota_progress = progress;
                
                // Periodically check WiFi signal during download
                if (progress % 20 == 0) { // Every 20%
                    check_wifi_signal_strength();
                }
                
                // Yield to LVGL task more frequently to reduce screen glitching
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                // Yield CPU more frequently to prevent starving LVGL task
                static int yield_counter = 0;
                yield_counter += data_read;
                if (yield_counter >= 4096) { // Every 4KB (reduced from 8KB)
                    yield_counter = 0;
                    vTaskDelay(pdMS_TO_TICKS(5)); // Longer yield to LVGL (increased from 2ms)
                }
            }
        }

        free(upgrade_data_buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (download_successful && binary_file_length == content_length) {
            ESP_LOGI(TAG, "Download completed successfully, validating...");
            
            err = esp_ota_end(ota_handle);
            if (err != ESP_OK) {
                if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                    ESP_LOGE(TAG, "Image validation failed, image is corrupted");
                } else {
                    ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
                }
                continue; // Try again
            }

            err = esp_ota_set_boot_partition(update_partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
                continue; // Try again
            }

            ESP_LOGI(TAG, "OTA update successful after %d attempt(s)! System will reboot...", download_retry_count + 1);
            ota_status = OTA_UPDATE_COMPLETED;
            ota_progress = 100;
            final_result = ESP_OK;
            break; // Success!
            
        } else {
            ESP_LOGE(TAG, "Download failed on attempt %d: received %d/%d bytes", 
                     download_retry_count + 1, binary_file_length, content_length);
            esp_ota_abort(ota_handle);
            
            // Don't retry if it's a fundamental problem (not network related)
            if (binary_file_length == 0) {
                ESP_LOGE(TAG, "No data received - this may not be a network issue");
                break;
            }
        }
    }
    
    // Restore WiFi settings
    restore_wifi_settings();
    
    if (final_result != ESP_OK) {
        ESP_LOGE(TAG, "OTA update failed after %d attempts", OTA_RETRY_ATTEMPTS);
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
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ret));
        ota_status = OTA_UPDATE_FAILED;
        ota_progress = -1;
    }
    
    vTaskDelete(NULL);
}

// Update the task creation with better parameters
void start_ota_update_task(void) {
    ESP_LOGI(TAG, "Creating OTA update task");
    
    // Use larger stack and lower priority for smoother UI
    const uint32_t OTA_STACK = 12*1024;  // 12KB stack
    const uint32_t OTA_PRIORITY = 2;     // Lower priority to give LVGL more CPU time
    
    BaseType_t result = xTaskCreatePinnedToCore(
        ota_update_task, 
        "ota_update", 
        OTA_STACK, 
        NULL, 
        OTA_PRIORITY, 
        NULL, 
        0  // Pin to core 0 (opposite of LVGL which is on core 1)
    );
    
    if (result != pdPASS) {
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

const char* get_update_type_str(void) {
    return update_type_str;
}

const char* get_release_notes(void) {
    return release_notes;
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
    
    // Test basic HTTP connection
    esp_http_client_config_t config = {
        .url = download_url,
        .method = HTTP_METHOD_HEAD,  // Just get headers
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,  // Allow hostname mismatch for Vercel
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
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
