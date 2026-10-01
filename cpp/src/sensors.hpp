// Tabla de sensores y vistas: la misma que la versión Python.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

constexpr double STALE_S = 3.0;            // sin muestra nueva en este tiempo = dato obsoleto
constexpr std::size_t MAX_POINTS = 1500;   // muestras por sensor en memoria
constexpr double SIM_HZ = 10.0;

// Cómo se interpreta la respuesta del ELM327
enum class Parse { Rpm, Tps, Temp, O2, MapL, Battery, MapR };

struct Sensor {
    const char* id;
    const char* cmd;      // PID (o comando AT) que se manda
    const char* name;
    const char* desc;
    const char* unit;
    double y_min, y_max;
    int dec;              // decimales al mostrar
    double period;        // s entre lecturas; 0 = en cada vuelta
    Parse parse;
    std::optional<double> warn, crit;          // umbrales altos
    std::optional<double> warn_lo, crit_lo;    // umbrales bajos
    bool on_by_default;
};

constexpr std::size_t SENSOR_COUNT = 7;

// warn/crit: umbrales de color del valor (verifícalos con el manual).
inline const std::array<Sensor, SENSOR_COUNT> SENSORS = {{
    // id      cmd       nombre     descripción                    unidad y_min y_max  dec period  parseo
    {"rpm",   "010C",   "RPM",     "Revoluciones",                "rpm", 0,    16000, 0,  0.25, Parse::Rpm,     {},  {},  {},   {},   true},
    {"tps",   "0111",   "TPS",     "Posición del acelerador",     "%",   0,    100,   0,  0.25, Parse::Tps,     {},  {},  {},   {},   true},
    {"temp",  "0105",   "TEMP",    "Refrigerante",                "°C",  40,   130,   0,  2.0,  Parse::Temp,    105, 115, {},   {},   true},
    // O2 en cada vuelta: necesita tasa alta para ver la oscilación
    {"o2",    "0114",   "O2",      "Sonda lambda B1S1",           "V",   0,    1,     2,  0,    Parse::O2,      {},  {},  {},   {},   true},
    {"map_l", "010B",   "MAP IZQ", "Presión de admisión",         "kPa", 15,   110,   0,  0.25, Parse::MapL,    {},  {},  {},   {},   true},
    {"bat",   "ATRV",   "BATERÍA", "Voltaje (lo mide el ELM327)", "V",   10,   15,    1,  5.0,  Parse::Battery, {},  {},  12.2, 11.5, true},
    // PID propietario Kawasaki: un ELM327 estándar casi nunca lo responde (OBD2_MAP_R=1 lo prende)
    {"map_r", "2201F0", "MAP DER", "Presión de admisión",         "kPa", 15,   110,   0,  0.5,  Parse::MapR,    {},  {},  {},   {},   false},
}};

inline int sensor_index(const char* id) {
    for (std::size_t i = 0; i < SENSORS.size(); ++i)
        if (std::strcmp(SENSORS[i].id, id) == 0) return static_cast<int>(i);
    return -1;
}

inline std::string fmt_value(const Sensor& s, double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", s.dec, v);
    return buf;
}

// Número compacto para JSON/CSV: hasta max_dec decimales, sin ceros de sobra ("null" si no es finito)
inline std::string fmt_num(double v, int max_dec) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", max_dec, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

// Vistas: el botón las recorre en orden. window = segundos visibles.
enum class ViewKind { Tiles, Graphs };

struct View {
    const char* name;
    const char* short_name;   // si las pestañas no caben
    ViewKind kind;
    double window;
    std::vector<const char*> sensors;
};

inline const std::array<View, 4> VIEWS = {{
    {"RESUMEN", "RESUMEN", ViewKind::Tiles, 30, {"rpm", "tps", "o2", "map_l", "temp", "bat", "map_r"}},
    {"O2", "O2", ViewKind::Graphs, 20, {"o2"}},
    {"MOTOR", "MOTOR", ViewKind::Graphs, 60, {"rpm", "tps", "map_l", "map_r"}},
    {"TEMP · BATERÍA", "TEMP·BAT", ViewKind::Graphs, 300, {"temp", "bat"}},
}};

// Líneas de referencia en las gráficas
struct RefLine {
    const char* sensor;
    double value;
    const char* label;
};

inline const std::array<RefLine, 3> REFS = {{
    {"o2", 0.45, "λ = 1  (0.45 V)"},
    {"map_l", 101, "101 kPa (atm)"},
    {"map_r", 101, "101 kPa (atm)"},
}};
