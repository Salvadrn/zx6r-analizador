#include "ui.hpp"

#include "obd.hpp"
#include "store.hpp"
#include "web.hpp"

#include <raylib.h>
#include <rlgl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace {

constexpr Color hex(unsigned v) {
    return Color{static_cast<unsigned char>((v >> 16) & 0xFF), static_cast<unsigned char>((v >> 8) & 0xFF),
                 static_cast<unsigned char>(v & 0xFF), 255};
}

// =====================================================
// COLORES — negro puro y grises; el color solo marca estados (advertencia / crítico)
// =====================================================
constexpr Color BG = hex(0x000000), BORDER = hex(0x262626), GRID = hex(0x161616), AXIS = hex(0x333333),
                TEXT = hex(0xF0F0F0), TEXT2 = hex(0x9A9A9A), TEXT3 = hex(0x666666), STALE = hex(0x444444),
                OK = hex(0x6FB583), WARN = hex(0xD6A24A), CRIT = hex(0xD65C57),
                REF_LINE = hex(0x3A3A3A), REF_TEXT = hex(0x7A7A7A);

bool same(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

const char* const CHIP_TEXTS[] = {"SIMULADOR", "CONECTANDO", "OBD2 EN VIVO", "SIN CONEXIÓN", "SIN DATOS ECU"};
const int TICK_STEPS[] = {1, 2, 5, 10, 15, 20, 30, 60, 120, 300};

// Espaciado entre letras para rótulos en mayúsculas (como la versión Python: un espacio entre letras)
std::string track(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) o += ' ';
        o += s[i];
    }
    return o;
}

std::string tlabel(int t) {
    if (t < 60) return "-" + std::to_string(t) + "s";
    if (t % 60 == 0) return "-" + std::to_string(t / 60) + "m";
    char buf[16];
    std::snprintf(buf, sizeof buf, "-%d:%02d", t / 60, t % 60);
    return buf;
}

// Cruces rico/pobre de la O2 en los últimos `span` s (con histéresis)
int crossings(const std::vector<double>& ts, const std::vector<double>& vs, double now, double span = 10.0,
              double hi = 0.5, double lo = 0.4) {
    int n = 0, state = 0;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (ts[i] < now - span) continue;
        const int cur = vs[i] > hi ? 1 : vs[i] < lo ? -1 : 0;
        if (cur && state && cur != state) ++n;
        if (cur) state = cur;
    }
    return n;
}

Color zone_color(const Sensor& s, double v) {
    if ((s.crit && v >= *s.crit) || (s.crit_lo && v <= *s.crit_lo)) return CRIT;
    if ((s.warn && v >= *s.warn) || (s.warn_lo && v <= *s.warn_lo)) return WARN;
    return TEXT;
}

// Valor -> y en pantalla dentro de [y0, y1] (4 px de margen arriba y abajo)
float ymap(const Sensor& s, float y0, float y1, double v) {
    const double norm = (v - s.y_min) / (s.y_max - s.y_min);
    return static_cast<float>(std::max<double>(y0 + 2, std::min<double>(y1 - 2, y1 - 4 - norm * (y1 - y0 - 8))));
}

int value_chars(const Sensor& s) {
    return static_cast<int>(std::max(fmt_value(s, s.y_min).size(), fmt_value(s, s.y_max).size()));
}

struct Shown {
    std::string text;
    Color color;
    bool stale;
};

// Texto, color y si está obsoleto el valor actual
Shown shown_value(const Sensor& s, const SensorView& sv, double now) {
    if (!sv.last) return {sv.no_data ? "N/D" : "--", STALE, true};
    const bool stale = now - sv.last_t > STALE_S;
    return {fmt_value(s, *sv.last), stale ? STALE : zone_color(s, *sv.last), stale};
}

// =====================================================
// FUENTES — TTF/OTF (raylib no lee .ttc), cargadas al tamaño exacto en píxeles del framebuffer
// =====================================================
enum Face { SANS, SANS_BOLD, MONO, MONO_BOLD, FACE_COUNT };
enum class Anchor { W, E, C, SW, N, NE };

const char* const TERMINAL_FONTS[] = {
    "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/",
    "/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/",
};
const char* const DEJAVU = "/usr/share/fonts/truetype/dejavu/";

std::vector<std::string> font_candidates(Face f, const Config& cfg) {
    std::vector<std::string> c;
    const std::string& env = (f == SANS || f == SANS_BOLD) ? cfg.font_sans : cfg.font_mono;
    if (!env.empty()) c.push_back(env);
    const std::string dv = DEJAVU;
    switch (f) {
        case SANS_BOLD:
            c.insert(c.end(), {"/System/Library/Fonts/Supplemental/Arial Bold.ttf", dv + "DejaVuSans-Bold.ttf"});
            break;
        case SANS:
            c.insert(c.end(), {"/System/Library/Fonts/Supplemental/Arial.ttf", dv + "DejaVuSans.ttf"});
            break;
        case MONO_BOLD:
            // SFNSMono.ttf es variable y raylib solo ve su instancia por defecto (Light):
            // primero las OTF estáticas de SF Mono que trae Terminal.app
            for (const char* d : TERMINAL_FONTS) c.push_back(std::string(d) + "SF-Mono-Bold.otf");
            c.insert(c.end(), {"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
                               dv + "DejaVuSansMono-Bold.ttf"});
            break;
        case MONO:
            for (const char* d : TERMINAL_FONTS) c.push_back(std::string(d) + "SF-Mono-Regular.otf");
            c.insert(c.end(), {"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
                               dv + "DejaVuSansMono.ttf", dv + "DejaVuSansMono-Bold.ttf"});
            break;
        default:
            break;
    }
    return c;
}

// (ascender − descender) / unitsPerEm (tablas hhea y head). raylib toma ascender−descender como el
// tamaño de la letra; con este factor el tamaño en px es el "em", igual que en Tk y CSS.
float em_ratio(const std::vector<unsigned char>& d) {
    auto u16 = [&](std::size_t o) -> unsigned { return o + 2 <= d.size() ? (d[o] << 8u) | d[o + 1] : 0u; };
    auto u32 = [&](std::size_t o) -> std::size_t { return (static_cast<std::size_t>(u16(o)) << 16u) | u16(o + 2); };
    std::size_t head = 0, hhea = 0;
    const unsigned tables = u16(4);
    for (unsigned i = 0; i < tables; ++i) {
        const std::size_t r = 12 + 16 * static_cast<std::size_t>(i);
        if (r + 16 > d.size()) break;
        const std::string tag(reinterpret_cast<const char*>(&d[r]), 4);
        if (tag == "head") head = u32(r + 8);
        if (tag == "hhea") hhea = u32(r + 8);
    }
    if (!head || !hhea) return 1.0f;
    const unsigned upem = u16(head + 18);
    const int asc = static_cast<int16_t>(u16(hhea + 4)), desc = static_cast<int16_t>(u16(hhea + 6));
    if (upem == 0 || asc - desc <= 0) return 1.0f;
    return static_cast<float>(asc - desc) / static_cast<float>(upem);
}

class Fonts {
public:
    struct Use {
        Font font;
        float size;      // tamaño para DrawTextEx (alto de la caja de la letra)
        float spacing;
    };

    void load(const Config& cfg) {
        static const char* const names[] = {"sans", "sans bold", "mono", "mono bold"};
        for (int f = 0; f < FACE_COUNT; ++f) {
            for (const std::string& p : font_candidates(static_cast<Face>(f), cfg)) {
                std::ifstream in(p, std::ios::binary);
                if (!in) continue;
                std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (data.size() < 64) continue;
                files_[f].k = em_ratio(data);
                files_[f].path = p;
                files_[f].data = std::move(data);
                break;
            }
            if (files_[f].data.empty())
                std::printf("[UI] Sin fuente TTF para %s: se usa la de raylib\n", names[f]);
        }
        for (int c = 32; c < 127; ++c) full_.push_back(c);
        // á é í ó ú Á É Í Ó Ú ñ Ñ ° λ · — ← →
        for (int c : {0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xF1, 0xD1, 0xB0, 0x3BB, 0xB7,
                      0x2014, 0x2190, 0x2192})
            full_.push_back(c);
        for (char c : std::string(" -./0123456789DN")) numeric_.push_back(c);   // valores grandes: "--", "N/D"
    }

    void set_dpi(float dpi) {
        if (std::fabs(dpi - dpi_) < 0.01f) return;
        clear();
        dpi_ = dpi;
    }

    // px = tamaño "em" en px de pantalla (nunca menos de 9); numeric = solo los glifos de los valores
    Use get(Face f, float px_in, bool numeric) {
        const int px = std::max(9, static_cast<int>(px_in));
        const File& file = files_[f];
        if (file.data.empty()) return {GetFontDefault(), static_cast<float>(px), px / 10.0f};
        const int phys = std::max(1, static_cast<int>(std::lround(px * file.k * dpi_)));
        const long key = (static_cast<long>(f) * 2 + (numeric ? 1 : 0)) * 100000L + phys;
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            std::vector<int>& cps = numeric ? numeric_ : full_;
            const bool otf = file.path.size() > 4 && (file.path.compare(file.path.size() - 4, 4, ".otf") == 0 ||
                                                      file.path.compare(file.path.size() - 4, 4, ".OTF") == 0);
            Font font = LoadFontFromMemory(otf ? ".otf" : ".ttf", file.data.data(), static_cast<int>(file.data.size()),
                                           phys, cps.data(), static_cast<int>(cps.size()));
            const bool owned = font.texture.id != GetFontDefault().texture.id;
            if (owned) SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            it = cache_.emplace(key, Entry{font, owned}).first;
        }
        if (!it->second.owned) return {it->second.font, static_cast<float>(px), px / 10.0f};
        return {it->second.font, phys / dpi_, 0.0f};   // 1 píxel del atlas = 1 píxel del framebuffer
    }

    void clear() {
        for (auto& kv : cache_)
            if (kv.second.owned) UnloadFont(kv.second.font);
        cache_.clear();
    }

private:
    struct File {
        std::string path;
        std::vector<unsigned char> data;
        float k = 1.0f;
    };
    struct Entry {
        Font font;
        bool owned;
    };
    std::array<File, FACE_COUNT> files_;
    std::map<long, Entry> cache_;
    std::vector<int> full_, numeric_;
    float dpi_ = 1.0f;
};

// =====================================================
// PRIMITIVAS — coordenadas lógicas; las rectas se alinean a píxeles del framebuffer
// =====================================================
class Canvas {
public:
    Fonts fonts;
    float dpi = 1.0f;

    float snap(float v) const { return std::round(v * dpi) / dpi; }
    float hair() const { return std::max(1.0f, std::round(dpi)) / dpi; }   // línea de 1 px

    float width(Face f, float px, const std::string& s, bool numeric = false) {
        const Fonts::Use u = fonts.get(f, px, numeric);
        return MeasureTextEx(u.font, s.c_str(), u.size, u.spacing).x;
    }

    // Anclas como en Tk: W/E = centro vertical a la izquierda/derecha, SW = abajo a la izquierda,
    // N/NE = arriba al centro/derecha, C = centro
    void text(Face f, float px, const std::string& s, float x, float y, Anchor a, Color c, bool numeric = false) {
        if (s.empty()) return;
        const Fonts::Use u = fonts.get(f, px, numeric);
        const float w = MeasureTextEx(u.font, s.c_str(), u.size, u.spacing).x;
        float x0 = x, y0 = y - u.size / 2;
        switch (a) {
            case Anchor::W: break;
            case Anchor::E: x0 = x - w; break;
            case Anchor::C: x0 = x - w / 2; break;
            case Anchor::SW: y0 = y - u.size; break;
            case Anchor::N: x0 = x - w / 2; y0 = y; break;
            case Anchor::NE: x0 = x - w; y0 = y; break;
        }
        DrawTextEx(u.font, s.c_str(), Vector2{snap(x0), snap(y0)}, u.size, u.spacing, c);
    }

    void rect(float x0, float y0, float x1, float y1, Color c) {
        const float a = snap(x0), b = snap(y0);
        DrawRectangleRec(Rectangle{a, b, snap(x1) - a, snap(y1) - b}, c);
    }

    void hline(float x0, float x1, float y, Color c) {
        const float yy = snap(y);
        rect(x0, yy, x1, yy + hair(), c);
    }

    void vline(float x, float y0, float y1, Color c) {
        const float xx = snap(x);
        rect(xx, y0, xx + hair(), y1, c);
    }

    void dashed_hline(float x0, float x1, float y, float on, float off, Color c) {
        for (float x = x0; x < x1; x += on + off) hline(x, std::min(x + on, x1), y, c);
    }

    // Contorno de 1 px por dentro del rectángulo
    void frame(float x0, float y0, float x1, float y1, Color c) {
        const float h = hair();
        hline(x0, x1, y0, c);
        hline(x0, x1, snap(y1) - h, c);
        vline(x0, y0, y1, c);
        vline(snap(x1) - h, y0, y1, c);
    }

    // Rectángulo redondeado: solo contorno de 1 px por dentro
    void rrect(float x0, float y0, float x1, float y1, float r, Color c) {
        x0 = snap(x0);
        y0 = snap(y0);
        x1 = snap(x1);
        y1 = snap(y1);
        const float lw = hair();
        r = std::max(lw, std::min(r, std::min(x1 - x0, y1 - y0) / 2));
        const int seg = std::clamp(static_cast<int>(std::ceil(r * dpi / 2)), 3, 24);
        const float cx[4] = {x0 + r, x1 - r, x1 - r, x0 + r}, cy[4] = {y0 + r, y0 + r, y1 - r, y1 - r};
        const float a0[4] = {PI, 1.5f * PI, 0.0f, 0.5f * PI};
        outer_.clear();
        inner_.clear();
        for (int k = 0; k < 4; ++k) {
            for (int i = 0; i <= seg; ++i) {
                const float a = a0[k] + 0.5f * PI * static_cast<float>(i) / static_cast<float>(seg);
                const float ca = std::cos(a), sa = std::sin(a);
                outer_.push_back({cx[k] + ca * r, cy[k] + sa * r});
                inner_.push_back({cx[k] + ca * (r - lw), cy[k] + sa * (r - lw)});
            }
        }
        rlBegin(RL_TRIANGLES);
        rlColor4ub(c.r, c.g, c.b, c.a);
        for (std::size_t i = 0; i < outer_.size(); ++i) {
            const std::size_t j = (i + 1) % outer_.size();
            quad(outer_[i], inner_[i], outer_[j], inner_[j]);
        }
        rlEnd();
    }

    // Línea gruesa: en vueltas suaves los segmentos comparten esquina (inglete); en vueltas cerradas
    // (picos de la O2) cada segmento termina recto y la unión se redondea, como las líneas de Tk
    void polyline(const std::vector<Vector2>& in, float thick, Color c) {
        pts_.clear();
        for (const Vector2& p : in)
            if (pts_.empty() || std::fabs(p.x - pts_.back().x) + std::fabs(p.y - pts_.back().y) > 0.01f) pts_.push_back(p);
        const std::size_t n = pts_.size();
        if (n < 2) return;
        auto dir = [](Vector2 a, Vector2 b) {
            const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
            return Vector2{dx / l, dy / l};
        };
        const float hw = thick / 2;
        dirs_.resize(n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i) dirs_[i] = dir(pts_[i], pts_[i + 1]);
        left_.resize(n);
        right_.resize(n);
        sharp_.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const Vector2 d0 = dirs_[i > 0 ? i - 1 : 0];
            const Vector2 d1 = dirs_[i + 1 < n ? i : n - 2];
            const Vector2 t{d0.x + d1.x, d0.y + d1.y};
            const float tl = std::sqrt(t.x * t.x + t.y * t.y);
            const float half_cos = tl / 2;   // coseno de la mitad del ángulo de la vuelta
            if (half_cos < 0.5f) {           // vuelta de más de 120°
                sharp_[i] = 1;
                continue;
            }
            const Vector2 nrm{-t.y / tl, t.x / tl};
            const float m = hw / half_cos;
            left_[i] = {pts_[i].x + nrm.x * m, pts_[i].y + nrm.y * m};
            right_[i] = {pts_[i].x - nrm.x * m, pts_[i].y - nrm.y * m};
        }
        rlBegin(RL_TRIANGLES);
        rlColor4ub(c.r, c.g, c.b, c.a);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Vector2 nrm{-dirs_[i].y * hw, dirs_[i].x * hw};   // extremo recto del segmento
            const Vector2 l0 = sharp_[i] ? Vector2{pts_[i].x + nrm.x, pts_[i].y + nrm.y} : left_[i];
            const Vector2 r0 = sharp_[i] ? Vector2{pts_[i].x - nrm.x, pts_[i].y - nrm.y} : right_[i];
            const Vector2 l1 = sharp_[i + 1] ? Vector2{pts_[i + 1].x + nrm.x, pts_[i + 1].y + nrm.y} : left_[i + 1];
            const Vector2 r1 = sharp_[i + 1] ? Vector2{pts_[i + 1].x - nrm.x, pts_[i + 1].y - nrm.y} : right_[i + 1];
            quad(l0, r0, l1, r1);
        }
        for (std::size_t i = 0; i < n; ++i)   // unión redonda en las vueltas cerradas
            if (sharp_[i]) fan(pts_[i], hw);
        rlEnd();
    }

    void disc(float x, float y, float r, Color c) { DrawCircleV(Vector2{x, y}, r, c); }

    void triangle(Vector2 a, Vector2 b, Vector2 c, Color col) { DrawTriangle(a, b, c, col); }

private:
    // Dos triángulos: (a, b) un lado, (c, d) el otro
    static void quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d) {
        rlVertex2f(a.x, a.y);
        rlVertex2f(b.x, b.y);
        rlVertex2f(c.x, c.y);
        rlVertex2f(c.x, c.y);
        rlVertex2f(b.x, b.y);
        rlVertex2f(d.x, d.y);
    }

    // Círculo chico (dentro de un rlBegin(RL_TRIANGLES) ya abierto)
    static void fan(Vector2 p, float r) {
        constexpr int SEG = 10;
        for (int k = 0; k < SEG; ++k) {
            const float a0 = 2 * PI * static_cast<float>(k) / SEG, a1 = 2 * PI * static_cast<float>(k + 1) / SEG;
            rlVertex2f(p.x, p.y);
            rlVertex2f(p.x + std::cos(a0) * r, p.y + std::sin(a0) * r);
            rlVertex2f(p.x + std::cos(a1) * r, p.y + std::sin(a1) * r);
        }
    }

    std::vector<Vector2> outer_, inner_, pts_, left_, right_, dirs_;
    std::vector<char> sharp_;
};

// =====================================================
// TABLERO
// =====================================================
class Dash {
public:
    Dash(const Config& cfg, const Telemetry& tel, const Store& store, const WebServer& web)
        : cfg_(cfg), tel_(tel), store_(store), web_(web) {
        for (const View& v : VIEWS)
            if (!view_sensors(v).empty()) views_.push_back(&v);
    }

    bool run(const std::atomic<bool>& quit);

private:
    float S(float v) const { return std::max(1.0f, std::round(v * u_)); }

    std::vector<std::size_t> view_sensors(const View& v) const {
        std::vector<std::size_t> ids;
        for (const char* id : v.sensors) {
            const int i = sensor_index(id);
            if (i >= 0 && cfg_.active[static_cast<std::size_t>(i)]) ids.push_back(static_cast<std::size_t>(i));
        }
        return ids;
    }

    std::pair<const char*, Color> status(const Snapshot& snap) const;
    void draw(const Snapshot& snap);
    void draw_chrome(const Snapshot& snap);
    void draw_tiles(const View& v, const std::vector<std::size_t>& ids, const Snapshot& snap, float x0, float y0,
                    float x1, float y1);
    void draw_tile(const Sensor& s, const SensorView& sv, double now, float x, float y, float w, float h, double window);
    void draw_graphs(const View& v, const std::vector<std::size_t>& ids, const Snapshot& snap, float x0, float y0,
                     float x1, float y1);
    void draw_panel(const Sensor& s, const SensorView& sv, double now, float x, float y, float w, float h, float iw,
                    float px0, float px1, double window, const std::vector<int>& ticks);
    void trace(const Sensor& s, const SensorView& sv, double now, double window, float x0, float y0, float x1,
               float y1, std::vector<double>* vals);

    const Config& cfg_;
    const Telemetry& tel_;
    const Store& store_;
    const WebServer& web_;
    Canvas cv_;
    std::vector<const View*> views_;
    int vi_ = 0;
    float W_ = 0, H_ = 0, u_ = 1, m_ = 0, top_h_ = 0, bot_h_ = 0;
    std::string ip_;
    double ip_at_ = -1e9;
    std::vector<Vector2> pts_;
};

std::pair<const char*, Color> Dash::status(const Snapshot& snap) const {
    switch (snap.state) {
        case LinkState::Sim:
            return {"SIMULADOR", WARN};
        case LinkState::Live: {
            bool any = false;
            for (std::size_t i = 0; i < SENSOR_COUNT; ++i) any = any || (cfg_.active[i] && snap.sensors[i].last);
            if (snap.now - snap.since > 5 && !any) return {"SIN DATOS ECU", WARN};
            return {"OBD2 EN VIVO", OK};
        }
        case LinkState::Connecting:
            return {"CONECTANDO", WARN};
        case LinkState::Lost:
            return {"SIN CONEXIÓN", CRIT};
    }
    return {"CONECTANDO", WARN};
}

void Dash::draw(const Snapshot& snap) {
    draw_chrome(snap);
    const View& v = *views_[static_cast<std::size_t>(vi_)];
    const auto ids = view_sensors(v);
    const float x0 = m_, y0 = top_h_ + S(16), x1 = W_ - m_, y1 = H_ - bot_h_ - S(10);
    if (v.kind == ViewKind::Tiles) draw_tiles(v, ids, snap, x0, y0, x1, y1);
    else draw_graphs(v, ids, snap, x0, y0, x1, y1);
}

void Dash::draw_chrome(const Snapshot& snap) {
    const float mid = std::floor(top_h_ / 2);
    const auto st = status(snap);
    const bool linked = snap.state == LinkState::Live || snap.state == LinkState::Sim;

    // derecha: estado del enlace, tasa de datos, reloj
    float xr = W_ - m_;
    char clock[16];
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    std::strftime(clock, sizeof clock, "%H:%M:%S", &lt);
    cv_.text(MONO_BOLD, S(16), clock, xr, mid, Anchor::E, TEXT2);
    xr -= cv_.width(MONO_BOLD, S(16), "00:00:00") + S(24);
    char hz[32];
    std::snprintf(hz, sizeof hz, "%4.1f Hz", snap.hz);
    cv_.text(MONO, S(13), hz, xr, mid, Anchor::E, linked ? TEXT2 : STALE);
    xr -= cv_.width(MONO, S(13), "00.0 Hz") + S(24);
    float chip_w = 0;
    for (const char* t : CHIP_TEXTS) chip_w = std::max(chip_w, cv_.width(SANS_BOLD, S(12), t));
    chip_w += S(52);
    const float cx0 = xr - chip_w;
    cv_.rrect(cx0, mid - S(15), xr, mid + S(15), S(15), BORDER);
    const bool blink = (snap.state == LinkState::Lost || snap.state == LinkState::Connecting) &&
                       static_cast<long>(snap.now * 2) % 2 == 0;
    if (!blink) cv_.disc(cx0 + S(19), mid, S(5), st.second);
    cv_.text(SANS_BOLD, S(12), st.first, cx0 + S(34), mid, Anchor::W, TEXT);
    const float right_start = cx0 - S(30);

    // izquierda: marca y pestañas; si no caben se acorta (sin subtítulo, nombres cortos)
    const float brand_w = cv_.width(SANS_BOLD, S(24), "ZX-6R");
    static const std::string sub = track("TELEMETRÍA");
    const float sub_w = cv_.width(SANS_BOLD, S(11), sub);
    const float gap = S(30);
    auto tabs_w = [&](bool shorts) {
        float w = gap * static_cast<float>(views_.size() - 1);
        for (const View* v : views_) w += cv_.width(SANS_BOLD, S(14), shorts ? v->short_name : v->name);
        return w;
    };
    bool with_sub = true, shorts = false;
    for (int k = 0; k < 3; ++k) {
        with_sub = k == 0;
        shorts = k == 2;
        const float need = m_ + brand_w + (with_sub ? S(32) + sub_w : 0) + S(40) + tabs_w(shorts);
        if (need <= right_start) break;
    }
    cv_.text(SANS_BOLD, S(24), "ZX-6R", m_, mid, Anchor::W, TEXT);
    float x = m_ + brand_w;
    if (with_sub) {
        cv_.vline(x + S(16), mid - S(11), mid + S(11), AXIS);
        cv_.text(SANS_BOLD, S(11), sub, x + S(32), mid, Anchor::W, TEXT3);
        x += S(32) + sub_w;
    }
    x += S(40);
    for (std::size_t i = 0; i < views_.size(); ++i) {
        const char* name = shorts ? views_[i]->short_name : views_[i]->name;
        const float w = cv_.width(SANS_BOLD, S(14), name);
        const bool active = static_cast<int>(i) == vi_;
        cv_.text(SANS_BOLD, S(14), name, x, mid, Anchor::W, active ? TEXT : TEXT3);
        if (active) cv_.rect(x, top_h_ - S(7), x + w, top_h_ - S(4), TEXT);
        x += w + gap;
    }
    const bool no_ecu = snap.state == LinkState::Live && same(st.second, WARN);
    cv_.rect(0, top_h_, W_, top_h_ + S(1), snap.state == LinkState::Lost ? CRIT : no_ecu ? WARN : AXIS);

    // abajo: ayuda del botón, contador de vistas, registro y visor web
    const float by = H_ - std::floor(bot_h_ / 2);
    cv_.triangle(Vector2{m_, by - S(6)}, Vector2{m_, by + S(6)}, Vector2{m_ + S(10), by}, TEXT2);
    static const std::string hint = track("CAMBIAR VISTA");
    cv_.text(SANS_BOLD, S(11), hint, m_ + S(20), by, Anchor::W, TEXT2);
    const float xh = m_ + S(20) + cv_.width(SANS_BOLD, S(11), hint) + S(18);
    cv_.text(MONO, S(12), std::to_string(vi_ + 1) + " / " + std::to_string(views_.size()), xh, by, Anchor::W, TEXT3);

    const bool rec_ok = store_.writing() && store_.session() >= 0;   // la base abrió y está guardando
    std::string rec = rec_ok ? "REC · SESIÓN " + std::to_string(store_.session()) : "SIN REGISTRO";
    if (web_.listening()) rec += "   WEB " + ip_ + ":" + std::to_string(web_.port());
    cv_.text(MONO, S(11), rec, W_ - m_, by, Anchor::E, rec_ok ? TEXT3 : WARN);
    cv_.disc(W_ - m_ - cv_.width(MONO, S(11), rec) - S(14), by, S(4), rec_ok ? CRIT : WARN);
}

void Dash::draw_tiles(const View& v, const std::vector<std::size_t>& ids, const Snapshot& snap, float x0, float y0,
                      float x1, float y1) {
    const int n = static_cast<int>(ids.size());
    if (n == 0) return;
    const int cols = n == 1 ? 1 : n <= 4 ? 2 : 3;
    const int rows = (n + cols - 1) / cols;
    const float gap = S(14);
    const float th = (y1 - y0 - (rows - 1) * gap) / rows;
    for (int i = 0; i < n; ++i) {
        const int r = i / cols, c = i % cols;
        const int in_row = r == rows - 1 ? n - r * cols : cols;   // la última fila reparte el ancho
        const float tw = (x1 - x0 - (in_row - 1) * gap) / in_row;
        const std::size_t id = ids[static_cast<std::size_t>(i)];
        draw_tile(SENSORS[id], snap.sensors[id], snap.now, x0 + c * (tw + gap), y0 + r * (th + gap), tw, th, v.window);
    }
}

void Dash::draw_tile(const Sensor& s, const SensorView& sv, double now, float x, float y, float w, float h,
                     double window) {
    cv_.rrect(x, y, x + w, y + h, S(14), BORDER);
    const float pad = S(26);
    const std::string name = track(s.name);
    cv_.text(SANS_BOLD, S(16), name, x + pad, y + S(30), Anchor::W, TEXT2);
    const float name_end = x + pad + cv_.width(SANS_BOLD, S(16), name);
    if (x + w - S(22) - cv_.width(SANS, S(12), s.desc) > name_end + S(12))   // solo si no se enciman
        cv_.text(SANS, S(12), s.desc, x + w - S(22), y + S(30), Anchor::E, TEXT3);

    const int vchars = value_chars(s);
    const float vpx = std::floor(std::min(h * 0.36f, w * 0.5f / (vchars * 0.62f)));
    const float xr = x + pad + cv_.width(MONO_BOLD, vpx, "0", true) * vchars;
    const float vy = y + h * 0.46f;
    const Shown val = shown_value(s, sv, now);
    cv_.text(MONO_BOLD, vpx, val.text, xr, vy, Anchor::E, val.color, true);
    cv_.text(SANS, std::max(S(14), vpx * 0.3f), s.unit, xr + S(10), vy + vpx * 0.30f, Anchor::SW, TEXT2);

    // mini gráfica con la ventana de la vista
    const float bx0 = x + pad, by0 = y + h * 0.66f, bx1 = x + w - S(22), by1 = y + h - S(22);
    cv_.hline(bx0, bx1, by1, AXIS);
    cv_.hline(bx0, bx1, by0, GRID);
    trace(s, sv, now, window, bx0, by0, bx1, by1, nullptr);
    cv_.polyline(pts_, std::max(2.0f, S(2)), val.stale ? STALE : TEXT);
}

void Dash::draw_graphs(const View& v, const std::vector<std::size_t>& ids, const Snapshot& snap, float x0, float y0,
                       float x1, float y1) {
    const int n = static_cast<int>(ids.size());
    if (n == 0) return;
    const float gap = S(14), axis_h = S(24);
    const float ph = (y1 - y0 - axis_h - (n - 1) * gap) / n;
    const float iw = std::max(S(190), std::min(S(330), (x1 - x0) * 0.25f));
    const float px0 = x0 + iw + S(58), px1 = x1 - S(20);
    const int window = static_cast<int>(v.window);
    int step = 300;
    for (int t : TICK_STEPS) {
        if (window / static_cast<double>(t) <= 8) {
            step = t;
            break;
        }
    }
    std::vector<int> ticks;
    for (int k = 0; k <= window / step; ++k) ticks.push_back(k * step);
    for (int i = 0; i < n; ++i) {
        const std::size_t id = ids[static_cast<std::size_t>(i)];
        draw_panel(SENSORS[id], snap.sensors[id], snap.now, x0, y0 + i * (ph + gap), x1 - x0, ph, iw, px0, px1,
                   v.window, ticks);
    }
    const float yl = y0 + n * ph + (n - 1) * gap + S(8);
    for (int t : ticks) {
        const float x = px1 - static_cast<float>(t / v.window) * (px1 - px0);
        cv_.text(MONO, S(11), t == 0 ? "ahora" : tlabel(t), x, yl, t == 0 ? Anchor::NE : Anchor::N, TEXT3);
    }
}

void Dash::draw_panel(const Sensor& s, const SensorView& sv, double now, float x, float y, float w, float h, float iw,
                      float px0, float px1, double window, const std::vector<int>& ticks) {
    cv_.rrect(x, y, x + w, y + h, S(14), BORDER);
    const float pad = S(26);
    cv_.text(SANS_BOLD, S(16), track(s.name), x + pad, y + S(30), Anchor::W, TEXT2);
    cv_.text(SANS, S(12), s.desc, x + pad, y + S(50), Anchor::W, TEXT3);

    // columna de datos: valor grande y, si cabe, estadísticas de la ventana
    const bool big = h >= S(230);
    const bool is_o2 = std::strcmp(s.id, "o2") == 0;
    const int rows = is_o2 ? 4 : 3;
    const int vchars = value_chars(s);
    float vpx, vy;
    if (big) {
        vpx = std::min(S(104), (h - S(62) - rows * S(24) - S(26)) * 0.62f);
        const float top = y + S(72), bottom = y + h - S(20) - rows * S(24);
        vy = (top + bottom) / 2;   // centrado entre el título y las estadísticas
    } else {
        vpx = std::min(S(54), h * 0.34f);
        vy = y + h * 0.64f;
    }
    vpx = std::floor(std::min(vpx, (iw - pad - S(50)) / (vchars * 0.62f)));
    const float xr = x + pad + cv_.width(MONO_BOLD, vpx, "0", true) * vchars;
    const Shown val = shown_value(s, sv, now);
    cv_.text(MONO_BOLD, vpx, val.text, xr, vy, Anchor::E, val.color, true);
    cv_.text(SANS, std::max(S(14), vpx * 0.3f), s.unit, xr + S(8), vy + vpx * 0.30f, Anchor::SW, TEXT2);

    const float py0 = y + S(16), py1 = y + h - S(16);
    std::vector<double> vals;
    trace(s, sv, now, window, px0, py0, px1, py1, &vals);
    if (big) {
        const char* const labels[] = {"MIN", "MAX", "PROM", "CRUCES λ / 10 s"};
        std::string values[4] = {"--", "--", "--", std::to_string(crossings(sv.t, sv.v, now))};
        if (!vals.empty()) {
            double sum = 0;
            for (double v : vals) sum += v;
            values[0] = fmt_value(s, *std::min_element(vals.begin(), vals.end()));
            values[1] = fmt_value(s, *std::max_element(vals.begin(), vals.end()));
            values[2] = fmt_value(s, sum / static_cast<double>(vals.size()));
        }
        for (int k = 0; k < rows; ++k) {
            const float sy = y + h - S(20) - (rows - 1 - k) * S(24);
            cv_.text(SANS_BOLD, S(11), labels[k], x + pad, sy, Anchor::W, TEXT3);
            cv_.text(MONO, S(14), values[k], x + iw - S(12), sy, Anchor::E, TEXT2);
        }
    }

    // área de la gráfica: umbrales, rejilla y referencias
    cv_.frame(px0, py0, px1, py1, BORDER);
    const std::pair<std::optional<double>, Color> limits[] = {
        {s.warn, WARN}, {s.crit, CRIT}, {s.warn_lo, WARN}, {s.crit_lo, CRIT}};
    for (const auto& lim : limits)
        if (lim.first && s.y_min < *lim.first && *lim.first < s.y_max)
            cv_.dashed_hline(px0, px1, ymap(s, py0, py1, *lim.first), S(2), S(6), lim.second);
    for (std::size_t k = 1; k < ticks.size(); ++k)
        cv_.vline(px1 - static_cast<float>(ticks[k] / window) * (px1 - px0), py0, py1, GRID);
    const double rng = s.y_max - s.y_min;
    for (int k = 0; k < 5; ++k) {
        const double gv = s.y_min + rng * k / 4;
        const float gy = ymap(s, py0, py1, gv);
        cv_.hline(px0, px1, gy, GRID);
        char buf[32];
        std::snprintf(buf, sizeof buf, rng <= 2 ? "%.2f" : "%.0f", gv);
        cv_.text(MONO, S(11), buf, px0 - S(8), gy, Anchor::E, TEXT3);
    }
    for (const RefLine& ref : REFS) {
        if (std::strcmp(ref.sensor, s.id) != 0 || !(s.y_min < ref.value && ref.value < s.y_max)) continue;
        const float ry = ymap(s, py0, py1, ref.value);
        cv_.dashed_hline(px0, px1, ry, S(5), S(5), REF_LINE);
        const float ly = ry - S(9) > py0 + S(26) ? ry - S(9) : ry + S(10);
        cv_.text(SANS, S(11), ref.label, px1 - S(10), ly, Anchor::E, REF_TEXT);
    }

    // traza, punto final y mensaje
    const Color lc = val.stale ? STALE : TEXT;
    if (pts_.size() >= 2) {
        cv_.polyline(pts_, std::max(2.0f, S(2)), lc);
        const Vector2 p = pts_.back();
        cv_.disc(p.x, p.y, S(5) + S(1), BG);   // aro negro que separa el punto de la línea
        cv_.disc(p.x, p.y, S(5), lc);
    }
    std::string msg;
    Color mc = STALE;
    if (!sv.last) {
        msg = sv.no_data ? "SIN DATOS — el ECU no responde este PID" : "ESPERANDO DATOS";
    } else if (val.stale) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SIN SEÑAL · hace %.0f s", now - sv.last_t);
        msg = buf;
        mc = WARN;
    }
    cv_.text(SANS_BOLD, S(15), msg, (px0 + px1) / 2, (py0 + py1) / 2, Anchor::C, mc);
}

// Puntos de la ventana: "ahora" siempre en el borde derecho
void Dash::trace(const Sensor& s, const SensorView& sv, double now, double window, float x0, float y0, float x1,
                 float y1, std::vector<double>* vals) {
    pts_.clear();
    if (vals) vals->clear();
    const double t_min = now - window, k = (x1 - x0) / window;
    for (std::size_t i = 0; i < sv.t.size(); ++i) {
        if (sv.t[i] < t_min) continue;
        pts_.push_back(Vector2{static_cast<float>(x0 + (sv.t[i] - t_min) * k), ymap(s, y0, y1, sv.v[i])});
        if (vals) vals->push_back(sv.v[i]);
    }
}

bool key_pressed(std::initializer_list<int> keys) {
    for (int k : keys)
        if (IsKeyPressed(k)) return true;
    return false;
}

void make_dirs(const std::string& dir) {
    for (std::size_t i = 1; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0755);
}

bool Dash::run(const std::atomic<bool>& quit) {
    if (views_.empty()) return false;
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);
    if (cfg_.windowed) InitWindow(1280, 720, "ZX-6R — Telemetría");
    else InitWindow(0, 0, "ZX-6R — Telemetría");   // 0 x 0 = tamaño del monitor
    if (!IsWindowReady()) return false;
    if (!cfg_.windowed) {
        ToggleBorderlessWindowed();
        HideCursor();
    }
    SetExitKey(KEY_ESCAPE);
    cv_.fonts.load(cfg_);

    // Gancho de prueba: a los 6 s guarda un PNG por vista (una cada 1.5 s) y sale
    const bool snapshot = !cfg_.snapshot_dir.empty();
    if (snapshot) make_dirs(cfg_.snapshot_dir);
    double next_shot = 6.0;
    std::size_t shot = 0;

    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1, cfg_.fps)));
    auto next_frame = std::chrono::steady_clock::now();
    const int nviews = static_cast<int>(views_.size());
    bool done = false;
    while (!done && !WindowShouldClose() && !quit) {
        // botones del manubrio = teclas (HID/Bluetooth, o gpio-key en la Pi)
        if (key_pressed({KEY_RIGHT, KEY_SPACE, KEY_ENTER, KEY_KP_ENTER, KEY_PAGE_DOWN, KEY_DOWN, KEY_N}))
            vi_ = (vi_ + 1) % nviews;
        if (key_pressed({KEY_LEFT, KEY_BACKSPACE, KEY_PAGE_UP, KEY_UP, KEY_P})) vi_ = (vi_ + nviews - 1) % nviews;

        W_ = static_cast<float>(GetScreenWidth());
        H_ = static_cast<float>(GetScreenHeight());
        cv_.dpi = std::max(1.0f, GetWindowScaleDPI().x);
        cv_.fonts.set_dpi(cv_.dpi);
        u_ = std::max(0.4f, std::min(W_ / 1280.0f, H_ / 720.0f));
        m_ = S(20);
        top_h_ = S(54);
        bot_h_ = S(38);

        const View& v = *views_[static_cast<std::size_t>(vi_)];
        unsigned mask = 0;
        for (std::size_t id : view_sensors(v)) mask |= 1u << id;
        const double now = tel_.elapsed();
        const Snapshot snap = tel_.snapshot(now - std::max(v.window, 10.0) - 1.0, mask);
        if (web_.listening() && now - ip_at_ >= 5.0) {   // la IP puede cambiar si cambia la red
            ip_ = WebServer::local_ip(cfg_.obd_ip);
            ip_at_ = now;
        }

        BeginDrawing();
        rlDisableBackfaceCulling();
        ClearBackground(BG);
        draw(snap);
        if (snapshot && snap.now >= next_shot) {
            rlDrawRenderBatchActive();   // todo lo dibujado, antes de cambiar de buffer
            Image img = LoadImageFromScreen();
            const std::string path = cfg_.snapshot_dir + "/view_" + std::to_string(shot) + ".png";
            const bool ok = ExportImage(img, path.c_str());
            std::printf("[UI] %s %s (%dx%d)\n", ok ? "Captura" : "No se pudo guardar", path.c_str(), img.width,
                        img.height);
            UnloadImage(img);
            std::fflush(stdout);
            ++shot;
            next_shot += 1.5;
            if (shot >= views_.size()) done = true;
            else vi_ = static_cast<int>(shot);
        }
        EndDrawing();

        // ritmo de cuadros sin espera activa
        next_frame += period;
        const auto t = std::chrono::steady_clock::now();
        if (next_frame < t) next_frame = t;
        else std::this_thread::sleep_until(next_frame);
    }
    cv_.fonts.clear();
    CloseWindow();
    return true;
}

}  // namespace

bool run_ui(const Config& cfg, const Telemetry& tel, const Store& store, const WebServer& web,
            const std::atomic<bool>& quit) {
    Dash dash(cfg, tel, store, web);
    return dash.run(quit);
}
