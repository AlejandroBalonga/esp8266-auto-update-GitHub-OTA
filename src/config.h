#pragma once

// ---------------------------------------------------------------------------
// Versión del firmware — debe coincidir exactamente con el tag de la release
// ---------------------------------------------------------------------------
#define FIRMWARE_VERSION "1.0.5"

// ---------------------------------------------------------------------------
// Valores por defecto — se usan solo si NVS está vacío (primer arranque)
// En runtime se leen desde Preferences mediante AppConfig
// ---------------------------------------------------------------------------
#define DEFAULT_WIFI_SSID       "DESKTOPT_440"
#define DEFAULT_WIFI_PASS       "aabbccdd"
#define DEFAULT_GHOTA_USER      "AlejandroBalonga"
#define DEFAULT_GHOTA_REPO      "esp8266-auto-update"

// ---------------------------------------------------------------------------
// Constantes fijas (no configurables en runtime)
// ---------------------------------------------------------------------------
#define GHOTA_BIN_FILE          "firmware.bin"
#define GHOTA_ACCEPT_PRERELEASE 0
#define OTA_CHECK_INTERVAL_MS   (1000UL * 60 * 10)   // 10 minutos
#define OTA_USER_AGENT          "ESP8266-Auto-Update"

// ---------------------------------------------------------------------------
// Menú serial — segundos de espera al arrancar para entrar al menú
// ---------------------------------------------------------------------------
#define MENU_BOOT_WAIT_SECS     5
