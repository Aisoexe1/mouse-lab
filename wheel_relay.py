#!/usr/bin/env python3
"""
Scroll wheel relay for Arduino HID mouse.
Intercepts hardware scroll events and re-injects via SendInput,
bypassing game/EAC filters that reject scroll from Arduino VID:0x2341.

Diagnostic mode: logs all mouse hook events so you can see exactly
what Windows is (and isn't) receiving from the Arduino.
"""

import ctypes
import ctypes.wintypes as wt
import platform
import sys
import time

u32  = ctypes.windll.user32
k32  = ctypes.windll.kernel32

IS64 = ctypes.sizeof(ctypes.c_void_p) == 8
ULONG_PTR = ctypes.c_ulonglong if IS64 else ctypes.c_ulong
LRESULT   = ctypes.c_longlong  if IS64 else ctypes.c_long

# ── Windows constants ──────────────────────────────────────────────────────────
WH_MOUSE_LL        = 14
WM_MOUSEMOVE       = 0x0200
WM_LBUTTONDOWN     = 0x0201
WM_LBUTTONUP       = 0x0202
WM_RBUTTONDOWN     = 0x0204
WM_RBUTTONUP       = 0x0205
WM_MBUTTONDOWN     = 0x0207
WM_MBUTTONUP       = 0x0208
WM_MOUSEWHEEL      = 0x020A
WM_XBUTTONDOWN     = 0x020B
WM_MOUSEHWHEEL     = 0x020E
MOUSEEVENTF_WHEEL  = 0x0800
MOUSEEVENTF_HWHEEL = 0x1000
INPUT_MOUSE        = 0
LLMHF_INJECTED     = 0x00000001
PM_REMOVE          = 0x0001
WM_QUIT            = 0x0012

_WM_NAMES = {
    WM_MOUSEMOVE:   "MOVE",
    WM_LBUTTONDOWN: "LMB↓",   WM_LBUTTONUP: "LMB↑",
    WM_RBUTTONDOWN: "RMB↓",   WM_RBUTTONUP: "RMB↑",
    WM_MBUTTONDOWN: "MMB↓",   WM_MBUTTONUP: "MMB↑",
    WM_MOUSEWHEEL:  "WHEEL_V", WM_MOUSEHWHEEL: "WHEEL_H",
    WM_XBUTTONDOWN: "XMB↓",
}


# ── ctypes structures ──────────────────────────────────────────────────────────
class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("pt",          wt.POINT),
        ("mouseData",   wt.DWORD),
        ("flags",       wt.DWORD),
        ("time",        wt.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx",          ctypes.c_long),
        ("dy",          ctypes.c_long),
        ("mouseData",   wt.DWORD),
        ("dwFlags",     wt.DWORD),
        ("time",        wt.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class _INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("_u",)
    _fields_ = [("type", wt.DWORD), ("_u", _INPUT_UNION)]


HOOKPROC    = ctypes.WINFUNCTYPE(LRESULT, ctypes.c_int, wt.WPARAM, wt.LPARAM)
_INPUT_SIZE = ctypes.sizeof(INPUT)
_hook       = None

# Explicit argtypes — critical on x64 where lParam is a 64-bit pointer
u32.SetWindowsHookExA.argtypes   = [ctypes.c_int, HOOKPROC, wt.HINSTANCE, wt.DWORD]
u32.SetWindowsHookExA.restype    = ctypes.c_void_p
u32.CallNextHookEx.argtypes      = [ctypes.c_void_p, ctypes.c_int, wt.WPARAM, wt.LPARAM]
u32.CallNextHookEx.restype       = LRESULT
u32.UnhookWindowsHookEx.argtypes = [ctypes.c_void_p]
u32.SendInput.argtypes           = [ctypes.c_uint, ctypes.c_void_p, ctypes.c_int]
u32.SendInput.restype            = ctypes.c_uint


# ── helpers ────────────────────────────────────────────────────────────────────
def _send_wheel(flags: int, delta: int) -> None:
    inp = INPUT()
    inp.type         = INPUT_MOUSE
    inp.mi.mouseData = wt.DWORD((delta << 16) & 0xFFFFFFFF)
    inp.mi.dwFlags   = flags
    inp.mi.dwExtraInfo = 0
    sent = u32.SendInput(1, ctypes.byref(inp), _INPUT_SIZE)
    if sent == 0:
        err = k32.GetLastError()
        print(f"  [!] SendInput FAILED  error={err}  (0=UIPI block or access denied)")
    else:
        print(f"  [+] SendInput OK      delta={delta:+d}")


# ── hook callback ──────────────────────────────────────────────────────────────
_move_total = 0   # count moves so we don't spam the console

def _proc(nCode: int, wParam: int, lParam: int) -> int:
    global _move_total

    if nCode < 0:
        return u32.CallNextHookEx(_hook, nCode, wParam, lParam)

    ms       = ctypes.cast(lParam, ctypes.POINTER(MSLLHOOKSTRUCT)).contents
    injected = bool(ms.flags & LLMHF_INJECTED)
    src      = "SYNTHETIC" if injected else "HW"
    name     = _WM_NAMES.get(wParam, f"0x{wParam:04X}")

    if wParam == WM_MOUSEMOVE:
        # Log every 300th move — just to prove the hook is alive
        _move_total += 1
        if _move_total % 300 == 1:
            print(f"[MOVE  ] #{_move_total:>6}  src={src}  hook alive ✓")

    elif wParam in (WM_MOUSEWHEEL, WM_MOUSEHWHEEL):
        delta = ctypes.c_short(ms.mouseData >> 16).value
        print(f"[{name:<8}] delta={delta:+d}  src={src}  flags=0x{ms.flags:08X}")
        if not injected:
            # Hardware scroll from Arduino — re-inject as SendInput to bypass EAC VID filter
            flags = MOUSEEVENTF_WHEEL if wParam == WM_MOUSEWHEEL else MOUSEEVENTF_HWHEEL
            _send_wheel(flags, delta)
            return 1   # block original so game doesn't receive it twice

    else:
        # Button or other event — log it
        print(f"[{name:<8}] src={src}  wParam=0x{wParam:04X}")

    return u32.CallNextHookEx(_hook, nCode, wParam, lParam)


# ── main ───────────────────────────────────────────────────────────────────────
def main() -> None:
    global _hook

    print("=" * 60)
    print(f"  wheel_relay diagnostic")
    print(f"  Python {sys.version.split()[0]}  |  {platform.machine()}  |  ptr={ctypes.sizeof(ctypes.c_void_p)*8}bit")
    print("=" * 60)

    cb    = HOOKPROC(_proc)
    _hook = u32.SetWindowsHookExA(WH_MOUSE_LL, cb, None, 0)

    if not _hook:
        err = k32.GetLastError()
        print(f"[!] SetWindowsHookEx FAILED  error={err}")
        sys.exit(1)

    print(f"[OK] Hook installed  handle=0x{_hook:X}")
    print()
    print("  Что ожидать в логе:")
    print("  [MOVE] каждые 300 движений — хук работает")
    print("  [WHEEL_V] delta=±N src=HW — Arduino шлёт HID-скролл (скетч старый)")
    print("  [WHEEL_V] ... ничего — Arduino НЕ шлёт HID-скролл (загружен новый скетч)")
    print("  [+] SendInput OK — реинъекция прошла")
    print("  [!] SendInput FAILED error=5 — нет прав (запусти от администратора)")
    print()
    print("  Ctrl+C — остановить")
    print("-" * 60)

    msg = wt.MSG()
    try:
        while True:
            if u32.PeekMessageA(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
                if msg.message == WM_QUIT:
                    break
                u32.TranslateMessage(ctypes.byref(msg))
                u32.DispatchMessageA(ctypes.byref(msg))
            else:
                time.sleep(0.001)
    except KeyboardInterrupt:
        pass
    finally:
        if _hook:
            u32.UnhookWindowsHookEx(_hook)
        print("\n[--] Hook removed. Стоп.")


if __name__ == "__main__":
    main()
