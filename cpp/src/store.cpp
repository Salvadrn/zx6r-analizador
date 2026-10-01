#include "store.hpp"

#include "sensors.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <sys/stat.h>

namespace {

const char* const SCHEMA =
    "CREATE TABLE IF NOT EXISTS sessions (id INTEGER PRIMARY KEY, started TEXT NOT NULL, sim INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS samples (session INTEGER NOT NULL, t REAL NOT NULL, sensor TEXT NOT NULL, value REAL NOT NULL);"
    "CREATE INDEX IF NOT EXISTS samples_by_sensor ON samples(session, sensor, t);";

constexpr std::size_t MAX_PENDING = 200000;   // tope en memoria si la base deja de aceptar escrituras

// mkdir -p del directorio que contiene `path`
void make_parent_dirs(const std::string& path) {
    for (std::size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return s != nullptr; }
};

// Conexión de solo lectura para el servidor web
struct ReadDb {
    sqlite3* db = nullptr;
    explicit ReadDb(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            db = nullptr;
            return;
        }
        sqlite3_busy_timeout(db, 2000);
        sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);   // una sola foto aunque se siga escribiendo
    }
    ~ReadDb() {
        if (!db) return;
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    ReadDb(const ReadDb&) = delete;
    ReadDb& operator=(const ReadDb&) = delete;
};

std::string col_text(sqlite3_stmt* s, int i) {
    const unsigned char* p = sqlite3_column_text(s, i);
    return p ? reinterpret_cast<const char*>(p) : "";
}

std::string local_timestamp() {
    char buf[32];
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &lt);
    return buf;
}

}  // namespace

bool Store::open(bool sim) {
    make_parent_dirs(path_);
    auto fail = [this](const char* what) {
        std::printf("[DB] Sin registro (%s): %s\n", what, db_ ? sqlite3_errmsg(db_) : "sin memoria");
        std::fflush(stdout);
        sqlite3_finalize(insert_);
        insert_ = nullptr;
        sqlite3_close(db_);
        db_ = nullptr;
        session_ = -1;
        writing_ = false;
        return false;
    };
    if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK)
        return fail("abrir");
    sqlite3_busy_timeout(db_, 5000);
    if (sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail("WAL");
    if (sqlite3_exec(db_, SCHEMA, nullptr, nullptr, nullptr) != SQLITE_OK) return fail("esquema");
    {
        Stmt st(db_, "INSERT INTO sessions(started, sim) VALUES(?, ?)");
        if (!st) return fail("sesión");
        const std::string started = local_timestamp();
        sqlite3_bind_text(st.s, 1, started.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st.s, 2, sim ? 1 : 0);
        if (sqlite3_step(st.s) != SQLITE_DONE) return fail("sesión");
    }
    session_ = static_cast<int>(sqlite3_last_insert_rowid(db_));
    if (sqlite3_prepare_v2(db_, "INSERT INTO samples(session, t, sensor, value) VALUES(?, ?, ?, ?)", -1, &insert_,
                           nullptr) != SQLITE_OK)
        return fail("insert");
    writing_ = true;
    stop_ = false;
    th_ = std::thread(&Store::writer_loop, this);
    return true;
}

void Store::add(double t, const char* sensor, double v) {
    if (session_ < 0 || !std::isfinite(v)) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.size() < MAX_PENDING) pending_.push_back({t, sensor, v});
}

// Cada 1 s guarda en lote lo pendiente dentro de una transacción
void Store::writer_loop() {
    std::vector<Sample> batch;
    for (;;) {
        bool last;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::seconds(1), [this] { return stop_; });
            last = stop_;
            batch.swap(pending_);
        }
        if (!batch.empty()) {
            const bool ok = write_batch(batch);
            if (ok != writing_) {
                if (ok) std::printf("[DB] Guardando otra vez\n");
                else std::printf("[DB] No se pudo guardar: %s\n", sqlite3_errmsg(db_));
                std::fflush(stdout);
            }
            writing_ = ok;
            if (!ok && !last) {   // se reintenta en el siguiente lote
                std::lock_guard<std::mutex> lk(mu_);
                if (pending_.size() + batch.size() <= MAX_PENDING)
                    pending_.insert(pending_.begin(), batch.begin(), batch.end());
            }
            batch.clear();
        }
        if (last) break;
    }
}

bool Store::write_batch(const std::vector<Sample>& batch) {
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    const int sid = session_;
    for (const Sample& s : batch) {
        sqlite3_bind_int(insert_, 1, sid);
        sqlite3_bind_double(insert_, 2, s.t);
        sqlite3_bind_text(insert_, 3, s.sensor, -1, SQLITE_STATIC);
        sqlite3_bind_double(insert_, 4, s.v);
        const int rc = sqlite3_step(insert_);
        sqlite3_reset(insert_);
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

void Store::close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    sqlite3_finalize(insert_);
    insert_ = nullptr;
    sqlite3_close(db_);
    db_ = nullptr;
    writing_ = false;
}

std::vector<SessionRow> Store::sessions(int limit) const {
    std::vector<SessionRow> out;
    ReadDb r(path_);
    if (!r.db) return out;
    Stmt st(r.db,
            "SELECT id, started, sim,"
            " COALESCE((SELECT MAX(t) FROM samples WHERE session = s.id), 0),"
            " (SELECT COUNT(*) FROM samples WHERE session = s.id)"
            " FROM sessions s ORDER BY id DESC LIMIT ?");
    if (!st) return out;
    sqlite3_bind_int(st.s, 1, limit);
    while (sqlite3_step(st.s) == SQLITE_ROW) {
        SessionRow row;
        row.id = sqlite3_column_int(st.s, 0);
        row.started = col_text(st.s, 1);
        row.sim = sqlite3_column_int(st.s, 2) != 0;
        row.duration = sqlite3_column_double(st.s, 3);
        row.samples = sqlite3_column_int64(st.s, 4);
        out.push_back(std::move(row));
    }
    return out;
}

std::optional<SessionDetail> Store::session_detail(int id, std::size_t max_points) const {
    ReadDb r(path_);
    if (!r.db) return std::nullopt;
    SessionDetail d;
    {
        Stmt st(r.db, "SELECT id, started, sim FROM sessions WHERE id = ?");
        if (!st) return std::nullopt;
        sqlite3_bind_int(st.s, 1, id);
        if (sqlite3_step(st.s) != SQLITE_ROW) return std::nullopt;
        d.row.id = sqlite3_column_int(st.s, 0);
        d.row.started = col_text(st.s, 1);
        d.row.sim = sqlite3_column_int(st.s, 2) != 0;
    }
    Stmt stats(r.db,
               "SELECT COUNT(*), MIN(value), MAX(value), AVG(value), MAX(t) FROM samples WHERE session = ? AND sensor = ?");
    Stmt pts(r.db, "SELECT t, value FROM samples WHERE session = ? AND sensor = ? ORDER BY t");
    if (!stats || !pts) return std::nullopt;

    const long long buckets = static_cast<long long>(std::max<std::size_t>(max_points / 2, 1));
    for (std::size_t i = 0; i < SENSORS.size(); ++i) {   // los sensores desconocidos se ignoran
        sqlite3_bind_int(stats.s, 1, id);
        sqlite3_bind_text(stats.s, 2, SENSORS[i].id, -1, SQLITE_STATIC);
        SensorSeries ss;
        ss.sensor = i;
        if (sqlite3_step(stats.s) == SQLITE_ROW) {
            ss.n = sqlite3_column_int64(stats.s, 0);
            ss.min = sqlite3_column_double(stats.s, 1);
            ss.max = sqlite3_column_double(stats.s, 2);
            ss.avg = sqlite3_column_double(stats.s, 3);
            if (ss.n > 0) d.row.duration = std::max(d.row.duration, sqlite3_column_double(stats.s, 4));
        }
        sqlite3_reset(stats.s);
        if (ss.n <= 0) continue;

        sqlite3_bind_int(pts.s, 1, id);
        sqlite3_bind_text(pts.s, 2, SENSORS[i].id, -1, SQLITE_STATIC);
        const bool reduce = ss.n > static_cast<long long>(max_points);
        // Reducción por tramos: de cada tramo se queda el mínimo y el máximo, en orden de t
        struct P { double t, v; long long k; } lo{}, hi{};
        long long k = 0, cur = -1;
        auto emit = [&] {
            const P& a = lo.k <= hi.k ? lo : hi;
            const P& b = lo.k <= hi.k ? hi : lo;
            ss.t.push_back(a.t);
            ss.v.push_back(a.v);
            if (b.k != a.k) {
                ss.t.push_back(b.t);
                ss.v.push_back(b.v);
            }
        };
        while (sqlite3_step(pts.s) == SQLITE_ROW) {
            const double t = sqlite3_column_double(pts.s, 0), v = sqlite3_column_double(pts.s, 1);
            if (!std::isfinite(t) || !std::isfinite(v)) continue;
            if (!reduce) {
                ss.t.push_back(t);
                ss.v.push_back(v);
                continue;
            }
            const long long b = std::min(k * buckets / ss.n, buckets - 1);
            if (b != cur) {
                if (cur >= 0) emit();
                cur = b;
                lo = hi = {t, v, k};
            } else {
                if (v < lo.v) lo = {t, v, k};
                if (v > hi.v) hi = {t, v, k};
            }
            ++k;
        }
        if (reduce && cur >= 0) emit();
        sqlite3_reset(pts.s);
        d.sensors.push_back(std::move(ss));
    }
    return d;
}

bool Store::export_csv(int id, const std::function<bool(const std::string&)>& out) const {
    ReadDb r(path_);
    if (!r.db) return false;
    {
        Stmt chk(r.db, "SELECT 1 FROM sessions WHERE id = ?");
        if (!chk) return false;
        sqlite3_bind_int(chk.s, 1, id);
        if (sqlite3_step(chk.s) != SQLITE_ROW) return false;
    }
    Stmt st(r.db, "SELECT t, sensor, value FROM samples WHERE session = ? ORDER BY t, rowid");
    std::string buf = "t,sensor,value\n";
    if (st) {
        sqlite3_bind_int(st.s, 1, id);
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const double t = sqlite3_column_double(st.s, 0), v = sqlite3_column_double(st.s, 2);
            std::string sensor = col_text(st.s, 1);
            if (sensor.find_first_of(",\"\r\n") != std::string::npos) {   // comillas solo si hacen falta
                std::string q = "\"";
                for (char ch : sensor) q += ch == '"' ? std::string("\"\"") : std::string(1, ch);
                sensor = q + "\"";
            }
            char num[64];
            std::snprintf(num, sizeof num, "%.3f,", t);
            buf += num;
            buf += sensor;
            std::snprintf(num, sizeof num, ",%.6g\n", v);
            buf += num;
            if (buf.size() >= 64 * 1024) {
                if (!out(buf)) return true;
                buf.clear();
            }
        }
    }
    if (!buf.empty()) out(buf);
    return true;
}
