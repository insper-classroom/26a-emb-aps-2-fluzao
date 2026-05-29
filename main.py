#!/usr/bin/env python3
"""
Controle Traffic Rider - receptor no PC (adaptado do main.py do python-mouse).
Mesma GUI, mesmo parsing (SYNC 0xFF, AXIS, LSB, MSB), mas em vez de mover o
mouse ele aperta/solta as teclas do jogo.

Protocolo (4 bytes):  SYNC(0xFF)  AXIS  VAL_0(LSB)  VAL_1(MSB)
  AXIS 0 -> estercamento, valor com sinal em [-255, +255]
  AXIS 1 -> bitmask: bit0 acel, bit1 freio, bit2 buzina, bit3 pause, bit4 wheelie

Dependencias: pyserial, pyautogui  (mesmas do lab).
"""

import sys
import glob
import threading

import serial
import pyautogui
pyautogui.PAUSE = 0
pyautogui.FAILSAFE = False

import tkinter as tk
from tkinter import ttk
from tkinter import messagebox

# ---------------------------------------------------------------------------
# Mapeamento
# ---------------------------------------------------------------------------
AXIS_STEER = 0
AXIS_BUTTONS = 1

STEER_THRESHOLD = 60   # valor analogico acima disso -> vira a moto

BIT_KEYS = {
    0: 'up',     # acelerar
    1: 'down',   # frear
    2: 'h',      # buzina
    3: 'p',      # pause
    4: 'y',      # wheelie (gesto IA - fase futura)
}

# Estado global das teclas (so press/release na mudanca).
_pressed = set()
_lock = threading.Lock()
_running = False
_ser = None


def key_set(key, want_down):
    with _lock:
        is_down = key in _pressed
        if want_down and not is_down:
            pyautogui.keyDown(key)
            _pressed.add(key)
        elif not want_down and is_down:
            pyautogui.keyUp(key)
            _pressed.discard(key)


def release_all():
    with _lock:
        for key in list(_pressed):
            pyautogui.keyUp(key)
        _pressed.clear()


def parse_data(data):
    """axis + valor (2 bytes little-endian com sinal) - identico ao seu main.py."""
    axis = data[0]
    value = int.from_bytes(data[1:3], byteorder='little', signed=True)
    return axis, value


def apply_frame(axis, value):
    if axis == AXIS_STEER:
        if value > STEER_THRESHOLD:
            key_set('right', True)
            key_set('left', False)
        elif value < -STEER_THRESHOLD:
            key_set('left', True)
            key_set('right', False)
        else:
            key_set('left', False)
            key_set('right', False)
    elif axis == AXIS_BUTTONS:
        for bit, key in BIT_KEYS.items():
            key_set(key, bool(value & (1 << bit)))


def controle(ser):
    """Loop de leitura: espera 0xFF e le os 3 bytes seguintes."""
    global _running
    while _running:
        sync_byte = ser.read(size=1)
        if not sync_byte:
            continue
        if sync_byte[0] == 0xFF:
            data = ser.read(size=3)
            if len(data) < 3:
                continue
            axis, value = parse_data(data)
            apply_frame(axis, value)


def serial_ports():
    ports = []
    if sys.platform.startswith('win'):
        for i in range(1, 256):
            port = f'COM{i}'
            try:
                s = serial.Serial(port); s.close(); ports.append(port)
            except (OSError, serial.SerialException):
                pass
    elif sys.platform.startswith('linux') or sys.platform.startswith('cygwin'):
        ports = glob.glob('/dev/tty[A-Za-z]*')
    elif sys.platform.startswith('darwin'):
        ports = glob.glob('/dev/tty.*')
    else:
        raise EnvironmentError('Plataforma nao suportada.')

    result = []
    for port in ports:
        try:
            s = serial.Serial(port); s.close(); result.append(port)
        except (OSError, serial.SerialException):
            pass
    return result


def conectar_porta(port_name, root, botao, status_label, mudar_cor):
    """Abre a porta e roda a leitura numa thread (GUI continua respondendo)."""
    global _running, _ser

    if not port_name:
        messagebox.showwarning("Aviso", "Selecione uma porta serial.")
        return

    try:
        _ser = serial.Serial(port_name, 115200, timeout=1)
        _ser.reset_input_buffer()
        _running = True
        status_label.config(text=f"Conectado em {port_name}", foreground="green")
        mudar_cor("green")
        botao.config(text="Conectado")

        t = threading.Thread(target=controle, args=(_ser,), daemon=True)
        t.start()
    except Exception as e:
        messagebox.showerror("Erro de Conexao",
                             f"Nao foi possivel conectar em {port_name}.\nErro: {e}")
        mudar_cor("red")


def criar_janela():
    global _running, _ser

    root = tk.Tk()
    root.title("Controle Traffic Rider")
    root.geometry("400x250")
    root.resizable(False, False)

    dark_bg = "#2e2e2e"; dark_fg = "#ffffff"; accent = "#007acc"
    root.configure(bg=dark_bg)

    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("TFrame", background=dark_bg)
    style.configure("TLabel", background=dark_bg, foreground=dark_fg, font=("Segoe UI", 11))
    style.configure("Accent.TButton", font=("Segoe UI", 12, "bold"),
                    foreground=dark_fg, background=accent, padding=6)
    style.map("Accent.TButton", background=[("active", "#005f9e")])
    style.configure("TCombobox", fieldbackground=dark_bg, background=dark_bg,
                    foreground=dark_fg, padding=4)
    style.map("TCombobox", fieldbackground=[("readonly", dark_bg)])

    frame = ttk.Frame(root, padding="20"); frame.pack(expand=True, fill="both")
    ttk.Label(frame, text="Controle Traffic Rider",
              font=("Segoe UI", 14, "bold")).pack(pady=(0, 10))

    porta_var = tk.StringVar(value="")
    botao = ttk.Button(frame, text="Conectar e Iniciar", style="Accent.TButton",
                       command=lambda: conectar_porta(porta_var.get(), root, botao,
                                                      status_label, mudar_cor))
    botao.pack(pady=10)

    footer = tk.Frame(root, bg=dark_bg)
    footer.pack(side="bottom", fill="x", padx=10, pady=(10, 0))
    status_label = tk.Label(footer, text="Aguardando porta...", font=("Segoe UI", 11),
                            bg=dark_bg, fg=dark_fg)
    status_label.grid(row=0, column=0, sticky="w")

    portas = serial_ports()
    if portas:
        porta_var.set(portas[0])
    dropdown = ttk.Combobox(footer, textvariable=porta_var, values=portas,
                            state="readonly", width=10)
    dropdown.grid(row=0, column=1, padx=10)

    canvas = tk.Canvas(footer, width=20, height=20, highlightthickness=0, bg=dark_bg)
    circle = canvas.create_oval(2, 2, 18, 18, fill="red", outline="")
    canvas.grid(row=0, column=2, sticky="e")
    footer.columnconfigure(1, weight=1)

    def mudar_cor(cor):
        canvas.itemconfig(circle, fill=cor)

    def ao_fechar():
        global _running, _ser
        _running = False
        release_all()
        if _ser is not None:
            try: _ser.close()
            except Exception: pass
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", ao_fechar)
    root.mainloop()


if __name__ == "__main__":
    criar_janela()