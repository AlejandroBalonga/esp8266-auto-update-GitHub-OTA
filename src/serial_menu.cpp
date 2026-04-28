#include "serial_menu.h"
#include "config.h"
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static const char *CONFIG_FILE = "/config.json";

// ---------------------------------------------------------------------------
// Helpers de lectura serial
// ---------------------------------------------------------------------------
static void serialFlush()
{
    while (Serial.available())
        Serial.read();
}

static String serialReadLine(const String &prompt = "")
{
    if (prompt.length())
        Serial.print(prompt);
    String line = "";
    while (true)
    {
        if (Serial.available())
        {
            char c = Serial.read();
            if (c == '\n' || c == '\r')
            {
                if (line.length() > 0)
                    break;
            }
            else
            {
                Serial.print(c);
                line += c;
            }
        }
        yield();
    }
    Serial.println();
    return line;
}

static String serialReadLineDefault(const String &prompt, const String &defaultVal)
{
    Serial.printf("%s [%s]: ", prompt.c_str(), defaultVal.c_str());
    String val = serialReadLine();
    return val.isEmpty() ? defaultVal : val;
}

static int serialReadInt(const String &prompt, int minVal, int maxVal)
{
    while (true)
    {
        String s = serialReadLine(prompt);
        int v = s.toInt();
        if (v >= minVal && v <= maxVal)
            return v;
        Serial.printf("  Ingresa un numero entre %d y %d.\n", minVal, maxVal);
    }
}

static void printSep(char c = '-', int n = 48)
{
    for (int i = 0; i < n; i++)
        Serial.print(c);
    Serial.println();
}

// ---------------------------------------------------------------------------
// configLoad / configSave  (LittleFS + JSON)
// ---------------------------------------------------------------------------
void configLoad(AppConfig &cfg)
{
    // Valores por defecto
    cfg.wifiSsid = DEFAULT_WIFI_SSID;
    cfg.wifiPass = DEFAULT_WIFI_PASS;
    cfg.ghotaUser = DEFAULT_GHOTA_USER;
    cfg.ghotaRepo = DEFAULT_GHOTA_REPO;

    if (!LittleFS.begin())
    {
        Serial.println("configLoad: LittleFS no disponible, usando defaults.");
        return;
    }

    if (!LittleFS.exists(CONFIG_FILE))
    {
        Serial.println("configLoad: config.json no encontrado, usando defaults.");
        // Guardar defaults para que el archivo exista en el próximo arranque
        configSave(cfg);
        return;
    }

    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f)
    {
        Serial.println("configLoad: no se pudo abrir config.json.");
        return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err)
    {
        Serial.printf("configLoad: JSON invalido (%s), usando defaults.\n", err.c_str());
        return;
    }

    if (doc.containsKey("wifi_ssid"))
        cfg.wifiSsid = doc["wifi_ssid"].as<String>();
    if (doc.containsKey("wifi_pass"))
        cfg.wifiPass = doc["wifi_pass"].as<String>();
    if (doc.containsKey("ghota_user"))
        cfg.ghotaUser = doc["ghota_user"].as<String>();
    if (doc.containsKey("ghota_repo"))
        cfg.ghotaRepo = doc["ghota_repo"].as<String>();

    Serial.println("configLoad: configuracion cargada desde LittleFS.");
}

void configSave(const AppConfig &cfg)
{
    if (!LittleFS.begin())
    {
        Serial.println("configSave: LittleFS no disponible.");
        return;
    }

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f)
    {
        Serial.println("configSave: no se pudo escribir config.json.");
        return;
    }

    StaticJsonDocument<512> doc;
    doc["wifi_ssid"] = cfg.wifiSsid;
    doc["wifi_pass"] = cfg.wifiPass;
    doc["ghota_user"] = cfg.ghotaUser;
    doc["ghota_repo"] = cfg.ghotaRepo;

    serializeJsonPretty(doc, f);
    f.close();
    Serial.println("  Configuracion guardada en LittleFS.");
}

// ---------------------------------------------------------------------------
// SUB-MENÚ 1: Información del módulo
// ---------------------------------------------------------------------------
static void menuInfo(const AppConfig &cfg)
{
    Serial.println();
    printSep('=');
    Serial.println("  INFORMACION DEL MODULO");
    printSep('=');

    Serial.printf("  Firmware version : %s\n", FIRMWARE_VERSION);
    Serial.println();

    Serial.println("  -- Repositorio --");
    Serial.printf("  Usuario GitHub   : %s\n", cfg.ghotaUser.c_str());
    Serial.printf("  Repo             : %s\n", cfg.ghotaRepo.c_str());
    Serial.printf("  URL              : https://github.com/%s/%s\n",
                  cfg.ghotaUser.c_str(), cfg.ghotaRepo.c_str());
    Serial.println();

    Serial.println("  -- WiFi --");
    Serial.printf("  SSID             : %s\n", cfg.wifiSsid.c_str());
    Serial.printf("  Estado           : %s\n",
                  WiFi.status() == WL_CONNECTED ? "Conectado" : "Desconectado");
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.printf("  IP               : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  Gateway          : %s\n", WiFi.gatewayIP().toString().c_str());
        Serial.printf("  DNS              : %s\n", WiFi.dnsIP().toString().c_str());
        Serial.printf("  RSSI             : %d dBm\n", WiFi.RSSI());
    }
    Serial.println();

    Serial.println("  -- Hardware --");
    uint8_t mac[6];
    WiFi.macAddress(mac);
    Serial.printf("  MAC address      : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("  Chip ID          : %08X\n", ESP.getChipId());
    Serial.printf("  Flash size       : %u KB\n", ESP.getFlashChipSize() / 1024);
    Serial.printf("  Free heap        : %u bytes\n", ESP.getFreeHeap());
    Serial.printf("  CPU freq         : %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("  SDK version      : %s\n", ESP.getSdkVersion());
    Serial.printf("  Uptime           : %lu s\n", millis() / 1000);
    Serial.println();

    Serial.println("  -- Almacenamiento --");
    FSInfo fsInfo;
    if (LittleFS.info(fsInfo))
    {
        Serial.printf("  LittleFS total   : %u KB\n", fsInfo.totalBytes / 1024);
        Serial.printf("  LittleFS usado   : %u KB\n", fsInfo.usedBytes / 1024);
    }

    printSep();
    Serial.println("  Presiona Enter para volver...");
    serialReadLine();
}

// ---------------------------------------------------------------------------
// SUB-MENÚ 2: Configurar repositorio
// ---------------------------------------------------------------------------
static void menuRepo(AppConfig &cfg)
{
    Serial.println();
    printSep('=');
    Serial.println("  CONFIGURAR REPOSITORIO GITHUB");
    printSep('=');
    Serial.println("  (Enter para conservar el valor actual)");
    Serial.println();

    String newUser = serialReadLineDefault("  Usuario GitHub", cfg.ghotaUser);
    String newRepo = serialReadLineDefault("  Nombre del repo", cfg.ghotaRepo);

    if (newUser == cfg.ghotaUser && newRepo == cfg.ghotaRepo)
    {
        Serial.println("  Sin cambios.");
        return;
    }

    Serial.println();
    Serial.printf("  Usuario : %s\n", newUser.c_str());
    Serial.printf("  Repo    : %s\n", newRepo.c_str());
    Serial.printf("  URL     : https://github.com/%s/%s\n",
                  newUser.c_str(), newRepo.c_str());
    Serial.println();

    String confirm = serialReadLine("  Confirmar? (s/n): ");
    if (confirm == "s" || confirm == "S")
    {
        cfg.ghotaUser = newUser;
        cfg.ghotaRepo = newRepo;
        configSave(cfg);
    }
    else
    {
        Serial.println("  Cancelado.");
    }
}

// ---------------------------------------------------------------------------
// SUB-MENÚ 3: Configurar WiFi
// ---------------------------------------------------------------------------
static int wifiScan(String ssids[], int maxNets)
{
    Serial.println("  Escaneando redes WiFi...");
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    if (n <= 0)
    {
        Serial.println("  No se encontraron redes.");
        return 0;
    }

    int count = min(n, maxNets);
    Serial.println();
    for (int i = 0; i < count; i++)
    {
        ssids[i] = WiFi.SSID(i);
        String sec = (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "Abierta" : "Segura";
        Serial.printf("  [%2d] %-32s  %d dBm  %s\n",
                      i + 1,
                      ssids[i].c_str(),
                      WiFi.RSSI(i),
                      sec.c_str());
    }
    WiFi.scanDelete();
    return count;
}

static bool menuWifi(AppConfig &cfg)
{
    Serial.println();
    printSep('=');
    Serial.println("  CONFIGURAR WIFI");
    printSep('=');
    Serial.println("  [1] Ingresar SSID manualmente");
    Serial.println("  [2] Elegir de redes disponibles");
    Serial.println("  [0] Volver");
    Serial.println();

    int op = serialReadInt("  Opcion: ", 0, 2);
    if (op == 0)
        return false;

    String newSsid = "";

    if (op == 1)
    {
        newSsid = serialReadLineDefault("  SSID", cfg.wifiSsid);
        if (newSsid.isEmpty())
            return false;
    }
    else
    {
        const int MAX_NETS = 20;
        String ssids[MAX_NETS];
        int count = wifiScan(ssids, MAX_NETS);
        if (count == 0)
            return false;

        Serial.println();
        Serial.println("  [0] Volver");
        Serial.println();
        int sel = serialReadInt("  Selecciona red (numero): ", 0, count);
        if (sel == 0)
            return false;
        newSsid = ssids[sel - 1];
        Serial.printf("  Red seleccionada: %s\n", newSsid.c_str());
    }

    // Contraseña
    Serial.println();
    Serial.println("  Contrasena:");
    Serial.println("  [1] Mantener la actual");
    Serial.println("  [2] Ingresar nueva contrasena");
    Serial.println();
    int pwdOp = serialReadInt("  Opcion: ", 1, 2);

    String newPass = cfg.wifiPass;
    if (pwdOp == 2)
    {
        Serial.print("  Nueva contrasena: ");
        String pwd = "";
        // Descartar cualquier \r\n residual antes de leer
        while (Serial.available())
            Serial.read();
        // Leer hasta Enter
        while (true)
        {
            if (Serial.available())
            {
                char c = Serial.read();
                if (c == '\n' || c == '\r')
                {
                    if (pwd.length() > 0)
                        break; // Enter con texto → terminar
                    // Enter vacío → ignorar y seguir esperando
                }
                else
                {
                    Serial.print('*');
                    pwd += c;
                }
            }
            yield();
        }
        Serial.println();
        if (!pwd.isEmpty())
            newPass = pwd;
    }

    // Confirmar — mostrar la contraseña real para que puedas verificarla
    Serial.println();
    Serial.printf("  SSID    : %s\n", newSsid.c_str());
    Serial.printf("  Pass    : %s\n", newPass.c_str());
    Serial.println();

    String confirm = serialReadLine("  Confirmar? (s/n): ");
    if (confirm != "s" && confirm != "S")
    {
        Serial.println("  Cancelado.");
        return false;
    }

    cfg.wifiSsid = newSsid;
    cfg.wifiPass = newPass;
    configSave(cfg);
    return true;
}

// ---------------------------------------------------------------------------
// Menú principal
// ---------------------------------------------------------------------------
static void printMainMenu(const AppConfig &cfg)
{
    Serial.println();
    printSep('=');
    Serial.println("  MENU DE CONFIGURACION  esp8266-auto-update");
    printSep('=');
    Serial.printf("  Firmware : %s\n", FIRMWARE_VERSION);
    Serial.printf("  WiFi     : %s\n", cfg.wifiSsid.c_str());
    Serial.printf("  Repo     : %s/%s\n", cfg.ghotaUser.c_str(), cfg.ghotaRepo.c_str());
    printSep();
    Serial.println("  [1] Informacion del modulo");
    Serial.println("  [2] Configurar repositorio GitHub");
    Serial.println("  [3] Configurar WiFi");
    Serial.println("  [0] Salir y continuar el arranque");
    printSep();
}

// ---------------------------------------------------------------------------
// Punto de entrada — llamado desde setup()
// ---------------------------------------------------------------------------
bool serialMenuRun(AppConfig &cfg)
{
    Serial.println();
    printSep('*');
    Serial.println("  Presiona cualquier tecla en los proximos");
    Serial.printf("  %d segundos para entrar al menu...\n", MENU_BOOT_WAIT_SECS);
    printSep('*');

    unsigned long deadline = millis() + (MENU_BOOT_WAIT_SECS * 1000UL);
    serialFlush();

    int lastSec = MENU_BOOT_WAIT_SECS + 1;
    while (millis() < deadline)
    {
        int remaining = (int)((deadline - millis()) / 1000) + 1;
        if (remaining != lastSec)
        {
            Serial.printf("  %d...\n", remaining);
            lastSec = remaining;
        }
        if (Serial.available())
        {
            serialFlush();
            goto enterMenu;
        }
        delay(50);
        yield();
    }

    Serial.println("  Continuando arranque normal...");
    return false;

enterMenu:
    bool wifiChanged = false;
    bool running = true;

    while (running)
    {
        printMainMenu(cfg);
        int op = serialReadInt("  Opcion: ", 0, 3);

        switch (op)
        {
        case 1:
            menuInfo(cfg);
            break;
        case 2:
            menuRepo(cfg);
            break;
        case 3:
            wifiChanged |= menuWifi(cfg);
            break;
        case 0:
            running = false;
            break;
        }
    }

    Serial.println("  Saliendo del menu. Continuando arranque...");
    Serial.println();
    return wifiChanged;
}