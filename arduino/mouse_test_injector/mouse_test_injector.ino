// Mouse driver/firmware test injector for Arduino Leonardo + USB Host Shield.
//
// - Passes through reports from a real USB mouse connected to the Host Shield
//   so the device behaves like a normal mouse.
// - Accepts relative-move test commands over the CDC serial port and injects
//   them into the same HID mouse endpoint, so a PC-side test tool can drive
//   reproducible cursor movement for driver regression tests.
//
// Serial protocol (115200 baud), one command per line:
//   M,<dx>,<dy>,<buttons>\n
//     dx, dy    : signed int, -127..127, relative move for this report
//     buttons   : bitmask, 1=left 2=right 4=middle, 0=leave as-is
//   Reply: "OK\n" or "ERR,<reason>\n"

#include <hidboot.h>
#include <usbhub.h>
#include <Mouse.h>

class MouseReportParser : public MouseReportParser {
 public:
  void OnMouseMove(MOUSEINFO *mi) override {
    Mouse.move(mi->dX, mi->dY, 0);
  }
  void OnLeftButtonUp(MOUSEINFO *mi) override { Mouse.release(MOUSE_LEFT); }
  void OnLeftButtonDown(MOUSEINFO *mi) override { Mouse.press(MOUSE_LEFT); }
  void OnRightButtonUp(MOUSEINFO *mi) override { Mouse.release(MOUSE_RIGHT); }
  void OnRightButtonDown(MOUSEINFO *mi) override { Mouse.press(MOUSE_RIGHT); }
  void OnMiddleButtonUp(MOUSEINFO *mi) override { Mouse.release(MOUSE_MIDDLE); }
  void OnMiddleButtonDown(MOUSEINFO *mi) override { Mouse.press(MOUSE_MIDDLE); }
};

USB Usb;
USBHub Hub(&Usb);
HIDBoot<USB_HID_PROTOCOL_MOUSE> HidMouse(&Usb);
MouseReportParser Parser;

static const size_t LINE_BUF_SIZE = 64;
char lineBuf[LINE_BUF_SIZE];
size_t lineLen = 0;

void setup() {
  Serial.begin(115200);
  Mouse.begin();

  if (Usb.Init() == -1) {
    Serial.println(F("ERR,usb_host_init_failed"));
  }
  HidMouse.SetReportParser(0, &Parser);
}

void applyTestMove(int dx, int dy, int buttons) {
  dx = constrain(dx, -127, 127);
  dy = constrain(dy, -127, 127);

  if (buttons & 1) Mouse.press(MOUSE_LEFT);
  if (buttons & 2) Mouse.press(MOUSE_RIGHT);
  if (buttons & 4) Mouse.press(MOUSE_MIDDLE);

  Mouse.move(dx, dy, 0);
}

bool parseAndRun(char *line) {
  char *cmd = strtok(line, ",");
  if (cmd == nullptr || cmd[0] != 'M') return false;

  char *dxTok = strtok(nullptr, ",");
  char *dyTok = strtok(nullptr, ",");
  char *btnTok = strtok(nullptr, ",");
  if (!dxTok || !dyTok || !btnTok) return false;

  int dx = atoi(dxTok);
  int dy = atoi(dyTok);
  int buttons = atoi(btnTok);

  applyTestMove(dx, dy, buttons);
  return true;
}

void loop() {
  Usb.Task();

  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen > 0) {
        bool ok = parseAndRun(lineBuf);
        Serial.println(ok ? F("OK") : F("ERR,bad_command"));
      }
      lineLen = 0;
    } else if (lineLen < LINE_BUF_SIZE - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}
