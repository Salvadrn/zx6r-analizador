// ZX-6R — Telemetría OBD2 (RPM, TPS, temperatura, O2, MAP, batería). Versión C++.
//
// Lee PIDs de un ELM327 WiFi sin parar (reconecta solo), los muestra en pantalla completa,
// guarda todo en SQLite y sirve un visor web para el celular. Las vistas se cambian con un
// botón (cualquier botón que mande teclas). Con OBD2_SIM=1 corre con datos simulados.
#include "config.hpp"
#include "obd.hpp"
#include "steer.hpp"
#include "store.hpp"
#include "ui.hpp"
#include "web.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <pwd.h>
#include <unistd.h>

namespace {

std::atomic<bool> g_quit{false};

void on_signal(int) { g_quit = true; }

std::string env(const char* key, const std::string& def = "") {
    const char* v = std::getenv(key);
    return v && *v ? std::string(v) : def;
}

int env_int(const char* key, int def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return *end == '\0' ? static_cast<int>(n) : def;
}

bool env_flag(const char* key) { return env(key) == "1"; }

std::string home_dir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    const passwd* pw = ::getpwuid(::getuid());
    return pw ? pw->pw_dir : ".";
}

std::string expand_home(const std::string& p) {
    return (p == "~" || p.rfind("~/", 0) == 0) ? home_dir() + p.substr(1) : p;
}

Config load_config() {
    Config c;
    c.sim = env_flag("OBD2_SIM");
    c.obd_ip = env("OBD2_IP", "192.168.0.10");
    c.obd_port = env_int("OBD2_PORT", 35000);
    c.windowed = env_flag("OBD2_WINDOWED");
    c.headless = env_flag("OBD2_HEADLESS");
    c.fps = std::clamp(env_int("OBD2_FPS", 30), 1, 240);
    c.db_path = expand_home(env("OBD2_DB", home_dir() + "/obd2_logs/telemetria.db"));
    c.web_port = env_int("OBD2_WEB_PORT", 8080);
    c.font_sans = expand_home(env("OBD2_FONT_SANS_FILE"));
    c.font_mono = expand_home(env("OBD2_FONT_MONO_FILE"));
    c.snapshot_dir = expand_home(env("OBD2_SNAPSHOT_DIR"));
    c.i2c_bus = env_int("OBD2_I2C_BUS", 1);
    const std::size_t slash = c.db_path.rfind('/');
    const std::string db_dir = slash == std::string::npos ? "." : slash == 0 ? "/" : c.db_path.substr(0, slash);
    c.steer_cal = expand_home(env("OBD2_STEER_CAL", db_dir + "/direccion.json"));
    const bool map_r = env_flag("OBD2_MAP_R");
    const bool steer = env("OBD2_STEER", "1") == "1";
    for (std::size_t i = 0; i < SENSOR_COUNT; ++i) {
        const std::string id = SENSORS[i].id;
        c.active[i] = id == "map_r" ? map_r : id == "steer" ? steer : SENSORS[i].on_by_default;
    }
    return c;
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);   // un cliente que se va no debe tumbar el programa
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const Config cfg = load_config();

    Telemetry tel;
    Store store(cfg.db_path);
    store.open(cfg.sim);
    tel.on_sample([&store](double t, std::size_t i, double v) { store.add(t, SENSORS[i].id, v); });
    Reader reader(cfg, tel);
    reader.start();
    Steering steer(cfg, tel);
    steer.start();
    WebServer web(cfg, tel, store, steer);
    web.start();

    std::string web_line = cfg.web_port > 0 ? "no disponible en el puerto " + std::to_string(cfg.web_port) : "apagado";
    if (web.listening()) {
        const std::string ip = WebServer::local_ip(cfg.obd_ip);
        web_line = "http://" + (ip.empty() ? std::string("<ip>") : ip) + ":" + std::to_string(cfg.web_port);
    }
    std::string db_line = store.session() >= 0 ? store.path() + " (sesión " + std::to_string(store.session()) + ")"
                                               : "SIN REGISTRO";
    std::printf("==========================================================\n");
    std::printf("  ZX-6R TELEMETRÍA — RPM / TPS / TEMP / O2 / MAP / BATERÍA\n");
    std::printf("==========================================================\n");
    std::printf("  Modo  : %s\n", cfg.sim ? "SIMULADOR" : "OBD2 REAL");
    std::printf("  OBD2  : %s:%d\n", cfg.obd_ip.c_str(), cfg.obd_port);
    std::printf("  Base  : %s\n", db_line.c_str());
    std::printf("  Web   : %s\n", web_line.c_str());
    const int si = sensor_index("steer");
    if (si >= 0 && cfg.active[static_cast<std::size_t>(si)])
        std::printf("  Dir.  : AS5600 en /dev/i2c-%d · calibración %s\n", cfg.i2c_bus, cfg.steer_cal.c_str());
    else
        std::printf("  Dir.  : apagada (OBD2_STEER=0)\n");
    std::printf("  Botón : → / espacio / Enter = siguiente · ← = anterior · Esc = salir\n");
    std::printf("==========================================================\n");
    std::fflush(stdout);

    // Sin pantalla (OBD2_HEADLESS=1 o si no abre la ventana): sigue leyendo, guardando y sirviendo
    if (cfg.headless || !run_ui(cfg, tel, store, web, steer, g_quit)) {
        if (!cfg.headless) std::printf("[UI] No se pudo abrir la pantalla; sigo leyendo (Ctrl+C para salir)\n");
        std::fflush(stdout);
        while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    reader.stop();
    steer.stop();
    web.stop();
    store.close();
    return 0;
}
