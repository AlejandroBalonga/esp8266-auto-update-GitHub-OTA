#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Updater.h>
#include <time.h>
#include "ota.h"
#include "config.h"
#include "serial_menu.h"

static uint8_t downloadBuffer[1024];

// ---------------------------------------------------------------------------
// Sincronización NTP
// ---------------------------------------------------------------------------
static bool isTimeValid()
{
    return time(nullptr) > 8 * 3600 * 2;
}

static void syncTime()
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
    if (isTimeValid())
    {
        time_t now = time(nullptr);
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);
        Serial.printf("Hora actual: %s", asctime(&timeinfo));
    }
    else
    {
        Serial.println("No se pudo sincronizar la hora (no es critico).");
    }
}

// ---------------------------------------------------------------------------
// Consulta la GitHub API y devuelve tag y URL de descarga del firmware
// ---------------------------------------------------------------------------
static bool getLatestReleaseInfo(const AppConfig &cfg,
                                  String &tagName, String &downloadUrl)
{
    Serial.println("Consultando GitHub API...");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    // Usar usuario y repo desde AppConfig (configurables en runtime)
    String apiUrl = String("https://api.github.com/repos/")
                    + cfg.ghotaUser + "/" + cfg.ghotaRepo + "/releases/latest";

    WiFiClientSecure apiClient;
    apiClient.setInsecure();
    apiClient.setBufferSizes(1024, 1024);

    HTTPClient http;
    http.begin(apiClient, apiUrl);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("User-Agent", OTA_USER_AGENT);
    http.addHeader("Accept", "application/vnd.github+json");

    int httpCode = http.GET();
    Serial.printf("GitHub API HTTP: %d\n", httpCode);

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("GitHub API error: %d (%s)\n", httpCode,
                      http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    StaticJsonDocument<128> filter;
    filter["tag_name"]                          = true;
    filter["draft"]                             = true;
    filter["prerelease"]                        = true;
    filter["assets"][0]["name"]                 = true;
    filter["assets"][0]["browser_download_url"] = true;

    DynamicJsonDocument doc(3072);
    WiFiClient *stream = http.getStreamPtr();
    DeserializationError error = deserializeJson(doc, *stream,
                                  DeserializationOption::Filter(filter));
    http.end();
    yield();

    if (error)
    {
        Serial.printf("JSON parse error: %s\n", error.c_str());
        return false;
    }

    tagName = doc["tag_name"].as<String>();
    if (tagName.isEmpty())
    {
        Serial.println("No se encontro tag_name.");
        return false;
    }

    if (doc["draft"].as<bool>())
    {
        Serial.println("Release es draft, ignorando.");
        return false;
    }

    if (doc["prerelease"].as<bool>() && !GHOTA_ACCEPT_PRERELEASE)
    {
        Serial.println("Release es prerelease y no esta aceptada.");
        return false;
    }

    for (JsonObject asset : doc["assets"].as<JsonArray>())
    {
        if (asset["name"].as<String>() == GHOTA_BIN_FILE)
        {
            downloadUrl = asset["browser_download_url"].as<String>();
            break;
        }
    }

    if (downloadUrl.isEmpty())
    {
        Serial.printf("Asset '%s' no encontrado en la release.\n", GHOTA_BIN_FILE);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Paso 1: resolver la URL firmada siguiendo el redirect 302 de GitHub
// ---------------------------------------------------------------------------
static String resolveGithubAssetUrl(const String &githubUrl)
{
    Serial.println("Resolviendo URL del asset...");

    WiFiClientSecure resolveClient;
    resolveClient.setInsecure();
    resolveClient.setBufferSizes(512, 512);
    resolveClient.setTimeout(10);

    HTTPClient http;
    http.begin(resolveClient, githubUrl);
    http.setTimeout(10000);
    http.addHeader("User-Agent", OTA_USER_AGENT);

    const char *keys[] = {"Location"};
    http.collectHeaders(keys, 1);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    int code = http.GET();
    Serial.printf("resolveGithubAssetUrl HTTP: %d\n", code);

    String location = "";
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
    {
        location = http.header("Location");
    }
    else if (code == HTTP_CODE_OK)
    {
        location = githubUrl;
    }

    http.end();
    resolveClient.stop();
    yield();
    delay(100);

    return location;
}

// ---------------------------------------------------------------------------
// Paso 2: descargar el firmware desde la URL firmada con un cliente nuevo
// ---------------------------------------------------------------------------
static bool downloadFromUrl(const String &finalUrl)
{
    Serial.println("Descargando firmware...");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    WiFiClientSecure *dlClient = new WiFiClientSecure();
    dlClient->setInsecure();
    dlClient->setBufferSizes(16384, 512);
    dlClient->setTimeout(30);

    HTTPClient *http = new HTTPClient();
    http->begin(*dlClient, finalUrl);
    http->setTimeout(30000);
    http->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http->useHTTP10(true);
    http->addHeader("User-Agent", OTA_USER_AGENT);
    http->addHeader("Accept", "application/octet-stream");

    int httpCode = http->GET();
    Serial.printf("HTTP GET: %d\n", httpCode);

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("Error HTTP: %d (%s)\n", httpCode,
                      http->errorToString(httpCode).c_str());
        http->end();
        delete http;
        delete dlClient;
        return false;
    }

    int contentLength = http->getSize();
    Serial.printf("Tamanio: %d bytes\n", contentLength);

    if (contentLength <= 0)
    {
        Serial.println("Content-Length invalido.");
        http->end();
        delete http;
        delete dlClient;
        return false;
    }

    if (!Update.begin((size_t)contentLength, U_FLASH))
    {
        Serial.printf("Update.begin() fallo: %s\n", Update.getErrorString().c_str());
        http->end();
        delete http;
        delete dlClient;
        return false;
    }

    WiFiClient *stream = http->getStreamPtr();
    if (!stream)
    {
        Serial.println("getStreamPtr() devolvio null.");
        Update.end();
        http->end();
        delete http;
        delete dlClient;
        return false;
    }

    int written      = 0;
    int lastProgress = -10;
    unsigned long lastActivity = millis();

    while (written < contentLength)
    {
        int avail = stream->available();

        if (avail > 0)
        {
            lastActivity = millis();
            int remaining = contentLength - written;
            int toRead    = min(min(avail, remaining), (int)sizeof(downloadBuffer));
            int bytesRead = stream->read(downloadBuffer, toRead);

            if (bytesRead > 0)
            {
                int bytesWritten = Update.write(downloadBuffer, bytesRead);
                if (bytesWritten != bytesRead)
                {
                    Serial.printf("Error escribiendo flash: %s\n",
                                  Update.getErrorString().c_str());
                    Update.end();
                    http->end();
                    delete http;
                    delete dlClient;
                    return false;
                }
                written += bytesWritten;

                int pct = (written * 100) / contentLength;
                if (pct - lastProgress >= 10)
                {
                    Serial.printf("Progreso: %d%% (%d/%d bytes)\n",
                                  pct, written, contentLength);
                    lastProgress = pct;
                }
            }
        }
        else
        {
            if (!dlClient->connected())
            {
                Serial.printf("Conexion cerrada prematuramente (%d/%d bytes).\n",
                              written, contentLength);
                Update.end();
                http->end();
                delete http;
                delete dlClient;
                return false;
            }
            if (millis() - lastActivity > 20000)
            {
                Serial.printf("Timeout sin datos (%d/%d bytes).\n",
                              written, contentLength);
                Update.end();
                http->end();
                delete http;
                delete dlClient;
                return false;
            }
            yield();
        }
    }

    http->end();
    delete http;
    delete dlClient;

    if (!Update.end(true))
    {
        Serial.printf("Update.end() fallo: %s\n", Update.getErrorString().c_str());
        return false;
    }

    Serial.printf("Firmware escrito: %d bytes.\n", written);
    return true;
}

// ---------------------------------------------------------------------------
// Orquesta los dos pasos de descarga
// ---------------------------------------------------------------------------
static bool downloadFirmware(const String &downloadUrl)
{
    String finalUrl = resolveGithubAssetUrl(downloadUrl);
    if (finalUrl.isEmpty())
    {
        Serial.println("No se pudo resolver la URL del asset.");
        return false;
    }
    return downloadFromUrl(finalUrl);
}

// ---------------------------------------------------------------------------
// Clase pública OTAUpdater
// ---------------------------------------------------------------------------
OTAUpdater::OTAUpdater() {}

void OTAUpdater::begin(const AppConfig &cfg)
{
    (void)cfg;
    Serial.println("OTAUpdater::begin()");
    syncTime();
    Serial.printf("Free heap tras begin: %u bytes\n", ESP.getFreeHeap());
}

void OTAUpdater::checkForUpdate(const AppConfig &cfg)
{
    Serial.println("Comprobando actualizacion OTA en GitHub...");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi no conectado, abortando.");
        return;
    }

    String latestTag;
    String downloadUrl;

    if (!getLatestReleaseInfo(cfg, latestTag, downloadUrl))
    {
        Serial.println("No se pudo obtener informacion de la ultima release.");
        return;
    }

    Serial.printf("Version actual:  %s\n", FIRMWARE_VERSION);
    Serial.printf("Ultima version:  %s\n", latestTag.c_str());

    if (latestTag == FIRMWARE_VERSION)
    {
        Serial.println("Firmware al dia, no hay actualizacion.");
        return;
    }

    Serial.println("Nueva version encontrada, descargando firmware...");
    if (downloadFirmware(downloadUrl))
    {
        Serial.println("Reiniciando...");
        delay(500);
        ESP.restart();
    }
    else
    {
        Serial.println("Fallo al actualizar el firmware.");
    }
}
