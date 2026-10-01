// OBD2: parsers del ELM327, simulador, memoria de muestras y el hilo lector.
#pragma once

#include "config.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class LinkState { Connecting, Live, Lost, Sim };

const char* link_state_id(LinkState s);   // "connecting" | "live" | "lost" | "sim"

// Respuesta del ELM327 -> valor (nullopt si no se pudo leer)
std::optional<double> parse_response(const Sensor& s, const std::string& resp);
double sim_value(const Sensor& s, double t);

// Copia de un sensor para dibujar o servir
struct SensorView {
    std::optional<double> last;
    double last_t = 0;            // t de la última muestra válida (vale si hay last)
    bool no_data = false;         // la última respuesta no se pudo leer
    std::vector<double> t, v;     // muestras desde t_from (solo las que se pidieron)
};

struct Snapshot {
    LinkState state = LinkState::Connecting;
    double since = 0;             // t en que pasó a live
    int retries = 0;
    double now = 0;               // s desde el arranque
    double hz = 0;                // lecturas válidas por segundo (últimos 3 s)
    std::array<SensorView, SENSOR_COUNT> sensors;
};

// Memoria compartida entre el lector, la UI y el servidor web
class Telemetry {
public:
    using SampleFn = std::function<void(double t, std::size_t sensor, double v)>;

    Telemetry();
    double elapsed() const;                       // s desde el arranque
    void on_sample(SampleFn fn) { sample_fn_ = std::move(fn); }   // antes de arrancar el lector

    void record(std::size_t sensor, double t, std::optional<double> v);
    void set_state(LinkState s);
    void went_live();
    int lost();                                   // devuelve el número de reintentos

    // series_mask: bit i = copiar las muestras del sensor i con t >= t_from
    Snapshot snapshot(double t_from, unsigned series_mask) const;

private:
    struct Slot {
        std::vector<double> t, v;     // buffer circular de MAX_POINTS
        std::size_t head = 0, n = 0;
        std::optional<double> last;
        double last_t = 0;
        bool no_data = false;
    };

    std::chrono::steady_clock::time_point t0_;
    mutable std::mutex mu_;
    std::array<Slot, SENSOR_COUNT> slots_;
    mutable std::deque<double> rate_;   // instante de cada muestra válida
    LinkState state_ = LinkState::Connecting;
    double since_ = 0;
    int retries_ = 0;
    SampleFn sample_fn_;
};

// Hilo que lee sin parar hasta salir (simulador o ELM327 por TCP; reconecta solo)
class Reader {
public:
    Reader(const Config& cfg, Telemetry& tel) : cfg_(cfg), tel_(tel) {}
    ~Reader() { stop(); }
    void start();
    void stop();

private:
    void run_sim();
    void run_elm();
    bool pause(double seconds);   // espera interrumpible; false si hay que salir

    const Config& cfg_;
    Telemetry& tel_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread th_;
};
