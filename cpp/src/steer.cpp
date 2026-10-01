#include "steer.hpp"

#include "obd.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include <sys/stat.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/i2c-dev.h>   // I2C_SLAVE
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using Clock = std::chrono::steady_clock;

namespace {

constexpr double STEER_HZ = 25.0;   // lecturas por segundo, en su propio hilo para no esperar al ELM327
constexpr double RETRY_S = 2.0;     // sin bus I²C o sin AS5600: reintenta

#ifdef __linux__
constexpr int AS5600_ADDR = 0x36;
constexpr std::uint8_t REG_STATUS = 0x0B;      // bit 0x20 = imán detectado
constexpr std::uint8_t REG_RAW_ANGLE = 0x0C;   // 0x0C-0x0D: 12 bits
constexpr std::uint8_t MAGNET_DETECTED = 0x20;

// Una transacción por registro: escribe la dirección y lee `n` bytes. STATUS y RAW ANGLE van por
// separado: el AS5600 trata RAW ANGLE de forma especial y no conviene fiarse del auto-incremento.
bool read_reg(int fd, std::uint8_t reg, std::uint8_t* out, std::size_t n) {
    if (::write(fd, &reg, 1) != 1) return false;
    return ::read(fd, out, n) == static_cast<ssize_t>(n);
}
#endif

void make_parent_dirs(const std::string& path) {
    for (std::size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

// El número más corto que se lee de vuelta idéntico (como repr() de Python)
std::string json_number(double v) {
    char buf[40];
    for (int p = 15; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

// Lo que sigue a "key": en un JSON sencillo como {"center": 200.2, "invert": false}
const char* json_value(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    const std::size_t i = s.find(k);
    if (i == std::string::npos) return nullptr;
    std::size_t j = s.find(':', i + k.size());
    if (j == std::string::npos) return nullptr;
    for (++j; j < s.size() && std::isspace(static_cast<unsigned char>(s[j])); ++j) {
    }
    return s.c_str() + j;
}

}  // namespace

Steering::Steering(const Config& cfg, Telemetry& tel) : cfg_(cfg), tel_(tel) {
    const int i = sensor_index("steer");
    index_ = i >= 0 ? static_cast<std::size_t>(i) : 0;
}

void Steering::start() {
    if (!cfg_.active[index_]) return;   // OBD2_STEER=0
    load_cal();
    stop_ = false;
    th_ = std::thread(&Steering::run, this);
}

void Steering::stop() {
    {
        std::lock_guard<std::mutex> lk(wait_mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool Steering::pause(double seconds) {
    std::unique_lock<std::mutex> lk(wait_mu_);
    return !cv_.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return stop_.load(); });
}

void Steering::run() {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / STEER_HZ));
    auto next = Clock::now();
#ifdef __linux__
    const std::string dev = "/dev/i2c-" + std::to_string(cfg_.i2c_bus);
    int fd = -1;
#endif
    while (!stop_) {
        const double t = tel_.elapsed();
        const char* status = "sin_sensor";   // fuera de Linux no hay I²C
        std::optional<double> raw;           // solo si hay imán
        if (cfg_.sim) {
            status = "sim";
            raw = std::fmod(200 + 9 * std::sin(t * 0.5) + 2.5 * std::sin(t * 2.3), 360.0);
        } else {
#ifdef __linux__
            if (fd < 0) {
                fd = ::open(dev.c_str(), O_RDWR);
                if (fd >= 0 && ::ioctl(fd, I2C_SLAVE, AS5600_ADDR) < 0) {
                    ::close(fd);
                    fd = -1;
                }
            }
            std::uint8_t st = 0, angle_bytes[2] = {0, 0};
            if (fd >= 0 && read_reg(fd, REG_STATUS, &st, 1) && read_reg(fd, REG_RAW_ANGLE, angle_bytes, 2)) {
                status = (st & MAGNET_DETECTED) ? "ok" : "sin_iman";
                if (st & MAGNET_DETECTED) raw = (((angle_bytes[0] & 0x0F) << 8) | angle_bytes[1]) * 360.0 / 4096.0;
            } else if (fd >= 0) {
                ::close(fd);   // error de lectura: se cierra y se vuelve a abrir en 2 s
                fd = -1;
            }
#endif
        }
        std::optional<double> value;
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_ = status;
            if (raw) {
                raw_ = raw;
                value = angle(*raw);
            }
        }
        tel_.record(index_, t, value);   // sin imán o sin sensor: no_data

        if (std::strcmp(status, "sin_sensor") == 0) {
            if (!pause(RETRY_S)) break;
            next = Clock::now();
            continue;
        }
        next = std::max(next + period, Clock::now());
        std::unique_lock<std::mutex> lk(wait_mu_);
        if (cv_.wait_until(lk, next, [this] { return stop_.load(); })) break;
    }
#ifdef __linux__
    if (fd >= 0) ::close(fd);
#endif
}

// Crudo -> ángulo del manubrio: 0° = centro calibrado, de −180 a 180 (sin brinco al pasar por 0°/360°)
double Steering::angle(double raw) const {
    const double center = center_ ? *center_ : (cfg_.sim ? 200.0 : 0.0);
    double a = std::fmod(raw - center + 180.0, 360.0);
    if (a < 0) a += 360.0;
    a -= 180.0;
    if (invert_) a = -a;
    return std::nearbyint(a * 10.0) / 10.0;
}

bool Steering::set_center() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!raw_) return false;
    center_ = raw_;
    save_cal();
    return true;
}

bool Steering::toggle_invert() {
    std::lock_guard<std::mutex> lk(mu_);
    invert_ = !invert_;
    save_cal();
    return true;
}

SteerStatus Steering::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {status_, center_.has_value(), invert_};
}

// Mismo archivo y formato que la versión Python: {"center": <grados o null>, "invert": <bool>}
void Steering::load_cal() {
    std::ifstream in(cfg_.steer_cal);
    if (!in) return;
    const std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::lock_guard<std::mutex> lk(mu_);
    if (const char* c = json_value(s, "center")) {
        char* end = nullptr;
        const double v = std::strtod(c, &end);
        if (end != c && std::isfinite(v)) center_ = v;
    }
    if (const char* v = json_value(s, "invert")) invert_ = std::strncmp(v, "true", 4) == 0;
}

void Steering::save_cal() const {
    const std::string& path = cfg_.steer_cal;
    make_parent_dirs(path);
    const std::string tmp = path + ".tmp";
    const std::string body = std::string("{\"center\": ") + (center_ ? json_number(*center_) : "null") +
                             ", \"invert\": " + (invert_ ? "true" : "false") + "}";
    bool ok = false;
    if (std::FILE* f = std::fopen(tmp.c_str(), "w")) {
        ok = std::fputs(body.c_str(), f) >= 0;
        ok = std::fclose(f) == 0 && ok;
    }
    ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;   // escritura atómica: nunca queda a medias
    if (!ok) {
        std::printf("[DIR] No se pudo guardar la calibración en %s: %s\n", path.c_str(), std::strerror(errno));
        std::fflush(stdout);
    }
}
