#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// AppConfig — configuración en runtime leída desde LittleFS
// Todos los módulos usan esta estructura en lugar de los #define de config.h
// ---------------------------------------------------------------------------
struct AppConfig
{
    String wifiSsid;
    String wifiPass;
    String ghotaUser;
    String ghotaRepo;
};

// ---------------------------------------------------------------------------
// Carga la config desde /config.json en LittleFS.
// Si el archivo no existe escribe los valores DEFAULT_* de config.h
// ---------------------------------------------------------------------------
void configLoad(AppConfig &cfg);

// ---------------------------------------------------------------------------
// Guarda la config en /config.json en LittleFS
// ---------------------------------------------------------------------------
void configSave(const AppConfig &cfg);

// ---------------------------------------------------------------------------
// Muestra el menú serial al arrancar y espera MENU_BOOT_WAIT_SECS segundos.
// Si el usuario presiona cualquier tecla entra al menú interactivo.
// Retorna true si se realizó algún cambio que requiere reconectar el WiFi.
// ---------------------------------------------------------------------------
bool serialMenuRun(AppConfig &cfg);