#!/usr/bin/env python3
"""
Scroll wheel relay for Arduino HID mouse.
Intercepts hardware scroll events and re-injects via SendInput,
bypassing game/EAC filters that reject scroll from Arduino VID:0x2341.

No external packages needed — stdlib only.
Run alongside the macro overlay (no serial port conflict).
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

u32 = ctypes.windll.user32

# ULONG_PTR is 8 bytes on 64-bit Windows, 4 bytes on 32-bit
ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong

WH_MOUSE_LL        = 14
WM_MOUSEWHEEL      = 0x020A
WM_MOUSEHWHEEL     = 0x020E
MOUSEEVENTF_WHEEL  = 0x0800
MOUSEEVENTF_HWHEEL = 0x1000
INPUT_MOUSE        = 0
LLMHF_INJECTED     = 0x00000001   # set by Windows on all SendInput events
PM_REMOVE          = 0x0001
WM_QUIT            = 0x0012


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


HOOKPROC    = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_int, wt.WPARAM, wt.LPARAM)
_INPUT_SIZE = ctypes.sizeof(INPUT)
_hook       = None


def _send_wheel(flags: int, delta: int) -> None:
    inp = INPUT()
    inp.type            = INPUT_MOUSE
    inp.mi.mouseData    = wt.DWORD((delta << 16) & 0xFFFFFFFF)
    inp.mi.dwFlags      = flags
    inp.mi.dwExtraInfo  = 0
    u32.SendInput(1, ctypes.byref(inp), _INPUT_SIZE)


def _proc(nCode: int, wParam: int, lParam: int) -> int:
    if nCode >= 0 and wParam in (WM_MOUSEWHEEL, WM_MOUSEHWHEEL):
        ms = ctypes.cast(lParam, ctypes.POINTER(MSLLHOOKSTRUCT)).contents
        if not (ms.flags & LLMHF_INJECTED):
            # Hardware scroll event — re-inject as synthetic and block original
            delta = ctypes.c_short(ms.mouseData >> 16).value
            flags = MOUSEEVENTF_WHEEL if wParam == WM_MOUSEWHEEL else MOUSEEVENTF_HWHEEL
            _send_wheel(flags, delta)
            return 1   # block original (prevents double-scroll in menus)
    return u32.CallNextHookEx(_hook, nCode, wParam, lParam)


def main() -> None:
    global _hook

    cb    = HOOKPROC(_proc)
    _hook = u32.SetWindowsHookExA(WH_MOUSE_LL, cb, None, 0)
    if not _hook:
        print("Ошибка: не удалось установить хук мыши")
        sys.exit(1)

    print("Scroll relay запущен. Оставь это окно открытым пока играешь.")
    print("Ctrl+C или закрой окно для остановки.")

    msg = wt.MSG()
    try:
        while True:
            if u32.PeekMessageA(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
                if msg.message == WM_QUIT:
                    break
                u32.TranslateMessage(ctypes.byref(msg))
                u32.DispatchMessageA(ctypes.byref(msg))
            else:
                time.sleep(0.001)   # 1ms idle — позволяет доставить Ctrl+C
    except KeyboardInterrupt:
        pass
    finally:
        if _hook:
            u32.UnhookWindowsHookEx(_hook)
        print("Остановлен.")


if __name__ == "__main__":
    main()
