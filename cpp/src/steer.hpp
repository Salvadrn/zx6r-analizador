// Dirección: encoder magnético AS5600 por I²C (no pasa por el ELM327), su calibración y su hilo.
#pragma once

#include "config.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class Telemetry;

struct SteerStatus {
    const char* status;   // "init" | "ok" | "sin_sensor" | "sin_iman" | "sim"
    bool calibrated;      // ya se fijó el centro
    bool invert;          // signo invertido (+ = izquierda)
};

class Steering {
public:
    Steering(const Config& cfg, Telemetry& tel);
    ~Steering() { stop(); }
    Steering(const Steering&) = delete;
    Steering& operator=(const Steering&) = delete;

    void start();             // si el sensor está activo: carga la calibración y arranca el hilo (25 Hz)
    void stop();

    bool set_center();        // el ángulo crudo actual pasa a ser 0°; false si todavía no hay lectura
    bool toggle_invert();
    SteerStatus status() const;

private:
    void run();
    bool pause(double seconds);
    double angle(double raw) const;   // con mu_ tomado
    void load_cal();
    void save_cal() const;            // con mu_ tomado

    const Config& cfg_;
    Telemetry& tel_;
    std::size_t index_ = 0;           // "steer" en SENSORS

    mutable std::mutex mu_;
    const char* status_ = "init";
    std::optional<double> raw_;       // último ángulo crudo del sensor (grados, 0–360)
    std::optional<double> center_;    // calibración: crudo que corresponde a 0°
    bool invert_ = false;

    std::atomic<bool> stop_{false};
    std::mutex wait_mu_;
    std::condition_variable cv_;
    std::thread th_;
};
