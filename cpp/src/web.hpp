// Servidor HTTP mínimo: el visor (web/index.html, embebido al compilar) y la API JSON.
// El contrato es el mismo de la versión Python; el HTML es compartido.
#pragma once

#include "config.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Telemetry;
class Store;

class WebServer {
public:
    WebServer(const Config& cfg, const Telemetry& tel, const Store& store) : cfg_(cfg), tel_(tel), store_(store) {}
    ~WebServer() { stop(); }
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    bool start();                  // false si está apagado (puerto 0) o no se pudo abrir el puerto
    void stop();
    bool listening() const { return listening_; }
    int port() const { return cfg_.web_port; }

    // IP local con la que se llega a `toward` (socket UDP "conectado": no manda nada)
    static std::string local_ip(const std::string& toward);

private:
    void accept_loop();
    void worker_loop();
    void handle(int fd);
    std::string live_json(double trace_s) const;
    std::string sessions_json() const;
    bool session_json(int id, std::string& out) const;
    void session_csv(int fd, int id) const;

    const Config& cfg_;
    const Telemetry& tel_;
    const Store& store_;
    int listen_fd_ = -1;
    std::atomic<bool> listening_{false};
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<int> queue_;        // conexiones aceptadas esperando a un trabajador
    std::thread acceptor_;
    std::vector<std::thread> workers_;
};
