// #pragma once

// // WiFi
// #define WIFI_SSID "DESKTOPT_440"
// #define WIFI_PASS "aabbccdd"

// // Firmware version
// #define FIRMWARE_VERSION "1.0.5"

// // GitHub OTA settings
// #define GHOTA_USER "AlejandroBalonga"
// #define GHOTA_REPO "esp8266-auto-update"
// #define GHOTA_BIN_FILE "firmware.bin"
// #define GHOTA_ACCEPT_PRERELEASE 0
// #define OTA_CHECK_INTERVAL_MS 1000UL * 60 * 10
// #define OTA_USER_AGENT "ESP8266-Auto-Update"

#ifndef CONFIG_H
#define CONFIG_H

// Valores por defecto (se sobreescriben con Preferences)
#define DEFAULT_WIFI_SSID "DESKTOPT_440"
#define DEFAULT_WIFI_PASS "aabbccdd"
#define DEFAULT_GHOTA_USER "AlejandroBalonga"
#define DEFAULT_GHOTA_REPO "esp8266-auto-update"

#define GHOTA_ACCEPT_PRERELEASE false
#define GHOTA_BIN_FILE "firmware.bin"
#define OTA_USER_AGENT "ESP8266-OTA"
#define FIRMWARE_VERSION "1.0.1"               // Cambia la versión para probar
#define OTA_CHECK_INTERVAL_MS 1000UL * 60 * 10 // 10 minutos

#endif