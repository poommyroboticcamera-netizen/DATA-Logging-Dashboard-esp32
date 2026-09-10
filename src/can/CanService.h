#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>
namespace canservice {
struct CandidateView {
  uint32_t id=0;
  uint8_t start=0,width=0;
  bool extended=false,motorola=false;
  float score=0;
};
bool begin(SemaphoreHandle_t sharedStorage);
bool setMode(bool active); // Enter CAN starts CSV; leave CAN drains/closes it.
bool active();
bool command(const char *text);
bool statusJson(char *buffer,size_t capacity);
bool reportText(char *buffer,size_t capacity);
void pollConsole(bool quiet); // Existing console remains the only UART owner.
bool serialPending();
}
