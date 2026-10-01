#include "web.hpp"

#include "obd.hpp"
#include "steer.hpp"
#include "store.hpp"
#include "web_index.hpp"   // generado por CMake desde web/index.html

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // macOS: SO_NOSIGPIPE en cada socket (y main ignora SIGPIPE)
#endif

namespace {

constexpr int WORKERS = 4;
constexpr std::size_t MAX_QUEUE = 64;
constexpr std::size_t MAX_REQUEST = 16 * 1024;
constexpr std::size_t MAX_SESSION_POINTS = 2000;   // por sensor en /api/session/<id>
constexpr int MAX_SESSIONS = 200;
const char* const JSON = "application/json; charset=utf-8";

// ── JSON hecho a mano ──
void jstr(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                    o += buf;
                } else {
                    o += static_cast<char>(ch);
                }
        }
    }
    o += '"';
}

void jarr(std::string& o, const std::vector<double>& v, int dec) {
    o += '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o += ',';
        o += fmt_num(v[i], dec);
    }
    o += ']';
}

void jseries(std::string& o, const std::vector<double>& t, const std::vector<double>& v) {
    o += "{\"t\":";
    jarr(o, t, 3);
    o += ",\"v\":";
    jarr(o, v, 4);
    o += '}';
}

// Metadatos del sensor sin cerrar la llave (el que llama puede agregar campos)
void jmeta_open(std::string& o, const Sensor& s) {
    o += "{\"id\":";
    jstr(o, s.id);
    o += ",\"name\":";
    jstr(o, s.name);
    o += ",\"desc\":";
    jstr(o, s.desc);
    o += ",\"unit\":";
    jstr(o, s.unit);
    o += ",\"dec\":" + std::to_string(s.dec);
    o += ",\"y_min\":" + fmt_num(s.y_min, 4);
    o += ",\"y_max\":" + fmt_num(s.y_max, 4);
    if (s.warn) o += ",\"warn\":" + fmt_num(*s.warn, 4);
    if (s.crit) o += ",\"crit\":" + fmt_num(*s.crit, 4);
    if (s.warn_lo) o += ",\"warn_lo\":" + fmt_num(*s.warn_lo, 4);
    if (s.crit_lo) o += ",\"crit_lo\":" + fmt_num(*s.crit_lo, 4);
    if (std::strcmp(s.id, "steer") == 0) o += ",\"calibrable\":true";   // el visor muestra los botones
}

// ── HTTP ──
bool send_all(int fd, const char* p, std::size_t n) {
    while (n > 0) {
        const ssize_t k = ::send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;   // cliente que se fue o que no lee (SO_SNDTIMEO)
        }
        p += k;
        n -= static_cast<std::size_t>(k);
    }
    return true;
}

bool send_all(int fd, const std::string& s) { return send_all(fd, s.data(), s.size()); }

const char* reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Internal Server Error";
    }
}

std::string head(int code, const char* ctype, long long length, const char* extra = "") {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\nContent-Type: " + ctype + "\r\n";
    if (length >= 0) h += "Content-Length: " + std::to_string(length) + "\r\n";
    h += "Connection: close\r\nCache-Control: no-store\r\n";
    h += extra;
    return h + "\r\n";
}

void reply(int fd, int code, const char* ctype, const char* body, std::size_t len, const char* extra = "") {
    if (send_all(fd, head(code, ctype, static_cast<long long>(len), extra))) send_all(fd, body, len);
}

void reply(int fd, int code, const char* ctype, const std::string& body, const char* extra = "") {
    reply(fd, code, ctype, body.data(), body.size(), extra);
}

const char* const TEXT_PLAIN = "text/plain; charset=utf-8";

void not_found(int fd, const char* what = "no encontrado") { reply(fd, 404, TEXT_PLAIN, std::string(what)); }

bool parse_id(const std::string& s, int& id) {
    if (s.empty() || s.size() > 9) return false;
    for (char ch : s)
        if (ch < '0' || ch > '9') return false;
    id = std::atoi(s.c_str());
    return true;
}

// ?trace=N (segundos; default 30)
double trace_param(const std::string& query) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (kv.rfind("trace=", 0) == 0) {
            char* end = nullptr;
            const double v = std::strtod(kv.c_str() + 6, &end);
            if (end != kv.c_str() + 6 && *end == '\0' && v == v) return std::max(0.0, std::min(v, 3600.0));
            return 30;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return 30;
}

// Cierre ordenado: manda FIN y espera a que el cliente cierre, para no cortarle la respuesta con un RST
void finish(int fd) {
    ::shutdown(fd, SHUT_WR);
    timeval tv{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[512];
    for (int i = 0; i < 64; ++i)
        if (::recv(fd, buf, sizeof buf, 0) <= 0) break;
    ::close(fd);
}

}  // namespace

bool WebServer::start() {
    if (cfg_.web_port <= 0) return false;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.web_port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 0.0.0.0: el celular entra por el WiFi
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 16) < 0) {
        std::printf("[WEB] No se pudo abrir el puerto %d: %s\n", cfg_.web_port, std::strerror(errno));
        std::fflush(stdout);
        ::close(fd);
        return false;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    listen_fd_ = fd;
    stop_ = false;
    listening_ = true;
    acceptor_ = std::thread(&WebServer::accept_loop, this);
    for (int i = 0; i < WORKERS; ++i) workers_.emplace_back(&WebServer::worker_loop, this);
    return true;
}

void WebServer::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (acceptor_.joinable()) acceptor_.join();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
    workers_.clear();
    for (int fd : queue_) ::close(fd);
    queue_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    listening_ = false;
}

void WebServer::accept_loop() {
    while (!stop_) {
        pollfd p{listen_fd_, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));   // p. ej. sin descriptores libres
            continue;
        }
        // En BSD/macOS el socket aceptado hereda O_NONBLOCK; aquí se usa bloqueante con timeouts
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.size() < MAX_QUEUE) {
                queue_.push_back(fd);
                queued = true;
            }
        }
        if (queued) cv_.notify_one();
        else ::close(fd);
    }
}

void WebServer::worker_loop() {
    for (;;) {
        int fd;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            fd = queue_.front();
            queue_.pop_front();
        }
        try {
            handle(fd);
        } catch (const std::exception& e) {
            std::printf("[WEB] Error: %s\n", e.what());
            std::fflush(stdout);
        }
        finish(fd);
    }
}

void WebServer::handle(int fd) {
    timeval rcv{5, 0}, snd{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    // Petición: solo importa la primera línea ("GET /ruta?query HTTP/1.1")
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.find("\n\n") == std::string::npos) {
        if (req.size() > MAX_REQUEST) return;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        req.append(buf, static_cast<std::size_t>(n));
    }
    const std::string line = req.substr(0, req.find_first_of("\r\n"));
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos) {
        reply(fd, 400, TEXT_PLAIN, std::string("petición inválida"));
        return;
    }
    const std::string method = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
    const std::size_t q = target.find('?');
    const std::string path = target.substr(0, q);
    const std::string query = q == std::string::npos ? "" : target.substr(q + 1);

    // Calibración de la dirección desde el celular (el cuerpo de la petición no se usa)
    if (method == "POST") {
        if (path == "/api/steer/center") reply(fd, 200, JSON, steer_json(steer_.set_center()));
        else if (path == "/api/steer/invert") reply(fd, 200, JSON, steer_json(steer_.toggle_invert()));
        else not_found(fd);
        return;
    }
    if (method != "GET") {
        reply(fd, 405, TEXT_PLAIN, std::string("solo GET y POST"), "Allow: GET, POST\r\n");
        return;
    }

    if (path == "/" || path == "/index.html") {
        reply(fd, 200, "text/html; charset=utf-8", kIndexHtml, sizeof kIndexHtml - 1);
    } else if (path == "/api/live") {
        reply(fd, 200, JSON, live_json(trace_param(query)));
    } else if (path == "/api/sessions") {
        reply(fd, 200, JSON, sessions_json());
    } else if (path.rfind("/api/session/", 0) == 0) {
        std::string rest = path.substr(13);
        const bool csv = rest.size() > 4 && rest.compare(rest.size() - 4, 4, ".csv") == 0;
        if (csv) rest.resize(rest.size() - 4);
        int id = 0;
        std::string body;
        if (!parse_id(rest, id)) not_found(fd);
        else if (csv) session_csv(fd, id);
        else if (session_json(id, body)) reply(fd, 200, JSON, body);
        else not_found(fd, "no existe");
    } else {
        not_found(fd);
    }
}

// Solo sensores activos; trace = últimos N s de la memoria; t = s desde el arranque
std::string WebServer::live_json(double trace_s) const {
    unsigned mask = 0;
    for (std::size_t i = 0; i < SENSOR_COUNT; ++i)
        if (cfg_.active[i]) mask |= 1u << i;
    const Snapshot s = tel_.snapshot(tel_.elapsed() - trace_s, mask);
    const int session = store_.session();

    std::string o;
    o.reserve(32 * 1024);
    o += "{\"state\":";
    jstr(o, link_state_id(s.state));
    o += ",\"session\":" + (session >= 0 ? std::to_string(session) : std::string("null"));
    o += ",\"hz\":" + fmt_num(s.hz, 1);
    o += ",\"t\":" + fmt_num(s.now, 3);
    o += ",\"sensors\":[";
    bool first = true;
    for (std::size_t i = 0; i < SENSOR_COUNT; ++i) {
        if (!cfg_.active[i]) continue;
        if (!first) o += ',';
        first = false;
        jmeta_open(o, SENSORS[i]);
        o += '}';
    }
    o += "],\"values\":{";
    first = true;
    for (std::size_t i = 0; i < SENSOR_COUNT; ++i) {
        if (!cfg_.active[i]) continue;
        const SensorView& sv = s.sensors[i];
        if (!first) o += ',';
        first = false;
        jstr(o, SENSORS[i].id);
        o += ":{\"v\":" + (sv.last ? fmt_num(*sv.last, 4) : std::string("null"));
        o += ",\"age\":" + (sv.last ? fmt_num(std::max(0.0, s.now - sv.last_t), 3) : std::string("null"));
        o += std::string(",\"no_data\":") + (sv.no_data ? "true" : "false") + "}";
    }
    o += "},\"trace\":{";
    first = true;
    for (std::size_t i = 0; i < SENSOR_COUNT; ++i) {
        if (!cfg_.active[i]) continue;
        if (!first) o += ',';
        first = false;
        jstr(o, SENSORS[i].id);
        o += ':';
        jseries(o, s.sensors[i].t, s.sensors[i].v);
    }
    const SteerStatus st = steer_.status();
    o += "},\"steer\":{\"status\":";
    jstr(o, st.status);
    o += std::string(",\"calibrated\":") + (st.calibrated ? "true" : "false");
    o += std::string(",\"invert\":") + (st.invert ? "true" : "false") + "}}";
    return o;
}

// {"ok":bool,"calibrated":bool,"invert":bool}
std::string WebServer::steer_json(bool ok) const {
    const SteerStatus st = steer_.status();
    return std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"calibrated\":" + (st.calibrated ? "true" : "false") +
           ",\"invert\":" + (st.invert ? "true" : "false") + "}";
}

std::string WebServer::sessions_json() const {
    std::string o = "[";
    bool first = true;
    for (const SessionRow& r : store_.sessions(MAX_SESSIONS)) {
        if (!first) o += ',';
        first = false;
        o += "{\"id\":" + std::to_string(r.id) + ",\"started\":";
        jstr(o, r.started);
        o += ",\"sim\":" + std::string(r.sim ? "1" : "0");
        o += ",\"duration\":" + fmt_num(r.duration, 1);
        o += ",\"samples\":" + std::to_string(r.samples) + "}";
    }
    return o + "]";
}

bool WebServer::session_json(int id, std::string& o) const {
    const auto d = store_.session_detail(id, MAX_SESSION_POINTS);
    if (!d) return false;
    o += "{\"id\":" + std::to_string(d->row.id) + ",\"started\":";
    jstr(o, d->row.started);
    o += ",\"sim\":" + std::string(d->row.sim ? "1" : "0");
    o += ",\"duration\":" + fmt_num(d->row.duration, 1);
    o += ",\"sensors\":[";
    for (std::size_t k = 0; k < d->sensors.size(); ++k) {
        const SensorSeries& ss = d->sensors[k];
        if (k) o += ',';
        jmeta_open(o, SENSORS[ss.sensor]);
        o += ",\"min\":" + fmt_num(ss.min, 4) + ",\"max\":" + fmt_num(ss.max, 4) + ",\"avg\":" + fmt_num(ss.avg, 4);
        o += ",\"n\":" + std::to_string(ss.n) + "}";
    }
    o += "],\"data\":{";
    for (std::size_t k = 0; k < d->sensors.size(); ++k) {
        const SensorSeries& ss = d->sensors[k];
        if (k) o += ',';
        jstr(o, SENSORS[ss.sensor].id);
        o += ':';
        jseries(o, ss.t, ss.v);
    }
    o += "}}";
    return true;
}

// Se manda en pedazos mientras se lee la base (sin Content-Length; Connection: close marca el final)
void WebServer::session_csv(int fd, int id) const {
    bool started = false, ok = true;
    const std::string extra = "Content-Disposition: attachment; filename=\"sesion_" + std::to_string(id) + ".csv\"\r\n";
    const bool exists = store_.export_csv(id, [&](const std::string& chunk) {
        if (!started) {
            started = true;
            ok = send_all(fd, head(200, "text/csv; charset=utf-8", -1, extra.c_str()));
        }
        ok = ok && send_all(fd, chunk);
        return ok;
    });
    if (!exists) not_found(fd, "no existe");
}

std::string WebServer::local_ip(const std::string& toward) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1);
    if (::inet_pton(AF_INET, toward.c_str(), &to.sin_addr) != 1) return "127.0.0.1";
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "127.0.0.1";
    std::string ip = "127.0.0.1";   // sin red hacia el ELM327
    sockaddr_in me{};
    socklen_t len = sizeof me;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr != 0) {
        char buf[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) ip = buf;
    }
    ::close(fd);
    return ip;
}
