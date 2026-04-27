/* esp_log.h stub for native-host tests — silences the logging macros so
 * widget_rules.c (and any other firmware source pulled in directly) can
 * compile without the real ESP-IDF logging library.
 *
 * Switch the bodies to printf if you want the chatter while debugging. */
#ifndef RDM_TEST_MOCK_ESP_LOG_H
#define RDM_TEST_MOCK_ESP_LOG_H

#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#define ESP_LOGV(tag, fmt, ...) ((void)(tag))

#endif /* RDM_TEST_MOCK_ESP_LOG_H */
