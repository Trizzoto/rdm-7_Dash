#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

// Web server configuration
#define WEB_SERVER_PORT 80
#define MAX_FILE_SIZE   (200*1024) // 200KB max file size

// Function declarations
esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);
bool web_server_is_running(void);

#endif // WEB_SERVER_H 