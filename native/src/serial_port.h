#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal cross-platform (macOS/Linux/Windows) blocking-free serial port
// wrapper used to talk to the mouse test injector firmware.
class SerialPort {
 public:
  SerialPort() = default;
  ~SerialPort();

  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;

  static std::vector<std::string> ListPorts();

  bool Open(const std::string& portName, int baudRate);
  void Close();
  bool IsOpen() const { return isOpen_; }

  // Writes a line, appending '\n'. Returns false on I/O error.
  bool WriteLine(const std::string& line);

  // Non-blocking: appends any newly available bytes to outLines as
  // complete lines (split on '\n', trimmed of '\r'). Returns false on I/O
  // error (e.g. device unplugged).
  bool PollLines(std::vector<std::string>& outLines);

 private:
#if defined(_WIN32)
  void* handle_ = nullptr;  // HANDLE
#else
  int fd_ = -1;
#endif
  bool isOpen_ = false;
  std::string rxBuffer_;
};
