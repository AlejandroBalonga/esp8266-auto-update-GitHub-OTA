#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include "ota.h"
#include <ESP_OTA_GitHub.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include "config.h"

BearSSL::CertStore certStore;
String currentTag = "1.0.1"; // Update this to match version.txt
ESPOTAGitHub espotagitHub(&certStore, GHOTA_USER, GHOTA_REPO, currentTag.c_str(), GHOTA_BIN_FILE, GHOTA_ACCEPT_PRERELEASE);

// Variables globales para descarga por chunks
static const size_t CHUNK_SIZE = 4096; // 4KB chunks
static uint8_t chunkBuffer[CHUNK_SIZE];
void setupOTA()
{
    ArduinoOTA.onStart([]()
                       {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else { // U_SPIFFS
            type = "filesystem";
        }
        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type); });
    ArduinoOTA.onEnd([]()
                     { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
    ArduinoOTA.onError([](ota_error_t error)
                       {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            Serial.println("End Failed");
        } });

    ArduinoOTA.begin();
}

void handleOTA()
{
    ArduinoOTA.handle();
}

OTAUpdater::OTAUpdater() {}

void OTAUpdater::begin()
{
    // Initialize LittleFS and certificate store for secure HTTPS connections
    LittleFS.begin();
    int numCerts = certStore.initCertStore(LittleFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
    Serial.printf("Number of CA certs read: %d\n", numCerts);
    if (numCerts == 0)
    {
        Serial.println("No certs found. Did you upload certs.ar to LittleFS?");
        // Continue anyway, but HTTPS may fail
    }
}

// Descarga por chunks directamente a la flash
bool downloadAndUpdateFirmware(const char *url)
{
    Serial.println("Iniciando descarga por chunks...");

    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();

    HTTPClient http;
    http.begin(*client, url);
    http.addHeader("User-Agent", "ESP8266-Auto-Update");

    int httpCode = http.GET();
    Serial.printf("HTTP Code: %d\n", httpCode);

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("Error HTTP: %d\n", httpCode);
        http.end();
        return false;
    }

    size_t contentLength = http.getSize();
    Serial.printf("Tamaño del firmware: %d bytes\n", contentLength);

    // Iniciar actualización OTA
    if (!Update.begin(contentLength))
    {
        Serial.println("No hay espacio suficiente para actualización");
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t written = 0;
    size_t lastProgress = 0;

    // Descargar y escribir por chunks
    while (http.connected() && written < contentLength)
    {
        size_t available = stream->available();
        if (available)
        {
            size_t bytesToRead = (available > CHUNK_SIZE) ? CHUNK_SIZE : available;
            int bytesRead = stream->readBytes(chunkBuffer, bytesToRead);

            if (bytesRead > 0)
            {
                // Escribir chunk a la flash
                size_t written_chunk = Update.write(chunkBuffer, bytesRead);
                written += written_chunk;

                // Mostrar progreso cada 10%
                size_t currentProgress = (written * 100) / contentLength;
                if (currentProgress - lastProgress >= 10)
                {
                    Serial.printf("Progreso: %d%% (%d/%d bytes)\n", currentProgress, written, contentLength);
                    lastProgress = currentProgress;
                }

                // Dar tiempo al ESP para otros procesos
                yield();
            }
        }
        else
        {
            delay(1); // Esperar datos disponibles
        }

        // Timeout de seguridad
        if (!http.connected() && written < contentLength)
        {
            Serial.println("Conexión perdida durante descarga");
            http.end();
            // Update.abort() does not exist on the ESP8266 Update class; end the update to clean up
            Update.end();
            return false;
        }
    }

    http.end();

    // Finalizar actualización
    if (Update.end())
    {
        Serial.printf("Actualización completada: %d bytes escritos\n", written);
        return true;
    }
    else
    {
        Serial.printf("Error al finalizar Update: %s\n", Update.getErrorString().c_str());
        return false;
    }
}

void OTAUpdater::checkForUpdate()
{
    Serial.println("Checking for OTA update from GitHub...");

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi not connected, aborting check.");
        return;
    }

    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());

    // Si se quiere validar certificado, inicializar tiempo en la librería embebida
    // no hace falta objeto externo client para checkUpgrade/doUpgrade.

    WiFiClientSecure test;
    test.setInsecure();
    if (test.connect("api.github.com", 443))
        Serial.println("GITHUB OK");
    else
        Serial.println("GITHUB FAIL");

    if (espotagitHub.checkUpgrade())
    {
        Serial.println("Upgrade available, starting update...");
        if (espotagitHub.doUpgrade())
        {
            Serial.println("Upgrade successful!");
        }
        else
        {
            Serial.printf("Upgrade failed: %s\n", espotagitHub.getLastError().c_str());
        }
    }
    else
    {
        Serial.println("No upgrade available or check failed.");
        Serial.printf("Last error: %s\n", espotagitHub.getLastError().c_str());
    }
}
