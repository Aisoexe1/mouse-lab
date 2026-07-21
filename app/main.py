"""
GUI tool for driving the mouse-driver test injector (Arduino Leonardo +
USB Host Shield) over serial.

Lets you build a scenario of relative mouse-move steps (dx, dy, delay_ms,
buttons), save/load it as JSON, and play it back against the board while
logging its responses. Intended for reproducible driver/firmware test runs,
not for live game input.
"""

import json
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import serial
import serial.tools.list_ports

BAUD_RATE = 115200
BUTTON_BITS = {"left": 1, "right": 2, "middle": 4}


class SerialLink:
    def __init__(self, on_line):
        self._ser = None
        self._reader_thread = None
        self._stop = threading.Event()
        self._on_line = on_line

    @property
    def is_open(self):
        return self._ser is not None and self._ser.is_open

    def connect(self, port):
        self._ser = serial.Serial(port, BAUD_RATE, timeout=0.2)
        time.sleep(2.0)  # allow Leonardo to reset and re-enumerate USB
        self._stop.clear()
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()

    def disconnect(self):
        self._stop.set()
        if self._reader_thread:
            self._reader_thread.join(timeout=1.0)
        if self._ser:
            self._ser.close()
        self._ser = None

    def send_move(self, dx, dy, buttons):
        line = f"M,{dx},{dy},{buttons}\n"
        self._ser.write(line.encode("ascii"))

    def _read_loop(self):
        while not self._stop.is_set():
            try:
                raw = self._ser.readline()
            except serial.SerialException:
                break
            if raw:
                self._on_line(raw.decode("ascii", errors="replace").strip())


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Mouse Driver Test Injector")
        self.geometry("720x520")

        self.msg_queue = queue.Queue()
        self.link = SerialLink(on_line=self.msg_queue.put)
        self.steps = []  # list of dicts: dx, dy, delay_ms, buttons(list[str])
        self.play_thread = None
        self.stop_playback = threading.Event()

        self._build_ui()
        self._refresh_ports()
        self.after(100, self._drain_queue)

    # ---- UI construction ----

    def _build_ui(self):
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")

        ttk.Label(top, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=24, state="readonly")
        self.port_combo.pack(side="left", padx=4)
        ttk.Button(top, text="Refresh", command=self._refresh_ports).pack(side="left", padx=4)
        self.connect_btn = ttk.Button(top, text="Connect", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=4)
        self.status_var = tk.StringVar(value="disconnected")
        ttk.Label(top, textvariable=self.status_var).pack(side="left", padx=8)

        manual = ttk.LabelFrame(self, text="Manual step", padding=8)
        manual.pack(fill="x", padx=8, pady=4)

        self.dx_var = tk.StringVar(value="0")
        self.dy_var = tk.StringVar(value="0")
        self.delay_var = tk.StringVar(value="50")
        self.btn_left = tk.BooleanVar()
        self.btn_right = tk.BooleanVar()
        self.btn_middle = tk.BooleanVar()

        ttk.Label(manual, text="dx").grid(row=0, column=0)
        ttk.Entry(manual, textvariable=self.dx_var, width=6).grid(row=0, column=1, padx=4)
        ttk.Label(manual, text="dy").grid(row=0, column=2)
        ttk.Entry(manual, textvariable=self.dy_var, width=6).grid(row=0, column=3, padx=4)
        ttk.Label(manual, text="delay ms").grid(row=0, column=4)
        ttk.Entry(manual, textvariable=self.delay_var, width=6).grid(row=0, column=5, padx=4)
        ttk.Checkbutton(manual, text="L", variable=self.btn_left).grid(row=0, column=6, padx=2)
        ttk.Checkbutton(manual, text="R", variable=self.btn_right).grid(row=0, column=7, padx=2)
        ttk.Checkbutton(manual, text="M", variable=self.btn_middle).grid(row=0, column=8, padx=2)
        ttk.Button(manual, text="Add step", command=self._add_step).grid(row=0, column=9, padx=8)
        ttk.Button(manual, text="Send once", command=self._send_manual).grid(row=0, column=10, padx=4)

        list_frame = ttk.LabelFrame(self, text="Scenario", padding=8)
        list_frame.pack(fill="both", expand=True, padx=8, pady=4)

        self.steps_list = tk.Listbox(list_frame)
        self.steps_list.pack(side="left", fill="both", expand=True)
        scrollbar = ttk.Scrollbar(list_frame, command=self.steps_list.yview)
        scrollbar.pack(side="left", fill="y")
        self.steps_list.config(yscrollcommand=scrollbar.set)

        list_btns = ttk.Frame(list_frame)
        list_btns.pack(side="left", fill="y", padx=6)
        ttk.Button(list_btns, text="Remove", command=self._remove_selected).pack(fill="x", pady=2)
        ttk.Button(list_btns, text="Clear", command=self._clear_steps).pack(fill="x", pady=2)
        ttk.Button(list_btns, text="Load JSON", command=self._load_scenario).pack(fill="x", pady=2)
        ttk.Button(list_btns, text="Save JSON", command=self._save_scenario).pack(fill="x", pady=2)
        ttk.Button(list_btns, text="Play", command=self._play_scenario).pack(fill="x", pady=(12, 2))
        ttk.Button(list_btns, text="Stop", command=self._stop_scenario).pack(fill="x", pady=2)

        log_frame = ttk.LabelFrame(self, text="Log", padding=8)
        log_frame.pack(fill="both", expand=True, padx=8, pady=4)
        self.log_text = tk.Text(log_frame, height=8, state="disabled")
        self.log_text.pack(fill="both", expand=True)

    # ---- port / connection ----

    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_connect(self):
        if self.link.is_open:
            self.link.disconnect()
            self.status_var.set("disconnected")
            self.connect_btn.config(text="Connect")
        else:
            port = self.port_var.get()
            if not port:
                messagebox.showwarning("No port", "Select a serial port first.")
                return
            try:
                self.link.connect(port)
            except serial.SerialException as exc:
                messagebox.showerror("Connection failed", str(exc))
                return
            self.status_var.set(f"connected: {port}")
            self.connect_btn.config(text="Disconnect")

    # ---- step management ----

    def _buttons_mask(self):
        mask = 0
        if self.btn_left.get():
            mask |= BUTTON_BITS["left"]
        if self.btn_right.get():
            mask |= BUTTON_BITS["right"]
        if self.btn_middle.get():
            mask |= BUTTON_BITS["middle"]
        return mask

    def _current_step(self):
        try:
            dx = int(self.dx_var.get())
            dy = int(self.dy_var.get())
            delay_ms = int(self.delay_var.get())
        except ValueError:
            messagebox.showwarning("Invalid input", "dx/dy/delay must be integers.")
            return None
        return {"dx": dx, "dy": dy, "delay_ms": delay_ms, "buttons": self._buttons_mask()}

    def _add_step(self):
        step = self._current_step()
        if step is None:
            return
        self.steps.append(step)
        self._refresh_steps_list()

    def _remove_selected(self):
        sel = list(self.steps_list.curselection())
        for idx in reversed(sel):
            del self.steps[idx]
        self._refresh_steps_list()

    def _clear_steps(self):
        self.steps.clear()
        self._refresh_steps_list()

    def _refresh_steps_list(self):
        self.steps_list.delete(0, "end")
        for s in self.steps:
            self.steps_list.insert(
                "end",
                f"dx={s['dx']:+d} dy={s['dy']:+d} delay={s['delay_ms']}ms buttons={s['buttons']}",
            )

    # ---- scenario file I/O ----

    def _load_scenario(self):
        path = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if not path:
            return
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, list):
            messagebox.showerror("Invalid file", "Expected a JSON array of steps.")
            return
        self.steps = data
        self._refresh_steps_list()

    def _save_scenario(self):
        path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("JSON", "*.json")])
        if not path:
            return
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.steps, f, indent=2)

    # ---- sending ----

    def _send_manual(self):
        step = self._current_step()
        if step is None:
            return
        self._send_step(step)

    def _send_step(self, step):
        if not self.link.is_open:
            messagebox.showwarning("Not connected", "Connect to the board first.")
            return
        try:
            self.link.send_move(step["dx"], step["dy"], step["buttons"])
            self._log(f"-> M,{step['dx']},{step['dy']},{step['buttons']}")
        except serial.SerialException as exc:
            self._log(f"send error: {exc}")

    def _play_scenario(self):
        if not self.link.is_open:
            messagebox.showwarning("Not connected", "Connect to the board first.")
            return
        if self.play_thread and self.play_thread.is_alive():
            return
        self.stop_playback.clear()
        self.play_thread = threading.Thread(target=self._play_worker, daemon=True)
        self.play_thread.start()

    def _stop_scenario(self):
        self.stop_playback.set()

    def _play_worker(self):
        for step in self.steps:
            if self.stop_playback.is_set():
                self.msg_queue.put("-- playback stopped --")
                return
            self._send_step(step)
            time.sleep(max(step.get("delay_ms", 0), 0) / 1000.0)
        self.msg_queue.put("-- playback finished --")

    # ---- logging ----

    def _log(self, text):
        self.log_text.config(state="normal")
        self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def _drain_queue(self):
        try:
            while True:
                line = self.msg_queue.get_nowait()
                self._log(line)
        except queue.Empty:
            pass
        self.after(100, self._drain_queue)

    def on_close(self):
        self.stop_playback.set()
        if self.link.is_open:
            self.link.disconnect()
        self.destroy()


if __name__ == "__main__":
    app = App()
    app.protocol("WM_DELETE_WINDOW", app.on_close)

    # Force the window to redraw/come forward: launching as a background
    # process on macOS can otherwise leave Tk with a blank, unfocused window.
    app.update_idletasks()
    app.lift()
    app.attributes("-topmost", True)
    app.after(200, lambda: app.attributes("-topmost", False))
    app.after(0, app.focus_force)

    app.mainloop()
