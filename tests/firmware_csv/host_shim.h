// Only platform primitives are replaced. Snapshot and CSV functions are
// extracted verbatim from src/main.cpp by scripts/test_firmware_csv.py.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <type_traits>
using std::isfinite;

class String {
  std::string value;
public:
  String() = default;
  String(const char *text) : value(text) {}
  String(std::string text) : value(std::move(text)) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T number) : value(std::to_string(number)) {}
  String(float number, unsigned decimals) {
    char text[96];
    std::snprintf(text, sizeof(text), "%.*f", int(decimals), double(number));
    value = text;
  }
  void reserve(size_t count) { value.reserve(count); }
  String &operator+=(const String &other) { value += other.value; return *this; }
  friend String operator+(const String &a, const String &b) { return String(a.value + b.value); }
  friend std::ostream &operator<<(std::ostream &out, const String &text) { return out << text.value; }
};

class Lock { public: explicit Lock(int) {} };
constexpr int stateMutex = 0;
std::atomic<bool> supplyBusEnabled{true};
