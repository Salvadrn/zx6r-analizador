#!/usr/bin/env python3
"""Pretest de la ZX-6R: ¿qué idioma habla la moto y lo entiende tu ELM327?

Se conecta al ELM327 WiFi, prueba OBD2 estándar y después KDS (el diagnóstico de Kawasaki:
KWP2000 por línea K) y te dice qué hacer y qué comprar. Solo LEE datos: no borra códigos
ni escribe nada en la ECU.

Uso (Mac o Pi conectada al WiFi del ELM327, llave en ON, motor apagado):
    python3 tools/pretest.py
    python3 tools/pretest.py --ip 192.168.0.10 --port 35000
    python3 tools/pretest.py --demo kds        # simulación: obd2 | kds | clon | sin-ecu

Guarda todo lo que pasó en ~/obd2_logs/pretest_<fecha>.txt.
"""
import argparse
import os
import re
import socket
import sys
import threading
import time
from datetime import datetime

# PIDs OBD2 que usa la app (modo 01)
APP_PIDS = {0x0C: "RPM", 0x11: "TPS", 0x05: "Temperatura", 0x0B: "MAP", 0x14: "O2"}

# Receta KDS: foro de RaceChrono y Kawaduino (ver docs/conexion.html §6). Todo es de lectura.
KDS_SETUP = ["ATSP5",            # ISO 14230-4 (KWP2000) con inicio rápido
             "ATWM8011F1013E",   # mensaje para mantener viva la sesión (TesterPresent)
             "ATSH8111F1"]       # encabezado: ECU 0x11, este equipo 0xF1
KDS_RETRY_S = 10                 # después de un inicio fallido, la ECU tarda en aceptar otro


class Log:
    """Imprime y guarda: lo que ves en pantalla queda también en el archivo."""

    def __init__(self):
        self.lines = []

    def __call__(self, text=""):
        print(text, flush=True)
        self.lines.append(text)

    def save(self, folder, demo):
        os.makedirs(folder, exist_ok=True)
        name = datetime.now().strftime("pretest_%Y%m%d_%H%M%S") + (f"_demo-{demo}" if demo else "") + ".txt"
        path = os.path.join(folder, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(self.lines) + "\n")
        return path


class Elm:
    def __init__(self, ip, port, log):
        self.log = log
        self.sock = socket.create_connection((ip, port), timeout=5)

    def cmd(self, command, timeout=4.0):
        """Manda un comando y espera el prompt '>'. Devuelve la respuesta en una línea."""
        self._drain()
        self.sock.sendall((command + "\r").encode())
        buf, end = b"", time.time() + timeout
        while b">" not in buf and time.time() < end:
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                chunk = self.sock.recv(1024)
            except socket.timeout:
                break
            if not chunk:
                raise ConnectionError("el ELM327 cerró la conexión")
            buf += chunk
        text = re.sub(r"\s+", " ", buf.decode(errors="ignore").replace(">", " ")).strip()
        self.log(f"    {command:<16} → {text or '(sin respuesta)'}")
        return text

    def _drain(self):
        """Tira respuestas tardías para no confundirlas con la del siguiente comando."""
        self.sock.settimeout(0)
        try:
            while self.sock.recv(1024):
                pass
        except (BlockingIOError, socket.timeout, OSError):
            pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def hexbytes(text):
    """Los bytes hex de una respuesta ('41 00 BE 1F' → [0x41, 0x00, 0xBE, 0x1F])."""
    return [int(t, 16) for t in text.upper().split() if re.fullmatch(r"[0-9A-F]{2}", t)]


def find(seq, pattern):
    """Posición de `pattern` dentro de `seq`, o -1."""
    for i in range(len(seq) - len(pattern) + 1):
        if seq[i:i + len(pattern)] == pattern:
            return i
    return -1


def refused(text):
    return "?" in text.split()


# ─────────────────────────────────────────────────────────────
def run(elm, log, scan):
    r = {"version": "", "volts": None, "obd2": False, "obd2_pids": {}, "custom": True,
         "bus_init": None, "kds": False, "kds_ids": {}}

    log("1 · El adaptador")
    elm.cmd("ATZ", 5)
    elm.cmd("ATE0")
    r["version"] = elm.cmd("ATI")
    stn = elm.cmd("STI")                       # los OBDLink (chip STN) contestan; los ELM327, "?"
    if stn and not refused(stn):
        r["version"] += f" · {stn}"
    volts = re.search(r"(\d{1,2}\.\d)\s*V", elm.cmd("ATRV"))
    r["volts"] = float(volts.group(1)) if volts else None

    log("\n2 · OBD2 estándar (lo que la app usa hoy)")
    elm.cmd("ATH0")
    elm.cmd("ATSP0")
    answer = elm.cmd("0100", 15)               # con protocolo automático, la búsqueda tarda
    data = hexbytes(answer)
    i = find(data, [0x41, 0x00])
    if i >= 0 and len(data) >= i + 6:
        r["obd2"] = True
        mask = int.from_bytes(bytes(data[i + 2:i + 6]), "big")
        for pid, name in APP_PIDS.items():
            listed = bool(mask >> (32 - pid) & 1)
            value = hexbytes(elm.cmd(f"01{pid:02X}", 4))
            r["obd2_pids"][name] = listed and find(value, [0x41, pid]) >= 0
        return r                               # si habla OBD2 estándar, KDS no hace falta

    log("\n3 · KDS (el diagnóstico de Kawasaki)")
    elm.cmd("ATPC")
    elm.cmd("ATH1")                            # con encabezados, para ver quién contesta
    for command in KDS_SETUP:
        if refused(elm.cmd(command)):
            r["custom"] = False
            return r
    for attempt in (1, 2):
        init = elm.cmd("ATFI", 10)
        if refused(init):
            r["custom"] = False
            return r
        r["bus_init"] = "OK" in init and "ERROR" not in init
        if r["bus_init"] or attempt == 2:
            break
        log(f"    (espero {KDS_RETRY_S} s: la ECU no acepta otro inicio enseguida)")
        time.sleep(KDS_RETRY_S)
    if not r["bus_init"]:
        return r

    elm.cmd("ATSH8011F1")
    elm.cmd("1080", 6)                         # abre la sesión de diagnóstico (Kawaduino)
    rpm = hexbytes(elm.cmd("2109", 6))         # 21 09 = RPM
    r["kds"] = find(rpm, [0x61, 0x09]) >= 0
    if r["kds"] and scan:
        log("\n4 · Qué datos da la ECU por KDS (21 00 a 21 7F, solo lectura)")
        for lid in range(0x00, 0x80):
            data = hexbytes(elm.cmd(f"21{lid:02X}", 2))
            i = find(data, [0x61, lid])
            if i >= 0:
                r["kds_ids"][lid] = data[i + 2:-1]  # sin el checksum del final
    elm.cmd("ATPC")
    return r


def verdict(r, log):
    log("\n" + "=" * 60)
    log("RESULTADO")
    log("=" * 60)
    log(f"Adaptador: {r['version'] or 'no contestó ATI'}")
    if r["volts"] is not None:
        mark = "✓" if 11.5 <= r["volts"] <= 15.5 else "✗ revisa la batería y el cable (pines 16 y 4/5)"
        log(f"Voltaje que recibe el ELM: {r['volts']:.1f} V {mark}")

    if r["obd2"]:
        log("\n✓ Tu moto habla OBD2 estándar. La app funciona tal cual con tu ELM327.")
        log("  No compres el OBDLink EX.")
        for name, ok in r["obd2_pids"].items():
            log(f"  {'✓' if ok else '✗'} {name}" + ("" if ok else "  (la ECU no lo da: saldrá N/D)"))
        return "obd2"
    if not r["custom"]:
        log("\n✗ Tu moto no habla OBD2 estándar y tu ELM327 no acepta el inicio KDS")
        log("  (contestó «?» a un comando de configuración). Es un clon limitado.")
        log("  → Compra el OBDLink EX (fase 3 del BOM). El cable adaptador sí te sirve.")
        return "comprar-obdlink"
    if not r["bus_init"]:
        log("\n✗ No habla OBD2 estándar y la ECU no contestó al inicio KDS (BUS INIT: ERROR).")
        log("  Revisa, en este orden:")
        log("   1. Llave en ON y el interruptor de paro en RUN.")
        log("   2. Que el adaptador lleve la línea K al pin 7 del OBD2 (continuidad, §2 de la hoja).")
        log("   3. Repite el pretest. Si sigue igual, puede ser el cable o el ELM327.")
        return "revisar-cable"
    if r["kds"]:
        log("\n✓ Tu moto habla KDS y tu ELM327 lo entiende. No compres el OBDLink EX.")
        log("  Falta que la app hable KDS: mándame este archivo y lo programo con lo que contestó tu ECU.")
        if r["kds_ids"]:
            log(f"  La ECU dio {len(r['kds_ids'])} datos distintos:")
            for lid, data in sorted(r["kds_ids"].items()):
                log(f"    21 {lid:02X} → {' '.join(f'{b:02X}' for b in data)}")
        return "kds"
    log("\n? La ECU aceptó el inicio KDS pero no contestó la RPM (21 09).")
    log("  Mándame este archivo: con la respuesta exacta se ve qué falta.")
    return "revisar-log"


# ─────────────────────────────────────────────────────────────
# Simulación (--demo): un ELM327 falso, para ver cómo se ve cada resultado sin la moto
def fake_elm(scenario):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    kds_ids = {0x04: "00 1A", 0x05: "02 F8", 0x06: "8C", 0x07: "6E", 0x09: "0B B8", 0x0A: "8E",
               0x0B: "00", 0x0C: "00 00"}

    def reply(cmd):
        if cmd == "ATZ":
            return "\r\rELM327 v1.5"
        if cmd == "ATI":
            return "ELM327 v1.5"
        if cmd == "STI":
            return "?"
        if cmd == "ATRV":
            return "12.6V"
        if cmd == "ATFI":
            return {"kds": "BUS INIT: ...OK", "sin-ecu": "BUS INIT: ...ERROR", "clon": "?"}.get(scenario, "OK")
        if cmd.startswith("ATWM") and scenario == "clon":
            return "?"
        if cmd.startswith("AT"):
            return "OK"
        if scenario == "obd2":
            return {"0100": "SEARCHING...\r41 00 BE 1F A8 13", "010C": "41 0C 0B B8", "0111": "41 11 26",
                    "0105": "41 05 5A", "010B": "41 0B 21"}.get(cmd, "NO DATA")
        if cmd.startswith("01"):
            return "SEARCHING...\rUNABLE TO CONNECT"
        if scenario == "kds" and cmd == "1080":
            return "80 F1 11 02 50 80 54"
        if scenario == "kds" and cmd.startswith("21"):
            lid = int(cmd[2:], 16)
            if lid in kds_ids:
                return f"80 F1 11 0{2 + len(kds_ids[lid].split())} 61 {lid:02X} {kds_ids[lid]} 5A"
            return f"80 F1 11 03 7F 21 {lid:02X} 12"
        return "NO DATA"

    def serve():
        conn, _ = srv.accept()
        buf = b""
        with conn:
            while True:
                try:
                    data = conn.recv(256)
                except OSError:
                    return
                if not data:
                    return
                buf += data
                while b"\r" in buf:
                    line, buf = buf.split(b"\r", 1)
                    cmd = line.decode().strip().upper().replace(" ", "")
                    if cmd:
                        time.sleep(0.01)
                        conn.sendall((reply(cmd) + "\r\r>").encode())

    threading.Thread(target=serve, daemon=True).start()
    return srv.getsockname()[1]


# ─────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Pretest de la ZX-6R: ¿OBD2 estándar o KDS?")
    ap.add_argument("--ip", default=os.environ.get("OBD2_IP", "192.168.0.10"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("OBD2_PORT", "35000")))
    ap.add_argument("--demo", choices=["obd2", "kds", "clon", "sin-ecu"],
                    help="simula un ELM327 para ver cómo se ve cada resultado")
    ap.add_argument("--no-scan", action="store_true", help="no recorre los datos KDS (más rápido)")
    ap.add_argument("--log-dir", default=os.path.join(os.path.expanduser("~"), "obd2_logs"))
    args = ap.parse_args()

    log = Log()
    log(f"PRETEST ZX-6R · {datetime.now():%Y-%m-%d %H:%M}" + (f" · SIMULACIÓN ({args.demo})" if args.demo else ""))
    if args.demo:
        args.ip, args.port = "127.0.0.1", fake_elm(args.demo)
    else:
        print("\nAntes de empezar:")
        print("  1. Cable adaptador en el conector KDS (bajo el asiento) y el ELM327 en el lado de 16 pines.")
        print("  2. Llave en ON y motor apagado, con la moto parada.")
        print("  3. Esta computadora conectada al WiFi del ELM327.")
        try:
            input("Enter para empezar (Ctrl+C para salir) ")
        except (KeyboardInterrupt, EOFError):
            print()
            return 1

    try:
        elm = Elm(args.ip, args.port, log)
    except OSError as e:
        log(f"\n✗ No me pude conectar al ELM327 en {args.ip}:{args.port} ({e}).")
        log("  ¿Está prendido (llave en ON) y esta computadora está en su WiFi?")
        log(f"\nRegistro: {log.save(args.log_dir, args.demo)}")
        return 2
    try:
        result = verdict(run(elm, log, scan=not args.no_scan), log)
    except (OSError, ConnectionError) as e:
        log(f"\n✗ Se cortó la conexión con el ELM327 a medio pretest ({e}). Repítelo.")
        result = "cortado"
    finally:
        elm.close()
    log(f"\nRegistro completo: {log.save(args.log_dir, args.demo)}")
    return 0 if result in ("obd2", "kds") else 1


if __name__ == "__main__":
    sys.exit(main())
