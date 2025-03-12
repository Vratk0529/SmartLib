#ifndef ESP_LOG_COMPAT_H
#define ESP_LOG_COMPAT_H

#if defined(ESP8266)

#define ESP_LOGE(tag, format, ...) Serial.printf("[E] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) Serial.printf("[W] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) Serial.printf("[I] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) Serial.printf("[D] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) Serial.printf("[V] %s: " format "\n", tag, ##__VA_ARGS__)

#else

#include "esp_log.h"

#endif

#endif