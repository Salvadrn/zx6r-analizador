// Pantalla con raylib: mismo diseño que la versión Python (negro puro, grises; el color solo marca estados).
#pragma once

#include "config.hpp"

#include <atomic>

class Telemetry;
class Store;
class WebServer;
class Steering;

// Corre en el hilo principal hasta Esc, cerrar la ventana o `quit` (señal).
// Devuelve false si no se pudo abrir la ventana.
bool run_ui(const Config& cfg, const Telemetry& tel, const Store& store, const WebServer& web, Steering& steer,
            const std::atomic<bool>& quit);
