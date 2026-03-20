#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include "ota.h"
#include <ESP_OTA_GitHub.h>
#include <WiFiClientSecureBearSSL.h>
#include "config.h"

BearSSL::CertStore certStore;
String currentTag = "1.0.1"; // Update this to match version.txt
ESP_OTA_GitHub ESPOTAGitHub(&certStore, GHOTA_USER, GHOTA_REPO, currentTag.c_str(), GHOTA_BIN_FILE, GHOTA_ACCEPT_PRERELEASE);

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
    // Initialize certStore with dummy data since we're using setInsecure
    // But for proper security, generate and upload certs.ar to SPIFFS
    // For now, we'll use insecure for simplicity
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

    // Use insecure client for simplicity
    BearSSL::WiFiClientSecure client;
    client.setInsecure();
    ESPOTAGitHub.setClient(&client);

    if (ESPOTAGitHub.checkUpgrade())
    {
        Serial.println("Upgrade available, starting update...");
        if (ESPOTAGitHub.doUpgrade())
        {
            Serial.println("Upgrade successful!");
        }
        else
        {
            Serial.printf("Upgrade failed: %s\n", ESPOTAGitHub.getLastError().c_str());
        }
    }
    else
    {
        Serial.println("No upgrade available or check failed.");
        Serial.printf("Last error: %s\n", ESPOTAGitHub.getLastError().c_str());
    }
}