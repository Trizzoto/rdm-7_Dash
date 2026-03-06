#include "web_server.h"
#include "cJSON.h"
#include "display_capture.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "layout/layout_manager.h"
#include "lvgl.h"
#include "system/rdm_settings.h"
#include "ui/dashboard.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/ui.h"
#include <sys/param.h>

/* Fallback for static-analyser builds that don't see layout_manager.h's define.
 */
#ifndef LAYOUT_MAX_FILE_BYTES
#define LAYOUT_MAX_FILE_BYTES 16384
#endif

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

/* LVGL lock helpers (defined in main.c) */
extern bool example_lvgl_lock(int timeout_ms);
extern void example_lvgl_unlock(void);

// HTML page for the web interface (Part 1)
static const char html_page_part1[] =
	"<!DOCTYPE html>"
	"<html>"
	"<head>"
	"    <title>ESP32 LVGL Display Viewer</title>"
	"    <meta charset='utf-8'>"
	"    <meta name='viewport' content='width=device-width, initial-scale=1'>"
	"    <style>"
	"        body {"
	"            font-family: Arial, sans-serif;"
	"            margin: 0;"
	"            padding: 20px;"
	"            background-color: #f0f0f0;"
	"            text-align: center;"
	"        }"
	"        .container {"
	"            max-width: 1000px;"
	"            margin: 0 auto;"
	"            background-color: white;"
	"            border-radius: 10px;"
	"            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);"
	"            padding: 20px;"
	"        }"
	"        h1 {"
	"            color: #333;"
	"            margin-bottom: 20px;"
	"        }"
	"        .display-container {"
	"            border: 2px solid #ccc;"
	"            border-radius: 5px;"
	"            display: inline-block;"
	"            background-color: #000;"
	"            margin: 20px 0;"
	"        }"
	"        #display {"
	"            max-width: 100%;"
	"            height: auto;"
	"            display: block;"
	"        }"
	"        .controls {"
	"            margin: 20px 0;"
	"        }"
	"        button {"
	"            background-color: #4CAF50;"
	"            color: white;"
	"            padding: 10px 20px;"
	"            border: none;"
	"            border-radius: 5px;"
	"            cursor: pointer;"
	"            margin: 0 10px;"
	"            font-size: 16px;"
	"        }"
	"        button:hover {"
	"            background-color: #45a049;"
	"        }"
	"        button:disabled {"
	"            background-color: #ccc;"
	"            cursor: not-allowed;"
	"        }"
	"        .status {"
	"            margin: 10px 0;"
	"            padding: 10px;"
	"            border-radius: 5px;"
	"        }"
	"        .status.connected {"
	"            background-color: #d4edda;"
	"            color: #155724;"
	"            border: 1px solid #c3e6cb;"
	"        }"
	"        .status.error {"
	"            background-color: #f8d7da;"
	"            color: #721c24;"
	"            border: 1px solid #f5c6cb;"
	"        }"
	"        .refresh-rate {"
	"            margin: 10px 0;"
	"        }"
	"        select {"
	"            padding: 5px;"
	"            font-size: 16px;"
	"        }"
	"    </style>"
	"</head>"
	"<body>"
	"    <div class='container'>"
	"        <h1>🚗 ESP32 LVGL Display Viewer</h1>"
	"        <div class='status connected' id='status'>Connected to "
	"ESP32</div>";

// HTML page part 2 (controls and canvas)
static const char html_page_part2[] =
	"        <div class='controls'>"
	"            <button onclick='captureScreenshot()'>📸 Take "
	"Screenshot</button>"
	"            <button onclick='toggleAutoRefresh()' id='autoBtn'>▶️ Start "
	"Auto Refresh</button>"
	"            <button onclick='downloadScreenshot()' id='downloadBtn' "
	"disabled>💾 Download</button>"
	"        </div>"
	"        "
	"        <div class='refresh-rate'>"
	"            <label for='layoutSelect'>Layout: </label>"
	"            <select id='layoutSelect'></select>"
	"            <button onclick='applyLayout()' id='applyLayoutBtn' "
	"style='padding: 5px 10px;'>Apply</button>"
	"        </div>"
	"        "
	"        <div class='refresh-rate'>"
	"            <label for='refreshRate'>Refresh Rate: </label>"
	"            <select id='refreshRate'>"
	"                <option value='1000'>1 second</option>"
	"                <option value='2000' selected>2 seconds</option>"
	"                <option value='5000'>5 seconds</option>"
	"                <option value='10000'>10 seconds</option>"
	"            </select>"
	"        </div>"
	"        "
	"        <div class='display-container'>"
	"            <canvas id='display' width='800' height='480'></canvas>"
	"        </div>"
	"        "
	"        <div id='info'>"
	"            <p>Display Size: 800x480 pixels</p>"
	"            <p>Last Update: <span id='lastUpdate'>Never</span></p>"
	"        </div>"
	"    </div>";

// JavaScript part 1
static const char html_page_part3[] =
	"    <script>"
	"        let autoRefreshInterval = null;"
	"        let isAutoRefreshing = false;"
	"        let lastScreenshotBlob = null;"
	"        "
	"        const canvas = document.getElementById('display');"
	"        const ctx = canvas.getContext('2d');"
	"        const statusDiv = document.getElementById('status');"
	"        const autoBtn = document.getElementById('autoBtn');"
	"        const downloadBtn = document.getElementById('downloadBtn');"
	"        const lastUpdateSpan = document.getElementById('lastUpdate');"
	"        "
	"        function updateStatus(message, isError = false) {"
	"            statusDiv.textContent = message;"
	"            statusDiv.className = isError ? 'status error' : 'status "
	"connected';"
	"        }"
	"        "
	"        function convertRGB565ToImageData(buffer, width, height) {"
	"            const imageData = ctx.createImageData(width, height);"
	"            const data = imageData.data;"
	"            "
	"            for (let i = 0; i < buffer.length; i += 2) {"
	"                const pixel565 = (buffer[i + 1] << 8) | buffer[i];"
	"                const pixelIndex = (i / 2) * 4;"
	"                "
	"                const r = ((pixel565 >> 11) & 0x1F) << 3;"
	"                const g = ((pixel565 >> 5) & 0x3F) << 2;"
	"                const b = (pixel565 & 0x1F) << 3;"
	"                "
	"                data[pixelIndex] = r;"
	"                data[pixelIndex + 1] = g;"
	"                data[pixelIndex + 2] = b;"
	"                data[pixelIndex + 3] = 255;"
	"            }"
	"            "
	"            return imageData;"
	"        }";

// JavaScript part 2 (functions)
static const char html_page_part4[] =
	"        async function captureScreenshot() {"
	"            try {"
	"                updateStatus('Capturing screenshot...');"
	"                const response = await fetch('/screenshot');"
	"                "
	"                if (!response.ok) {"
	"                    throw new Error(`HTTP ${response.status}`);"
	"                }"
	"                "
	"                const arrayBuffer = await response.arrayBuffer();"
	"                const uint8Array = new Uint8Array(arrayBuffer);"
	"                "
	"                const imageData = convertRGB565ToImageData(uint8Array, "
	"800, 480);"
	"                ctx.putImageData(imageData, 0, 0);"
	"                "
	"                canvas.toBlob((blob) => {"
	"                    lastScreenshotBlob = blob;"
	"                    downloadBtn.disabled = false;"
	"                });"
	"                "
	"                lastUpdateSpan.textContent = new "
	"Date().toLocaleTimeString();"
	"                updateStatus('Screenshot captured successfully');"
	"                "
	"            } catch (error) {"
	"                console.error('Screenshot error:', error);"
	"                updateStatus('Failed to capture screenshot: ' + "
	"error.message, true);"
	"            }"
	"        }"
	"        "
	"        function toggleAutoRefresh() {"
	"            if (isAutoRefreshing) {"
	"                clearInterval(autoRefreshInterval);"
	"                autoBtn.textContent = '▶️ Start Auto Refresh';"
	"                isAutoRefreshing = false;"
	"                updateStatus('Auto refresh stopped');"
	"            } else {"
	"                const refreshRate = "
	"parseInt(document.getElementById('refreshRate').value);"
	"                autoRefreshInterval = setInterval(captureScreenshot, "
	"refreshRate);"
	"                autoBtn.textContent = '⏸️ Stop Auto Refresh';"
	"                isAutoRefreshing = true;"
	"                updateStatus('Auto refresh started');"
	"                captureScreenshot();"
	"            }"
	"        }"
	"        "
	"        function downloadScreenshot() {"
	"            if (lastScreenshotBlob) {"
	"                const url = URL.createObjectURL(lastScreenshotBlob);"
	"                const a = document.createElement('a');"
	"                a.href = url;"
	"                a.download = 'esp32_display_' + new "
	"Date().toISOString().replace(/[:.]/g, '-') + '.png';"
	"                document.body.appendChild(a);"
	"                a.click();"
	"                document.body.removeChild(a);"
	"                URL.revokeObjectURL(url);"
	"            }"
	"        }"
	"        "
	"        document.getElementById('refreshRate').addEventListener('change', "
	"function() {"
	"            if (isAutoRefreshing) {"
	"                toggleAutoRefresh();"
	"                toggleAutoRefresh();"
	"            }"
	"        });"
	"        "
	"        async function fetchLayouts() {"
	"            try {"
	"                const response = await fetch('/api/layout/list');"
	"                if (response.ok) {"
	"                    const data = await response.json();"
	"                    const select = "
	"document.getElementById('layoutSelect');"
	"                    select.innerHTML = '';"
	"                    data.layouts.forEach(layout => {"
	"                        const option = document.createElement('option');"
	"                        option.value = layout;"
	"                        option.textContent = layout;"
	"                        if (layout === data.active) option.selected = "
	"true;"
	"                        select.appendChild(option);"
	"                    });"
	"                }"
	"            } catch (e) { console.error('Failed to fetch layouts', e); }"
	"        }"
	"        "
	"        async function applyLayout() {"
	"            const select = document.getElementById('layoutSelect');"
	"            const layoutName = select.value;"
	"            if (!layoutName) return;"
	"            try {"
	"                updateStatus('Applying layout...');"
	"                const response = await fetch('/api/layout/set', { "
	"                    method: 'POST', "
	"                    headers: {'Content-Type': 'application/json'}, "
	"                    body: JSON.stringify({name: layoutName}) "
	"                });"
	"                if (response.ok) {"
	"                    updateStatus('Layout applied successfully');"
	"                    setTimeout(captureScreenshot, 1000);"
	"                } else { updateStatus('Failed to apply layout', true); }"
	"            } catch (e) { updateStatus('Failed to apply layout: ' + "
	"e.message, true); }"
	"        }"
	"        "
	"        fetchLayouts();"
	"        captureScreenshot();"
	"    </script>"
	"</body>"
	"</html>";

// HTTP handler for the main page
static esp_err_t index_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html");

	// Send HTML in parts to avoid string length limitations
	httpd_resp_send_chunk(req, html_page_part1, HTTPD_RESP_USE_STRLEN);
	httpd_resp_send_chunk(req, html_page_part2, HTTPD_RESP_USE_STRLEN);
	httpd_resp_send_chunk(req, html_page_part3, HTTPD_RESP_USE_STRLEN);
	httpd_resp_send_chunk(req, html_page_part4, HTTPD_RESP_USE_STRLEN);

	// End response
	httpd_resp_send_chunk(req, NULL, 0);

	return ESP_OK;
}

// HTTP handler for screenshot API
static esp_err_t screenshot_handler(httpd_req_t *req) {
	ESP_LOGI(TAG, "Screenshot requested");

	uint8_t *screenshot_buffer = NULL;
	size_t screenshot_size = 0;

	esp_err_t ret =
		display_capture_screenshot(&screenshot_buffer, &screenshot_size);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to capture screenshot: %s", esp_err_to_name(ret));
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Screenshot capture failed");
		return ESP_FAIL;
	}

	// Set headers for binary data
	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

	// Send the screenshot data
	esp_err_t send_ret =
		httpd_resp_send(req, (const char *)screenshot_buffer, screenshot_size);

	// Clean up
	display_capture_free_buffer(screenshot_buffer);

	if (send_ret == ESP_OK) {
		ESP_LOGI(TAG, "Screenshot sent successfully (%zu bytes)",
				 screenshot_size);
	} else {
		ESP_LOGE(TAG, "Failed to send screenshot");
	}

	return send_ret;
}

// URI handlers
static const httpd_uri_t index_uri = {
	.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};

static const httpd_uri_t screenshot_uri = {.uri = "/screenshot",
										   .method = HTTP_GET,
										   .handler = screenshot_handler,
										   .user_ctx = NULL};

// HTTP handler for exporting current layout JSON
static esp_err_t layout_current_handler(httpd_req_t *req) {
	// Get active layout name (for the "name" field)
	char layout_name[LAYOUT_MAX_NAME];
	if (rdm_settings_get_active_layout(layout_name, sizeof(layout_name)) !=
		ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to read active layout name");
		return ESP_FAIL;
	}

	// Snapshot current widgets from the dashboard/registry
	widget_t **widgets = dashboard_get_widgets();
	uint8_t count = dashboard_get_widget_count();

	cJSON *root = layout_manager_build_json(layout_name, widgets, count);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to build layout JSON");
		return ESP_FAIL;
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to serialise layout JSON");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

	esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

static const httpd_uri_t layout_current_uri = {.uri = "/api/layout/current",
											   .method = HTTP_GET,
											   .handler =
												   layout_current_handler,
											   .user_ctx = NULL};

// HTTP handler for importing/saving a new layout JSON
static esp_err_t layout_save_handler(httpd_req_t *req) {
	int total_len = req->content_len;
	if (total_len <= 0 || total_len > LAYOUT_MAX_FILE_BYTES) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid layout size");
		return ESP_FAIL;
	}

	char *buf = malloc(total_len + 1);
	if (!buf) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Out of memory");
		return ESP_FAIL;
	}

	int received = 0;
	while (received < total_len) {
		int r = httpd_req_recv(req, buf + received, total_len - received);
		if (r <= 0) {
			free(buf);
			if (r == HTTPD_SOCK_ERR_TIMEOUT) {
				httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT,
									"Request timeout");
			} else {
				httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
									"Failed to receive body");
			}
			return ESP_FAIL;
		}
		received += r;
	}
	buf[received] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	cJSON *widgets_arr = cJSON_GetObjectItemCaseSensitive(root, "widgets");
	if (!cJSON_IsString(name_item) || !cJSON_IsArray(widgets_arr)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Missing or invalid 'name'/'widgets'");
		return ESP_FAIL;
	}

	/* Copy layout name out of the cJSON tree so it remains valid after
	 * cJSON_Delete(). name_item->valuestring points into root's memory. */
	char layout_name[LAYOUT_MAX_NAME];
	strncpy(layout_name, name_item->valuestring, sizeof(layout_name) - 1);
	layout_name[sizeof(layout_name) - 1] = '\0';

	// Persist raw JSON to LittleFS
	esp_err_t err = layout_manager_save_raw(layout_name, root);
	cJSON_Delete(root);
	if (err != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to save layout to LittleFS");
		return ESP_FAIL;
	}

	// Update active layout name in NVS
	if (rdm_settings_set_active_layout(layout_name) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to set active layout");
		return ESP_FAIL;
	}

	// Reload Screen3 layout under LVGL lock to apply changes live
	if (example_lvgl_lock(1000)) {
		lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
		ui_Screen3_screen_init();
		lv_scr_load(ui_Screen3);
		if (old && old != ui_Screen3)
			lv_obj_del(old);
		example_lvgl_unlock();
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	const char *ok = "{\"status\":\"ok\"}";
	return httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t layout_save_uri = {.uri = "/api/layout/save",
											.method = HTTP_POST,
											.handler = layout_save_handler,
											.user_ctx = NULL};

static esp_err_t layout_list_handler(httpd_req_t *req) {
	char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
	int count = layout_manager_list(names, LAYOUT_MAX_COUNT);
	if (count < 0) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to list layouts");
		return ESP_FAIL;
	}

	char active_name[LAYOUT_MAX_NAME];
	if (layout_manager_get_active(active_name, sizeof(active_name)) != ESP_OK) {
		strcpy(active_name, "default");
	}

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "active", active_name);
	cJSON *arr = cJSON_AddArrayToObject(root, "layouts");
	for (int i = 0; i < count; i++) {
		cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
	}

	char *json_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to serialize JSON");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
	free(json_str);
	return res;
}

static const httpd_uri_t layout_list_uri = {.uri = "/api/layout/list",
											.method = HTTP_GET,
											.handler = layout_list_handler,
											.user_ctx = NULL};

static esp_err_t layout_set_handler(httpd_req_t *req) {
	char buf[128];
	int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
	if (received <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
							"Failed to receive body");
		return ESP_FAIL;
	}
	buf[received] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
		return ESP_FAIL;
	}

	cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	if (!cJSON_IsString(name_item)) {
		cJSON_Delete(root);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name'");
		return ESP_FAIL;
	}

	char layout_name[LAYOUT_MAX_NAME];
	strncpy(layout_name, name_item->valuestring, sizeof(layout_name) - 1);
	layout_name[sizeof(layout_name) - 1] = '\0';
	cJSON_Delete(root);

	if (layout_manager_set_active(layout_name) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
							"Failed to set active layout");
		return ESP_FAIL;
	}

	if (example_lvgl_lock(1000)) {
		lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
		ui_Screen3_screen_init();
		lv_scr_load(ui_Screen3);
		if (old && old != ui_Screen3)
			lv_obj_del(old);
		example_lvgl_unlock();
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t layout_set_uri = {.uri = "/api/layout/set",
										   .method = HTTP_POST,
										   .handler = layout_set_handler,
										   .user_ctx = NULL};

esp_err_t web_server_start(void) {
	if (server != NULL) {
		ESP_LOGW(TAG, "Web server already running");
		return ESP_OK;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = WEB_SERVER_PORT;
	/* Increase stack size to handle LVGL snapshot + capture logic safely. */
	config.stack_size = 8192;
	config.max_uri_handlers = 12;
	config.max_resp_headers = 8;
	config.lru_purge_enable = true;

	ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

	esp_err_t ret = httpd_start(&server, &config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
		return ret;
	}

	// Register URI handlers
	httpd_register_uri_handler(server, &index_uri);
	httpd_register_uri_handler(server, &screenshot_uri);
	httpd_register_uri_handler(server, &layout_current_uri);
	httpd_register_uri_handler(server, &layout_save_uri);
	httpd_register_uri_handler(server, &layout_list_uri);
	httpd_register_uri_handler(server, &layout_set_uri);

	ESP_LOGI(TAG, "Web server started successfully");
	return ESP_OK;
}

esp_err_t web_server_stop(void) {
	if (server == NULL) {
		ESP_LOGW(TAG, "Web server not running");
		return ESP_OK;
	}

	esp_err_t ret = httpd_stop(server);
	if (ret == ESP_OK) {
		server = NULL;
		ESP_LOGI(TAG, "Web server stopped");
	} else {
		ESP_LOGE(TAG, "Failed to stop web server: %s", esp_err_to_name(ret));
	}

	return ret;
}

bool web_server_is_running(void) { return (server != NULL); }