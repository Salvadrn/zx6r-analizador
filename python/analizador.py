"""ZX-6R — Telemetría OBD2 (RPM, TPS, temperatura, O2, MAP, batería).

Lee PIDs de un ELM327 WiFi sin parar (reconecta solo), los muestra en pantalla completa,
guarda cada lectura en SQLite y sirve un visor web para verlos en vivo o después desde el
celular. Las vistas se cambian con un botón (pensado para los botones del manubrio).
Con OBD2_SIM=1 corre con datos simulados.
"""
import os
import re
import json
import math
import queue
import socket
import sqlite3
import threading
import time
import traceback
try:
    import tkinter as tk
    import tkinter.font as tkfont
except ImportError:                 # sin Tk solo funciona el modo OBD2_HEADLESS
    tk = tkfont = None
from collections import deque
from contextlib import closing
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# =====================================================
# CONFIGURACIÓN
# =====================================================
MODO_SIMULADOR = os.environ.get("OBD2_SIM", "0") == "1"
IP_OBD2        = os.environ.get("OBD2_IP", "192.168.0.10")
PORT_OBD2      = int(os.environ.get("OBD2_PORT", "35000"))
DELAY_CMD      = 0.02     # pausa mínima entre comandos (se espera el prompt '>')
CMD_TIMEOUT    = 1.5      # s máximos esperando respuesta a un comando
RETRY_S        = 2.0      # espera entre intentos de reconexión
STALE_S        = 3.0      # sin muestra nueva en este tiempo = dato obsoleto
MAX_POINTS     = 1500     # tope de muestras por sensor en memoria
SIM_HZ         = 10

UI_FPS    = int(os.environ.get("OBD2_FPS", "12"))
WINDOWED  = os.environ.get("OBD2_WINDOWED", "0") == "1"    # ventana en vez de pantalla completa
HEADLESS  = os.environ.get("OBD2_HEADLESS", "0") == "1"    # sin pantalla: solo lee, guarda y sirve la web
FONT_SANS = os.environ.get("OBD2_FONT_SANS", "")
FONT_MONO = os.environ.get("OBD2_FONT_MONO", "")

# Botones del manubrio: teclas (cualquier botón HID/Bluetooth) o pines GPIO (Raspberry Pi)
NEXT_KEYS = {"Right", "space", "Return", "KP_Enter", "Next", "Down", "n", "N"}
PREV_KEYS = {"Left", "BackSpace", "Prior", "Up", "p", "P"}
GPIO_NEXT = os.environ.get("BTN_NEXT_GPIO")
GPIO_PREV = os.environ.get("BTN_PREV_GPIO")

DB_PATH  = os.environ.get("OBD2_DB", os.path.join(os.path.expanduser("~"), "obd2_logs", "telemetria.db"))
WEB_PORT = int(os.environ.get("OBD2_WEB_PORT", "8080"))   # 0 = sin visor web

# Dirección: encoder magnético AS5600 por I²C (no pasa por el ELM327)
STEER_HZ  = 25                                        # lecturas por segundo
I2C_BUS   = int(os.environ.get("OBD2_I2C_BUS", "1"))   # /dev/i2c-1 en la Raspberry Pi
STEER_CAL = os.environ.get("OBD2_STEER_CAL",
                           os.path.join(os.path.dirname(DB_PATH) or ".", "direccion.json"))

# =====================================================
# COLORES — negro puro y grises; el color solo marca estados (advertencia / crítico)
# =====================================================
BG     = "#000000"
PANEL  = "#000000"
PLOT   = "#000000"
BORDER = "#262626"
GRID   = "#161616"
AXIS   = "#333333"
TEXT   = "#F0F0F0"
TEXT2  = "#9A9A9A"
TEXT3  = "#666666"
STALE  = "#444444"
OK, WARN, CRIT = "#6FB583", "#D6A24A", "#D65C57"


def _track(text):
    """Espaciado entre letras para rótulos en mayúsculas."""
    return " ".join(text)


# =====================================================
# PARSERS
# =====================================================
def _payload(resp, header):
    """Devuelve los hex que siguen a `header` (p. ej. '4111') o None."""
    c = "".join(resp.split()).upper()
    i = c.find(header)
    return c[i + len(header):] if i >= 0 else None


def _byte(resp, header):
    p = _payload(resp, header)
    if p is None or len(p) < 2:
        return None
    try:
        return int(p[:2], 16)
    except ValueError:
        return None


def _p_tps(resp):
    b = _byte(resp, "4111")
    return None if b is None else round(b * 100 / 255, 1)


def _p_temp(resp):
    b = _byte(resp, "4105")
    return None if b is None else b - 40


def _p_o2(resp):
    """B1S1 (PID 0114): 0–1.275 V. Sano = oscila entre ~0.1 y ~0.9 V."""
    b = _byte(resp, "4114")
    return None if b is None else round(b * 0.005, 3)


def _p_map_l(resp):
    return _byte(resp, "410B")


def _p_map_r(resp):
    """PID propietario Kawasaki; escala sin confirmar (/10)."""
    p = _payload(resp, "6201F0")
    if p is None or len(p) < 4:
        return None
    try:
        return round(int(p[:4], 16) / 10.0, 1)
    except ValueError:
        return None


def _p_rpm(resp):
    p = _payload(resp, "410C")
    if p is None or len(p) < 4:
        return None
    try:
        return round(int(p[:4], 16) / 4)
    except ValueError:
        return None


def _p_bat(resp):
    """ATRV: voltaje en la entrada del ELM327 (la batería de la moto), p. ej. '12.6V'."""
    m = re.search(r"(\d{1,2}\.\d)\s*V", resp)
    return float(m.group(1)) if m else None


# period: segundos entre lecturas; 0 = en cada vuelta (la O2 lo necesita para ver la oscilación).
# warn/crit (altos) y warn_lo/crit_lo (bajos): umbrales de color, configurables; verifícalos con el manual.
SENSORS = [
    {"id": "rpm", "pid": "010C", "name": "RPM", "desc": "Revoluciones", "unit": "rpm",
     "y_min": 0, "y_max": 16000, "dec": 0, "parser": _p_rpm, "enabled": True, "period": 0.25},
    {"id": "tps", "pid": "0111", "name": "TPS", "desc": "Posición del acelerador", "unit": "%",
     "y_min": 0, "y_max": 100, "dec": 0, "parser": _p_tps, "enabled": True, "period": 0.25},
    {"id": "temp", "pid": "0105", "name": "TEMP", "desc": "Refrigerante", "unit": "°C",
     "y_min": 40, "y_max": 120, "dec": 0, "parser": _p_temp, "enabled": True, "period": 2.0,
     "warn": 105, "crit": 115},
    {"id": "o2", "pid": "0114", "name": "O2", "desc": "Sonda lambda B1S1", "unit": "V",
     "y_min": 0.0, "y_max": 1.0, "dec": 2, "parser": _p_o2, "enabled": True, "period": 0},
    {"id": "map_l", "pid": "010B", "name": "MAP IZQ", "desc": "Presión de admisión", "unit": "kPa",
     "y_min": 20, "y_max": 120, "dec": 0, "parser": _p_map_l, "enabled": True, "period": 0.25},
    {"id": "bat", "pid": "ATRV", "name": "BATERÍA", "desc": "Voltaje (lo mide el ELM327)", "unit": "V",
     "y_min": 11, "y_max": 15, "dec": 1, "parser": _p_bat, "enabled": True, "period": 5.0,
     "warn_lo": 12.2, "crit_lo": 11.5},
    # Apagado por defecto: un ELM327 estándar casi nunca responde este PID (OBD2_MAP_R=1 lo prende).
    {"id": "map_r", "pid": "2201F0", "name": "MAP DER", "desc": "Presión de admisión", "unit": "kPa",
     "y_min": 20, "y_max": 120, "dec": 0, "parser": _p_map_r, "period": 0.5,
     "enabled": os.environ.get("OBD2_MAP_R", "0") == "1"},
    # No viene del ELM327: encoder AS5600 en el eje de la dirección (OBD2_STEER=0 lo apaga).
    {"id": "steer", "source": "i2c", "name": "DIRECCIÓN", "desc": "Manubrio · − izq / + der", "unit": "°",
     "y_min": -40, "y_max": 40, "dec": 1, "enabled": os.environ.get("OBD2_STEER", "1") == "1"},
]
SENSOR = {s["id"]: s for s in SENSORS}

# Vistas: el botón las recorre en orden. window = segundos visibles.
VIEWS = [
    {"name": "RESUMEN", "short": "RESUMEN", "kind": "tiles", "window": 30,
     "sensors": ["rpm", "tps", "o2", "map_l", "temp", "bat", "steer", "map_r"]},
    {"name": "O2", "short": "O2", "kind": "graphs", "window": 20, "sensors": ["o2"]},
    {"name": "MOTOR", "short": "MOTOR", "kind": "graphs", "window": 60,
     "sensors": ["rpm", "tps", "map_l", "map_r"]},
    {"name": "DIRECCIÓN", "short": "DIR", "kind": "graphs", "window": 20, "sensors": ["steer"]},
    {"name": "TEMP · BATERÍA", "short": "TEMP·BAT", "kind": "graphs", "window": 300,
     "sensors": ["temp", "bat"]},
]

# líneas de referencia en las gráficas: (valor, texto, color de línea, color de texto)
REFS = {
    "o2":    [(0.45, "λ = 1  (0.45 V)", "#3A3A3A", "#7A7A7A")],
    "map_l": [(101, "101 kPa (atm)", "#3A3A3A", "#7A7A7A")],
    "map_r": [(101, "101 kPa (atm)", "#3A3A3A", "#7A7A7A")],
    "temp":  [(100, "ventilador (100 °C)", "#3A3A3A", "#7A7A7A")],
    "steer": [(0, "centro", "#3A3A3A", "#7A7A7A")],
}


def _obd(s):
    return s.get("source", "obd") == "obd"


def _axis_label(v, step):
    """Rótulo del eje con los decimales justos para que el paso de la rejilla sea exacto."""
    d = next((d for d in range(4) if abs(step * 10 ** d - round(step * 10 ** d)) < 1e-6), 3)
    return f"{v:.{d}f}"


def _fmt(s, v):
    return f"{v:.{s['dec']}f}"


def _zone_color(s, v):
    if v >= s.get("crit", math.inf) or v <= s.get("crit_lo", -math.inf):
        return CRIT
    if v >= s.get("warn", math.inf) or v <= s.get("warn_lo", -math.inf):
        return WARN
    return TEXT


def _ymap(s, y0, y1, v):
    """Valor -> y en pantalla dentro de [y0, y1] (4 px de margen arriba y abajo)."""
    norm = (v - s["y_min"]) / (s["y_max"] - s["y_min"])
    return max(y0 + 2, min(y1 - 2, y1 - 4 - norm * (y1 - y0 - 8)))


def _tlabel(t):
    if t < 60:
        return f"-{t}s"
    return f"-{t // 60}m" if t % 60 == 0 else f"-{t // 60}:{t % 60:02d}"


def _crossings(ts, vs, now, span=10.0, hi=0.5, lo=0.4):
    """Cruces rico/pobre de la O2 en los últimos `span` s (con histéresis)."""
    n, state = 0, 0
    for tv, v in zip(ts, vs):
        if tv < now - span:
            continue
        cur = 1 if v > hi else -1 if v < lo else 0
        if cur and state and cur != state:
            n += 1
        if cur:
            state = cur
    return n


# =====================================================
# ESTADO
# =====================================================
data_lock = threading.Lock()
sensor_data = {}                   # id -> {"t","v","last","no_data"}
rate_times = deque(maxlen=600)     # instante de cada muestra válida (para las lecturas/s)
link = {"state": "connecting", "retries": 0, "since": 0.0}   # connecting | live | lost | sim
session_t0 = time.time()
session_id = None                  # id de la sesión en la base de datos (None = sin registro)
db_ok = False
db_queue = queue.SimpleQueue()     # (t, sensor, valor) pendientes de guardar
stop_event = threading.Event()
worker = None
writer = None
steerer = None


def reset_data():
    with data_lock:
        rate_times.clear()
        for s in SENSORS:
            sensor_data[s["id"]] = {"t": deque(maxlen=MAX_POINTS), "v": deque(maxlen=MAX_POINTS),
                                    "last": None, "no_data": False}


reset_data()


# =====================================================
# OBD2
# =====================================================
def _drain(sock):
    """Tira lo que haya quedado en el socket (respuestas tardías) para no desfasar."""
    sock.settimeout(0)
    try:
        while sock.recv(1024):
            pass
    except (BlockingIOError, socket.timeout):
        pass
    finally:
        sock.settimeout(0.5)


def _send(sock, cmd):
    """Manda un comando y lee hasta el prompt '>' del ELM327."""
    _drain(sock)
    sock.sendall((cmd + "\r").encode())
    buf = b""
    deadline = time.time() + CMD_TIMEOUT
    while time.time() < deadline:
        try:
            chunk = sock.recv(1024)
        except socket.timeout:
            break
        if not chunk:
            raise ConnectionError("socket cerrado")
        buf += chunk
        if b">" in buf:
            break
    if DELAY_CMD:
        time.sleep(DELAY_CMD)
    return buf.decode(errors="ignore")


def _connect():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(5)
    sock.connect((IP_OBD2, PORT_OBD2))
    sock.settimeout(0.5)
    for cmd in ["ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0"]:
        _send(sock, cmd)
    return sock


def _sim_value(sid, t):
    tps = 20 + 55 * abs(math.sin(t * 0.4))
    if sid == "rpm":
        return round(2500 + 5500 * abs(math.sin(t * 0.4)) + 300 * math.sin(t * 3.1))
    if sid == "tps":
        return max(0, min(100, tps + 8 * math.sin(t * 3.1)))
    if sid == "temp":
        return 78 + 12 * math.sin(t * 0.04)
    if sid == "o2":
        return max(0.0, min(1.0, round(0.5 + 0.4 * math.sin(t * 6.0) + 0.05 * math.sin(t * 17.3), 3)))
    if sid == "map_l":
        return max(20, min(105, 30 + tps * 0.72 + 5 * math.sin(t * 2.1)))
    if sid == "map_r":
        return max(20, min(105, 28 + tps * 0.74 + 5 * math.sin(t * 2.1 + 0.3)))
    if sid == "bat":
        return 13.9 + 0.15 * math.sin(t * 0.7)
    return 0.0


def _record(sid, t, val):
    with data_lock:
        sd = sensor_data[sid]
        if val is None:
            sd["no_data"] = True
            return
        sd["t"].append(t)
        sd["v"].append(val)
        sd["last"] = val
        sd["no_data"] = False
        rate_times.append(time.time())
    if session_id is not None:
        db_queue.put((t, sid, val))


def _pick_batch(enabled, last_read):
    """Los de period 0 en cada vuelta + el lento más atrasado respecto a su periodo."""
    batch = [s for s in enabled if s["period"] == 0]
    slow = [s for s in enabled if s["period"] > 0]
    if slow:
        now = time.time()
        batch.append(max(slow, key=lambda s: (now - last_read.get(s["id"], 0)) / s["period"]))
    return batch


def read_thread(stop, t0):
    """Lee sin parar y reconecta solo; solo se detiene al salir."""
    sock = None
    last_read = {}
    try:
        while not stop.is_set():
            enabled = [s for s in SENSORS if s["enabled"] and _obd(s)]
            if MODO_SIMULADOR:
                link.update(state="sim", since=time.time())
                t = time.time() - t0
                for s in enabled:
                    _record(s["id"], t, _sim_value(s["id"], t))
                stop.wait(1 / SIM_HZ)
                continue

            try:
                if sock is None:
                    link["state"] = "connecting"
                    sock = _connect()
                    link.update(state="live", retries=0, since=time.time())
                    print("✅ [OBD2] Conectado")
                for s in _pick_batch(enabled, last_read):
                    if stop.is_set():
                        break
                    last_read[s["id"]] = time.time()
                    resp = _send(sock, s["pid"])
                    _record(s["id"], time.time() - t0, s["parser"](resp))
            except Exception as e:            # nunca dejar de leer: cualquier fallo = reconectar
                if not isinstance(e, OSError):
                    traceback.print_exc()
                link["state"] = "lost"
                link["retries"] += 1
                print(f"❌ [OBD2] {e} — reintentando ({link['retries']})...")
                if sock:
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = None
                stop.wait(RETRY_S)
    finally:
        if sock:
            try:
                sock.close()
            except OSError:
                pass


# =====================================================
# BASE DE DATOS — SQLite local, una fila por lectura (mismo esquema que la versión C++)
# =====================================================
SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (id INTEGER PRIMARY KEY, started TEXT NOT NULL, sim INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS samples (session INTEGER NOT NULL, t REAL NOT NULL, sensor TEXT NOT NULL,
                                    value REAL NOT NULL);
CREATE INDEX IF NOT EXISTS samples_by_sensor ON samples(session, sensor, t);
"""


def _db():
    con = sqlite3.connect(DB_PATH, timeout=5)
    con.execute("PRAGMA journal_mode=WAL")
    return con


def open_session():
    """Crea la sesión de esta corrida. Devuelve su id, o None si no se puede escribir."""
    global db_ok
    try:
        os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
        with closing(_db()) as con:
            con.executescript(SCHEMA)
            cur = con.execute("INSERT INTO sessions (started, sim) VALUES (?, ?)",
                              (datetime.now().isoformat(timespec="seconds"), int(MODO_SIMULADOR)))
            con.commit()
        db_ok = True
        print(f"💾 Sesión {cur.lastrowid} en {DB_PATH}")
        return cur.lastrowid
    except (sqlite3.Error, OSError) as e:
        print(f"⚠ Sin base de datos: {e}")
        db_ok = False
        return None


def writer_thread(stop, sid):
    """Guarda en lotes cada segundo: un commit por lote, no por muestra."""
    global db_ok
    con = _db()
    try:
        while True:
            done = stop.wait(1.0)
            rows = []
            while True:
                try:
                    t, sensor, value = db_queue.get_nowait()
                except queue.Empty:
                    break
                rows.append((sid, t, sensor, value))
            if rows:
                try:
                    with con:
                        con.executemany("INSERT INTO samples VALUES (?, ?, ?, ?)", rows)
                    db_ok = True
                except sqlite3.Error as e:
                    db_ok = False
                    print(f"⚠ No se pudo guardar: {e}")
            if done:
                break
    finally:
        con.close()


def start_reading():
    global worker, writer, steerer, session_t0, session_id
    reset_data()
    stop_event.clear()
    session_id = open_session()
    session_t0 = time.time()
    worker = threading.Thread(target=read_thread, args=(stop_event, session_t0),
                              daemon=True, name="obd-reader")
    worker.start()
    if session_id is not None:
        writer = threading.Thread(target=writer_thread, args=(stop_event, session_id),
                                  daemon=True, name="db-writer")
        writer.start()
    if SENSOR["steer"]["enabled"]:
        _load_steer_cal()
        steerer = threading.Thread(target=steer_thread, args=(stop_event, session_t0),
                                   daemon=True, name="steer")
        steerer.start()


def stop_reading():
    stop_event.set()
    for th in (worker, writer, steerer):
        if th is not None:
            th.join(3)


# =====================================================
# DIRECCIÓN — encoder magnético AS5600 (I²C 0x36): imán en el eje, sensor fijo al cuadro
# =====================================================
AS5600_ADDR = 0x36
I2C_SLAVE = 0x0703                  # ioctl de Linux para elegir el dispositivo del bus
steer_cal = {"center": None, "invert": False}     # centro en grados crudos del sensor
steer_state = {"status": "init", "raw": None}     # ok | sin_sensor | sin_iman | sim


def _load_steer_cal():
    try:
        with open(STEER_CAL) as f:
            data = json.load(f)
        steer_cal.update(center=data.get("center"), invert=bool(data.get("invert", False)))
    except (OSError, ValueError):
        pass


def _save_steer_cal():
    try:
        os.makedirs(os.path.dirname(STEER_CAL) or ".", exist_ok=True)
        tmp = STEER_CAL + ".tmp"
        with open(tmp, "w") as f:
            json.dump(steer_cal, f)
        os.replace(tmp, STEER_CAL)                # escritura atómica: nunca queda a medias
    except OSError as e:
        print(f"⚠ No se pudo guardar la calibración de la dirección: {e}")


def steer_set_center():
    """El ángulo actual pasa a ser 0° (manubrio derecho)."""
    if steer_state["raw"] is None:
        return False
    steer_cal["center"] = steer_state["raw"]
    _save_steer_cal()
    return True


def steer_toggle_invert():
    steer_cal["invert"] = not steer_cal["invert"]
    _save_steer_cal()
    return True


def _steer_angle(raw):
    center = steer_cal["center"]
    if center is None:
        center = 200.0 if MODO_SIMULADOR else 0.0
    a = (raw - center + 180) % 360 - 180          # de −180 a 180, sin brinco al pasar por 0°/360°
    return round(-a if steer_cal["invert"] else a, 1)


def _as5600_read(fd):
    """(ángulo crudo en grados, ¿hay imán?). Dos lecturas: STATUS (0x0B) y RAW ANGLE (0x0C-0x0D)."""
    os.write(fd, b"\x0b")
    status = os.read(fd, 1)[0]
    os.write(fd, b"\x0c")
    hi, lo = os.read(fd, 2)
    return ((hi & 0x0F) << 8 | lo) * 360 / 4096, bool(status & 0x20)


def steer_thread(stop, t0):
    """Lee la dirección 25 veces por segundo, en su propio hilo para no esperar al ELM327."""
    fd = None
    next_at = time.time()
    try:
        while not stop.is_set():
            next_at += 1 / STEER_HZ
            t = time.time() - t0
            if MODO_SIMULADOR:
                raw = (200 + 9 * math.sin(t * 0.5) + 2.5 * math.sin(t * 2.3)) % 360
                steer_state.update(status="sim", raw=raw)
                _record("steer", t, _steer_angle(raw))
                stop.wait(max(0.0, next_at - time.time()))
                continue
            try:
                if fd is None:
                    import fcntl
                    fd = os.open(f"/dev/i2c-{I2C_BUS}", os.O_RDWR)
                    fcntl.ioctl(fd, I2C_SLAVE, AS5600_ADDR)
                raw, magnet = _as5600_read(fd)
                if magnet:
                    steer_state.update(status="ok", raw=raw)
                    _record("steer", t, _steer_angle(raw))
                else:
                    steer_state.update(status="sin_iman")
                    _record("steer", t, None)
                stop.wait(max(0.0, next_at - time.time()))
            except (OSError, ImportError):            # sin bus I²C o sin AS5600: reintenta
                steer_state.update(status="sin_sensor")
                _record("steer", t, None)
                if fd is not None:
                    os.close(fd)
                    fd = None
                stop.wait(RETRY_S)
                next_at = time.time()
    finally:
        if fd is not None:
            os.close(fd)


# =====================================================
# SERVIDOR WEB — en vivo y sesiones guardadas desde el celular o la laptop en la misma red
# =====================================================
WEB_PAGE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web", "index.html")


def _meta(s):
    m = {k: s[k] for k in ("id", "name", "desc", "unit", "dec", "y_min", "y_max")}
    m.update({k: s[k] for k in ("warn", "crit", "warn_lo", "crit_lo") if k in s})
    if s["id"] == "steer":
        m["calibrable"] = True
    return m


def _live(trace_s):
    now = time.time() - session_t0
    with data_lock:
        hz = sum(1 for t in rate_times if time.time() - t <= 3.0) / 3.0
        values, trace = {}, {}
        for s in SENSORS:
            if not s["enabled"]:
                continue
            sd = sensor_data[s["id"]]
            values[s["id"]] = {"v": sd["last"], "age": (now - sd["t"][-1]) if sd["t"] else None,
                               "no_data": sd["no_data"]}
            pts = [(t, v) for t, v in zip(sd["t"], sd["v"]) if t >= now - trace_s]
            trace[s["id"]] = {"t": [round(t, 3) for t, _ in pts], "v": [v for _, v in pts]}
    return {"state": link["state"], "session": session_id, "hz": hz, "t": now,
            "sensors": [_meta(s) for s in SENSORS if s["enabled"]], "values": values, "trace": trace,
            "steer": {"status": steer_state["status"], "calibrated": steer_cal["center"] is not None,
                      "invert": steer_cal["invert"]}}


def _decimate(rows, n=2000):
    """Reduce a ~n puntos conservando el mínimo y el máximo de cada tramo (en orden de t)."""
    if len(rows) <= n:
        return rows
    size = len(rows) / (n / 2)
    out, i = [], 0.0
    while int(i) < len(rows):
        chunk = rows[int(i):max(int(i + size), int(i) + 1)]
        lo, hi = min(chunk, key=lambda r: r[1]), max(chunk, key=lambda r: r[1])
        out.extend(sorted({lo, hi}))
        i += size
    return out


def _sessions(con):
    rows = con.execute("""SELECT s.id, s.started, s.sim, COALESCE(MAX(m.t), 0), COUNT(m.t)
                          FROM sessions s LEFT JOIN samples m ON m.session = s.id
                          GROUP BY s.id ORDER BY s.id DESC LIMIT 200""").fetchall()
    return [{"id": i, "started": st, "sim": sim, "duration": dur, "samples": n}
            for i, st, sim, dur, n in rows]


def _session(con, sid):
    row = con.execute("SELECT id, started, sim FROM sessions WHERE id = ?", (sid,)).fetchone()
    if row is None:
        return None
    stats = con.execute("""SELECT sensor, MIN(value), MAX(value), AVG(value), COUNT(*), MAX(t)
                           FROM samples WHERE session = ? GROUP BY sensor""", (sid,)).fetchall()
    out = {"id": row[0], "started": row[1], "sim": row[2], "duration": 0, "sensors": [], "data": {}}
    for sensor, mn, mx, avg, n, tmax in stats:
        if sensor not in SENSOR:
            continue
        out["duration"] = max(out["duration"], tmax)
        out["sensors"].append({**_meta(SENSOR[sensor]), "min": mn, "max": mx, "avg": avg, "n": n})
        pts = _decimate(con.execute("SELECT t, value FROM samples WHERE session = ? AND sensor = ? ORDER BY t",
                                    (sid, sensor)).fetchall())
        out["data"][sensor] = {"t": [round(t, 3) for t, _ in pts], "v": [v for _, v in pts]}
    order = [s["id"] for s in SENSORS]
    out["sensors"].sort(key=lambda m: order.index(m["id"]))
    return out


class _Web(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _reply(self, code, body, ctype, extra=None):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj):
        self._reply(200, json.dumps(obj, separators=(",", ":")), "application/json")

    def do_GET(self):
        url = urlparse(self.path)
        try:
            if url.path == "/":
                try:
                    with open(WEB_PAGE_PATH, "rb") as f:
                        return self._reply(200, f.read(), "text/html; charset=utf-8")
                except OSError:
                    return self._reply(500, "falta web/index.html", "text/plain; charset=utf-8")
            if url.path == "/api/live":
                trace = float(parse_qs(url.query).get("trace", ["30"])[0])
                return self._json(_live(trace))
            m = re.fullmatch(r"/api/session/(\d+)(\.csv)?", url.path)
            if url.path == "/api/sessions" or m:
                with closing(_db()) as con:
                    if not m:
                        return self._json(_sessions(con))
                    sid = int(m.group(1))
                    if m.group(2):
                        rows = con.execute("SELECT t, sensor, value FROM samples WHERE session = ? ORDER BY t",
                                           (sid,)).fetchall()
                        body = "t,sensor,value\n" + "".join(f"{t:.3f},{s},{v:.6g}\n" for t, s, v in rows)
                        return self._reply(200, body, "text/csv; charset=utf-8",
                                           {"Content-Disposition": f'attachment; filename="sesion_{sid}.csv"'})
                    data = _session(con, sid)
                    return self._json(data) if data else self._reply(404, "no existe", "text/plain")
            self._reply(404, "no encontrado", "text/plain")
        except Exception as e:
            self._reply(500, str(e), "text/plain; charset=utf-8")

    def do_POST(self):
        """Calibración de la dirección desde el celular: /api/steer/center y /api/steer/invert."""
        actions = {"/api/steer/center": steer_set_center, "/api/steer/invert": steer_toggle_invert}
        action = actions.get(urlparse(self.path).path)
        if action is None:
            return self._reply(404, "no encontrado", "text/plain")
        ok = action()
        self._json({"ok": ok, "calibrated": steer_cal["center"] is not None, "invert": steer_cal["invert"]})


def start_web():
    if not WEB_PORT:
        return None
    try:
        srv = ThreadingHTTPServer(("0.0.0.0", WEB_PORT), _Web)
    except OSError as e:
        print(f"⚠ Web no disponible en el puerto {WEB_PORT}: {e}")
        return None
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True, name="web").start()
    print(f"🌐 Visor web en el puerto {WEB_PORT}")
    return srv


def _local_ip():
    """IP de esta máquina en la red del ELM327 (UDP 'conectado' no manda paquetes)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((IP_OBD2, 1))
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


# =====================================================
# BOTONES DEL MANUBRIO
# =====================================================
def _attach_gpio(cmds):
    """Botones por GPIO (Raspberry Pi). Los callbacks llegan de otro hilo: solo encolan."""
    pins = {"next": GPIO_NEXT, "prev": GPIO_PREV}
    if not any(pins.values()):
        return []
    try:
        from gpiozero import Button
    except ImportError:
        print("⚠ BTN_*_GPIO definido pero falta gpiozero (pip install gpiozero)")
        return []
    buttons = []
    for cmd, pin in pins.items():
        if pin:
            b = Button(int(pin), pull_up=True, bounce_time=0.05)
            b.when_pressed = lambda cmd=cmd: cmds.put(cmd)
            buttons.append(b)
    return buttons


# =====================================================
# UI
# =====================================================
class Dash:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("ZX-6R — Telemetría")
        self.root.configure(bg=BG)
        if WINDOWED:
            self.root.geometry("1280x720+60+60")
        else:
            self.root.attributes("-fullscreen", True)
            try:
                self.root.configure(cursor="none")
            except tk.TclError:
                pass
        self.c = tk.Canvas(self.root, bg=BG, highlightthickness=0, bd=0)
        self.c.pack(fill="both", expand=True)

        have = set(tkfont.families())
        pick = lambda pref, cands, fb: next((f for f in [pref, *cands] if f and f in have), fb)
        self.fam = {
            "sans": pick(FONT_SANS, ["Helvetica Neue", "Segoe UI", "Noto Sans", "DejaVu Sans", "Arial"],
                         "Helvetica"),
            "mono": pick(FONT_MONO, ["Menlo", "SF Mono", "Consolas", "DejaVu Sans Mono",
                                     "Liberation Mono", "Courier New"], "Courier"),
        }
        self._fonts = {}
        self.views = [v for v in VIEWS if any(SENSOR[i]["enabled"] for i in v["sensors"])]
        self.vi = 0
        self.cmds = queue.SimpleQueue()
        self._buttons = _attach_gpio(self.cmds)
        self._cache = {}
        self._built = None
        self.dyn = []
        self.hz = 0.0
        self._errs = 0

        self.root.bind("<Key>", self._on_key)
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self.root.focus_force()
        start_reading()
        self.web = start_web()
        self.ip, self._ip_at = _local_ip(), time.time()
        self.root.after(100, self._tick)

    # ── entrada ─────────────────────────────────
    def _on_key(self, e):
        if e.keysym == "Escape":
            self.quit()
        elif e.keysym in NEXT_KEYS:
            self.step(1)
        elif e.keysym in PREV_KEYS:
            self.step(-1)
        elif e.keysym in ("c", "C"):                 # teclado de taller: fija el centro de la dirección
            steer_set_center()

    def step(self, d):
        self.vi = (self.vi + d) % len(self.views)

    def quit(self):
        stop_reading()
        if self.web:
            self.web.shutdown()
        self.root.destroy()

    # ── utilidades de dibujo ────────────────────
    def S(self, v):
        return max(1, int(round(v * self.u)))

    def f(self, kind, px, bold=False):
        px = -max(9, int(px))
        return (self.fam[kind], px, "bold") if bold else (self.fam[kind], px)

    def measure(self, kind, px, bold, text):
        key = (kind, max(9, int(px)), bold)
        if key not in self._fonts:
            self._fonts[key] = tkfont.Font(family=self.fam[kind], size=-key[1],
                                           weight="bold" if bold else "normal")
        return self._fonts[key].measure(text)

    def _set(self, item, **opts):
        """itemconfigure solo con lo que cambió: Tk repinta cada vez que se toca un item."""
        cur = self._cache.setdefault(item, {})
        changed = {k: v for k, v in opts.items() if cur.get(k) != v}
        if changed:
            cur.update(changed)
            self.c.itemconfigure(item, **changed)

    def _rrect(self, x0, y0, x1, y1, r, **kw):
        pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1,
               x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
        return self.c.create_polygon(pts, smooth=True, **kw)

    # ── construcción (solo al cambiar de vista o de tamaño) ──
    def _rebuild(self, W, H):
        self.c.delete("all")
        self._cache.clear()
        self.W, self.H = W, H
        self.u = max(0.4, min(W / 1280, H / 720))
        self.m, self.top_h, self.bot_h = self.S(20), self.S(54), self.S(38)
        self.dyn = []
        self._build_chrome()
        v = self.views[self.vi]
        sensors = [SENSOR[i] for i in v["sensors"] if SENSOR[i]["enabled"]]
        area = (self.m, self.top_h + self.S(16), W - self.m, H - self.bot_h - self.S(10))
        (self._build_tiles if v["kind"] == "tiles" else self._build_graphs)(v, sensors, area)

    def _build_chrome(self):
        c, S, W, H, m = self.c, self.S, self.W, self.H, self.m
        mid = self.top_h // 2

        # derecha: estado del enlace, tasa de datos, reloj
        xr = W - m
        self.clock = c.create_text(xr, mid, anchor="e", text="", fill=TEXT2,
                                   font=self.f("mono", S(16), True))
        xr -= self.measure("mono", S(16), True, "00:00:00") + S(24)
        self.rate = c.create_text(xr, mid, anchor="e", text="", fill=TEXT2, font=self.f("mono", S(13)))
        xr -= self.measure("mono", S(13), False, "00.0 Hz") + S(24)
        chip_w = max(self.measure("sans", S(12), True, t) for t in
                     ("SIMULADOR", "CONECTANDO", "OBD2 EN VIVO", "SIN CONEXIÓN", "SIN DATOS ECU")) + S(52)
        x0 = xr - chip_w
        self._rrect(x0, mid - S(15), xr, mid + S(15), S(15), fill=PANEL, outline=BORDER)
        self.chip_dot = c.create_oval(x0 + S(14), mid - S(5), x0 + S(24), mid + S(5), fill=OK, outline="")
        self.chip_txt = c.create_text(x0 + S(34), mid, anchor="w", text="", fill=TEXT,
                                      font=self.f("sans", S(12), True))
        right_start = x0 - S(30)

        # izquierda: marca y pestañas; si no caben se acorta (sin subtítulo, nombres cortos)
        brand_w = self.measure("sans", S(24), True, "ZX-6R")
        sub = _track("TELEMETRÍA")
        sub_w = self.measure("sans", S(11), True, sub)
        gap = S(30)
        tabs_w = lambda key: (sum(self.measure("sans", S(14), True, v[key]) for v in self.views)
                              + gap * (len(self.views) - 1))
        for with_sub, key in ((True, "name"), (False, "name"), (False, "short")):
            need = m + brand_w + (S(32) + sub_w if with_sub else 0) + S(40) + tabs_w(key)
            if need <= right_start:
                break
        c.create_text(m, mid, anchor="w", text="ZX-6R", fill=TEXT, font=self.f("sans", S(24), True))
        x = m + brand_w
        if with_sub:
            c.create_line(x + S(16), mid - S(11), x + S(16), mid + S(11), fill=AXIS)
            c.create_text(x + S(32), mid, anchor="w", text=sub, fill=TEXT3, font=self.f("sans", S(11), True))
            x += S(32) + sub_w
        x += S(40)
        for i, v in enumerate(self.views):
            w = self.measure("sans", S(14), True, v[key])
            active = i == self.vi
            c.create_text(x, mid, anchor="w", text=v[key], fill=TEXT if active else TEXT3,
                          font=self.f("sans", S(14), True))
            if active:
                c.create_rectangle(x, self.top_h - S(7), x + w, self.top_h - S(4), fill=TEXT, outline="")
            x += w + gap
        self.status_line = c.create_rectangle(0, self.top_h, W, self.top_h + S(1), fill=AXIS, outline="")

        # abajo: ayuda del botón, contador de vistas y registro
        by = H - self.bot_h // 2
        c.create_polygon(m, by - S(6), m + S(10), by, m, by + S(6), fill=TEXT2, outline="")
        hint = _track("CAMBIAR VISTA")
        c.create_text(m + S(20), by, anchor="w", text=hint, fill=TEXT2, font=self.f("sans", S(11), True))
        xh = m + S(20) + self.measure("sans", S(11), True, hint) + S(18)
        c.create_text(xh, by, anchor="w", text=f"{self.vi + 1} / {len(self.views)}", fill=TEXT3,
                      font=self.f("mono", S(12)))
        self.by = by
        self.rec_dot = c.create_oval(0, 0, 0, 0, fill=CRIT, outline="")
        self.rec_txt = c.create_text(W - m, by, anchor="e", text="", fill=TEXT3, font=self.f("mono", S(11)))

    def _build_tiles(self, v, sensors, area):
        x0, y0, x1, y1 = area
        n = len(sensors)
        cols = 1 if n == 1 else 2 if n <= 4 else 3 if n <= 6 else 4
        rows = (n + cols - 1) // cols
        gap = self.S(14)
        th = (y1 - y0 - (rows - 1) * gap) / rows
        for r in range(rows):
            row = sensors[r * cols:(r + 1) * cols]          # la última fila reparte el ancho que sobra
            tw = (x1 - x0 - (len(row) - 1) * gap) / len(row)
            for k, s in enumerate(row):
                self._tile(s, x0 + k * (tw + gap), y0 + r * (th + gap), tw, th, v["window"])

    def _tile(self, s, x, y, w, h, window):
        c, S = self.c, self.S
        self._rrect(x, y, x + w, y + h, S(14), fill=PANEL, outline=BORDER)
        pad = S(26)
        c.create_text(x + pad, y + S(30), anchor="w", text=_track(s["name"]), fill=TEXT2,
                      font=self.f("sans", S(16), True))
        c.create_text(x + w - S(22), y + S(30), anchor="e", text=s["desc"], fill=TEXT3,
                      font=self.f("sans", S(12)))
        vchars = max(len(_fmt(s, s["y_min"])), len(_fmt(s, s["y_max"])))
        vpx = int(min(h * 0.36, w * 0.58 / (vchars * 0.62)))
        xr = x + pad + self.measure("mono", vpx, True, "0") * vchars
        vy = y + h * 0.46
        val = c.create_text(xr, vy, anchor="e", text="--", fill=STALE, font=self.f("mono", vpx, True))
        c.create_text(xr + S(10), vy + vpx * 0.30, anchor="sw", text=s["unit"], fill=TEXT2,
                      font=self.f("sans", max(S(14), vpx * 0.3)))
        box = (x + pad, y + h * 0.66, x + w - S(22), y + h - S(22))
        c.create_line(box[0], box[3], box[2], box[3], fill=AXIS)
        c.create_line(box[0], box[1], box[2], box[1], fill=GRID)
        line = c.create_line(0, 0, 0, 0, fill=TEXT, width=max(2, S(2)), state="hidden")
        self.dyn.append((self._upd_tile, {"s": s, "val": val, "line": line, "box": box, "window": window}))

    def _build_graphs(self, v, sensors, area):
        c, S = self.c, self.S
        x0, y0, x1, y1 = area
        window = v["window"]
        n = len(sensors)
        gap, axis_h = S(14), S(24)
        ph = (y1 - y0 - axis_h - (n - 1) * gap) / n
        iw = max(S(190), min(S(330), (x1 - x0) * 0.25))
        px0, px1 = x0 + iw + S(58), x1 - S(20)
        step = next((t for t in (1, 2, 5, 10, 15, 20, 30, 60, 120, 300) if window / t <= 8), 300)
        ticks = [k * step for k in range(int(window // step) + 1)]
        for i, s in enumerate(sensors):
            self._graph_panel(s, x0, y0 + i * (ph + gap), x1 - x0, ph, iw, (px0, px1), window, ticks)
        yl = y0 + n * ph + (n - 1) * gap + S(8)
        for t in ticks:
            x = px1 - t / window * (px1 - px0)
            c.create_text(x, yl, anchor="ne" if t == 0 else "n", fill=TEXT3, font=self.f("mono", S(11)),
                          text="ahora" if t == 0 else _tlabel(t))

    def _graph_panel(self, s, x, y, w, h, iw, xs, window, ticks):
        c, S = self.c, self.S
        self._rrect(x, y, x + w, y + h, S(14), fill=PANEL, outline=BORDER)
        pad = S(26)
        c.create_text(x + pad, y + S(30), anchor="w", text=_track(s["name"]), fill=TEXT2,
                      font=self.f("sans", S(16), True))
        c.create_text(x + pad, y + S(50), anchor="w", text=s["desc"], fill=TEXT3, font=self.f("sans", S(12)))

        # columna de datos: valor grande y, si cabe, estadísticas de la ventana
        big = h >= S(230)
        rows = [("min", "MIN"), ("max", "MAX"), ("avg", "PROM")] + ([("cross", "CRUCES λ / 10 s")]
                                                                   if s["id"] == "o2" else [])
        vchars = max(len(_fmt(s, s["y_min"])), len(_fmt(s, s["y_max"])))
        if big:
            vpx = min(S(104), (h - S(62) - len(rows) * S(24) - S(26)) * 0.62)
            top, bottom = y + S(72), y + h - S(20) - len(rows) * S(24)
            vy = (top + bottom) / 2                           # centrado entre el título y las estadísticas
        else:
            vpx = min(S(54), h * 0.34)
            vy = y + h * 0.64
        vpx = int(min(vpx, (iw - pad - S(50)) / (vchars * 0.62)))
        xr = x + pad + self.measure("mono", vpx, True, "0") * vchars
        val = c.create_text(xr, vy, anchor="e", text="--", fill=STALE, font=self.f("mono", vpx, True))
        c.create_text(xr + S(8), vy + vpx * 0.30, anchor="sw", text=s["unit"], fill=TEXT2,
                      font=self.f("sans", max(S(14), vpx * 0.3)))
        stats = {}
        if big:
            for k, (key, label) in enumerate(rows):
                sy = y + h - S(20) - (len(rows) - 1 - k) * S(24)
                c.create_text(x + pad, sy, anchor="w", text=label, fill=TEXT3, font=self.f("sans", S(11), True))
                stats[key] = c.create_text(x + iw - S(12), sy, anchor="e", text="--", fill=TEXT2,
                                           font=self.f("mono", S(14)))

        # área de la gráfica
        px0, px1 = xs
        py0, py1 = y + S(16), y + h - S(16)
        c.create_rectangle(px0, py0, px1, py1, fill=PLOT, outline=BORDER)
        rng = s["y_max"] - s["y_min"]
        special = []                                   # y de umbrales y referencias, para no tapar una con un rótulo
        for lim, tint in ((s.get("warn"), WARN), (s.get("crit"), CRIT),      # umbrales
                          (s.get("warn_lo"), WARN), (s.get("crit_lo"), CRIT)):
            if lim is not None and s["y_min"] < lim < s["y_max"]:
                ty = _ymap(s, py0, py1, lim)
                c.create_line(px0, ty, px1, ty, fill=tint, dash=(2, 6))
                special.append(ty)
        for t in ticks[1:]:
            gx = px1 - t / window * (px1 - px0)
            c.create_line(gx, py0, gx, py1, fill=GRID)
        for k in range(5):
            gv = s["y_min"] + rng * k / 4
            gy = _ymap(s, py0, py1, gv)
            c.create_line(px0, gy, px1, gy, fill=GRID)
            c.create_text(px0 - S(8), gy, anchor="e", fill=TEXT3, font=self.f("mono", S(11)),
                          text=_axis_label(gv, rng / 4))
        refs = [(rv, text, line_c, text_c) for rv, text, line_c, text_c in REFS.get(s["id"], [])
                if s["y_min"] < rv < s["y_max"]]
        special += [_ymap(s, py0, py1, rv) for rv, *_ in refs]
        labels = []
        for rv, text, line_c, text_c in refs:
            ry = _ymap(s, py0, py1, rv)
            c.create_line(px0, ry, px1, ry, fill=line_c, dash=(5, 5))
            crowded = any(ry - S(22) < oy < ry for oy in special)   # otra línea justo arriba
            ly = ry - S(9) if ry - S(9) > py0 + S(26) and not crowded else ry + S(10)
            labels.append((ly, text, text_c))

        # items que se mueven: línea, punto y mensaje
        line = c.create_line(0, 0, 0, 0, fill=TEXT, width=max(2, S(2)), state="hidden")
        r = S(5)
        dot = c.create_oval(0, 0, 0, 0, fill=TEXT, outline=PLOT, width=S(2), state="hidden")
        for ly, text, text_c in labels:                # encima de la traza, con fondo negro para que se lea
            t_id = c.create_text(px1 - S(10), ly, anchor="e", text=text, fill=text_c, font=self.f("sans", S(11)))
            x0, y0b, x1, y1b = c.bbox(t_id)
            c.tag_lower(c.create_rectangle(x0 - S(4), y0b, x1 + S(2), y1b, fill=PLOT, outline=""), t_id)
        msg_bg = c.create_rectangle(0, 0, 0, 0, fill=PLOT, outline="", state="hidden")   # que no lo tache una línea
        msg = c.create_text((px0 + px1) / 2, (py0 + py1) / 2, text="", fill=STALE, font=self.f("sans", S(15), True))
        self.dyn.append((self._upd_graph, {"s": s, "val": val, "line": line, "dot": dot,
                                           "msg": msg, "msg_bg": msg_bg, "stats": stats, "r": r, "window": window,
                                           "box": (px0, py0, px1, py1)}))

    # ── actualización por cuadro ────────────────
    def _snapshot(self):
        with data_lock:
            now = time.time()
            self.hz = sum(1 for t in rate_times if now - t <= 3.0) / 3.0
            return {sid: {"t": list(sd["t"]), "v": list(sd["v"]), "last": sd["last"],
                          "no_data": sd["no_data"]} for sid, sd in sensor_data.items()}

    def _status(self, snap):
        """(texto, color) del enlace con el OBD2."""
        st = link["state"]
        if st == "sim":
            return "SIMULADOR", WARN
        if st == "live":
            ids = [s["id"] for s in SENSORS if s["enabled"] and _obd(s)]
            if time.time() - link["since"] > 5 and all(snap[i]["last"] is None for i in ids):
                return "SIN DATOS ECU", WARN
            return "OBD2 EN VIVO", OK
        return ("CONECTANDO", WARN) if st == "connecting" else ("SIN CONEXIÓN", CRIT)

    def _upd_chrome(self, snap):
        text, color = self._status(snap)
        blink = link["state"] in ("lost", "connecting") and int(time.time() * 2) % 2 == 0
        self._set(self.chip_dot, fill=PANEL if blink else color)
        self._set(self.chip_txt, text=text)
        self._set(self.status_line, fill=color if link["state"] == "lost" or color == WARN and link["state"] == "live" else AXIS)
        live = link["state"] in ("live", "sim")
        self._set(self.rate, text=f"{self.hz:4.1f} Hz", fill=TEXT2 if live else STALE)
        self._set(self.clock, text=time.strftime("%H:%M:%S"))

        # registro y dirección del visor web (la IP puede cambiar si cambia la red)
        if time.time() - self._ip_at > 5:
            self.ip, self._ip_at = _local_ip(), time.time()
        rec = f"REC · SESIÓN {session_id}" if db_ok and session_id else "SIN REGISTRO"
        text = rec + (f"   WEB {self.ip}:{WEB_PORT}" if self.web else "")
        if self._cache.get(self.rec_txt, {}).get("text") != text:
            self._set(self.rec_txt, text=text, fill=TEXT3 if db_ok else WARN)
            x = self.W - self.m - self.measure("mono", self.S(11), False, text) - self.S(14)
            r = self.S(4)
            self.c.coords(self.rec_dot, x - r, self.by - r, x + r, self.by + r)
        self._set(self.rec_dot, fill=CRIT if db_ok else WARN)

    def _trace(self, s, sd, now, window, box):
        x0, y0, x1, y1 = box
        t_min, k = now - window, (x1 - x0) / window
        pts, vals = [], []
        for tv, v in zip(sd["t"], sd["v"]):
            if tv >= t_min:
                pts += (x0 + (tv - t_min) * k, _ymap(s, y0, y1, v))
                vals.append(v)
        return pts, vals

    @staticmethod
    def _value(s, sd, now):
        """(texto, color, obsoleto) del valor actual."""
        age = (now - sd["t"][-1]) if sd["t"] else None
        stale = age is None or age > STALE_S
        if sd["last"] is None:
            return ("N/D" if sd["no_data"] else "--"), STALE, True
        return _fmt(s, sd["last"]), (STALE if stale else _zone_color(s, sd["last"])), stale

    def _upd_tile(self, d, snap, now):
        s, sd = d["s"], snap[d["s"]["id"]]
        text, color, stale = self._value(s, sd, now)
        self._set(d["val"], text=text, fill=color)
        pts, _ = self._trace(s, sd, now, d["window"], d["box"])
        shown = len(pts) >= 4
        if shown:
            self.c.coords(d["line"], *pts)
        self._set(d["line"], state="normal" if shown else "hidden", fill=STALE if stale else TEXT)

    def _upd_graph(self, d, snap, now):
        s, sd, c = d["s"], snap[d["s"]["id"]], self.c
        text, color, stale = self._value(s, sd, now)
        self._set(d["val"], text=text, fill=color)
        pts, vals = self._trace(s, sd, now, d["window"], d["box"])
        shown = len(pts) >= 4
        if shown:
            c.coords(d["line"], *pts)
            c.coords(d["dot"], pts[-2] - d["r"], pts[-1] - d["r"], pts[-2] + d["r"], pts[-1] + d["r"])
        state = "normal" if shown else "hidden"
        lc = STALE if stale else TEXT
        self._set(d["line"], state=state, fill=lc)
        self._set(d["dot"], state=state, fill=lc)

        if s["id"] == "steer" and steer_state["status"] in ("sin_sensor", "sin_iman"):
            msg, mc = ({"sin_sensor": "SIN SENSOR — conecta el AS5600 al I²C",
                        "sin_iman": "SIN IMÁN — el AS5600 no lo detecta (0.5–3 mm)"}[steer_state["status"]], WARN)
        elif s["id"] == "steer" and steer_cal["center"] is None and not MODO_SIMULADOR and sd["v"]:
            msg, mc = "SIN CALIBRAR — fija el centro desde el celular", WARN
        elif sd["last"] is None:
            msg, mc = ("SIN DATOS — el ECU no responde este PID" if sd["no_data"] else "ESPERANDO DATOS"), STALE
        elif stale:
            msg, mc = f"SIN SEÑAL · hace {now - sd['t'][-1]:.0f} s", WARN
        else:
            msg, mc = "", STALE
        if msg != self._cache.get(d["msg"], {}).get("text"):
            self._set(d["msg"], text=msg, fill=mc)
            if msg:
                x0, y0, x1, y1 = self.c.bbox(d["msg"])
                self.c.coords(d["msg_bg"], x0 - self.S(10), y0 - self.S(4), x1 + self.S(10), y1 + self.S(4))
            self._set(d["msg_bg"], state="normal" if msg else "hidden")
        else:
            self._set(d["msg"], fill=mc)

        if d["stats"]:
            st = d["stats"]
            have = bool(vals)
            self._set(st["min"], text=_fmt(s, min(vals)) if have else "--")
            self._set(st["max"], text=_fmt(s, max(vals)) if have else "--")
            self._set(st["avg"], text=_fmt(s, sum(vals) / len(vals)) if have else "--")
            if "cross" in st:
                self._set(st["cross"], text=str(_crossings(sd["t"], sd["v"], now)))

    def _frame(self):
        while True:
            try:
                cmd = self.cmds.get_nowait()
            except queue.Empty:
                break
            self.step(1 if cmd == "next" else -1)
        W, H = self.c.winfo_width(), self.c.winfo_height()
        if W < 100 or H < 100:
            return
        if (W, H, self.vi) != self._built:
            self._built = (W, H, self.vi)
            self._rebuild(W, H)
        snap = self._snapshot()
        now = time.time() - session_t0
        self._upd_chrome(snap)
        for fn, d in self.dyn:
            fn(d, snap, now)

    def _tick(self):
        t = time.perf_counter()
        try:
            self._frame()
        except Exception:
            self._errs += 1
            if self._errs <= 3:
                traceback.print_exc()
        self.root.after(max(1, int(1000 / UI_FPS - (time.perf_counter() - t) * 1000)), self._tick)

    def run(self):
        self.root.mainloop()


def run_headless():
    """Sin pantalla (p. ej. una Pi en la moto sin display): lee, guarda y sirve el visor web."""
    start_reading()
    srv = start_web()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        stop_reading()
        if srv:
            srv.shutdown()


if __name__ == "__main__":
    print("=" * 58)
    print("  ZX-6R TELEMETRÍA — RPM / TPS / TEMP / O2 / MAP / BATERÍA")
    print("=" * 58)
    print(f"  Modo  : {'SIMULADOR' if MODO_SIMULADOR else 'OBD2 REAL'}")
    print(f"  OBD2  : {IP_OBD2}:{PORT_OBD2}")
    print(f"  Datos : {DB_PATH}")
    print(f"  Web   : puerto {WEB_PORT or 'apagado'}")
    print("  Botón : → / espacio / Enter = siguiente · ← = anterior · Esc = salir")
    print("=" * 58)
    run_headless() if HEADLESS else Dash().run()
