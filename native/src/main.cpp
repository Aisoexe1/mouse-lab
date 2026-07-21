// Mouse driver test injector - GUI front-end (C++/Dear ImGui).
//
// Talks to the Arduino Leonardo firmware in ../arduino/mouse_test_injector
// over serial to build and play back relative-move test scenarios for
// driver/firmware regression testing.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "serial_port.h"

struct Step {
  int dx = 0;
  int dy = 0;
  int delayMs = 50;
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

static std::string OpenLuaFileDialog() {
#ifdef _WIN32
  char path[MAX_PATH] = {};
  OPENFILENAMEA dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.lpstrFilter = "Lua files\0*.lua\0All files\0*.*\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  return GetOpenFileNameA(&dialog) ? path : "";
#else
#ifdef __APPLE__
  const char* command = "osascript -e 'POSIX path of (choose file with prompt \"Select Lua test\" of type {\"lua\"})' 2>/dev/null";
#else
  const char* command = "zenity --file-selection --file-filter='Lua files | *.lua' 2>/dev/null";
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
  style.WindowPadding = ImVec2(22, 20);
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
  std::vector<Particle> pts;
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

static std::vector<Step> LoadScenario(const std::string& path, std::string& err) {
  std::vector<Step> steps;
  std::ifstream f(path);
  if (!f) {
    err = "cannot open file";
    return steps;
  }
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::istringstream ss(line);
    Step s;
    int l = 0, r = 0, m = 0;
    if (ss >> s.dx >> s.dy >> s.delayMs >> l >> r >> m) {
      s.left = l != 0;
      s.right = r != 0;
      s.middle = m != 0;
      steps.push_back(s);
    }
  }
  return steps;
}

static bool SaveScenario(const std::string& path, const std::vector<Step>& steps) {
  std::ofstream f(path);
  if (!f) return false;
  for (const auto& s : steps) {
    f << s.dx << ' ' << s.dy << ' ' << s.delayMs << ' '
      << (s.left ? 1 : 0) << ' ' << (s.right ? 1 : 0) << ' ' << (s.middle ? 1 : 0) << '\n';
  }
  return true;
}

static std::vector<Step> LoadLuaScenario(const std::string& path, std::string& err) {
  std::ifstream f(path);
  if (!f) {
    err = "cannot open Lua file";
    return {};
  }

  std::vector<Step> imported;
  bool left = false, right = false, middle = false;
  int pendingDelay = 50;
  std::string line;
  int lineNumber = 0;
  while (std::getline(f, line)) {
    ++lineNumber;
    const size_t comment = line.find("--");
    if (comment != std::string::npos) line.erase(comment);

    size_t move = line.find("move(");
    size_t argumentOffset = 5;
    if (move == std::string::npos) {
      move = line.find("MoveMouseRelative(");
      argumentOffset = 18;
    }
    if (move != std::string::npos) {
      Step step;
      int delay = pendingDelay;
      const int count = std::sscanf(line.c_str() + move + argumentOffset, "%d , %d , %d", &step.dx, &step.dy, &delay);
      if (count < 2) {
        err = "invalid move() on line " + std::to_string(lineNumber);
        return {};
      }
      step.dx = std::clamp(step.dx, -127, 127);
      step.dy = std::clamp(step.dy, -127, 127);
      step.delayMs = std::max(0, delay);
      step.left = left;
      step.right = right;
      step.middle = middle;
      imported.push_back(step);
      pendingDelay = 50;
      continue;
    }

    size_t wait = line.find("sleep(");
    if (wait == std::string::npos) wait = line.find("Sleep(");
    if (wait == std::string::npos) wait = line.find("wait(");
    if (wait != std::string::npos) {
      const size_t open = line.find('(', wait);
      int delay = 0;
      if (std::sscanf(line.c_str() + open + 1, "%d", &delay) != 1) {
        err = "invalid sleep()/wait() on line " + std::to_string(lineNumber);
        return {};
      }
      if (imported.empty()) pendingDelay = std::max(0, delay);
      else imported.back().delayMs = std::max(0, delay);
      continue;
    }

    if (line.find("left_down(") != std::string::npos) left = true;
    if (line.find("left_up(") != std::string::npos) left = false;
    if (line.find("right_down(") != std::string::npos) right = true;
    if (line.find("right_up(") != std::string::npos) right = false;
    if (line.find("middle_down(") != std::string::npos) middle = true;
    if (line.find("middle_up(") != std::string::npos) middle = false;
  }
  if (imported.empty()) err = "no move() commands found";
  return imported;
}

static std::vector<std::string> gDroppedLuaPaths;

static void OnFileDrop(GLFWwindow*, int count, const char** paths) {
  for (int i = 0; i < count; ++i)
    if (paths && paths[i]) gDroppedLuaPaths.emplace_back(paths[i]);
}

int main(int /*argc*/, char* argv[]) {
  if (!glfwInit()) return 1;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

  GLFWwindow* window = glfwCreateWindow(1120, 720, "Mouse Driver Test Injector", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  glfwSetDropCallback(window, OnFileDrop);

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
  std::string statusText = "disconnected";

  std::vector<Step> steps;
  std::vector<LuaTest> luaTests;
  int selectedTestIdx = -1;
  int bindingTestIdx = -1;
  std::vector<char> keyWasDown(GLFW_KEY_LAST + 1, false);
  std::vector<char> mouseWasDown(GLFW_MOUSE_BUTTON_LAST + 1, false);
  const std::string libraryPath = "lua_tests.library";

  std::vector<std::string> logLines;
  char luaPathBuf[512] = "";

  bool playing = false;
  size_t playIdx = 0;
  double nextSendTime = 0.0;
  double lastLogTime = 0.0;

  auto log = [&](const std::string& s) {
    const double t = glfwGetTime();
    lastLogTime = t;
    char ts[10];
    snprintf(ts, sizeof(ts), "[%02d:%02d] ", static_cast<int>(t) / 60, static_cast<int>(t) % 60);
    logLines.push_back(std::string(ts) + s);
    if (logLines.size() > 500) logLines.erase(logLines.begin());
  };

  auto sendStep = [&](const Step& s) {
    if (!serial.IsOpen()) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "M,%d,%d,%d", s.dx, s.dy, s.ButtonsMask());
    if (serial.WriteLine(buf)) {
      log(std::string("-> ") + buf);
    } else {
      log("send error");
    }
  };

  auto saveLibrary = [&]() {
    if (!SaveLuaLibrary(libraryPath, luaTests)) log("could not save Lua test library");
  };

  auto importLua = [&](const std::string& path) {
    std::string err;
    auto imported = LoadLuaScenario(path, err);
    if (!err.empty()) {
      log("Lua import failed: " + err);
      return;
    }
    LuaTest test;
    test.name = FileNameWithoutExtension(path);
    test.path = path;
    test.steps = std::move(imported);
    luaTests.push_back(std::move(test));
    selectedTestIdx = static_cast<int>(luaTests.size()) - 1;
    log("added Lua test: " + luaTests.back().name + " (" + std::to_string(luaTests.back().steps.size()) + " steps)");
  };

  auto playTest = [&](int index) {
    if (index < 0 || index >= static_cast<int>(luaTests.size())) return;
    if (!connected) {
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
    nextSendTime = glfwGetTime();
    log("-- running: " + luaTests[index].name + " --");
  };

  for (const auto& [hotkey, path] : LoadLuaLibrary(libraryPath)) {
    const size_t previousSize = luaTests.size();
    importLua(path);
    if (luaTests.size() > previousSize) luaTests.back().hotkey = hotkey;
  }

  // Auto-load all .lua files from the weapons/ folder next to the project root
  {
    namespace fs = std::filesystem;
    const fs::path weaponsDir = fs::path(argv[0]).parent_path() / ".." / ".." / "weapons";
    std::error_code ec;
    if (fs::is_directory(weaponsDir, ec)) {
      for (const auto& entry : fs::directory_iterator(weaponsDir, ec)) {
        if (entry.path().extension() == ".lua") {
          const std::string p = fs::canonical(entry.path(), ec).string();
          if (ec) continue;
          bool already = false;
          for (const auto& t : luaTests) if (t.path == p) { already = true; break; }
          if (!already) importLua(p);
        }
      }
    }
  }

  float speedMult = 1.0f;

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
    const ImVec2 p = ImGui::GetWindowPos();
    dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + ImGui::GetWindowHeight()), IM_COL32(55, 105, 215, 140));
  };

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (!gDroppedLuaPaths.empty()) {
      for (const auto& path : gDroppedLuaPaths) {
        std::snprintf(luaPathBuf, sizeof(luaPathBuf), "%s", path.c_str());
        importLua(path);
      }
      saveLibrary();
      gDroppedLuaPaths.clear();
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
              log("hotkey " + KeyLabel(key) + " pressed -> " + luaTests[i].name);
              playTest(i);
            }
          }
        }
      }
      keyWasDown[key] = down;
    }

    for (int button = GLFW_MOUSE_BUTTON_4; button <= GLFW_MOUSE_BUTTON_LAST; ++button) {
      const bool down = glfwGetMouseButton(window, button) == GLFW_PRESS;
      if (down && !mouseWasDown[button]) {
        const int hotkey = MouseHotkey(button);
        if (bindingTestIdx >= 0 && bindingTestIdx < static_cast<int>(luaTests.size())) {
          for (auto& test : luaTests) if (test.hotkey == hotkey) test.hotkey = GLFW_KEY_UNKNOWN;
          luaTests[bindingTestIdx].hotkey = hotkey;
          log("bound " + KeyLabel(hotkey) + " to " + luaTests[bindingTestIdx].name);
          bindingTestIdx = -1;
          saveLibrary();
        } else {
          for (int i = 0; i < static_cast<int>(luaTests.size()); ++i) {
            if (luaTests[i].hotkey == hotkey) {
              log("hotkey " + KeyLabel(hotkey) + " pressed -> " + luaTests[i].name);
              playTest(i);
            }
          }
        }
      }
      mouseWasDown[button] = down;
    }

    // Drain any incoming serial lines.
    if (connected) {
      std::vector<std::string> lines;
      if (!serial.PollLines(lines)) {
        log("!! device disconnected");
        serial.Close();
        connected = false;
        statusText = "disconnected";
      }
      for (auto& l : lines) log(l);
    }

    // Scenario playback (time-based, non-blocking).
    if (playing && connected) {
      double now = glfwGetTime();
      if (now >= nextSendTime) {
        if (playIdx < steps.size()) {
          sendStep(steps[playIdx]);
          nextSendTime = now + steps[playIdx].delayMs / 1000.0 / static_cast<double>(speedMult);
          playIdx++;
        } else {
          playing = false;
          log("-- playback finished --");
        }
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    DrawParticles(vp, static_cast<float>(glfwGetTime()));
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("root", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

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

      // Title glow
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
      ImGui::TextDisabled("\xe2\x80\x94  test injector");

      // Device status (right-aligned)
      ImGui::SameLine(winW - (connected ? 178.0f : 195.0f));
      if (connected) {
        const float pulse = 0.70f + 0.30f * std::sin(now * 2.4f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f * pulse + 0.20f, pulse, 0.60f * pulse + 0.20f, 1.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.75f, 1.0f));
      }
      ImGui::Text("%s", connected ? "\xe2\x97\x8f  DEVICE ONLINE" : "\xe2\x97\x8b  DEVICE OFFLINE");
      ImGui::PopStyleColor();

      // Subtitle
      ImGui::TextDisabled("precision mouse input validation \xe2\x80\x94 send relative-move sequences to firmware");

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

    ImGui::BeginChild("connection", ImVec2(0, 76), true, ImGuiWindowFlags_NoScrollbar);
    panelAccent();
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
          statusText = "connected: " + availablePorts[selectedPort];
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
        statusText = "disconnected";
        playing = false;
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusText.c_str());
    ImGui::EndChild();

    ImGui::Spacing();
    const float gap = 14.0f;
    const float leftWidth = ImGui::GetContentRegionAvail().x * 0.43f;
    const float rightWidth = ImGui::GetContentRegionAvail().x - leftWidth - gap;
    ImGui::BeginGroup();
    ImGui::BeginChild("library", ImVec2(leftWidth, 270), true);
    panelAccent();
    sectionHeader("LUA TEST LIBRARY");
    ImGui::SameLine();
    ImGui::TextDisabled("%d saved", static_cast<int>(luaTests.size()));
    ImGui::TextDisabled("Drop .lua files to add tests.");
    ImGui::BeginChild("testList", ImVec2(0, 170), false);
    for (int i = 0; i < static_cast<int>(luaTests.size()); ++i) {
      const LuaTest& test = luaTests[i];
      const bool selected = selectedTestIdx == i;
      if (ImGui::Selectable(("##sel" + std::to_string(i)).c_str(), selected, 0, ImVec2(0, 18)))
        selectedTestIdx = i;
      ImGui::SameLine(8);
      ImGui::Text("%s", test.name.c_str());
      ImGui::SameLine();
      // step count badge
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const std::string countStr = std::to_string(test.steps.size());
      const ImVec2 countSize = ImGui::CalcTextSize(countStr.c_str());
      const ImVec2 badgeMin = ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + 2);
      const ImVec2 badgeMax = ImVec2(badgeMin.x + countSize.x + 10, badgeMin.y + countSize.y + 2);
      dl->AddRectFilled(badgeMin, badgeMax, IM_COL32(60, 80, 120, 180), 4.0f);
      dl->AddText(ImVec2(badgeMin.x + 5, badgeMin.y + 1), IM_COL32(180, 210, 255, 255), countStr.c_str());
      ImGui::Dummy(ImVec2(countSize.x + 14, 0));
      // hotkey badge
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
    if (luaTests.empty()) ImGui::TextDisabled("No tests yet — drop a .lua file here.");
    ImGui::EndChild();
    if (ImGui::Button("\xe2\x9c\x95  Remove") && selectedTestIdx >= 0 && selectedTestIdx < static_cast<int>(luaTests.size())) {
      luaTests.erase(luaTests.begin() + selectedTestIdx);
      selectedTestIdx = luaTests.empty() ? -1 : std::min(selectedTestIdx, static_cast<int>(luaTests.size()) - 1);
      saveLibrary();
    }
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::BeginChild("testDetails", ImVec2(rightWidth, 270), true);
    panelAccent();
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
      ImGui::TextDisabled("%s", test.path.c_str());
      ImGui::Spacing();

      // Two columns: step list | trajectory canvas
      const float colH = 108.0f;
      const float avail = ImGui::GetContentRegionAvail().x;

      ImGui::BeginChild("stepList2", ImVec2(avail * 0.42f, colH), false);
      for (int si = 0; si < (int)test.steps.size(); ++si) {
        const Step& s = test.steps[si];
        const bool active = playing && (int)playIdx == si;
        if (active) {
          ImDrawList* adl = ImGui::GetWindowDrawList();
          const ImVec2 ap = ImGui::GetCursorScreenPos();
          adl->AddRectFilled(ap, ImVec2(ap.x + ImGui::GetContentRegionAvail().x,
            ap.y + ImGui::GetTextLineHeightWithSpacing()), IM_COL32(40, 90, 180, 75));
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.88f, 1.00f, 1.00f));
        }
        ImGui::TextDisabled("%2d", si + 1);
        ImGui::SameLine();
        char sb[40];
        snprintf(sb, sizeof(sb), "(%+d,%+d) %dms", s.dx, s.dy, s.delayMs);
        ImGui::Text("%s", sb);
        if (active) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
      }
      ImGui::EndChild();

      ImGui::SameLine(0, 8);

      // Trajectory canvas
      ImGui::BeginChild("traj", ImVec2(0, colH), true, ImGuiWindowFlags_NoScrollbar);
      if (!test.steps.empty()) {
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        const ImVec2 cp = ImGui::GetWindowPos();
        const ImVec2 cs = ImGui::GetWindowSize();
        const float pad = 12.0f;

        std::vector<ImVec2> pos;
        pos.reserve(test.steps.size() + 1);
        float cx2 = 0, cy2 = 0, minX2 = 0, maxX2 = 0, minY2 = 0, maxY2 = 0;
        pos.push_back({0.0f, 0.0f});
        for (const auto& s : test.steps) {
          cx2 += s.dx; cy2 += s.dy;
          pos.push_back({cx2, cy2});
          if (cx2 < minX2) minX2 = cx2; if (cx2 > maxX2) maxX2 = cx2;
          if (cy2 < minY2) minY2 = cy2; if (cy2 > maxY2) maxY2 = cy2;
        }
        const float rng = std::max(std::max(maxX2 - minX2, maxY2 - minY2), 1.0f);
        const float sc2 = (std::min(cs.x, cs.y) - pad * 2) / rng;
        const float ox2 = cp.x + cs.x * 0.5f - (minX2 + maxX2) * 0.5f * sc2;
        const float oy2 = cp.y + cs.y * 0.5f - (minY2 + maxY2) * 0.5f * sc2;

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
      if (ImGui::Button("\xe2\x96\xb6 Run", ImVec2(72, 0))) playTest(selectedTestIdx);
      ImGui::PopStyleColor(2);
      ImGui::SameLine();
      if (playing && ImGui::Button("\xe2\x96\xa0 Stop")) {
        playing = false;
        log("-- playback stopped --");
      }

      if (playing && !steps.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.50f, 0.78f, 1.00f, 0.90f));
        ImGui::ProgressBar((float)playIdx / (float)steps.size(), ImVec2(-1.0f, 5.0f), "");
        ImGui::PopStyleColor();
      }
    } else {
      ImGui::Spacing();
      ImGui::TextDisabled("Select a test from the library to preview it.");
      ImGui::Spacing();
      ImGui::TextDisabled("Drop a .lua file anywhere in the window to add it.");
    }
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::BeginChild("storage", ImVec2(0, 70), true, ImGuiWindowFlags_NoScrollbar);
    panelAccent();
    ImGui::TextColored(ImVec4(0.82f, 0.90f, 1.00f, 1.0f), "ADD LUA TEST");
    ImGui::SameLine(130);
    ImGui::SetNextItemWidth(350);
    ImGui::InputTextWithHint("##luaPath", "Drop a .lua file or paste its path", luaPathBuf, sizeof(luaPathBuf));
    ImGui::SameLine();
    if (ImGui::Button("Add test")) {
      importLua(luaPathBuf);
      saveLibrary();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
      const std::string selectedPath = OpenLuaFileDialog();
      if (!selectedPath.empty()) {
        std::snprintf(luaPathBuf, sizeof(luaPathBuf), "%s", selectedPath.c_str());
        importLua(selectedPath);
        saveLibrary();
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("move(dx, dy[, delay]) · sleep(ms)");
    ImGui::EndChild();

    // --- Log ---
    const float statusBarH = 28.0f;
    ImGui::Spacing();
    ImGui::BeginChild("logPanel", ImVec2(0, ImGui::GetContentRegionAvail().y - statusBarH - 6), true);
    panelAccent();
    sectionHeader("ACTIVITY");
    ImGui::SameLine();
    ImGui::TextDisabled("live serial output");
    ImGui::SameLine(ImGui::GetWindowWidth() - 72);
    if (ImGui::SmallButton("Clear")) logLines.clear();
    ImGui::BeginChild("log", ImVec2(0, 0), false);
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
        const std::string& port = (!availablePorts.empty() && selectedPort < (int)availablePorts.size())
          ? availablePorts[selectedPort] : statusText;
        ImGui::TextColored(ImVec4(0.50f, 0.95f, 0.65f, 1.0f), "\xe2\x97\x8f  %s  @ 115200 baud", port.c_str());
      } else {
        ImGui::TextDisabled("\xe2\x97\x8b  not connected");
      }
      if (!luaTests.empty()) {
        ImGui::SameLine(360);
        ImGui::TextDisabled("%d tests  \xc2\xb7  %d steps", static_cast<int>(luaTests.size()), totalSteps);
      }
      if (playing && !steps.empty()) {
        ImGui::SameLine(560);
        ImGui::TextColored(ImVec4(0.50f, 0.82f, 1.00f, 1.0f), "\xe2\x96\xb6  step %d / %d",
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
