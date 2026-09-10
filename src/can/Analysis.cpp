#include "Analysis.h"
#include <math.h>
#include <string.h>
#include <algorithm>

namespace canlab {
void Moments::add(uint32_t x) {
  ++n;
  const double d = double(x) - mean;
  mean += d / double(n);
  m2 += d * (double(x) - mean);
  lo = std::min(lo, x); hi = std::max(hi, x);
}
void ByteStats::add(uint8_t x, uint8_t previous, bool adjacent) {
  value.add(x);
  if (!adjacent) return;
  ++transitions;
  uint8_t mask = x ^ previous;
  changes += mask != 0; xorMask |= mask;
  for (unsigned b = 0; b < 8; ++b)
    if ((mask >> b) & 1) { if (toggles[b] != UINT32_MAX) ++toggles[b]; }
}
static Sample sample(const Frame &f) {
  Sample s; s.us = f.us; s.gap = f.gap; s.dlc = f.dlc;
  memcpy(s.data, f.data, 8); return s;
}
void Samples::append(const Frame &f) {
  values[next] = sample(f); next = (next + 1) % SAMPLE_COUNT;
  if (count < SAMPLE_COUNT) ++count;
}
const Sample &Samples::at(unsigned i) const {
  return values[((count == SAMPLE_COUNT ? next : 0) + i) % SAMPLE_COUNT];
}
void Window::add(const Frame &f, uint64_t start, uint64_t duration) {
  ++frames;
  if (f.rtr) return;
  for (unsigned i = 0; i < f.dlc; ++i) {
    if (!bytes[i].n) bitAnd[i] = f.data[i];
    else bitAnd[i] &= f.data[i];
    bitOr[i] |= f.data[i]; bytes[i].add(f.data[i]);
  }
  // Keep the latest observation in each of <= SAMPLE_COUNT time bins.
  const uint32_t bucket = uint32_t((f.us - start) * SAMPLE_COUNT / duration);
  if (bucket != lastBucket) { samples.append(f); lastBucket = bucket; }
  else samples.values[(samples.next + SAMPLE_COUNT - 1) % SAMPLE_COUNT] = sample(f);
}
void Record::add(const Frame &f) {
  if (!count) { first = f.us; id = f.id; extended = f.extended; rtr = f.rtr; used = true; }
  bool adjacent = count && lastGap == f.gap && lastDlc == f.dlc;
  if (count && lastGap == f.gap && f.us >= last) {
    const uint64_t dt = f.us - last;
    minPeriod = std::min(minPeriod, dt); maxPeriod = std::max(maxPeriod, dt);
    meanPeriod += (double(dt) - meanPeriod) / double(++periodCount);
  }
  ++count; last = f.us; dlcMask |= uint16_t(1u << f.dlc);
  if (!f.rtr) {
    for (unsigned i = 0; i < f.dlc; ++i) bytes[i].add(f.data[i], previous[i], adjacent);
    recent.append(f); memcpy(previous, f.data, 8);
  }
  lastDlc = f.dlc; lastGap = f.gap;
}
bool validField(Field f) {
  if (!f.width || f.width > 32 || f.start >= 64) return false;
  int bit = f.start;
  for (unsigned i = 0; i < f.width; ++i) {
    if (bit < 0 || bit >= 64) return false;
    bit = f.motorola ? ((bit % 8 == 0) ? bit + 15 : bit - 1) : bit + 1;
  }
  return true;
}
bool extract(const Sample &s, Field f, uint32_t &value) {
  if (!validField(f)) return false;
  int bit = f.start; value = 0;
  for (unsigned i = 0; i < f.width; ++i) {
    if (bit / 8 >= s.dlc) return false;
    uint32_t b = (s.data[bit / 8] >> (bit % 8)) & 1;
    if (f.motorola) value = (value << 1) | b;
    else value |= b << i;
    bit = f.motorola ? ((bit % 8 == 0) ? bit + 15 : bit - 1) : bit + 1;
  }
  return true;
}
double Evidence::activity() const { return transitions ? double(changed) / transitions : 0; }
double Evidence::counter() const { return transitions >= MIN_EVIDENCE - 1 ? double(increment) / transitions : 0; }
double Evidence::monotonic() const { return changed ? double(std::max(up, down)) / changed : 0; }
double Evidence::smoothness() const {
  double range = double(values.hi) - values.lo;
  return transitions && range > 0 ? std::max(0.0, 1.0 - absDelta / transitions / range) : 0;
}
Evidence evaluate(const Samples &samples, Field field, bool consecutive) {
  Evidence e; uint32_t previous = 0; const Sample *prev = nullptr;
  // Online covariance in seconds relative to first sample avoids timestamp cancellation.
  double mt = 0, my = 0, st = 0, sy = 0, cov = 0;
  for (unsigned i = 0; i < samples.count; ++i) {
    const Sample &s = samples.at(i); uint32_t v;
    if (!extract(s, field, v)) { prev = nullptr; continue; }
    e.values.add(v);
    double t = double(s.us - samples.at(0).us) * 1e-6;
    double dt = t - mt, dy = double(v) - my;
    mt += dt / e.values.n; my += dy / e.values.n;
    st += dt * (t - mt); sy += dy * (double(v) - my); cov += dt * (double(v) - my);
    if (prev && prev->gap == s.gap && prev->dlc == s.dlc) {
      ++e.transitions; e.changed += v != previous;
      e.up += v > previous; e.down += v < previous;
      e.absDelta += fabs(double(v) - previous);
      if (consecutive) {
        uint32_t mask = field.width == 32 ? UINT32_MAX : ((uint32_t(1) << field.width) - 1);
        bool inc = v == ((previous + uint32_t(1)) & mask);
        e.increment += inc; e.wraps += inc && v < previous;
      }
    }
    previous = v; prev = &s;
  }
  if (st > 0 && sy > 0) e.correlation = std::max(-1.0, std::min(1.0, cov / sqrt(st * sy)));
  return e;
}
Ranking compare(const Evidence &b, const Evidence &a) {
  Ranking r;
  if (b.values.n < 4 || a.values.n < 4) return r;
  double bv = b.values.variance(), av = a.values.variance();
  r.varianceGain = std::max(0.0, (av - bv) / (av + bv + 1));
  double delta = a.values.mean - b.values.mean;
  // Balanced phase association (equal weight to baseline and action), not ground truth.
  r.phaseCorrelation = fabs(delta) / sqrt(delta * delta + 2 * (av + bv) + 1);
  r.activityGain = std::max(0.0, a.activity() - b.activity());
  r.monotonic = a.monotonic();
  double ar = double(a.values.hi) - a.values.lo, br = double(b.values.hi) - b.values.lo;
  r.rangeGain = std::max(0.0, (ar - br) / (ar + br + 1));
  // Monotonicity alone cannot promote a background ramp.
  double change = (r.varianceGain + r.phaseCorrelation + r.activityGain + r.rangeGain) / 4;
  r.score = (0.30*r.varianceGain + 0.25*r.phaseCorrelation + 0.15*r.activityGain +
             0.20*r.rangeGain + 0.10*r.monotonic*change) *
            std::min(1.0, double(std::min(b.values.n, a.values.n)) / MIN_EVIDENCE);
  return r;
}
static unsigned ones(uint8_t x) { unsigned n = 0; while (x) { n += x & 1; x >>= 1; } return n; }
ChecksumEvidence checksum(const Samples &samples, unsigned byte) {
  ChecksumEvidence e; const Sample *prev = nullptr; unsigned pairCount = 0;
  for (unsigned i = 0; i < samples.count; ++i) {
    const Sample &s = samples.at(i);
    if (byte >= s.dlc || s.dlc < 2) { prev = nullptr; continue; }
    uint8_t sum = 0, x = 0;
    for (unsigned j = 0; j < s.dlc; ++j) if (j != byte) { sum += s.data[j]; x ^= s.data[j]; }
    ++e.samples; e.sum += s.data[byte] == sum;
    e.complement += s.data[byte] == uint8_t(0 - sum); e.xor8 += s.data[byte] == x;
    for (unsigned b = 0; b < 8; ++b) {
      unsigned parity = ones(x) + ones(s.data[byte] & uint8_t(~(1u << b)));
      bool even = ((parity + ((s.data[byte] >> b) & 1)) & 1) == 0;
      // Total payload parity is equivalent for each proposed parity bit; no localization proof.
      e.parityEven[b] += even; e.parityOdd[b] += !even;
    }
    if (prev && prev->gap == s.gap && prev->dlc == s.dlc) {
      bool payloadChanged = false;
      for (unsigned j = 0; j < s.dlc; ++j) if (j != byte && s.data[j] != prev->data[j]) payloadChanged = true;
      if (payloadChanged) {
        ++pairCount; unsigned changed = ones(s.data[byte] ^ prev->data[byte]);
        e.changes += changed != 0; e.avalanche += changed / 8.0;
      }
    }
    prev = &s;
  }
  if (pairCount) e.avalanche /= pairCount;
  return e;
}
} // namespace canlab
