// Configuración por variables de entorno (mismos nombres que la versión Python).
#pragma once

#include "sensors.hpp"

#include <array>
#include <string>

struct Config {
    bool sim = false;                       // OBD2_SIM=1
    std::string obd_ip = "192.168.0.10";    // OBD2_IP
    int obd_port = 35000;                   // OBD2_PORT
    bool windowed = false;                  // OBD2_WINDOWED=1: ventana 1280x720
    int fps = 30;                           // OBD2_FPS
    std::string db_path;                    // OBD2_DB (default ~/obd2_logs/telemetria.db)
    int web_port = 8080;                    // OBD2_WEB_PORT (0 = apagado)
    std::string font_sans, font_mono;       // OBD2_FONT_SANS_FILE / OBD2_FONT_MONO_FILE
    std::string snapshot_dir;               // OBD2_SNAPSHOT_DIR: guarda un PNG por vista y sale
    std::array<bool, SENSOR_COUNT> active{};  // sensores que se leen (MAP DER con OBD2_MAP_R=1)
};
