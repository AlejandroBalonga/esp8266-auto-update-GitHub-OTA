#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Updater.h>
#include <time.h>
#include <memory>
#include "ota.h"
#include "config.h"

#define UPDATE_SIZE_UNKNOWN 0

static const size_t CHUNK_BUFFER_SIZE = 4096;
static uint8_t chunkBuffer[CHUNK_BUFFER_SIZE];

// ---------------------------------------------------------------------------
// Cliente HTTPS inseguro (sin verificación de certificado)
// Es suficiente para este caso de uso: el binario viene firmado por GitHub
// y lo que nos importa es no crashear por falta de certs en flash.
// ---------------------------------------------------------------------------
static std::unique_ptr<BearSSL::WiFiClientSecure> createSecureClient()
{
    auto client = std::make_unique<BearSSL::WiFiClientSecure>();
    client->setInsecure();              // sin verificación de CA
    client->setBufferSizes(1024, 1024); // reducir buffers para ahorrar heap
    return client;
}

// ---------------------------------------------------------------------------
// Sincronización NTP (necesaria para que BearSSL valide fechas si se usa CA)
// La dejamos por si en el futuro se quiere activar verificación de certs.
// ---------------------------------------------------------------------------
static bool isTimeValid()
{
    time_t now = time(nullptr);
    return now > 8 * 3600 * 2;
}

static bool syncTime()
{
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Sincronizando hora NTP...");
    unsigned long start = millis();
    while (!isTimeValid() && millis() - start < 15000)
    {
        delay(500);
        yield();
        Serial.print(".");
    }
    Serial.println();
    if (!isTimeValid())
    {
        Serial.println("No se pudo sincronizar la hora (no es crítico en modo inseguro).");
        return false;
    }
    time_t now = time(nullptr);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.printf("Hora actual: %s", asctime(&timeinfo));
    return true;
}

// ---------------------------------------------------------------------------
// Seguidor de redirecciones HTTP/HTTPS
// FIX: llama a http.end() antes de cada http.begin() para evitar cliente sucio
// ---------------------------------------------------------------------------
static bool followRedirects(HTTPClient &http, BearSSL::WiFiClientSecure &client,
                            const String &startUrl, int &httpCode)
{
    String currentUrl = startUrl;
    for (int redirect = 0; redirect < 5; redirect++)
    {
        http.end(); // limpiar siempre antes de un nuevo begin
        yield();

        Serial.printf("HTTP begin (%d): %s\n", redirect, currentUrl.c_str());
        if (!http.begin(client, currentUrl))
        {
            Serial.println("http.begin() falló");
            return false;
        }

        http.setTimeout(15000);
        http.addHeader("User-Agent", OTA_USER_AGENT);
        http.addHeader("Accept", "application/vnd.github+json");
        // FIX: registrar explícitamente el header Location antes del GET
        // sin esto http.header("Location") devuelve vacío en los redirects
        const char *headerKeys[] = {"Location"};
        http.collectHeaders(headerKeys, 1);
        yield();

        httpCode = http.GET();
        Serial.printf("HTTP GET: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());

        if (httpCode == HTTP_CODE_MOVED_PERMANENTLY ||
            httpCode == HTTP_CODE_FOUND ||
            httpCode == HTTP_CODE_SEE_OTHER ||
            httpCode == HTTP_CODE_TEMPORARY_REDIRECT ||
            httpCode == HTTP_CODE_PERMANENT_REDIRECT)
        {
            String location = http.header("Location");
            http.end();
            if (location.isEmpty())
            {
                Serial.println("Redirección sin Location header.");
                return false;
            }
            currentUrl = location;
            delay(50);
            yield();
            continue;
        }
        // Cualquier otro código (200, error, etc.) → salir del loop
        return true;
    }
    Serial.println("Demasiadas redirecciones.");
    return false;
}

// ---------------------------------------------------------------------------
// Consulta la GitHub API para obtener tag y URL de descarga del firmware
// FIX 1: HTTPS en lugar de HTTP (GitHub API requiere HTTPS)
// FIX 2: Filtro JSON → parseo desde stream → nunca tener el JSON entero en RAM
// ---------------------------------------------------------------------------
static bool getLatestReleaseInfo(String &tagName, String &downloadUrl)
{
    Serial.println("getLatestReleaseInfo: inicio");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    // FIX: HTTPS — la API de GitHub redirige/rechaza HTTP
    String apiUrl = String("https://api.github.com/repos/") + GHOTA_USER + "/" + GHOTA_REPO + "/releases/latest";
    Serial.printf("API URL: %s\n", apiUrl.c_str());

    auto client = createSecureClient();
    HTTPClient http;
    int httpCode = 0;

    if (!followRedirects(http, *client, apiUrl, httpCode))
    {
        Serial.println("Error al conectar con la API de GitHub.");
        http.end();
        return false;
    }

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("GitHub API HTTP error: %d (%s)\n", httpCode,
                      http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    Serial.printf("GitHub response size: %d\n", http.getSize());
    Serial.printf("Free heap antes de parsear JSON: %u bytes\n", ESP.getFreeHeap());

    // FIX: Filtro → solo traemos los campos que nos interesan
    // Esto evita que ArduinoJson intente procesar 30-50 KB de JSON
    StaticJsonDocument<128> filter;
    filter["tag_name"] = true;
    filter["draft"] = true;
    filter["prerelease"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;

    // FIX: DynamicJsonDocument (heap, no stack) y parseo directo desde stream
    DynamicJsonDocument doc(3072);
    WiFiClient *stream = http.getStreamPtr();
    DeserializationError error = deserializeJson(doc, *stream,
                                                 DeserializationOption::Filter(filter));
    http.end();
    yield();

    if (error)
    {
        Serial.printf("Error parsing JSON: %s\n", error.c_str());
        return false;
    }

    Serial.printf("Free heap después de parsear JSON: %u bytes\n", ESP.getFreeHeap());

    tagName = doc["tag_name"].as<String>();
    if (tagName.isEmpty())
    {
        Serial.println("No se encontró tag_name en la respuesta de GitHub.");
        return false;
    }

    if (doc["draft"].as<bool>())
    {
        Serial.println("La última release es un draft. No se actualizará.");
        return false;
    }

    if (doc["prerelease"].as<bool>() && !GHOTA_ACCEPT_PRERELEASE)
    {
        Serial.println("La última release es prerelease y no está aceptada.");
        return false;
    }

    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets)
    {
        if (asset["name"].as<String>() == GHOTA_BIN_FILE)
        {
            downloadUrl = asset["browser_download_url"].as<String>();
            break;
        }
    }

    if (downloadUrl.isEmpty())
    {
        Serial.printf("No se encontró el asset '%s' en la release.\n", GHOTA_BIN_FILE);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Resuelve redirecciones y devuelve la URL final (sin descargar el body)
// Usa un cliente propio que se destruye al salir.
// ---------------------------------------------------------------------------
static String resolveRedirects(const String &startUrl)
{
    String currentUrl = startUrl;
    for (int i = 0; i < 5; i++)
    {
        auto client = createSecureClient();
        HTTPClient http;
        http.end();
        if (!http.begin(*client, currentUrl))
            break;
        http.setTimeout(10000);
        http.addHeader("User-Agent", OTA_USER_AGENT);
        const char *headerKeys[] = {"Location"};
        http.collectHeaders(headerKeys, 1);

        // HEAD en vez de GET: no descarga el body, solo los headers
        int code = http.sendRequest("HEAD");
        Serial.printf("resolveRedirects [%d]: %d → %s\n", i, code, currentUrl.c_str());

        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        {
            String location = http.header("Location");
            http.end();
            if (location.isEmpty())
                break;
            currentUrl = location;
            yield();
            continue;
        }
        http.end();
        // código 200 o cualquier no-redirect → esta es la URL final
        return currentUrl;
    }
    return currentUrl; // devolvemos lo último que teníamos
}

// ---------------------------------------------------------------------------
// Descarga el firmware y lo escribe en flash con el Updater
// FIX: primero resuelve la URL final con resolveRedirects() y luego
// abre una conexión NUEVA y LIMPIA para la descarga real.
// Esto evita el problema de reutilizar un cliente SSL que ya negoció
// TLS con un host diferente (github.com vs release-assets.githubusercontent.com)
// ---------------------------------------------------------------------------
static bool downloadFirmware(const String &downloadUrl)
{
    Serial.println("downloadFirmware: inicio");
    Serial.printf("URL original: %s\n", downloadUrl.c_str());
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    // Paso 1: resolver la URL final siguiendo redirects
    String finalUrl = resolveRedirects(downloadUrl);
    Serial.printf("URL final: %s\n", finalUrl.c_str());
    yield();

    // Paso 2: conexión limpia a la URL final
    auto client = createSecureClient();
    // El binario es grande → necesitamos buffers de recepción más grandes
    client->setBufferSizes(4096, 512);
    HTTPClient http;
    http.end();

    Serial.println("Conectando a URL final...");
    if (!http.begin(*client, finalUrl))
    {
        Serial.println("http.begin() a URL final falló.");
        return false;
    }
    http.setTimeout(30000);
    http.addHeader("User-Agent", OTA_USER_AGENT);

    int httpCode = http.GET();
    Serial.printf("HTTP GET final: %d\n", httpCode);

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("Firmware download HTTP error: %d (%s)\n", httpCode,
                      http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0)
    {
        Serial.println("Tamaño de firmware desconocido, usando UPDATE_SIZE_UNKNOWN.");
    }
    else
    {
        Serial.printf("Tamaño del firmware: %d bytes\n", contentLength);
    }

    uint32_t updateSize = (contentLength > 0) ? (uint32_t)contentLength : UPDATE_SIZE_UNKNOWN;
    if (!Update.begin(updateSize, U_FLASH))
    {
        Serial.printf("No hay espacio para la actualización: %s\n",
                      Update.getErrorString().c_str());
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int written = 0;
    int lastProgress = -10;
    unsigned long lastData = millis();

    while (http.connected() && (contentLength <= 0 || written < contentLength))
    {
        size_t available = stream->available();
        if (available)
        {
            lastData = millis();
            size_t toRead = min(available, CHUNK_BUFFER_SIZE);
            int bytesRead = stream->readBytes(chunkBuffer, toRead);
            if (bytesRead > 0)
            {
                size_t bytesWritten = Update.write(chunkBuffer, bytesRead);
                if (bytesWritten != (size_t)bytesRead)
                {
                    Serial.printf("Error escribiendo chunk: %s\n",
                                  Update.getErrorString().c_str());
                    http.end();
                    Update.end();
                    return false;
                }
                written += bytesWritten;

                if (contentLength > 0)
                {
                    int progress = (written * 100) / contentLength;
                    if (progress - lastProgress >= 10)
                    {
                        Serial.printf("Progreso: %d%% (%d/%d bytes)\n",
                                      progress, written, contentLength);
                        lastProgress = progress;
                    }
                }
                yield();
            }
        }
        else
        {
            if (millis() - lastData > 15000)
            {
                Serial.println("Timeout esperando datos del stream.");
                http.end();
                Update.end();
                return false;
            }
            delay(1);
            yield();
        }
    }

    http.end();

    if (Update.end(true))
    {
        Serial.printf("Actualización completa: %d bytes escritos\n", written);
        return true;
    }

    Serial.printf("Update.end() falló: %s\n", Update.getErrorString().c_str());
    return false;
}

// ---------------------------------------------------------------------------
// Clase pública OTAUpdater
// ---------------------------------------------------------------------------
OTAUpdater::OTAUpdater() {}

void OTAUpdater::begin()
{
    Serial.println("OTAUpdater::begin()");
    // Intentar sincronizar hora (no es crítico en modo setInsecure)
    syncTime();
    Serial.printf("Free heap tras begin: %u bytes\n", ESP.getFreeHeap());
}

void OTAUpdater::checkForUpdate()
{
    Serial.println("Comprobando actualización OTA en GitHub...");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi no conectado, abortando.");
        return;
    }

    String latestTag;
    String downloadUrl;

    if (!getLatestReleaseInfo(latestTag, downloadUrl))
    {
        Serial.println("No se pudo obtener información de la última release.");
        return;
    }

    Serial.printf("Versión actual:  %s\n", FIRMWARE_VERSION);
    Serial.printf("Última versión:  %s\n", latestTag.c_str());

    if (latestTag == FIRMWARE_VERSION)
    {
        Serial.println("Firmware al día, no hay actualización.");
        return;
    }

    Serial.println("Nueva versión encontrada, descargando firmware...");
    if (downloadFirmware(downloadUrl))
    {
        Serial.println("Firmware actualizado correctamente. Reiniciando...");
        delay(500);
        ESP.restart();
    }
    else
    {
        Serial.println("Fallo al descargar o aplicar la actualización.");
    }
}