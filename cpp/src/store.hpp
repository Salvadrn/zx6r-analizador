// Registro en SQLite: mismo esquema que la versión Python (cada una lee los datos de la otra).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

struct SessionRow {
    int id = 0;
    std::string started;       // hora local YYYY-MM-DDTHH:MM:SS
    bool sim = false;
    double duration = 0;       // t de la última muestra
    long long samples = 0;
};

struct SensorSeries {
    std::size_t sensor = 0;    // índice en SENSORS
    long long n = 0;           // lecturas guardadas
    double min = 0, max = 0, avg = 0;
    std::vector<double> t, v;  // reducido a max_points (mínimo y máximo de cada tramo)
};

struct SessionDetail {
    SessionRow row;
    std::vector<SensorSeries> sensors;   // solo sensores conocidos con datos, en el orden de la tabla
};

class Store {
public:
    explicit Store(std::string path) : path_(std::move(path)) {}
    ~Store() { close(); }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    bool open(bool sim);                        // crea esquema y sesión; arranca el hilo escritor
    void add(double t, const char* sensor, double v);
    void close();                               // guarda lo pendiente y cierra

    const std::string& path() const { return path_; }
    int session() const { return session_; }    // -1 = sin registro
    bool writing() const { return writing_; }   // la base abrió y el último lote se guardó

    // Lectura: cada llamada abre su propia conexión (WAL deja leer mientras se escribe)
    std::vector<SessionRow> sessions(int limit) const;
    std::optional<SessionDetail> session_detail(int id, std::size_t max_points) const;
    // false si la sesión no existe; `out` recibe el CSV en pedazos y devuelve false para cortar
    bool export_csv(int id, const std::function<bool(const std::string&)>& out) const;

private:
    struct Sample {
        double t;
        const char* sensor;    // apunta a la tabla SENSORS (estática)
        double v;
    };

    void writer_loop();
    bool write_batch(const std::vector<Sample>& batch);

    std::string path_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_ = nullptr;
    std::atomic<int> session_{-1};
    std::atomic<bool> writing_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Sample> pending_;
    bool stop_ = false;
    std::thread th_;
};
