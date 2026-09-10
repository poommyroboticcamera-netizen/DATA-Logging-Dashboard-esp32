#pragma once
#include "Config.h"
#include <stdint.h>
#include <stddef.h>

namespace canlab {
struct Frame {
  uint64_t us;
  uint32_t id, gap;
  uint8_t data[8];
  uint8_t dlc, extended, rtr;
};
struct Moments {
  uint64_t n = 0;
  double mean = 0, m2 = 0;
  uint32_t lo = UINT32_MAX, hi = 0;
  void add(uint32_t x);
  double variance() const { return n > 1 ? m2 / (n - 1) : 0; }
};
struct ByteStats {
  Moments value;
  uint64_t transitions = 0, changes = 0;
  uint32_t toggles[8] = {};
  uint8_t xorMask = 0;
  void add(uint8_t x, uint8_t previous, bool adjacent);
};
struct Sample {
  uint64_t us = 0;
  uint32_t gap = 0;
  uint8_t data[8] = {};
  uint8_t dlc = 0;
};
struct Samples {
  Sample values[SAMPLE_COUNT] = {};
  uint8_t count = 0, next = 0;
  void append(const Frame &f);
  const Sample &at(unsigned chronologicalIndex) const;
};
struct Window {
  uint64_t frames = 0;
  Moments bytes[8];
  uint8_t bitOr[8] = {}, bitAnd[8] = {};
  Samples samples;
  uint32_t lastBucket = UINT32_MAX;
  void add(const Frame &f, uint64_t start, uint64_t duration);
};
struct Record {
  bool used = false;
  uint32_t id = 0;
  bool extended = false, rtr = false;
  uint16_t dlcMask = 0;
  uint64_t count = 0, first = 0, last = 0, periodCount = 0;
  uint64_t minPeriod = UINT64_MAX, maxPeriod = 0;
  double meanPeriod = 0;
  uint32_t lastGap = 0;
  uint8_t lastDlc = 0, previous[8] = {};
  ByteStats bytes[8];
  Samples recent;
  Window baseline, action;
  void add(const Frame &f);
};
struct Field {
  uint8_t start, width; bool motorola;
  Field(uint8_t s=0, uint8_t w=1, bool m=false) : start(s), width(w), motorola(m) {}
};
bool extract(const Sample &s, Field f, uint32_t &value);
bool validField(Field f);
struct Evidence {
  Moments values;
  unsigned transitions = 0, changed = 0, increment = 0, wraps = 0, up = 0, down = 0;
  double absDelta = 0, correlation = 0;
  double activity() const;
  double counter() const;
  double monotonic() const;
  double smoothness() const;
};
Evidence evaluate(const Samples &samples, Field field, bool consecutive);
struct Ranking {
  double score = 0, varianceGain = 0, phaseCorrelation = 0;
  double activityGain = 0, monotonic = 0, rangeGain = 0;
};
Ranking compare(const Evidence &baseline, const Evidence &action);
struct ChecksumEvidence {
  unsigned samples = 0, changes = 0, sum = 0, complement = 0, xor8 = 0;
  unsigned parityEven[8] = {}, parityOdd[8] = {};
  double avalanche = 0;
};
ChecksumEvidence checksum(const Samples &samples, unsigned byte);
} // namespace canlab
