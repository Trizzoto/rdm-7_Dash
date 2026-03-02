#include "web_server.h"
#include "display_capture.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <sys/param.h>

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

// HTML page for the web interface (Part 1)
static const char html_page_part1[] = "<!DOCTYPE html>"
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
"        <div class='status connected' id='status'>Connected to ESP32</div>";

// HTML page part 2 (controls and canvas)
static const char html_page_part2[] = 
"        <div class='controls'>"
"            <button onclick='captureScreenshot()'>📸 Take Screenshot</button>"
"            <button onclick='toggleAutoRefresh()' id='autoBtn'>▶️ Start Auto Refresh</button>"
"            <button onclick='downloadScreenshot()' id='downloadBtn' disabled>💾 Download</button>"
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
"            statusDiv.className = isError ? 'status error' : 'status connected';"
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
"                const imageData = convertRGB565ToImageData(uint8Array, 800, 480);"
"                ctx.putImageData(imageData, 0, 0);"
"                "
"                canvas.toBlob((blob) => {"
"                    lastScreenshotBlob = blob;"
"                    downloadBtn.disabled = false;"
"                });"
"                "
"                lastUpdateSpan.textContent = new Date().toLocaleTimeString();"
"                updateStatus('Screenshot captured successfully');"
"                "
"            } catch (error) {"
"                console.error('Screenshot error:', error);"
"                updateStatus('Failed to capture screenshot: ' + error.message, true);"
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
"                const refreshRate = parseInt(document.getElementById('refreshRate').value);"
"                autoRefreshInterval = setInterval(captureScreenshot, refreshRate);"
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
"                a.download = 'esp32_display_' + new Date().toISOString().replace(/[:.]/g, '-') + '.png';"
"                document.body.appendChild(a);"
"                a.click();"
"                document.body.removeChild(a);"
"                URL.revokeObjectURL(url);"
"            }"
"        }"
"        "
"        document.getElementById('refreshRate').addEventListener('change', function() {"
"            if (isAutoRefreshing) {"
"                toggleAutoRefresh();"
"                toggleAutoRefresh();"
"            }"
"        });"
"        "
"        captureScreenshot();"
"    </script>"
"</body>"
"</html>";

// HTTP handler for the main page
static esp_err_t index_handler(httpd_req_t *req)
{
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
static esp_err_t screenshot_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Screenshot requested");
    
    uint8_t *screenshot_buffer = NULL;
    size_t screenshot_size = 0;
    
    esp_err_t ret = display_capture_screenshot(&screenshot_buffer, &screenshot_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to capture screenshot: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Screenshot capture failed");
        return ESP_FAIL;
    }
    
    // Set headers for binary data
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    
    // Send the screenshot data
    esp_err_t send_ret = httpd_resp_send(req, (const char*)screenshot_buffer, screenshot_size);
    
    // Clean up
    display_capture_free_buffer(screenshot_buffer);
    
    if (send_ret == ESP_OK) {
        ESP_LOGI(TAG, "Screenshot sent successfully (%zu bytes)", screenshot_size);
    } else {
        ESP_LOGE(TAG, "Failed to send screenshot");
    }
    
    return send_ret;
}

// URI handlers
static const httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t screenshot_uri = {
    .uri       = "/screenshot",
    .method    = HTTP_GET,
    .handler   = screenshot_handler,
    .user_ctx  = NULL
};

esp_err_t web_server_start(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = 10;
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
    
    ESP_LOGI(TAG, "Web server started successfully");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
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

bool web_server_is_running(void)
{
    return (server != NULL);
} 