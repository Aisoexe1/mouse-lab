# Mouse Driver Test Injector

Стенд для тестирования драйвера/прошивки мыши: Arduino Leonardo + USB Host Shield
пропускает реальную мышь как есть (pass-through) и одновременно умеет по команде
с ПК впрыснуть в тот же HID-поток заранее заготовленные relative-move события
(dx/dy/кнопки) — удобно для воспроизводимых регрессионных тестов курсора,
проверки антидребезга, чувствительности/DPI-скейлинга в драйвере и т.п.

## Состав

- `arduino/mouse_test_injector/` — прошивка для Arduino Leonardo
- `app/` — кроссплатформенное (macOS/Windows) GUI-приложение на Python (Tkinter + pyserial)
  для составления и прогона тестовых сценариев

## Железо и библиотеки

- Arduino Leonardo (или любая плата на ATmega32u4 с нативным USB — Micro, Pro Micro)
- USB Host Shield 2.0 (Sparkfun/китайский клон)
- Библиотека `USB_Host_Shield_2.0` (Kristian Lauszus) — установить через Arduino Library Manager
- Стандартная библиотека `Mouse.h` (входит в Arduino IDE, для плат на 32u4)

## Протокол Serial (115200 baud)

Однострочные ASCII-команды, разделитель `,`, конец строки `\n`:

```
M,<dx>,<dy>,<buttons>
```

- `dx`, `dy` — целые числа -127..127 (relative move за один HID-репорт)
- `buttons` — битовая маска: 1=левая, 2=правая, 4=средняя, 0=не менять поверх pass-through

Ответ платы: `OK\n` при успешной обработке или `ERR,<причина>\n`.

Плата также автоматически транслирует на компьютер отчёты физической мыши,
подключённой к USB Host Shield, независимо от инжектируемых команд.

## Быстрый старт GUI

```bash
cd app
python3 -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r requirements.txt
python3 main.py
```

В окне: выбрать serial-порт → Connect → задать шаги сценария (dx, dy, задержка,
кнопки) → Save/Load JSON для переиспользования → Play для прогона со логом
ответов платы.
