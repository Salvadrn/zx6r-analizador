#include "obd.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // macOS: se usa SO_NOSIGPIPE (y main ignora SIGPIPE)
#endif

using Clock = std::chrono::steady_clock;

namespace {

constexpr double CMD_TIMEOUT = 1.5;      // s máximos esperando respuesta a un comando
constexpr int READ_TIMEOUT_MS = 500;     // sin datos en este tiempo = fin de la respuesta
constexpr int CONNECT_TIMEOUT_MS = 5000;
constexpr auto DELAY_CMD = std::chrono::milliseconds(20);   // pausa después de cada comando
constexpr double RETRY_S = 2.0;          // espera entre intentos de reconexión
const char* const INIT_CMDS[] = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0"};

// =====================================================
// PARSERS
// =====================================================

// Quita espacios, pasa a mayúsculas y devuelve los hex que siguen a `header` (p. ej. "4111")
std::optional<std::string> payload(const std::string& resp, const std::string& header) {
    std::string c;
    c.reserve(resp.size());
    for (unsigned char ch : resp)
        if (!std::isspace(ch)) c += static_cast<char>(std::toupper(ch));
    const auto i = c.find(header);
    if (i == std::string::npos) return std::nullopt;
    return c.substr(i + header.size());
}

// Primeros `count` bytes en hex como un entero (A, o 256A+B)
std::optional<long> hex_bytes(const std::optional<std::string>& p, int count) {
    if (!p || p->size() < static_cast<std::size_t>(2 * count)) return std::nullopt;
    long v = 0;
    for (int i = 0; i < 2 * count; ++i) {
        const char ch = (*p)[i];
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return std::nullopt;
        v = v * 16 + d;
    }
    return v;
}

double round_to(double v, int dec) {
    const double f = std::pow(10.0, dec);
    return std::round(v * f) / f;
}

// Igual que re.search(r"(\d{1,2}\.\d)\s*V", resp)
std::optional<double> parse_volts(const std::string& r) {
    auto digit = [&](std::size_t i) { return i < r.size() && r[i] >= '0' && r[i] <= '9'; };
    for (std::size_t i = 0; i < r.size(); ++i) {
        for (std::size_t nd = 2; nd >= 1; --nd) {     // \d{1,2}: primero dos dígitos
            bool ok = true;
            for (std::size_t k = 0; k < nd; ++k) ok = ok && digit(i + k);
            if (!ok || i + nd >= r.size() || r[i + nd] != '.' || !digit(i + nd + 1)) continue;
            std::size_t j = i + nd + 2;
            while (j < r.size() && std::isspace(static_cast<unsigned char>(r[j]))) ++j;
            if (j < r.size() && r[j] == 'V') {
                double v = 0;
                for (std::size_t k = 0; k < nd; ++k) v = v * 10 + (r[i + k] - '0');
                return v + (r[i + nd + 1] - '0') / 10.0;
            }
        }
    }
    return std::nullopt;
}

double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

// =====================================================
// ELM327 POR TCP
// =====================================================
struct Stopped {};   // hay que salir: corta cualquier espera

std::runtime_error sys_error(const char* what) {
    return std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

int ms_until(Clock::time_point t) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t - Clock::now()).count());
}

class Elm {
public:
    explicit Elm(const std::atomic<bool>& stop) : stop_(stop) {}
    ~Elm() { close(); }
    Elm(const Elm&) = delete;
    Elm& operator=(const Elm&) = delete;

    bool is_open() const { return fd_ >= 0; }

    void connect(const std::string& host, int port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        if (rc != 0) throw std::runtime_error(std::string("dirección inválida: ") + ::gai_strerror(rc));
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(res, ::freeaddrinfo);

        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ < 0) throw sys_error("socket");
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);
        int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (::connect(fd_, res->ai_addr, res->ai_addrlen) < 0) {
            if (errno != EINPROGRESS) throw sys_error("connect");
            if (!wait(POLLOUT, CONNECT_TIMEOUT_MS)) throw std::runtime_error("connect: tiempo agotado");
            int err = 0;
            socklen_t len = sizeof err;
            ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) throw std::runtime_error(std::string("connect: ") + std::strerror(err));
        }
        for (const char* cmd : INIT_CMDS) send(cmd);
    }

    // Manda un comando y lee hasta el prompt '>' del ELM327
    std::string send(const std::string& cmd) {
        drain();
        const std::string out = cmd + "\r";
        std::size_t off = 0;
        while (off < out.size()) {
            const ssize_t n = ::send(fd_, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (n > 0) {
                off += static_cast<std::size_t>(n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (!wait(POLLOUT, READ_TIMEOUT_MS)) throw std::runtime_error("send: tiempo agotado");
            } else {
                throw sys_error("send");
            }
        }
        std::string buf;
        const auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(CMD_TIMEOUT * 1000));
        char tmp[1024];
        for (;;) {
            const int left = ms_until(deadline);
            if (left <= 0 || !wait(POLLIN, std::min(left, READ_TIMEOUT_MS))) break;
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n == 0) throw std::runtime_error("socket cerrado");
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                throw sys_error("recv");
            }
            buf.append(tmp, static_cast<std::size_t>(n));
            if (buf.find('>') != std::string::npos) break;
        }
        std::this_thread::sleep_for(DELAY_CMD);
        return buf;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    // true si el socket está listo; en pedazos de 100 ms para poder salir rápido
    bool wait(short events, int timeout_ms) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (stop_) throw Stopped{};
            const int left = ms_until(deadline);
            if (left <= 0) return false;
            pollfd p{fd_, events, 0};
            const int r = ::poll(&p, 1, std::min(left, 100));
            if (r > 0) return true;
            if (r < 0 && errno != EINTR) throw sys_error("poll");
        }
    }

    // Tira lo que haya quedado en el socket (respuestas tardías) para no desfasarse
    void drain() {
        char tmp[1024];
        for (;;) {
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, MSG_DONTWAIT);
            if (n > 0) continue;
            if (n == 0) throw std::runtime_error("socket cerrado");
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            throw sys_error("recv");
        }
    }

    int fd_ = -1;
    const std::atomic<bool>& stop_;
};

}  // namespace

const char* link_state_id(LinkState s) {
    switch (s) {
        case LinkState::Connecting: return "connecting";
        case LinkState::Live: return "live";
        case LinkState::Lost: return "lost";
        case LinkState::Sim: return "sim";
    }
    return "connecting";
}

std::optional<double> parse_response(const Sensor& s, const std::string& resp) {
    switch (s.parse) {
        case Parse::Battery:
            return parse_volts(resp);
        case Parse::MapR: {   // propietario Kawasaki; escala sin confirmar (/10)
            const auto b = hex_bytes(payload(resp, "6201F0"), 2);
            if (!b) return std::nullopt;
            return round_to(*b / 10.0, 1);
        }
        default:
            break;
    }
    // Modo 01: la respuesta empieza con "41" + PID sin el "01"
    const auto p = payload(resp, std::string("41") + (s.cmd + 2));
    switch (s.parse) {
        case Parse::Rpm: {
            const auto ab = hex_bytes(p, 2);   // (256A + B) / 4
            if (!ab) return std::nullopt;
            return *ab / 4.0;
        }
        case Parse::Tps: {
            const auto a = hex_bytes(p, 1);
            if (!a) return std::nullopt;
            return round_to(*a * 100.0 / 255.0, 1);
        }
        case Parse::Temp: {
            const auto a = hex_bytes(p, 1);
            if (!a) return std::nullopt;
            return static_cast<double>(*a - 40);
        }
        case Parse::O2: {     // B1S1: 0–1.275 V; sano = oscila entre ~0.1 y ~0.9 V
            const auto a = hex_bytes(p, 1);
            if (!a) return std::nullopt;
            return round_to(*a * 0.005, 3);
        }
        case Parse::MapL: {
            const auto a = hex_bytes(p, 1);
            if (!a) return std::nullopt;
            return static_cast<double>(*a);
        }
        default:
            return std::nullopt;
    }
}

double sim_value(const Sensor& s, double t) {
    const double load = 20 + 55 * std::fabs(std::sin(t * 0.4));
    switch (s.parse) {
        case Parse::Rpm: return std::round(2500 + 5500 * std::fabs(std::sin(t * 0.4)) + 300 * std::sin(t * 3.1));
        case Parse::Tps: return clampd(load + 8 * std::sin(t * 3.1), 0, 100);
        case Parse::Temp: return 78 + 12 * std::sin(t * 0.04);
        case Parse::O2: return clampd(round_to(0.5 + 0.4 * std::sin(t * 6.0) + 0.05 * std::sin(t * 17.3), 3), 0, 1);
        case Parse::MapL: return clampd(30 + load * 0.72 + 5 * std::sin(t * 2.1), 20, 105);
        case Parse::MapR: return clampd(28 + load * 0.74 + 5 * std::sin(t * 2.1 + 0.3), 20, 105);
        case Parse::Battery: return 13.9 + 0.15 * std::sin(t * 0.7);
    }
    return 0.0;
}

// =====================================================
// MEMORIA DE MUESTRAS
// =====================================================
Telemetry::Telemetry() : t0_(Clock::now()) {
    for (auto& s : slots_) {
        s.t.assign(MAX_POINTS, 0.0);
        s.v.assign(MAX_POINTS, 0.0);
    }
}

double Telemetry::elapsed() const {
    return std::chrono::duration<double>(Clock::now() - t0_).count();
}

void Telemetry::record(std::size_t i, double t, std::optional<double> v) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        Slot& s = slots_[i];
        if (!v) {
            s.no_data = true;
            return;
        }
        s.t[s.head] = t;
        s.v[s.head] = *v;
        s.head = (s.head + 1) % MAX_POINTS;
        if (s.n < MAX_POINTS) ++s.n;
        s.last = v;
        s.last_t = t;
        s.no_data = false;
        const double now = elapsed();
        rate_.push_back(now);
        while (!rate_.empty() && (rate_.front() < now - 3.0 || rate_.size() > 4096)) rate_.pop_front();
    }
    if (sample_fn_) sample_fn_(t, i, *v);
}

void Telemetry::set_state(LinkState s) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = s;
}

void Telemetry::went_live() {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = LinkState::Live;
    retries_ = 0;
    since_ = elapsed();
}

int Telemetry::lost() {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = LinkState::Lost;
    return ++retries_;
}

Snapshot Telemetry::snapshot(double t_from, unsigned series_mask) const {
    Snapshot out;
    std::lock_guard<std::mutex> lk(mu_);
    out.state = state_;
    out.since = since_;
    out.retries = retries_;
    out.now = elapsed();
    while (!rate_.empty() && rate_.front() < out.now - 3.0) rate_.pop_front();
    out.hz = static_cast<double>(rate_.size()) / 3.0;
    for (std::size_t i = 0; i < SENSOR_COUNT; ++i) {
        const Slot& s = slots_[i];
        SensorView& o = out.sensors[i];
        o.last = s.last;
        o.last_t = s.last_t;
        o.no_data = s.no_data;
        if (!(series_mask & (1u << i))) continue;
        const std::size_t start = (s.head + MAX_POINTS - s.n) % MAX_POINTS;
        for (std::size_t k = 0; k < s.n; ++k) {
            const std::size_t j = (start + k) % MAX_POINTS;
            if (s.t[j] < t_from) continue;
            o.t.push_back(s.t[j]);
            o.v.push_back(s.v[j]);
        }
    }
    return out;
}

// =====================================================
// LECTOR
// =====================================================
void Reader::start() {
    stop_ = false;
    th_ = std::thread([this] { cfg_.sim ? run_sim() : run_elm(); });
}

void Reader::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool Reader::pause(double seconds) {
    std::unique_lock<std::mutex> lk(mu_);
    return !cv_.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return stop_.load(); });
}

// Simulador: todos los sensores activos en cada tick (10 Hz)
void Reader::run_sim() {
    tel_.set_state(LinkState::Sim);
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / SIM_HZ));
    auto next = Clock::now();
    while (!stop_) {
        const double t = tel_.elapsed();
        for (std::size_t i = 0; i < SENSOR_COUNT; ++i)
            if (cfg_.active[i]) tel_.record(i, t, sim_value(SENSORS[i], t));
        next = std::max(next + period, Clock::now());
        std::unique_lock<std::mutex> lk(mu_);
        if (cv_.wait_until(lk, next, [this] { return stop_.load(); })) break;
    }
}

// ELM327: cada vuelta lee los sensores de periodo 0 + el lento más atrasado. Reconecta solo, para siempre.
void Reader::run_elm() {
    Elm elm(stop_);
    std::vector<std::size_t> fast, slow;
    for (std::size_t i = 0; i < SENSOR_COUNT; ++i)
        if (cfg_.active[i]) (SENSORS[i].period <= 0 ? fast : slow).push_back(i);
    std::array<double, SENSOR_COUNT> last_read;
    last_read.fill(-1e9);

    while (!stop_) {
        try {
            if (!elm.is_open()) {
                tel_.set_state(LinkState::Connecting);
                elm.connect(cfg_.obd_ip, cfg_.obd_port);
                tel_.went_live();
                std::printf("[OBD2] Conectado a %s:%d\n", cfg_.obd_ip.c_str(), cfg_.obd_port);
                std::fflush(stdout);
            }
            std::vector<std::size_t> batch = fast;
            if (!slow.empty()) {
                const double now = tel_.elapsed();
                std::size_t best = slow.front();
                double best_r = -1;
                for (std::size_t i : slow) {
                    const double r = (now - last_read[i]) / SENSORS[i].period;
                    if (r > best_r) {
                        best_r = r;
                        best = i;
                    }
                }
                batch.push_back(best);
            }
            if (batch.empty() && !pause(0.5)) break;
            for (std::size_t i : batch) {
                if (stop_) break;
                last_read[i] = tel_.elapsed();
                const std::string resp = elm.send(SENSORS[i].cmd);
                tel_.record(i, tel_.elapsed(), parse_response(SENSORS[i], resp));
            }
        } catch (const Stopped&) {
            break;
        } catch (const std::exception& e) {   // nunca dejar de leer: cualquier fallo = reconectar
            elm.close();
            const int n = tel_.lost();
            std::printf("[OBD2] %s — reintentando (%d)...\n", e.what(), n);
            std::fflush(stdout);
            if (!pause(RETRY_S)) break;
        }
    }
}
