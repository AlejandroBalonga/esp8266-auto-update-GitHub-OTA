#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "config.h"
#include "ota.h"
#include "serial_menu.h"

#define LED_PIN D4

OTAUpdater updater;
AppConfig   appCfg;

unsigned long lastCheck          = 0;
unsigned long lastReconnectAttempt = 0;

void blinkLed(int times, int onMs, int offMs)
{
    for (int i = 0; i < times; i++)
    {
        digitalWrite(LED_PIN, LOW);
        delay(onMs);
        digitalWrite(LED_PIN, HIGH);
        delay(offMs);
    }
}

static void connectWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(appCfg.wifiSsid.c_str(), appCfg.wifiPass.c_str());

    Serial.printf("Conectando a WiFi: %s\n", appCfg.wifiSsid.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000)
    {
        delay(500);
        Serial.print(".");
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
        delay(100);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi conectado.");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        blinkLed(5, 50, 50);
    }
    else
    {
        Serial.println("No se pudo conectar al WiFi.");
        blinkLed(5, 200, 200);
    }
}

void setup()
{
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("=====================================");
    Serial.println("  ESP8266 OTA GitHub Updater");
    Serial.printf ("  Version: %s\n", FIRMWARE_VERSION);
    Serial.println("=====================================");

    blinkLed(3, 100, 100);

    // Cargar configuración desde NVS
    configLoad(appCfg);

    // Menú serial — si el usuario cambió el WiFi reconectar con los nuevos datos
    bool wifiChanged = serialMenuRun(appCfg);
    (void)wifiChanged;   // siempre reconectamos después del menú

    // Conectar WiFi con la config activa (default o la guardada en NVS)
    connectWifi();

    updater.begin(appCfg);

    // Primer check OTA a los 30s para que el sistema esté estable
    lastCheck = millis() - OTA_CHECK_INTERVAL_MS + 30000UL;
}

void loop()
{
    // Parpadeo lento del LED como heartbeat
    static unsigned long lastToggle = 0;
    if (millis() - lastToggle > 2000)
    {
        lastToggle = millis();
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        if (millis() - lastCheck >= OTA_CHECK_INTERVAL_MS)
        {
            lastCheck = millis();
            Serial.println("\nIniciando comprobacion OTA...");
            Serial.printf("Free heap antes de OTA: %u bytes\n", ESP.getFreeHeap());
            updater.checkForUpdate(appCfg);
        }
    }
    else if (millis() - lastReconnectAttempt > 30000)
    {
        lastReconnectAttempt = millis();
        Serial.println("WiFi desconectado, intentando reconectar...");
        WiFi.reconnect();
    }

    delay(100);
}
