// Macro by Aiso.exe — recoil-pattern playback tool.
// Sends relative mouse-move commands to a hardware HID emulator over serial,
// or simulates them in-process (sim mode). Hotkey-bindable, toggle start/stop.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "serial_port.h"

#ifdef HAVE_EMBEDDED_WEAPONS
#include "weapons_embedded.h"
#endif

struct Step {
  int dx = 0;
  int dy = 0;
  float delayMs = 50.0f;
  bool left = false, right = false, middle = false;

  int ButtonsMask() const {
    return (left ? 1 : 0) | (right ? 2 : 0) | (middle ? 4 : 0);
  }
};

struct LuaTest {
  std::string name;
  std::string path;
  std::vector<Step> steps;
  int hotkey = GLFW_KEY_UNKNOWN;
  float recordedSens = 0.0f;  // 0 = not set (manual)
};

static std::string FileNameWithoutExtension(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  return path.substr(start, dot == std::string::npos || dot < start ? std::string::npos : dot - start);
}

static std::string KeyLabel(int key) {
  if (key <= -1000) {
    const int button = -1000 - key;
    if (button == GLFW_MOUSE_BUTTON_4) return "Mouse side 1";
    if (button == GLFW_MOUSE_BUTTON_5) return "Mouse side 2";
    return "Mouse " + std::to_string(button + 1);
  }
  if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return std::string(1, static_cast<char>('A' + key - GLFW_KEY_A));
  if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) return std::string(1, static_cast<char>('0' + key - GLFW_KEY_0));
  if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
  if (key == GLFW_KEY_SPACE) return "Space";
  if (key == GLFW_KEY_ENTER) return "Enter";
  if (key == GLFW_KEY_TAB) return "Tab";
  return "Not bound";
}

static int MouseHotkey(int button) {
  return -1000 - button;
}

#ifdef _WIN32
static int GlfwKeyToVK(int key) {
  if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)   return 'A' + (key - GLFW_KEY_A);
  if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)   return '0' + (key - GLFW_KEY_0);
  if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) return VK_F1 + (key - GLFW_KEY_F1);
  if (key == GLFW_KEY_SPACE) return VK_SPACE;
  if (key == GLFW_KEY_ENTER) return VK_RETURN;
  if (key == GLFW_KEY_TAB)   return VK_TAB;
  return 0;
}
#endif

static bool SaveLuaLibrary(const std::string& path, const std::vector<LuaTest>& tests) {
  std::ofstream f(path);
  if (!f) return false;
  for (const auto& test : tests) f << test.hotkey << '\t' << test.path << '\n';
  return true;
}

static std::vector<std::pair<int, std::string>> LoadLuaLibrary(const std::string& path) {
  std::vector<std::pair<int, std::string>> entries;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    try {
      entries.emplace_back(std::stoi(line.substr(0, tab)), line.substr(tab + 1));
    } catch (...) {
    }
  }
  return entries;
}

static std::string OpenJsonFileDialog() {
#ifdef _WIN32
  char path[MAX_PATH] = {};
  OPENFILENAMEA dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.lpstrFilter = "JSON files\0*.json\0All files\0*.*\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  return GetOpenFileNameA(&dialog) ? path : "";
#else
#ifdef __APPLE__
  const char* command = "osascript -e 'POSIX path of (choose file with prompt \"Select weapon JSON\" of type {\"json\"})' 2>/dev/null";
#else
  const char* command = "zenity --file-selection --file-filter='JSON files | *.json' 2>/dev/null";
#endif
  FILE* pipe = popen(command, "r");
  if (!pipe) return "";
  char path[1024] = {};
  const bool read = std::fgets(path, sizeof(path), pipe) != nullptr;
  pclose(pipe);
  if (!read) return "";
  std::string result(path);
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
  return result;
#endif
}

static void ApplyDarkTheme() {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 18.0f;
  style.ChildRounding = 14.0f;
  style.FrameRounding = 9.0f;
  style.GrabRounding = 9.0f;
  style.PopupRounding = 10.0f;
  style.ScrollbarRounding = 10.0f;
  style.WindowPadding = ImVec2(22, 14);
  style.FramePadding = ImVec2(11, 8);
  style.ItemSpacing = ImVec2(10, 10);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;

  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.008f, 0.009f, 0.012f, 0.92f);
  c[ImGuiCol_ChildBg] = ImVec4(0.030f, 0.033f, 0.042f, 0.94f);
  c[ImGuiCol_FrameBg] = ImVec4(0.070f, 0.075f, 0.092f, 0.94f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.125f, 0.132f, 0.158f, 0.98f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.185f, 0.195f, 0.230f, 1.00f);
  c[ImGuiCol_TitleBg] = ImVec4(0.015f, 0.017f, 0.022f, 1.00f);
  c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBg];
  c[ImGuiCol_Button] = ImVec4(0.095f, 0.102f, 0.122f, 0.96f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.188f, 0.220f, 1.00f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.282f, 0.325f, 1.00f);
  c[ImGuiCol_Header] = ImVec4(0.105f, 0.112f, 0.135f, 0.92f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.198f, 0.232f, 0.96f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.282f, 0.325f, 1.00f);
  c[ImGuiCol_CheckMark] = ImVec4(0.94f, 0.97f, 1.00f, 1.00f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.82f, 0.89f, 1.00f, 1.00f);
  c[ImGuiCol_Border] = ImVec4(0.72f, 0.76f, 0.86f, 0.14f);
  c[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.90f, 0.93f, 0.98f, 1.00f);
}

static void DrawParticles(const ImGuiViewport* vp, float time) {
  ImDrawList* draw = ImGui::GetBackgroundDrawList();
  const ImVec2 br(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

  // Deep space background with subtle vignette
  draw->AddRectFilled(vp->Pos, br, IM_COL32(1, 3, 9, 255));
  draw->AddRectFilledMultiColor(vp->Pos, br,
    IM_COL32(8, 14, 35, 90), IM_COL32(2, 5, 15, 40),
    IM_COL32(2, 5, 15, 40), IM_COL32(6, 10, 28, 80));

  const auto frac = [](float v) { return v - std::floor(v); };
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  struct Particle {
    ImVec2 pos, prev;
    float radius;
    int alpha;
    ImU32 color, glow;
  };

  const int N = 300;
  static std::vector<Particle> pts;
  pts.clear();
  pts.reserve(N);

  for (int i = 0; i < N; ++i) {
    const float seed = static_cast<float>(i) * 31.731f;
    const float bx = vp->Pos.x + frac(std::sin(seed * 12.9898f) * 43758.5f) * vp->Size.x;
    const float by = vp->Pos.y + frac(std::sin(seed * 78.233f)  * 12731.7f) * vp->Size.y;

    const float fx = 0.19f + frac(seed * 0.137f) * 0.13f;
    const float fy = 0.14f + frac(seed * 0.241f) * 0.11f;
    const float ax = 13.0f + frac(seed * 0.373f) * 20.0f;
    const float ay = 11.0f + frac(seed * 0.519f) * 16.0f;

    float x = bx + std::sin(time * fx + seed) * ax;
    float y = by + std::cos(time * fy + seed * 1.7f) * ay;
    const float px = bx + std::sin((time - 0.07f) * fx + seed) * ax;
    const float py = by + std::cos((time - 0.07f) * fy + seed * 1.7f) * ay;

    // Mouse repels gently
    const float mdx = x - mouse.x, mdy = y - mouse.y;
    const float md = std::sqrt(mdx * mdx + mdy * mdy);
    if (md > 0.1f && md < 200.0f) {
      const float push = (1.0f - md / 200.0f) * 6.0f;
      x += mdx / md * push;
      y += mdy / md * push;
    }

    const float pulse = 0.5f + 0.5f * std::sin(time * (0.6f + frac(seed) * 1.6f) + seed);
    const int alpha = static_cast<int>(35.0f + pulse * 155.0f);

    // Color palette: blue / cyan / lavender
    const float cs = frac(seed * 0.0713f);
    ImU32 col, glow;
    if (cs < 0.38f) {
      col  = IM_COL32(195, 218, 255, alpha);
      glow = IM_COL32(140, 185, 255, alpha / 6);
    } else if (cs < 0.70f) {
      col  = IM_COL32(148, 224, 255, alpha);
      glow = IM_COL32(100, 200, 255, alpha / 6);
    } else {
      col  = IM_COL32(205, 185, 255, alpha);
      glow = IM_COL32(170, 148, 255, alpha / 6);
    }

    const float ss = frac(seed * 0.157f);
    const float r = ss < 0.05f ? 2.0f + pulse * 1.6f
                  : ss < 0.28f ? 1.1f + pulse * 1.0f
                  :              0.55f + pulse * 0.7f;

    pts.push_back({ImVec2(x, y), ImVec2(px, py), r, alpha, col, glow});
  }

  // Connection lines
  for (size_t i = 0; i < pts.size(); ++i) {
    for (size_t j = i + 1; j < pts.size(); ++j) {
      const float dx = pts[i].pos.x - pts[j].pos.x;
      const float dy = pts[i].pos.y - pts[j].pos.y;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d < 118.0f) {
        const int a = static_cast<int>((1.0f - d / 118.0f) * 48.0f);
        draw->AddLine(pts[i].pos, pts[j].pos, IM_COL32(170, 205, 255, a), 0.65f);
      }
    }
  }

  // Draw particles
  for (const auto& p : pts) {
    // Trail
    const float tdx = p.pos.x - p.prev.x, tdy = p.pos.y - p.prev.y;
    if (tdx * tdx + tdy * tdy > 0.3f)
      draw->AddLine(p.prev, p.pos, IM_COL32(180, 215, 255, p.alpha / 7), p.radius * 0.9f);

    // Mouse beam + halo
    const float dx = p.pos.x - mouse.x, dy = p.pos.y - mouse.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 115.0f) {
      const float s = 1.0f - d / 115.0f;
      draw->AddLine(mouse, p.pos, IM_COL32(210, 235, 255, static_cast<int>(s * 35.0f)), 0.55f);
      draw->AddCircle(p.pos, p.radius * (3.0f + s * 2.8f), p.glow, 12, 0.55f);
    }

    // Outer glow for larger particles
    if (p.radius > 1.4f)
      draw->AddCircleFilled(p.pos, p.radius * 3.5f, p.glow);

    draw->AddCircleFilled(p.pos, p.radius, p.color);
  }
}


// Minimal parser for {"weaponname": [{dx, dy, delay}, ...]} weapon JSON format.
static std::vector<Step> ParseJsonContent(const std::string& src, std::string& err) {
  const size_t arrStart = src.find('[');
  if (arrStart == std::string::npos) { err = "no step array found"; return {}; }

  std::vector<Step> steps;
  size_t pos = arrStart + 1;

  while (pos < src.size()) {
    while (pos < src.size() && (src[pos] <= ' ' || src[pos] == ',')) ++pos;
    if (pos >= src.size() || src[pos] == ']') break;
    if (src[pos] != '{') { ++pos; continue; }
    const size_t objEnd = src.find('}', pos);
    if (objEnd == std::string::npos) break;
    const std::string obj = src.substr(pos, objEnd - pos + 1);
    pos = objEnd + 1;

    auto getField = [&](const char* key, float& out) -> bool {
      const std::string needle = std::string("\"") + key + "\"";
      const size_t k = obj.find(needle);
      if (k == std::string::npos) return false;
      const size_t colon = obj.find(':', k + needle.size());
      if (colon == std::string::npos) return false;
      return std::sscanf(obj.c_str() + colon + 1, " %f", &out) == 1;
    };

    float fdx = 0, fdy = 0, delay = 50;
    if (!getField("dx", fdx) || !getField("dy", fdy) || !getField("delay", delay)) {
      err = "missing dx/dy/delay in step";
      return {};
    }
    Step s;
    s.dx = static_cast<int>(std::clamp(fdx, -127.0f, 127.0f));
    s.dy = static_cast<int>(std::clamp(fdy, -127.0f, 127.0f));
    s.delayMs = delay;
    steps.push_back(s);
  }

  if (steps.empty()) err = "no steps found";
  return steps;
}

static std::vector<Step> LoadJsonScenario(const std::string& path, std::string& err) {
  std::ifstream f(path);
  if (!f) { err = "cannot open file"; return {}; }
  const std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return ParseJsonContent(src, err);
}

static std::vector<std::string> gDroppedPaths;

static void OnFileDrop(GLFWwindow*, int count, const char** paths) {
  for (int i = 0; i < count; ++i)
    if (paths && paths[i]) gDroppedPaths.emplace_back(paths[i]);
}

int main(int /*argc*/, char* argv[]) {
#ifdef _WIN32
  SetProcessDPIAware();
#endif
  if (!glfwInit()) return 1;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

  GLFWwindow* window = glfwCreateWindow(1120, 720, "Mouse Driver Test Injector", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  {
    const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    if (mode) glfwSetWindowPos(window, (mode->width - 1120) / 2, (mode->height - 720) / 2);
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  glfwSetDropCallback(window, OnFileDrop);

#ifdef __APPLE__
  {
    const void* keys[]   = { kAXTrustedCheckOptionPrompt };
    const void* values[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(nullptr, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
  }
#endif

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ApplyDarkTheme();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 150");

  SerialPort serial;
  std::vector<std::string> availablePorts = SerialPort::ListPorts();
  int selectedPort = 0;
  bool connected = false;
  bool simulate = false;
  std::string statusText = "disconnected";
  std::string connectedPort;
  double lastPortScan = 0.0;

  std::vector<Step> steps;
  std::vector<LuaTest> luaTests;
  int selectedTestIdx = -1;
  int bindingTestIdx = -1;
  std::vector<char> keyWasDown(GLFW_KEY_LAST + 1, false);
  std::vector<char> globalSideMouseWasDown(8, false);
  bool prevGlobalRmb = false, prevGlobalLmb = false;
  bool rmbFirst = false, playingViaMouse = false;
  namespace fs = std::filesystem;
  const fs::path exeDir = fs::path(argv[0]).parent_path();
  const std::string libraryPath = (exeDir / "lua_tests.library").string();
  const std::string kEmbedPfx = "embedded://";

  std::vector<std::string> logLines;
  char jsonPathBuf[512] = "";

  bool playing = false;
  size_t playIdx = 0;
  double nextSendTime = 0.0;
  double lastLogTime = 0.0;
  float sensitivity = 1.0f;
  float scriptSens = 0.8333f;
  float gameSens = 0.8333f;
  float smoothness = 1.0f;
  int subIdx = 0;

  auto log = [&](const std::string& s) {
    const double t = glfwGetTime();
    lastLogTime = t;
    char ts[10];
    snprintf(ts, sizeof(ts), "[%02d:%02d] ", static_cast<int>(t) / 60, static_cast<int>(t) % 60);
    logLines.push_back(std::string(ts) + s);
    if (logLines.size() > 500) logLines.erase(logLines.begin());
  };

  auto sendStep = [&](const Step& s) {
    const int sdx = static_cast<int>(std::clamp(s.dx * sensitivity, -127.0f, 127.0f));
    const int sdy = static_cast<int>(std::clamp(s.dy * sensitivity, -127.0f, 127.0f));
    if (simulate) {
#ifdef __APPLE__
      CGEventRef ref = CGEventCreate(nullptr);
      CGPoint pos = CGEventGetLocation(ref);
      CFRelease(ref);
      pos.x += sdx;
      pos.y += sdy;
      if (AXIsProcessTrusted()) {
        CGEventRef mv = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, pos, kCGMouseButtonLeft);
        CGEventSetIntegerValueField(mv, kCGMouseEventDeltaX, sdx);
        CGEventSetIntegerValueField(mv, kCGMouseEventDeltaY, sdy);
        CGEventPost(kCGHIDEventTap, mv);
        CFRelease(mv);
      } else {
        CGWarpMouseCursorPosition(pos);
      }
#elif defined(_WIN32)
      INPUT inp = {};
      inp.type = INPUT_MOUSE;
      inp.mi.dwFlags = MOUSEEVENTF_MOVE;
      inp.mi.dx = sdx;
      inp.mi.dy = sdy;
      SendInput(1, &inp, sizeof(INPUT));
#endif
      return;
    }
    if (!serial.IsOpen()) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "M,%d,%d,%d", sdx, sdy, s.ButtonsMask());
    if (!serial.WriteLine(buf)) log("send error");
  };

  auto saveLibrary = [&]() {
    if (!SaveLuaLibrary(libraryPath, luaTests)) log("could not save library");
  };

  auto importJson = [&](const std::string& pathArg) {
    const bool isEmbedScheme = pathArg.size() > kEmbedPfx.size() &&
                               pathArg.compare(0, kEmbedPfx.size(), kEmbedPfx) == 0;
    std::string err, usedPath = pathArg;
    std::vector<Step> imported;

    if (isEmbedScheme) {
#ifdef HAVE_EMBEDDED_WEAPONS
      const std::string fname = pathArg.substr(kEmbedPfx.size());
      for (int i = 0; i < kEmbeddedWeaponCount; ++i) {
        if (fname == kEmbeddedWeapons[i].filename) {
          std::string content(kEmbeddedWeapons[i].data, kEmbeddedWeapons[i].size);
          imported = ParseJsonContent(content, err);
          break;
        }
      }
      if (imported.empty() && err.empty()) err = "not found in embedded data";
#else
      err = "embedded weapons not compiled in";
#endif
    } else {
      imported = LoadJsonScenario(pathArg, err);
#ifdef HAVE_EMBEDDED_WEAPONS
      if (imported.empty()) {
        const std::string fname = fs::path(pathArg).filename().string();
        for (int i = 0; i < kEmbeddedWeaponCount; ++i) {
          if (fname == kEmbeddedWeapons[i].filename) {
            std::string content(kEmbeddedWeapons[i].data, kEmbeddedWeapons[i].size);
            std::string e2;
            auto attempt = ParseJsonContent(content, e2);
            if (!attempt.empty()) {
              imported = std::move(attempt);
              usedPath = kEmbedPfx + fname;
              err.clear();
            }
            break;
          }
        }
      }
#endif
    }

    if (!err.empty()) { log("JSON import failed: " + err); return; }
    if (imported.empty()) { log("JSON import failed: no steps"); return; }

    LuaTest test;
    test.name = FileNameWithoutExtension(usedPath);
    test.path = usedPath;
    test.steps = std::move(imported);
    test.recordedSens = 0.8333f;
    luaTests.push_back(std::move(test));
    selectedTestIdx = static_cast<int>(luaTests.size()) - 1;
    log("loaded: " + luaTests.back().name + " (" + std::to_string(luaTests.back().steps.size()) + " steps)");
  };

  auto playTest = [&](int index) {
    if (index < 0 || index >= static_cast<int>(luaTests.size())) return;
    if (!connected && !simulate) {
      log("cannot run " + luaTests[index].name + ": device is offline");
      return;
    }
    steps = luaTests[index].steps;
    if (steps.empty()) {
      log("cannot run " + luaTests[index].name + ": no imported commands");
      return;
    }
    playing = true;
    playIdx = 0;
    subIdx = 0;
    nextSendTime = glfwGetTime();
    log("macro " + luaTests[index].name + " active");
  };

  for (const auto& [hotkey, path] : LoadLuaLibrary(libraryPath)) {
    const size_t previousSize = luaTests.size();
    importJson(path);
    if (luaTests.size() > previousSize) luaTests.back().hotkey = hotkey;
  }

  // Auto-load all .json files from weapons/.
  // Search order: next to exe → Contents/Resources/weapons (app bundle) → ../../weapons (dev build).
  {
    fs::path weaponsDir = exeDir / "weapons";
    std::error_code ecCheck;
    if (!fs::is_directory(weaponsDir, ecCheck))
      weaponsDir = exeDir / ".." / "Resources" / "weapons";
    if (!fs::is_directory(weaponsDir, ecCheck))
      weaponsDir = exeDir / ".." / ".." / "weapons";
    std::error_code ec;
    if (fs::is_directory(weaponsDir, ec)) {
      for (const auto& entry : fs::directory_iterator(weaponsDir, ec)) {
        if (entry.path().extension() != ".json") continue;
        const std::string p = fs::canonical(entry.path(), ec).string();
        if (ec) continue;
        bool already = false;
        for (const auto& t : luaTests) if (t.path == p) { already = true; break; }
        if (!already) importJson(p);
      }
    }
  }

  // Load any embedded weapons not yet covered by the library or weapons folder.
#ifdef HAVE_EMBEDDED_WEAPONS
  for (int i = 0; i < kEmbeddedWeaponCount; ++i) {
    const std::string fname(kEmbeddedWeapons[i].filename);
    const std::string embPath = kEmbedPfx + fname;
    bool already = false;
    for (const auto& t : luaTests)
      if (t.path == embPath || fs::path(t.path).filename().string() == fname)
        { already = true; break; }
    if (!already) importJson(embPath);
  }
#endif

  bool   isDragging = false;
  double dragCurX = 0, dragCurY = 0;

  auto sectionHeader = [](const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::CalcTextSize(label);
    dl->AddRectFilled(ImVec2(p.x, p.y + sz.y + 2), ImVec2(p.x + sz.x + 2, p.y + sz.y + 4),
                      IM_COL32(65, 115, 215, 200), 1.0f);
    ImGui::TextColored(ImVec4(0.82f, 0.90f, 1.00f, 1.0f), "%s", label);
  };

  auto panelAccent = []() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetWindowPos();
    dl->AddRectFilled(ImVec2(p.x + 10, p.y), ImVec2(p.x + 13, p.y + ImGui::GetWindowHeight()),
                      IM_COL32(55, 105, 215, 140));
  };

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (!gDroppedPaths.empty()) {
      for (const auto& path : gDroppedPaths) importJson(path);
      saveLibrary();
      gDroppedPaths.clear();
    }

    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key) {
      const bool down = glfwGetKey(window, key) == GLFW_PRESS;
      if (down && !keyWasDown[key]) {
        if (bindingTestIdx >= 0 && bindingTestIdx < static_cast<int>(luaTests.size())) {
          if (key == GLFW_KEY_ESCAPE) {
            bindingTestIdx = -1;
          } else {
            for (auto& test : luaTests) if (test.hotkey == key) test.hotkey = GLFW_KEY_UNKNOWN;
            luaTests[bindingTestIdx].hotkey = key;
            log("bound " + KeyLabel(key) + " to " + luaTests[bindingTestIdx].name);
            bindingTestIdx = -1;
            saveLibrary();
          }
        } else {
          for (int i = 0; i < static_cast<int>(luaTests.size()); ++i) {
            if (luaTests[i].hotkey == key) {
              selectedTestIdx = i;
            }
          }
        }
      }
      keyWasDown[key] = down;
    }

#ifdef _WIN32
    // Global side-button detection on Windows (GetAsyncKeyState works without focus)
    // GLFW btn index → Windows VK code
    static const struct { int btn; int vk; } winSideMap[] = {
      { 2, VK_MBUTTON }, { 3, VK_XBUTTON1 }, { 4, VK_XBUTTON2 }
    };
    for (const auto& m : winSideMap) {
      const bool down = (GetAsyncKeyState(m.vk) & 0x8000) != 0;
      if (down && !globalSideMouseWasDown[m.btn]) {
        const int hotkey = MouseHotkey(m.btn);
        if (bindingTestIdx >= 0 && bindingTestIdx < static_cast<int>(luaTests.size())) {
          for (auto& test : luaTests) if (test.hotkey == hotkey) test.hotkey = GLFW_KEY_UNKNOWN;
          luaTests[bindingTestIdx].hotkey = hotkey;
          log("bound " + KeyLabel(hotkey) + " to " + luaTests[bindingTestIdx].name);
          bindingTestIdx = -1;
          saveLibrary();
        } else {
          for (int i = 0; i < static_cast<int>(luaTests.size()); ++i) {
            if (luaTests[i].hotkey == hotkey) {
              selectedTestIdx = i;
            }
          }
        }
      }
      globalSideMouseWasDown[m.btn] = down;
    }
    // Global keyboard hotkey detection on Windows (works without focus)
    if (!glfwGetWindowAttrib(window, GLFW_FOCUSED)) {
      for (int i = 0; i < (int)luaTests.size(); ++i) {
        const int hotkey = luaTests[i].hotkey;
        if (hotkey < 0 || hotkey == GLFW_KEY_UNKNOWN) continue;
        const int vk = GlfwKeyToVK(hotkey);
        if (vk == 0) continue;
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !keyWasDown[hotkey]) {
          selectedTestIdx = i;
        }
        keyWasDown[hotkey] = down;
      }
    }
#endif
#ifdef __APPLE__
    // Global side-button detection (works without app focus)
    for (int btn = 2; btn < static_cast<int>(globalSideMouseWasDown.size()); ++btn) {
      const bool down = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, (CGMouseButton)btn);
      if (down && !globalSideMouseWasDown[btn]) {
        const int hotkey = MouseHotkey(btn);
        if (bindingTestIdx >= 0 && bindingTestIdx < static_cast<int>(luaTests.size())) {
          for (auto& test : luaTests) if (test.hotkey == hotkey) test.hotkey = GLFW_KEY_UNKNOWN;
          luaTests[bindingTestIdx].hotkey = hotkey;
          log("bound " + KeyLabel(hotkey) + " to " + luaTests[bindingTestIdx].name);
          bindingTestIdx = -1;
          saveLibrary();
        } else {
          for (int i = 0; i < static_cast<int>(luaTests.size()); ++i) {
            if (luaTests[i].hotkey == hotkey) {
              selectedTestIdx = i;
            }
          }
        }
      }
      globalSideMouseWasDown[btn] = down;
    }
#endif

    // Global RMB→LMB trigger (works without focus on the app window).
    {
#ifdef __APPLE__
      const bool curRmb = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonRight);
      const bool curLmb = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
#elif defined(_WIN32)
      const bool curRmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
      const bool curLmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
      const bool curRmb = false, curLmb = false;
#endif
      if (curRmb && !prevGlobalRmb && !curLmb) rmbFirst = true;
      if (!curRmb) { rmbFirst = false; }

      if (rmbFirst && curLmb && !prevGlobalLmb && !playing && selectedTestIdx >= 0) {
        playTest(selectedTestIdx);
        playingViaMouse = true;
      }
      if (playingViaMouse && !curLmb) {
        playing = false;
        playingViaMouse = false;
        log("-- stopped (LMB released) --");
      }
      prevGlobalRmb = curRmb;
      prevGlobalLmb = curLmb;
    }

    // Drain any incoming serial lines.
    if (connected) {
      std::vector<std::string> lines;
      if (!serial.PollLines(lines)) {
        log("!! device disconnected");
        serial.Close();
        connected = false;
        connectedPort = "";
        statusText = "disconnected";
      }
      for (auto& l : lines) log(l);
    }

    // Auto-scan ports every 2 s when not connected so hot-plugged devices appear.
    if (!connected) {
      const double scanNow = glfwGetTime();
      if (scanNow - lastPortScan > 2.0) {
        lastPortScan = scanNow;
        auto fresh = SerialPort::ListPorts();
        if (fresh != availablePorts) {
          availablePorts = fresh;
          if (selectedPort >= (int)availablePorts.size()) selectedPort = 0;
        }
      }
    }

    // Scenario playback (time-based, non-blocking).
    if (playing && (connected || simulate)) {
      double now = glfwGetTime();
      if (now >= nextSendTime) {
        if (playIdx < steps.size()) {
          const Step& cs = steps[playIdx];
          const int nSub = std::max(1, (int)std::round(smoothness));
          Step sub = cs;
          sub.dx = static_cast<int>(std::round(cs.dx / static_cast<float>(nSub)));
          sub.dy = static_cast<int>(std::round(cs.dy / static_cast<float>(nSub)));
          sendStep(sub);
          nextSendTime = now + cs.delayMs / 1000.0 / nSub;
          if (++subIdx >= nSub) {
            subIdx = 0;
            playIdx++;
          }
        } else {
          playing = false;
          subIdx = 0;
          log("-- playback finished --");
        }
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    DrawParticles(vp, static_cast<float>(glfwGetTime()));

    // --- Custom window controls (close / minimize) + drag ---
    {
      const ImVec2& mp = ImGui::GetIO().MousePos;
      const float btnR  = 6.5f;
      const float btnY  = vp->Pos.y + 18.0f;
      const ImVec2 closeC(vp->Pos.x + 18.0f, btnY);
      const ImVec2 miniC (vp->Pos.x + 38.0f, btnY);

      // Drag zone: top strip, right of buttons
      const bool inDragZone = mp.y >= vp->Pos.y && mp.y <= vp->Pos.y + 38.0f
                           && mp.x  > vp->Pos.x + 58.0f;
      if (ImGui::IsMouseClicked(0) && inDragZone) {
        isDragging = true;
        glfwGetCursorPos(window, &dragCurX, &dragCurY);
      }
      if (!ImGui::IsMouseDown(0)) isDragging = false;
      if (isDragging) {
        double curX, curY;
        glfwGetCursorPos(window, &curX, &curY);
        int wx, wy;
        glfwGetWindowPos(window, &wx, &wy);
        glfwSetWindowPos(window, wx + (int)(curX - dragCurX),
                                 wy + (int)(curY - dragCurY));
      }

      // Draw buttons on foreground
      ImDrawList* fdl = ImGui::GetForegroundDrawList();
      const bool closeHov = std::hypot(mp.x - closeC.x, mp.y - closeC.y) < btnR + 2.0f;
      const bool miniHov  = std::hypot(mp.x - miniC.x,  mp.y - miniC.y)  < btnR + 2.0f;

      fdl->AddCircleFilled(closeC, btnR, closeHov ? IM_COL32(255, 65, 55, 255) : IM_COL32(185, 50, 42, 200));
      if (closeHov) {
        fdl->AddLine({closeC.x - 3, closeC.y - 3}, {closeC.x + 3, closeC.y + 3}, IM_COL32(100, 10, 5, 255), 1.4f);
        fdl->AddLine({closeC.x + 3, closeC.y - 3}, {closeC.x - 3, closeC.y + 3}, IM_COL32(100, 10, 5, 255), 1.4f);
      }
      if (closeHov && ImGui::IsMouseClicked(0)) glfwSetWindowShouldClose(window, GLFW_TRUE);

      fdl->AddCircleFilled(miniC, btnR, miniHov ? IM_COL32(235, 170, 0, 255) : IM_COL32(175, 128, 0, 200));
      if (miniHov)
        fdl->AddLine({miniC.x - 3.5f, miniC.y}, {miniC.x + 3.5f, miniC.y}, IM_COL32(100, 70, 0, 255), 1.6f);
      if (miniHov && ImGui::IsMouseClicked(0)) glfwIconifyWindow(window);
    }

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("root", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // --- Header ---
    {
      ImDrawList* hdl = ImGui::GetWindowDrawList();
      const float now = static_cast<float>(glfwGetTime());
      const float winW = ImGui::GetWindowWidth();

      // Gradient top line
      const ImVec2 tlp = ImGui::GetCursorScreenPos();
      hdl->AddRectFilledMultiColor(
        ImVec2(tlp.x, tlp.y - 4), ImVec2(tlp.x + winW * 0.5f, tlp.y - 3),
        IM_COL32(0,0,0,0), IM_COL32(65,115,220,160), IM_COL32(65,115,220,160), IM_COL32(0,0,0,0));
      hdl->AddRectFilledMultiColor(
        ImVec2(tlp.x + winW * 0.5f, tlp.y - 4), ImVec2(tlp.x + winW, tlp.y - 3),
        IM_COL32(65,115,220,160), IM_COL32(0,0,0,0), IM_COL32(0,0,0,0), IM_COL32(65,115,220,160));

      // Title glow (shifted right to clear custom window buttons)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 52.0f);
      const char* title = "MOUSE LAB";
      const ImVec2 tPos = ImGui::GetCursorScreenPos();
      for (int d = 3; d >= 1; --d) {
        const int a = 9 / d;
        for (int ox = -d; ox <= d; ox += d)
          for (int oy = -d; oy <= d; oy += d)
            if (ox || oy)
              hdl->AddText(ImVec2(tPos.x + ox, tPos.y + oy), IM_COL32(80, 150, 255, a), title);
      }
      ImGui::Text("%s", title);
      ImGui::SameLine();
      ImGui::TextDisabled("-  Macro by Aiso.exe");

      // Device status (right-aligned)
      const char* statusLabel = connected  ? "[*] DEVICE ONLINE"
                              : simulate   ? "[S] SIM MODE"
                                           : "[ ] DEVICE OFFLINE";
      ImGui::SameLine(winW - (connected ? 178.0f : simulate ? 158.0f : 195.0f));
      if (connected) {
        const float pulse = 0.70f + 0.30f * std::sin(now * 2.4f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f * pulse + 0.20f, pulse, 0.60f * pulse + 0.20f, 1.0f));
      } else if (simulate) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.75f, 1.00f, 1.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.75f, 1.0f));
      }
      ImGui::Text("%s", statusLabel);
      ImGui::PopStyleColor();

      // Subtitle
      ImGui::TextDisabled("recoil pattern playback  ·  bind hotkeys  ·  sim mode");

      // Gradient separator line below header
      const ImVec2 sep = ImGui::GetCursorScreenPos();
      hdl->AddRectFilledMultiColor(
        sep, ImVec2(sep.x + winW * 0.5f, sep.y + 1.0f),
        IM_COL32(0,0,0,0), IM_COL32(55,100,210,130), IM_COL32(55,100,210,130), IM_COL32(0,0,0,0));
      hdl->AddRectFilledMultiColor(
        ImVec2(sep.x + winW * 0.5f, sep.y), ImVec2(sep.x + winW, sep.y + 1.0f),
        IM_COL32(55,100,210,130), IM_COL32(0,0,0,0), IM_COL32(0,0,0,0), IM_COL32(55,100,210,130));
      ImGui::Dummy(ImVec2(0, 5));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("connection", ImVec2(0, 62), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    panelAccent();
    ImGui::Indent(6.0f);
    ImGui::TextColored(ImVec4(0.82f, 0.90f, 1.00f, 1.0f), "CONNECTION");
    ImGui::SameLine(128);
    ImGui::SetNextItemWidth(300);
    std::string preview = availablePorts.empty() ? "(none found)"
                           : (selectedPort < (int)availablePorts.size() ? availablePorts[selectedPort] : "");
    if (ImGui::BeginCombo("##port", preview.c_str())) {
      for (int i = 0; i < (int)availablePorts.size(); ++i) {
        bool sel = (i == selectedPort);
        if (ImGui::Selectable(availablePorts[i].c_str(), sel)) selectedPort = i;
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine(440);
    if (ImGui::Button("Refresh")) {
      availablePorts = SerialPort::ListPorts();
    }
    ImGui::SameLine();
    if (!connected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.82f, 0.88f, 1.00f, 0.94f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.06f, 0.10f, 1.00f));
      if (ImGui::Button("Connect") && !availablePorts.empty()) {
        if (serial.Open(availablePorts[selectedPort], 115200)) {
          connected = true;
          connectedPort = availablePorts[selectedPort];
          statusText = "connected: " + connectedPort;
          log(statusText);
        } else {
          log("failed to open port");
        }
      }
      ImGui::PopStyleColor(2);
    } else {
      if (ImGui::Button("Disconnect")) {
        serial.Close();
        connected = false;
        connectedPort = "";
        statusText = "disconnected";
        playing = false;
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusText.c_str());
    ImGui::Unindent(6.0f);
    ImGui::EndChild();

    ImGui::Spacing();
    const float gap = 14.0f;
    const float leftWidth = ImGui::GetContentRegionAvail().x * 0.43f;
    const float rightWidth = ImGui::GetContentRegionAvail().x - leftWidth - gap;
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("library", ImVec2(leftWidth, 270), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    panelAccent();
    ImGui::Indent(6.0f);
    sectionHeader("SCRIPT LIBRARY");
    ImGui::SameLine();
    ImGui::TextDisabled("%d loaded", static_cast<int>(luaTests.size()));
    for (int i = 0; i < static_cast<int>(luaTests.size()); ++i) {
      const LuaTest& test = luaTests[i];
      const bool selected = selectedTestIdx == i;
      const float rowX = ImGui::GetCursorPosX();
      if (ImGui::Selectable(("##sel" + std::to_string(i)).c_str(), selected, 0, ImVec2(0, 20)))
        selectedTestIdx = i;
      ImGui::SameLine();
      ImGui::SetCursorPosX(rowX + 4.0f);
      ImGui::Text("%s", test.name.c_str());
      ImGui::SameLine();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const std::string countStr = std::to_string(test.steps.size());
      const ImVec2 countSize = ImGui::CalcTextSize(countStr.c_str());
      const ImVec2 badgeMin = ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + 2);
      const ImVec2 badgeMax = ImVec2(badgeMin.x + countSize.x + 10, badgeMin.y + countSize.y + 2);
      dl->AddRectFilled(badgeMin, badgeMax, IM_COL32(60, 80, 120, 180), 4.0f);
      dl->AddText(ImVec2(badgeMin.x + 5, badgeMin.y + 1), IM_COL32(180, 210, 255, 255), countStr.c_str());
      ImGui::Dummy(ImVec2(countSize.x + 14, 0));
      if (test.hotkey != GLFW_KEY_UNKNOWN) {
        ImGui::SameLine();
        const std::string hk = KeyLabel(test.hotkey);
        const ImVec2 hkSize = ImGui::CalcTextSize(hk.c_str());
        const ImVec2 hkMin = ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + 2);
        const ImVec2 hkMax = ImVec2(hkMin.x + hkSize.x + 10, hkMin.y + hkSize.y + 2);
        dl->AddRectFilled(hkMin, hkMax, IM_COL32(30, 40, 60, 200), 4.0f);
        dl->AddRect(hkMin, hkMax, IM_COL32(100, 130, 200, 120), 4.0f);
        dl->AddText(ImVec2(hkMin.x + 5, hkMin.y + 1), IM_COL32(160, 190, 255, 220), hk.c_str());
        ImGui::Dummy(ImVec2(hkSize.x + 14, 0));
      }
    }
    if (luaTests.empty()) ImGui::TextDisabled("No weapons — drop .json here.");
    if (ImGui::Button("x  Remove") && selectedTestIdx >= 0 && selectedTestIdx < static_cast<int>(luaTests.size())) {
      luaTests.erase(luaTests.begin() + selectedTestIdx);
      selectedTestIdx = luaTests.empty() ? -1 : std::min(selectedTestIdx, static_cast<int>(luaTests.size()) - 1);
      saveLibrary();
    }
    ImGui::Unindent(6.0f);
    ImGui::EndChild();
    ImGui::EndGroup();

    // Auto-fill scriptSens when switching to a weapon that has a known recorded sensitivity
    {
      static int prevSelectedTestIdx = -1;
      if (selectedTestIdx != prevSelectedTestIdx) {
        if (selectedTestIdx >= 0 && selectedTestIdx < static_cast<int>(luaTests.size())) {
          const float rs = luaTests[selectedTestIdx].recordedSens;
          if (rs > 0.0f) scriptSens = rs;
        }
        prevSelectedTestIdx = selectedTestIdx;
      }
    }

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("testDetails", ImVec2(rightWidth, 270), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    panelAccent();
    ImGui::Indent(6.0f);
    sectionHeader("SELECTED TEST");
    if (selectedTestIdx >= 0 && selectedTestIdx < static_cast<int>(luaTests.size())) {
      LuaTest& test = luaTests[selectedTestIdx];

      // Name + badges row
      ImGui::Text("%s", test.name.c_str());
      ImGui::SameLine();
      {
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        const std::string cnt = std::to_string(test.steps.size()) + " steps";
        const ImVec2 bsz = ImGui::CalcTextSize(cnt.c_str());
        const ImVec2 bmin = ImGui::GetCursorScreenPos();
        const ImVec2 bmax(bmin.x + bsz.x + 10, bmin.y + bsz.y + 2);
        bdl->AddRectFilled(bmin, bmax, IM_COL32(40, 68, 140, 210), 4.0f);
        bdl->AddText(ImVec2(bmin.x + 5, bmin.y + 1), IM_COL32(155, 200, 255, 245), cnt.c_str());
        ImGui::Dummy(ImVec2(bsz.x + 14, 0));
        if (test.hotkey != GLFW_KEY_UNKNOWN) {
          ImGui::SameLine();
          const std::string hk = KeyLabel(test.hotkey);
          const ImVec2 hsz = ImGui::CalcTextSize(hk.c_str());
          const ImVec2 hmin = ImGui::GetCursorScreenPos();
          const ImVec2 hmax(hmin.x + hsz.x + 10, hmin.y + hsz.y + 2);
          bdl->AddRectFilled(hmin, hmax, IM_COL32(28, 38, 62, 210), 4.0f);
          bdl->AddRect(hmin, hmax, IM_COL32(80, 120, 200, 120), 4.0f);
          bdl->AddText(ImVec2(hmin.x + 5, hmin.y + 1), IM_COL32(140, 175, 255, 220), hk.c_str());
          ImGui::Dummy(ImVec2(hsz.x + 14, 0));
        }
      }
      const bool isEmbedded = test.path.compare(0, kEmbedPfx.size(), kEmbedPfx) == 0;
      ImGui::TextDisabled("%s", isEmbedded ? "(built-in)" : test.path.c_str());
      ImGui::Spacing();

      // Trajectory canvas — path cached, only rebuilt on weapon change
      static int cachedTrajIdx = -1;
      static std::vector<ImVec2> trajPos;
      static float trajMinX = 0, trajMaxX = 0, trajMinY = 0, trajMaxY = 0;
      if (cachedTrajIdx != selectedTestIdx) {
        cachedTrajIdx = selectedTestIdx;
        trajPos.clear();
        trajMinX = trajMaxX = trajMinY = trajMaxY = 0;
        float cx2 = 0, cy2 = 0;
        trajPos.push_back({0.0f, 0.0f});
        for (const auto& s : test.steps) {
          cx2 += s.dx; cy2 += s.dy;
          trajPos.push_back({cx2, cy2});
          if (cx2 < trajMinX) trajMinX = cx2; if (cx2 > trajMaxX) trajMaxX = cx2;
          if (cy2 < trajMinY) trajMinY = cy2; if (cy2 > trajMaxY) trajMaxY = cy2;
        }
      }

      const float colH = 120.0f;
      ImGui::BeginChild("traj", ImVec2(0, colH), true, ImGuiWindowFlags_NoScrollbar);
      if (trajPos.size() > 1) {
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        const ImVec2 cp = ImGui::GetWindowPos();
        const ImVec2 cs = ImGui::GetWindowSize();
        const float pad = 12.0f;

        const float rng = std::max(std::max(trajMaxX - trajMinX, trajMaxY - trajMinY), 1.0f);
        const float sc2 = (std::min(cs.x, cs.y) - pad * 2) / rng;
        const float ox2 = cp.x + cs.x * 0.5f - (trajMinX + trajMaxX) * 0.5f * sc2;
        const float oy2 = cp.y + cs.y * 0.5f - (trajMinY + trajMaxY) * 0.5f * sc2;
        const auto& pos = trajPos;

        // Cross-hair at origin
        tdl->AddLine(ImVec2(ox2 - 5, oy2), ImVec2(ox2 + 5, oy2), IM_COL32(60, 80, 120, 100));
        tdl->AddLine(ImVec2(ox2, oy2 - 5), ImVec2(ox2, oy2 + 5), IM_COL32(60, 80, 120, 100));

        // Path: cyan → orange gradient
        for (int pi = 1; pi < (int)pos.size(); ++pi) {
          const float t = (float)pi / (float)(pos.size() - 1);
          const ImVec2 a(ox2 + pos[pi-1].x * sc2, oy2 + pos[pi-1].y * sc2);
          const ImVec2 b(ox2 + pos[pi].x * sc2,   oy2 + pos[pi].y * sc2);
          tdl->AddLine(a, b, IM_COL32((int)(80+160*t), (int)(195-80*t), (int)(255-155*t), 215), 1.6f);
        }
        // Start (cyan dot) / end (orange dot)
        tdl->AddCircleFilled(ImVec2(ox2 + pos.front().x * sc2, oy2 + pos.front().y * sc2), 3.5f, IM_COL32(80, 205, 255, 230));
        if (pos.size() > 1)
          tdl->AddCircleFilled(ImVec2(ox2 + pos.back().x * sc2, oy2 + pos.back().y * sc2), 3.5f, IM_COL32(255, 125, 70, 230));

        // Playback cursor (pulsing yellow dot)
        if (playing && !pos.empty()) {
          const size_t ci = std::min(playIdx, pos.size() - 1);
          const float pp = 0.6f + 0.4f * std::sin(static_cast<float>(glfwGetTime()) * 7.0f);
          const ImVec2 cur(ox2 + pos[ci].x * sc2, oy2 + pos[ci].y * sc2);
          tdl->AddCircleFilled(cur, 5.5f * pp, IM_COL32(255, 225, 60, (int)(210 * pp)));
          tdl->AddCircle(cur, 9.0f * pp, IM_COL32(255, 200, 50, (int)(70 * pp)), 16);
        }
      } else {
        const ImVec2 ctr(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f - 25,
                         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f - 7);
        ImGui::GetWindowDrawList()->AddText(ctr, IM_COL32(70, 85, 115, 180), "no steps");
      }
      ImGui::EndChild();

      ImGui::Spacing();

      // Controls: Bind | Run | Stop | speed slider
      if (bindingTestIdx == selectedTestIdx) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "Press any key  (Esc cancels)");
      } else if (ImGui::Button("Bind key", ImVec2(72, 0))) {
        bindingTestIdx = selectedTestIdx;
        log("waiting for hotkey -> " + test.name);
      }
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.82f, 0.88f, 1.00f, 0.94f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.06f, 0.10f, 1.00f));
      if (ImGui::Button("> Run", ImVec2(72, 0))) playTest(selectedTestIdx);
      ImGui::PopStyleColor(2);
      ImGui::SameLine();
      ImGui::Checkbox("sim", &simulate);
      ImGui::SameLine();
      if (playing && ImGui::Button("Stop")) {
        playing = false;
        log("-- playback stopped --");
      }

      ImGui::Spacing();
      {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.00f, 1.00f));
        ImGui::Text("my sens");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputFloat("##gsens", &gameSens, 0.0f, 0.0f, "%.6f"))
          gameSens = std::clamp(gameSens, 0.01f, 2.0f);
        sensitivity = scriptSens / gameSens;
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.70f, 0.70f, 0.70f, 1.00f));
        ImGui::TextDisabled("scale: %.4fx", sensitivity);
        ImGui::PopStyleColor();
      }
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("##smooth", &smoothness, 0.1f, 2.5f, "smooth  %.1f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split each move into N sub-steps");

      if (playing && !steps.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.50f, 0.78f, 1.00f, 0.90f));
        ImGui::ProgressBar((float)playIdx / (float)steps.size(), ImVec2(-1.0f, 5.0f), "");
        ImGui::PopStyleColor();
      }
    } else {
      ImGui::Spacing();
      ImGui::TextDisabled("Select a script from the library to preview it.");
      ImGui::Spacing();
      ImGui::TextDisabled("Drop .json files anywhere to add them.");
    }
    ImGui::Unindent(6.0f);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("storage", ImVec2(0, 44), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    panelAccent();
    ImGui::Indent(6.0f);
    ImGui::TextColored(ImVec4(0.82f, 0.90f, 1.00f, 1.0f), "ADD WEAPON");
    ImGui::SameLine(130);
    ImGui::SetNextItemWidth(350);
    ImGui::InputTextWithHint("##jsonPath", "Paste .json path or drop file into window", jsonPathBuf, sizeof(jsonPathBuf));
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
      importJson(std::string(jsonPathBuf));
      saveLibrary();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
      const std::string p = OpenJsonFileDialog();
      if (!p.empty()) {
        std::snprintf(jsonPathBuf, sizeof(jsonPathBuf), "%s", p.c_str());
        importJson(p);
        saveLibrary();
      }
    }
    ImGui::Unindent(6.0f);
    ImGui::EndChild();

    // --- Log ---
    const float statusBarH = 28.0f;
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("logPanel", ImVec2(0, ImGui::GetContentRegionAvail().y - statusBarH - 6), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    panelAccent();
    ImGui::Indent(6.0f);
    sectionHeader("ACTIVITY");
    ImGui::SameLine();
    ImGui::TextDisabled("live serial output");
    ImGui::SameLine(ImGui::GetWindowWidth() - 72);
    if (ImGui::SmallButton("Clear")) logLines.clear();
    ImGui::BeginChild("log", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
    {
      const float flashDur = 0.75f;
      const float logAge = static_cast<float>(glfwGetTime() - lastLogTime);
      const float flash = (logAge < flashDur) ? (1.0f - logAge / flashDur) : 0.0f;
      const int totalLines = static_cast<int>(logLines.size());
      ImDrawList* ldl = ImGui::GetWindowDrawList();

      for (int li = 0; li < totalLines; ++li) {
        const auto& l = logLines[li];
        const bool hasTs = l.size() > 8 && l[0] == '[';
        const char* lc = l.c_str() + (hasTs ? 8 : 0);
        const size_t lcsz = l.size() - (hasTs ? 8 : 0);

        ImVec4 color;
        if (lcsz >= 2 && lc[0] == '-' && lc[1] == '>') color = ImVec4(0.50f, 0.82f, 1.00f, 1.00f);
        else if (lcsz >= 2 && lc[0] == 'O' && lc[1] == 'K') color = ImVec4(0.50f, 1.00f, 0.65f, 1.00f);
        else if ((lcsz >= 3 && lc[0] == 'E' && lc[1] == 'R' && lc[2] == 'R') ||
                 (lcsz >= 2 && lc[0] == '!' && lc[1] == '!')) color = ImVec4(1.00f, 0.42f, 0.42f, 1.00f);
        else if (lcsz >= 2 && lc[0] == '-' && lc[1] == '-') color = ImVec4(0.58f, 0.64f, 0.78f, 1.00f);
        else color = ImVec4(0.95f, 0.97f, 1.00f, 1.00f);

        // Flash highlight on newest entries
        if (flash > 0.0f && li >= totalLines - 2) {
          const float f2 = flash * flash;
          const ImVec2 lp = ImGui::GetCursorScreenPos();
          ldl->AddRectFilled(lp,
            ImVec2(lp.x + ImGui::GetContentRegionAvail().x, lp.y + ImGui::GetTextLineHeightWithSpacing()),
            IM_COL32(55, 115, 220, static_cast<int>(f2 * 38.0f)));
        }

        if (hasTs) {
          ImGui::TextDisabled("%.8s", l.c_str());
          ImGui::SameLine(0, 0);
          ImGui::TextColored(color, "%s", lc);
        } else {
          ImGui::TextColored(color, "%s", l.c_str());
        }
      }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::Unindent(6.0f);
    ImGui::EndChild();

    // --- Status bar ---
    {
      int totalSteps = 0;
      for (const auto& t : luaTests) totalSteps += static_cast<int>(t.steps.size());
      ImDrawList* sbdl = ImGui::GetWindowDrawList();
      const ImVec2 sbPos = ImGui::GetCursorScreenPos();
      const float sbW = ImGui::GetContentRegionAvail().x;
      sbdl->AddRectFilled(sbPos, ImVec2(sbPos.x + sbW, sbPos.y + statusBarH), IM_COL32(10, 13, 22, 230));
      sbdl->AddLine(sbPos, ImVec2(sbPos.x + sbW, sbPos.y), IM_COL32(50, 85, 160, 90));
      ImGui::BeginChild("##statusbar", ImVec2(0, statusBarH), false, ImGuiWindowFlags_NoScrollbar);
      ImGui::SetCursorPosY(6);
      ImGui::SetCursorPosX(14);
      if (connected) {
        ImGui::TextColored(ImVec4(0.50f, 0.95f, 0.65f, 1.0f), "[*] %s  @ 115200 baud", connectedPort.c_str());
      } else if (simulate) {
        ImGui::TextColored(ImVec4(0.65f, 0.75f, 1.00f, 1.0f), "[S] SIM MODE  --  no hardware needed");
      } else {
        ImGui::TextDisabled("[ ] not connected");
      }
      if (!luaTests.empty()) {
        ImGui::SameLine(360);
        ImGui::TextDisabled("%d tests  |  %d steps", static_cast<int>(luaTests.size()), totalSteps);
      }
      if (playing && !steps.empty()) {
        ImGui::SameLine(560);
        ImGui::TextColored(ImVec4(0.50f, 0.82f, 1.00f, 1.0f), ">  step %d / %d",
                           static_cast<int>(playIdx), static_cast<int>(steps.size()));
      }
      ImGui::EndChild();
    }

    ImGui::End();

    ImGui::Render();
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.09f, 0.09f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  serial.Close();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
