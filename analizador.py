"""ZX-6R — Analizador de sensores OBD2 (TPS, temperatura, O2, MAP).

Lee PIDs por un ELM327 WiFi, los grafica en tiempo real y guarda cada
sesión en CSV. Con OBD2_SIM=1 corre con datos simulados.
"""
import customtkinter as ctk
import tkinter as tk
import threading
import socket
import time
import math
import csv
import os
from datetime import datetime
from collections import deque

# =====================================================
# CONFIGURACIÓN
# =====================================================
MODO_SIMULADOR = os.environ.get("OBD2_SIM", "0") == "1"
IP_OBD2        = os.environ.get("OBD2_IP", "192.168.0.10")
PORT_OBD2      = int(os.environ.get("OBD2_PORT", "35000"))
DELAY_CMD      = 0.02     # pausa mínima entre comandos (se espera el prompt '>')
CMD_TIMEOUT    = 1.5      # s máximos esperando respuesta a un comando

GRAPH_SECONDS  = 60
MAX_POINTS     = 1200     # tope por sensor (O2 llega a ~15 Hz)
SIM_HZ         = 10

CSV_DIR = os.path.join(os.path.expanduser("~"), "obd2_logs")

# =====================================================
# COLORES
# =====================================================
BG        = "#0A0A0A"
BG_PANEL  = "#111111"
BG_CARD   = "#161616"
BG_CARD2  = "#1E1E1E"
GRID      = "#1E1E1E"
AXIS      = "#333333"

C_TPS   = "#00A3FF"
C_TEMP  = "#FF5A00"
C_O2    = "#00FF88"
C_MAP_L = "#FFD700"
C_MAP_R = "#BB44FF"

TEXT_B  = "#E0E0E0"
TEXT_D  = "#505050"
TEXT_W  = "#FF4444"
TEXT_G  = "#00FF88"

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


# fast=True: se lee en cada vuelta (O2 necesita tasa alta para ver la oscilación)
SENSORS = [
    {"id": "tps",   "pid": "0111",   "name": "TPS",         "unit": "%",   "color": C_TPS,
     "y_min": 0,  "y_max": 100, "parser": _p_tps,   "enabled": True,  "fast": False},
    {"id": "temp",  "pid": "0105",   "name": "TEMP REFRIG", "unit": "°C",  "color": C_TEMP,
     "y_min": 40, "y_max": 130, "parser": _p_temp,  "enabled": True,  "fast": False},
    {"id": "o2",    "pid": "0114",   "name": "SENSOR O2",   "unit": "V",   "color": C_O2,
     "y_min": 0,  "y_max": 1.0, "parser": _p_o2,    "enabled": True,  "fast": True},
    {"id": "map_l", "pid": "010B",   "name": "MAP IZQ",     "unit": "kPa", "color": C_MAP_L,
     "y_min": 15, "y_max": 110, "parser": _p_map_l, "enabled": True,  "fast": False},
    # Apagado por defecto: el ELM327 estándar casi nunca responde este PID.
    {"id": "map_r", "pid": "2201F0", "name": "MAP DER",     "unit": "kPa", "color": C_MAP_R,
     "y_min": 15, "y_max": 110, "parser": _p_map_r, "enabled": False, "fast": False},
]

# =====================================================
# ESTADO
# =====================================================
data_lock = threading.Lock()
sensor_data = {}          # id -> {"t","v","last","no_data"}
obd_ok = False
session_t0 = None         # time.time() al iniciar la sesión
session_start = None      # datetime
csv_path = None
csv_writer = None
csv_file = None
stop_event = threading.Event()
worker = None             # hilo de lectura vigente


def reset_data():
    with data_lock:
        for s in SENSORS:
            sensor_data[s["id"]] = {"t": deque(maxlen=MAX_POINTS),
                                    "v": deque(maxlen=MAX_POINTS),
                                    "last": None, "no_data": False}


reset_data()

# =====================================================
# OBD2
# =====================================================
def _send(sock, cmd):
    """Manda un comando y lee hasta el prompt '>' del ELM327."""
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
    sock.settimeout(0.5)
    sock.connect((IP_OBD2, PORT_OBD2))
    for cmd in ["ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0"]:
        _send(sock, cmd)
    return sock


def _sim_value(sid, t):
    if sid == "tps":
        return max(0, min(100, 20 + 55 * abs(math.sin(t * 0.4)) + 8 * math.sin(t * 3.1)))
    if sid == "temp":
        return 78 + 12 * math.sin(t * 0.04)
    if sid == "o2":
        return max(0.0, min(1.0, round(0.5 + 0.4 * math.sin(t * 6.0) + 0.05 * math.sin(t * 17.3), 3)))
    tps = 20 + 55 * abs(math.sin(t * 0.4))
    if sid == "map_l":
        return max(20, min(105, 30 + tps * 0.72 + 5 * math.sin(t * 2.1)))
    if sid == "map_r":
        return max(20, min(105, 28 + tps * 0.74 + 5 * math.sin(t * 2.1 + 0.3)))
    return 0.0


def _record(sid, t, val, row):
    with data_lock:
        sd = sensor_data[sid]
        if val is None:
            sd["no_data"] = True
            return
        sd["t"].append(t)
        sd["v"].append(val)
        sd["last"] = val
        sd["no_data"] = False
    row[sid] = val


def read_thread(stop, t0):
    """Un hilo por sesión. Cada vuelta: sensores rápidos + 1 lento (round-robin)."""
    global obd_ok
    sock = None
    slow_i = 0
    try:
        while not stop.is_set():
            enabled = [s for s in SENSORS if s["enabled"]]
            fast = [s for s in enabled if s["fast"]]
            slow = [s for s in enabled if not s["fast"]]
            batch = list(fast)
            if slow:
                batch.append(slow[slow_i % len(slow)])
                slow_i += 1
            if not batch:
                stop.wait(0.2)
                continue

            if MODO_SIMULADOR:
                obd_ok = True
                t = time.time() - t0
                row = {"timestamp": round(t, 3)}
                for s in enabled:
                    _record(s["id"], t, _sim_value(s["id"], t), row)
                _write_csv(row)
                stop.wait(1 / SIM_HZ)
                continue

            try:
                if sock is None:
                    sock = _connect()
                    obd_ok = True
                    print("✅ [SENSORES] Conectado")
                for s in batch:
                    if stop.is_set():
                        break
                    resp = _send(sock, s["pid"])
                    t = time.time() - t0
                    row = {"timestamp": round(t, 3)}
                    _record(s["id"], t, s["parser"](resp), row)
                    _write_csv(row)
            except (OSError, ConnectionError) as e:
                print(f"❌ [SENSORES] {e} — reintentando...")
                obd_ok = False
                if sock:
                    try: sock.close()
                    except OSError: pass
                    sock = None
                stop.wait(2)
    finally:
        obd_ok = False
        if sock:
            try: sock.close()
            except OSError: pass


# =====================================================
# CSV
# =====================================================
def _write_csv(row):
    w, f = csv_writer, csv_file
    if w and f:
        try:
            w.writerow(row)
            f.flush()
        except (ValueError, OSError):
            pass


def start_csv():
    global csv_path, csv_writer, csv_file, session_start
    os.makedirs(CSV_DIR, exist_ok=True)
    session_start = datetime.now()
    csv_path = os.path.join(CSV_DIR, session_start.strftime("sesion_%Y%m%d_%H%M%S.csv"))
    csv_file = open(csv_path, "w", newline="", encoding="utf-8")
    csv_writer = csv.DictWriter(csv_file, fieldnames=["timestamp"] + [s["id"] for s in SENSORS])
    csv_writer.writeheader()
    print(f"📄 Guardando en: {csv_path}")


def stop_csv():
    global csv_writer, csv_file
    csv_writer = None
    if csv_file:
        try: csv_file.close()
        except OSError: pass
        csv_file = None


# =====================================================
# UI
# =====================================================
class SensorGrapher:
    def __init__(self):
        ctk.set_appearance_mode("dark")
        self.root = ctk.CTk()
        self.root.title("ZX-6R — Analizador de Sensores")
        self.root.attributes("-fullscreen", True)
        self.root.configure(fg_color=BG)
        self.root.bind("<Escape>", lambda e: self._quit())

        self.sw = self.root.winfo_screenwidth()
        self.sh = self.root.winfo_screenheight()

        self.fT = ("Arial", 20, "bold")
        self.fS = ("Arial", 12, "bold")
        self.fXS = ("Arial", 11, "normal")

        self.sensor_vars = {}
        self.has_session = False   # hay datos de una sesión (activa o terminada)
        self.t_end = 0.0           # instante final de la gráfica al congelarla
        self._build_ui()
        self._tick()

    @property
    def running(self):
        return self.has_session and not stop_event.is_set()

    def _build_ui(self):
        hdr = ctk.CTkFrame(self.root, fg_color=BG_PANEL, height=62)
        hdr.pack(fill="x", side="top")
        hdr.pack_propagate(False)

        ctk.CTkLabel(hdr, text="📊  ZX-6R ANALIZADOR DE SENSORES",
                     font=self.fT, text_color=C_TPS).pack(side="left", padx=18)
        self.lbl_status = ctk.CTkLabel(hdr, text="● Detenido", font=self.fXS, text_color=TEXT_D)
        self.lbl_status.pack(side="left", padx=10)

        bf = ctk.CTkFrame(hdr, fg_color="transparent")
        bf.pack(side="right", padx=14)
        self.btn_start = ctk.CTkButton(
            bf, text="▶️  INICIAR SESIÓN", font=self.fS, fg_color="#003A00",
            border_color=TEXT_G, border_width=2, text_color=TEXT_G,
            hover_color="#005000", width=175, height=40, command=self._start_session)
        self.btn_start.pack(side="left", padx=4)
        self.btn_stop = ctk.CTkButton(
            bf, text="⏹  DETENER", font=self.fS, fg_color="#3A0000",
            border_color=TEXT_W, border_width=2, text_color=TEXT_W,
            hover_color="#550000", width=130, height=40,
            command=self._stop_session, state="disabled")
        self.btn_stop.pack(side="left", padx=4)
        ctk.CTkButton(bf, text="✕ SALIR", font=self.fS, fg_color="#1A1A1A",
                      hover_color="#2A2A2A", text_color=TEXT_D, width=90, height=40,
                      command=self._quit).pack(side="left", padx=4)

        body = ctk.CTkFrame(self.root, fg_color="transparent")
        body.pack(fill="both", expand=True, padx=6, pady=6)

        left_w = max(220, int(self.sw * 0.18))
        left = ctk.CTkFrame(body, fg_color=BG_PANEL, width=left_w, corner_radius=10)
        left.pack(side="left", fill="y", padx=(0, 6))
        left.pack_propagate(False)
        ctk.CTkLabel(left, text="SENSORES ACTIVOS", font=self.fS, text_color=TEXT_D).pack(pady=(14, 6))

        self.val_labels = {}
        for s in SENSORS:
            sid = s["id"]
            var = ctk.BooleanVar(value=s["enabled"])
            self.sensor_vars[sid] = var
            card = ctk.CTkFrame(left, fg_color=BG_CARD2, corner_radius=8)
            card.pack(fill="x", padx=10, pady=4)
            row = ctk.CTkFrame(card, fg_color="transparent")
            row.pack(fill="x", padx=8, pady=(6, 2))
            tk.Canvas(row, width=12, height=12, bg=s["color"],
                      highlightthickness=0).pack(side="left", padx=(0, 6))
            ctk.CTkCheckBox(
                row, text=s["name"], variable=var, font=self.fS, text_color=s["color"],
                fg_color=s["color"], hover_color=s["color"], checkmark_color=BG,
                command=lambda sid=sid, v=var: self._toggle_sensor(sid, v)).pack(side="left")
            lbl = ctk.CTkLabel(card, text="-- " + s["unit"], font=("Arial", 18, "bold"),
                               text_color=s["color"])
            lbl.pack(pady=(0, 8))
            self.val_labels[sid] = lbl

        ctk.CTkFrame(left, fg_color="#222", height=1).pack(fill="x", padx=10, pady=14)
        ctk.CTkLabel(left, text="SESIÓN ACTUAL", font=self.fS, text_color=TEXT_D).pack(pady=(0, 6))
        self.lbl_duration = ctk.CTkLabel(left, text="Duración: --", font=self.fXS, text_color=TEXT_B)
        self.lbl_duration.pack()
        self.lbl_samples = ctk.CTkLabel(left, text="Muestras: --", font=self.fXS, text_color=TEXT_B)
        self.lbl_samples.pack()
        self.lbl_file = ctk.CTkLabel(left, text="Archivo: --", font=("Arial", 9, "normal"),
                                     text_color=TEXT_D, wraplength=left_w - 20)
        self.lbl_file.pack(pady=(4, 0))

        ctk.CTkFrame(left, fg_color="#222", height=1).pack(fill="x", padx=10, pady=14)
        ctk.CTkLabel(
            left,
            text="ℹ  MAP DER usa PID\npropietario Kawasaki y\nviene apagado. Un ELM327\nestándar casi nunca lo\nresponde.",
            font=("Arial", 10, "normal"), text_color=TEXT_D, justify="left").pack(padx=10)

        right = ctk.CTkFrame(body, fg_color=BG_PANEL, corner_radius=10)
        right.pack(side="left", fill="both", expand=True)
        gh = ctk.CTkFrame(right, fg_color="transparent", height=36)
        gh.pack(fill="x", padx=10, pady=(10, 4))
        gh.pack_propagate(False)
        ctk.CTkLabel(gh, text="GRÁFICAS EN TIEMPO REAL", font=self.fS, text_color=TEXT_D).pack(side="left")
        ctk.CTkLabel(gh, text=f"Ventana: {GRAPH_SECONDS}s", font=self.fXS,
                     text_color=TEXT_D).pack(side="right")
        self.graph_canvas = tk.Canvas(right, bg=BG_CARD, highlightthickness=1,
                                      highlightbackground="#222")
        self.graph_canvas.pack(fill="both", expand=True, padx=8, pady=(0, 8))

    def _toggle_sensor(self, sid, var):
        for s in SENSORS:
            if s["id"] == sid:
                s["enabled"] = var.get()

    # ── sesión ──────────────────────────────────
    def _start_session(self):
        global worker, session_t0
        if worker is not None and worker.is_alive():
            return                      # el hilo anterior aún está cerrando
        reset_data()
        start_csv()
        stop_event.clear()
        session_t0 = time.time()
        worker = threading.Thread(target=read_thread, args=(stop_event, session_t0),
                                  daemon=True, name="sensor-reader")
        worker.start()
        self.has_session = True
        self.btn_start.configure(state="disabled")
        self.btn_stop.configure(state="normal")

    def _stop_session(self):
        if not self.running:
            return
        stop_event.set()
        self.t_end = time.time() - session_t0
        stop_csv()
        self.btn_stop.configure(state="disabled")   # Iniciar se reactiva en _tick al morir el hilo
        self.lbl_status.configure(text="● Cerrando...", text_color=TEXT_D)

    def _quit(self):
        stop_event.set()
        stop_csv()
        self.root.quit()

    # ── dibujo ──────────────────────────────────
    def _snapshot(self):
        with data_lock:
            return {sid: {"t": list(sd["t"]), "v": list(sd["v"]),
                          "last": sd["last"], "no_data": sd["no_data"]}
                    for sid, sd in sensor_data.items()}

    def _draw_graphs(self):
        c = self.graph_canvas
        cw, ch = c.winfo_width(), c.winfo_height()
        c.delete("all")
        if cw < 10 or ch < 10:
            return

        active = [s for s in SENSORS if s["enabled"]]
        n = len(active)
        if n == 0:
            c.create_text(cw // 2, ch // 2, text="Activa al menos un sensor",
                          fill=TEXT_D, font=("Arial", 16))
            return
        if not self.has_session:
            c.create_text(cw // 2, ch // 2, text="Presiona ▶️ INICIAR SESIÓN para comenzar",
                          fill=TEXT_D, font=("Arial", 16))
            return

        snap = self._snapshot()
        pad_top, pad_bottom, pad_left, pad_right = 8, 22, 58, 12
        gh = (ch - pad_top - pad_bottom) // n
        gw = cw - pad_left - pad_right

        t_now = (time.time() - session_t0) if self.running else self.t_end
        t_min = max(0, t_now - GRAPH_SECONDS)
        span = GRAPH_SECONDS

        c.create_line(pad_left, ch - pad_bottom, cw - pad_right, ch - pad_bottom, fill=AXIS)
        for tick in range(0, GRAPH_SECONDS + 1, 10):
            x = pad_left + int(tick / GRAPH_SECONDS * gw)
            lbl = f"-{GRAPH_SECONDS - tick}s" if tick < GRAPH_SECONDS else "ahora"
            c.create_line(x, ch - pad_bottom - 3, x, ch - pad_bottom + 3, fill=AXIS)
            c.create_text(x, ch - pad_bottom + 10, text=lbl, fill=TEXT_D,
                          font=("Arial", 9), anchor="n")

        for idx, s in enumerate(active):
            sd = snap[s["id"]]
            y0 = pad_top + idx * gh
            y1 = y0 + gh - 2
            color = s["color"]
            y_range = s["y_max"] - s["y_min"]

            c.create_rectangle(pad_left, y0, cw - pad_right, y1,
                               fill=BG_CARD if idx % 2 == 0 else BG_CARD2, outline=GRID)
            for gi in range(1, 4):
                gy = y0 + int(gh * gi / 4)
                c.create_line(pad_left, gy, cw - pad_right, gy, fill=GRID, dash=(3, 6))
                c.create_text(pad_left - 4, gy, text=f"{s['y_max'] - y_range * gi / 4:.0f}",
                              fill=TEXT_D, font=("Arial", 8), anchor="e")
            c.create_text(pad_left - 4, y1, text=f"{s['y_min']:.0f}", fill=TEXT_D,
                          font=("Arial", 8), anchor="e")
            c.create_text(pad_left - 4, y0 + 4, text=f"{s['y_max']:.0f}", fill=TEXT_D,
                          font=("Arial", 8), anchor="e")
            c.create_text(pad_left + 6, y0 + 10, text=s["name"], fill=color,
                          font=("Arial", 11, "bold"), anchor="w")

            if not sd["v"]:
                msg = "SIN DATOS — el ECU no responde este PID" if sd["no_data"] else "Esperando datos..."
                c.create_text(pad_left + gw // 2, y0 + gh // 2, text=msg,
                              fill=TEXT_D, font=("Arial", 10))
                continue

            pts = []
            for tv, val in zip(sd["t"], sd["v"]):
                if tv < t_min:
                    continue
                px = pad_left + int((tv - t_min) / span * gw)
                norm = (val - s["y_min"]) / max(y_range, 0.001)
                py = max(y0 + 2, min(y1 - 2, y1 - int(norm * (gh - 10))))
                pts.extend([px, py])
            if len(pts) >= 4:
                c.create_line(*pts, fill=color, width=2)
                c.create_oval(pts[-2] - 4, pts[-1] - 4, pts[-2] + 4, pts[-1] + 4,
                              fill=color, outline=BG, width=2)
            if sd["last"] is not None:
                c.create_text(cw - pad_right - 4, y0 + 10, text=f"{sd['last']:.2f} {s['unit']}",
                              fill=color, font=("Arial", 11, "bold"), anchor="e")

            if s["id"] == "o2":
                ry = y1 - int((0.45 - s["y_min"]) / y_range * (gh - 10))
                c.create_line(pad_left, ry, cw - pad_right, ry, fill="#334433", dash=(4, 4))
                c.create_text(pad_left + 6, ry - 8, text="λ=1 (0.45V)", fill="#446644",
                              font=("Arial", 8), anchor="w")
            if s["id"] in ("map_l", "map_r"):
                ry = y1 - int((101 - s["y_min"]) / y_range * (gh - 10))
                if y0 < ry < y1:
                    c.create_line(pad_left, ry, cw - pad_right, ry, fill="#332222", dash=(4, 4))
                    c.create_text(pad_left + 6, ry - 8, text="101kPa (atm)", fill="#553333",
                                  font=("Arial", 8), anchor="w")

    def _update_sidebar(self):
        snap = self._snapshot()
        for s in SENSORS:
            sd, lbl = snap[s["id"]], self.val_labels[s["id"]]
            if sd["last"] is not None and not sd["no_data"]:
                lbl.configure(text=f"{sd['last']:.2f} {s['unit']}", text_color=s["color"])
            elif sd["no_data"]:
                lbl.configure(text="SIN DATOS", text_color=TEXT_D)

        if self.has_session and session_t0:
            dur = int((time.time() - session_t0) if self.running else self.t_end)
            m, sec = divmod(dur, 60)
            self.lbl_duration.configure(text=f"Duración: {m:02d}:{sec:02d}")
            self.lbl_samples.configure(text=f"Muestras: {sum(len(d['v']) for d in snap.values())}")
            if csv_path:
                self.lbl_file.configure(text=f"📄 {os.path.basename(csv_path)}", text_color=C_O2)

        if self.running:
            self.lbl_status.configure(
                text="● Leyendo sensores" if obd_ok else "● Reconectando...",
                text_color=C_O2 if obd_ok else TEXT_W)
        elif self.has_session:
            if worker is not None and worker.is_alive():
                self.lbl_status.configure(text="● Cerrando...", text_color=TEXT_D)
            else:
                self.lbl_status.configure(text="● Sesión guardada", text_color=C_O2)
                self.btn_start.configure(state="normal")

    def _tick(self):
        self._update_sidebar()
        self._draw_graphs()
        self.root.after(100, self._tick)

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    print("=" * 58)
    print("  ZX-6R ANALIZADOR — TPS / TEMP / O2 / MAP IZQ / MAP DER")
    print("=" * 58)
    print(f"  Modo  : {'SIMULADOR' if MODO_SIMULADOR else 'OBD2 REAL'}")
    print(f"  OBD2  : {IP_OBD2}:{PORT_OBD2}")
    print(f"  Logs  : {CSV_DIR}")
    print("=" * 58)
    SensorGrapher().run()
