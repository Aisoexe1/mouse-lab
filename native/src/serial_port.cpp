#include "serial_port.h"

#if defined(_WIN32)

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>

std::vector<std::string> SerialPort::ListPorts() {
  std::vector<std::string> ports;
  HDEVINFO devInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
  if (devInfo == INVALID_HANDLE_VALUE) return ports;

  SP_DEVINFO_DATA data{};
  data.cbSize = sizeof(SP_DEVINFO_DATA);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &data); ++i) {
    HKEY key = SetupDiOpenDevRegKey(devInfo, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) continue;
    char portName[256];
    DWORD size = sizeof(portName);
    DWORD type = 0;
    if (RegQueryValueExA(key, "PortName", nullptr, &type, (LPBYTE)portName, &size) == ERROR_SUCCESS) {
      ports.emplace_back(portName);
    }
    RegCloseKey(key);
  }
  SetupDiDestroyDeviceInfoList(devInfo);
  return ports;
}

bool SerialPort::Open(const std::string& portName, int baudRate) {
  Close();
  std::string path = "\\\\.\\" + portName;
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  DCB dcb{};
  dcb.DCBlength = sizeof(DCB);
  GetCommState(h, &dcb);
  dcb.BaudRate = baudRate;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  SetCommState(h, &dcb);

  COMMTIMEOUTS timeouts{};
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutConstant = 0;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 200;
  timeouts.WriteTotalTimeoutMultiplier = 0;
  SetCommTimeouts(h, &timeouts);

  handle_ = h;
  isOpen_ = true;
  return true;
}

void SerialPort::Close() {
  if (isOpen_) {
    CloseHandle((HANDLE)handle_);
    handle_ = nullptr;
    isOpen_ = false;
  }
}

bool SerialPort::WriteLine(const std::string& line) {
  if (!isOpen_) return false;
  std::string data = line + "\n";
  DWORD written = 0;
  return WriteFile((HANDLE)handle_, data.data(), (DWORD)data.size(), &written, nullptr) != 0;
}

bool SerialPort::PollLines(std::vector<std::string>& outLines) {
  if (!isOpen_) return false;
  char buf[256];
  DWORD read = 0;
  if (!ReadFile((HANDLE)handle_, buf, sizeof(buf), &read, nullptr)) return false;
  if (read > 0) rxBuffer_.append(buf, read);

  size_t pos;
  while ((pos = rxBuffer_.find('\n')) != std::string::npos) {
    std::string line = rxBuffer_.substr(0, pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    outLines.push_back(line);
    rxBuffer_.erase(0, pos + 1);
  }
  return true;
}

SerialPort::~SerialPort() { Close(); }

#else  // POSIX (macOS / Linux)

#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

std::vector<std::string> SerialPort::ListPorts() {
  std::vector<std::string> ports;
  const char* dirPath = "/dev";
  DIR* dir = opendir(dirPath);
  if (!dir) return ports;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name(entry->d_name);
#if defined(__APPLE__)
    if (name.rfind("cu.", 0) == 0 || name.rfind("tty.usb", 0) == 0) {
      ports.push_back(std::string(dirPath) + "/" + name);
    }
#else
    if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0) {
      ports.push_back(std::string(dirPath) + "/" + name);
    }
#endif
  }
  closedir(dir);
  return ports;
}

static speed_t BaudToSpeed(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return B115200;
  }
}

bool SerialPort::Open(const std::string& portName, int baudRate) {
  Close();
  int fd = ::open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return false;

  struct termios tty{};
  if (tcgetattr(fd, &tty) != 0) {
    ::close(fd);
    return false;
  }

  speed_t speed = BaudToSpeed(baudRate);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);

  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
  tty.c_cflag |= CLOCAL | CREAD;
  tty.c_cflag &= ~(PARENB | PARODD);
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CRTSCTS;

  tty.c_lflag = 0;
  tty.c_oflag = 0;
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);

  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    ::close(fd);
    return false;
  }

  fd_ = fd;
  isOpen_ = true;
  return true;
}

void SerialPort::Close() {
  if (isOpen_) {
    ::close(fd_);
    fd_ = -1;
    isOpen_ = false;
  }
}

bool SerialPort::WriteLine(const std::string& line) {
  if (!isOpen_) return false;
  std::string data = line + "\n";
  ssize_t n = ::write(fd_, data.data(), data.size());
  return n == (ssize_t)data.size();
}

bool SerialPort::PollLines(std::vector<std::string>& outLines) {
  if (!isOpen_) return false;
  char buf[256];
  ssize_t n = ::read(fd_, buf, sizeof(buf));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
    return false;
  }
  if (n > 0) rxBuffer_.append(buf, n);

  size_t pos;
  while ((pos = rxBuffer_.find('\n')) != std::string::npos) {
    std::string line = rxBuffer_.substr(0, pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    outLines.push_back(line);
    rxBuffer_.erase(0, pos + 1);
  }
  return true;
}

SerialPort::~SerialPort() { Close(); }

#endif
