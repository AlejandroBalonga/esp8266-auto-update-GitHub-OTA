#pragma once

#include "serial_menu.h"   // AppConfig

class OTAUpdater
{
public:
    OTAUpdater();
    void begin(const AppConfig &cfg);
    void checkForUpdate(const AppConfig &cfg);
};
