# ZX-6R Analizador de sensores

Lee TPS, temperatura, O2 y MAP por un ELM327 WiFi, los grafica en vivo y guarda cada sesión en CSV (`~/obd2_logs`).

```bash
pip install -r requirements.txt
python3 analizador.py            # OBD2 real (192.168.0.10:35000)
OBD2_SIM=1 python3 analizador.py # simulador
```

Variables: `OBD2_IP`, `OBD2_PORT`, `OBD2_SIM`. Esc para salir.

- O2 se lee en cada vuelta (~8 Hz con el adaptador simulado); los demás sensores rotan uno por vuelta.
- MAP DER (`2201F0`, propietario Kawasaki) viene apagado; un ELM327 estándar casi nunca lo responde.
- Probado: parsers y hilo de lectura contra un ELM falso. La GUI no se ha corrido con la moto.
