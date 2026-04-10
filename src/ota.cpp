#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <CertStoreBearSSL.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Updater.h>
#include <time.h>
#include <memory>
#include "ota.h"
#include "config.h"

static const size_t CHUNK_BUFFER_SIZE = 4096;
static uint8_t chunkBuffer[CHUNK_BUFFER_SIZE];

BearSSL::CertStore certStore;
bool secureMode = false;

static bool initCertificateStore()
{
    if (!LittleFS.begin())
    {
        Serial.println("LittleFS no pudo inicializarse.");
        return false;
    }

    int numCerts = certStore.initCertStore(LittleFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
    Serial.printf("Number of CA certs read: %d\n", numCerts);

    if (numCerts > 0)
    {
        Serial.println("Certificados cargados correctamente.");
        secureMode = false; // Forzar inseguro para probar
        return true;
    }

    Serial.println("No se encontraron certificados o el archivo no es válido.");
    secureMode = false;
    return false;
}

static bool isTimeValid()
{
    time_t now = time(nullptr);
    return now > 8 * 3600 * 2;
}

static bool syncTime()
{
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Sincronizando hora NTP...");
    time_t now = time(nullptr);
    unsigned long start = millis();
    while (!isTimeValid() && millis() - start < 15000)
    {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    Serial.println();
    if (!isTimeValid())
    {
        Serial.println("No se pudo sincronizar la hora.");
        return false;
    }
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.print("Hora actual: ");
    Serial.print(asctime(&timeinfo));
    return true;
}

static std::unique_ptr<BearSSL::WiFiClientSecure> createSecureClient()
{
    Serial.printf("createSecureClient: secureMode=%d\n", secureMode ? 1 : 0);
    auto client = std::make_unique<BearSSL::WiFiClientSecure>();
    if (secureMode)
    {
        Serial.println("createSecureClient: usando cert store");
        client->setCertStore(&certStore);
    }
    else
    {
        Serial.println("createSecureClient: usando inseguro");
        client->setInsecure();
    }
    Serial.println("createSecureClient: cliente creado");
    return client;
}

static bool followRedirects(HTTPClient &http, BearSSL::WiFiClientSecure &client, const String &url, int &httpCode)
{
    String currentUrl = url;
    for (int redirect = 0; redirect < 4; redirect++)
    {
        Serial.printf("HTTP begin: %s\n", currentUrl.c_str());
        if (!http.begin(client, currentUrl))
        {
            Serial.println("http.begin() falló");
            return false;
        }
        http.setTimeout(15000);
        http.addHeader("User-Agent", OTA_USER_AGENT);
        httpCode = http.GET();
        Serial.printf("HTTP GET returned: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        if (httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_SEE_OTHER || httpCode == HTTP_CODE_TEMPORARY_REDIRECT || httpCode == HTTP_CODE_PERMANENT_REDIRECT)
        {
            currentUrl = http.header("Location");
            http.end();
            if (currentUrl.length() == 0)
            {
                Serial.println("Redirección sin Location header.");
                return false;
            }
            delay(10);
            continue;
        }
        return true;
    }
    Serial.println("Demasiadas redirecciones.");
    return false;
}

static bool getLatestReleaseInfo(String &tagName, String &downloadUrl)
{
    Serial.println("getLatestReleaseInfo: inicio");
    String apiUrl = String("https://api.github.com/repos/") + GHOTA_USER + "/" + GHOTA_REPO + "/releases/latest";
    Serial.printf("API URL: %s\n", apiUrl.c_str());
    auto client = createSecureClient();
    Serial.printf("createSecureClient completo, secureMode=%d\n", secureMode ? 1 : 0);
    HTTPClient http;
    Serial.println("HTTPClient creado");
    int httpCode = 0;

    if (!followRedirects(http, *client, apiUrl, httpCode))
    {
        Serial.println("Error al inicializar la petición al API de GitHub.");
        return false;
    }

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("GitHub API HTTP error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    Serial.printf("GitHub response size: %d\n", http.getSize());
    Serial.printf("Free heap before JSON parse: %u\n", ESP.getFreeHeap());
    Stream &responseStream = *http.getStreamPtr();

    StaticJsonDocument<8192> doc;
    Serial.printf("Free heap after document alloc: %u\n", ESP.getFreeHeap());
    DeserializationError error = deserializeJson(doc, responseStream);
    if (error)
    {
        Serial.print("Error parsing JSON from stream: ");
        Serial.println(error.c_str());
        http.end();
        return false;
    }
    http.end();

    tagName = doc["tag_name"].as<String>();
    if (tagName.length() == 0)
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
        String assetName = asset["name"].as<String>();
        if (assetName == GHOTA_BIN_FILE)
        {
            downloadUrl = asset["browser_download_url"].as<String>();
            break;
        }
    }

    if (downloadUrl.length() == 0)
    {
        Serial.println("No se encontró el asset de firmware en la release.");
        return false;
    }

    return true;
}

static bool downloadFirmware(const String &downloadUrl)
{
    Serial.println("downloadFirmware: inicio");
    Serial.printf("downloadFirmware secureMode=%d\n", secureMode ? 1 : 0);
    Serial.printf("downloadUrl: %s\n", downloadUrl.c_str());
    auto client = createSecureClient();
    Serial.println("downloadFirmware: createSecureClient completo");
    HTTPClient http;
    Serial.println("downloadFirmware: HTTPClient creado");
    int httpCode = 0;

    if (!followRedirects(http, *client, downloadUrl, httpCode))
    {
        Serial.println("Error al inicializar la descarga de firmware.");
        return false;
    }

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("Firmware download HTTP error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0)
    {
        Serial.println("Tamaño de firmware inválido.");
        http.end();
        return false;
    }

    Serial.printf("Tamaño del firmware: %d bytes\n", contentLength);
    if (!Update.begin((uint32_t)contentLength, U_FLASH))
    {
        Serial.println("No hay espacio suficiente para la actualización.");
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int written = 0;
    int lastProgress = 0;

    while (http.connected() && written < contentLength)
    {
        size_t available = stream->available();
        if (available)
        {
            size_t toRead = available;
            if (toRead > CHUNK_BUFFER_SIZE)
            {
                toRead = CHUNK_BUFFER_SIZE;
            }
            int bytesRead = stream->readBytes(chunkBuffer, toRead);
            if (bytesRead > 0)
            {
                size_t bytesWritten = Update.write(chunkBuffer, bytesRead);
                if (bytesWritten != (size_t)bytesRead)
                {
                    Serial.println("Error escribiendo el chunk de firmware.");
                    http.end();
                    Update.end();
                    return false;
                }

                written += bytesWritten;
                int progress = (written * 100) / contentLength;
                if (progress - lastProgress >= 10)
                {
                    Serial.printf("Progreso: %d%% (%d/%d)\n", progress, written, contentLength);
                    lastProgress = progress;
                }
                yield();
            }
        }
        else
        {
            delay(1);
        }
    }

    http.end();

    if (Update.end(true))
    {
        Serial.printf("Actualización completa: %d bytes escritos\n", written);
        return true;
    }

    Serial.printf("Update failed: %s\n", Update.getErrorString().c_str());
    return false;
}

OTAUpdater::OTAUpdater() {}

void OTAUpdater::begin()
{
    if (!initCertificateStore())
    {
        Serial.println("Usando modo inseguro para conexiones HTTPS.");
        return;
    }

    if (!syncTime())
    {
        Serial.println("Usando modo inseguro porque no se pudo sincronizar la hora.");
        secureMode = false;
    }
}

void OTAUpdater::checkForUpdate()
{
    Serial.println("Comprobando actualización OTA en GitHub...");

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi no conectado, abortando.");
        return;
    }

    Serial.print("IP local: ");
    Serial.println(WiFi.localIP());

    String latestTag;
    String downloadUrl;
    if (!getLatestReleaseInfo(latestTag, downloadUrl))
    {
        Serial.println("No se pudo obtener información de la última release.");
        return;
    }

    Serial.printf("Versión actual: %s\n", FIRMWARE_VERSION);
    Serial.printf("Última versión: %s\n", latestTag.c_str());

    if (latestTag == FIRMWARE_VERSION)
    {
        Serial.println("No hay actualización disponible.");
        return;
    }

    Serial.println("Nueva versión encontrada, descargando firmware...");
    Serial.printf("URL de descarga: %s\n", downloadUrl.c_str());
    if (downloadFirmware(downloadUrl))
    {
        Serial.println("Firmware descargado correctamente. Reiniciando...");
        delay(100);
        ESP.restart();
    }
    else
    {
        Serial.println("Fallo al descargar o aplicar la actualización.");
    }
}
