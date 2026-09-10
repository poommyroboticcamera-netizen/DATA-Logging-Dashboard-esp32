#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <driver/twai.h>
#include <esp_timer.h>
#include <atomic>
#include <new>
#include <math.h>
#include <stdarg.h>
#include "Analysis.h"

#include "CanService.h"
namespace canservice {
using namespace canlab;
QueueHandle_t frameQueue, commandQueue, noticeQueue, logQueue, logCommands;
QueueHandle_t reportQueue, outputQueue;
SemaphoreHandle_t storageMutex=nullptr;
std::atomic<bool> ready{false}, modeActive{false};
std::atomic<bool> driverRequested{false}, driverRunning{false};
std::atomic<uint32_t> configuredBitrate{DEFAULT_CAN_BITRATE}, runningBitrate{0};
std::atomic<const char *> driverState{"DISABLED"};
std::atomic<const char *> logState{"STOPPED"};
Preferences canPreferences;
bool canPreferencesReady=false;
static char logPath[32]={};
static CandidateView candidateCache[12]={};
static unsigned candidateCount=0;
static char candidateLabel[24]={};
static char lastReport[4096]={};
static size_t lastReportSize=0;
static uint32_t reportRevision=0;
SemaphoreHandle_t dbMutex, logGate;
Record *records[MAX_IDS] = {};
static Record snapshot;
std::atomic<bool> acquiring{false}, logEnabled{false};
std::atomic<uint32_t> rxFrames{0}, analysisDrops{0}, logDrops{0}, logWritten{0};
std::atomic<uint32_t> logErrors{0}, logDiscarded{0}, invalidFrames{0}, dbFullFrames{0};
std::atomic<uint32_t> epoch{0}, driverMissed{0}, driverOverruns{0}, busErrors{0};
std::atomic<uint32_t> rxBusyUs{0}, analysisBusyUs{0}, rxHighWater{0}, logHighWater{0};
std::atomic<uint32_t> statusRequests{0};
std::atomic<bool> reportBusy{false};
uint64_t analyzedFrames = 0, payloadBytes = 0, wireBits = 0, activeUs = 0, activeSince = 0;
uint64_t acceptAfter = 0;
unsigned recordCount = 0;
enum class Phase : uint8_t { Idle, Baseline, Action };
Phase phase = Phase::Idle;
bool baselineReady = false, actionReady = false;
uint64_t phaseStart = 0, phaseEnd = 0, baselineDuration = 0, actionDuration = 0;
uint32_t phaseDropStart = 0, baselineLoss = 0, actionLoss = 0;
char experimentName[24] = "ACTION";
struct Command { char text[80]; uint64_t us; };
struct Notice { char text[112]; bool summary; };
struct OutputChunk { char text[192]; };
// Candidate/report worker formats into bounded chunks. Only the UI writes Serial.
class ReportOutput : public Print {
  OutputChunk chunk = {}; unsigned length = 0;
public:
  size_t write(uint8_t c) override {
    chunk.text[length++]=char(c);
    if(c=='\n' || length==sizeof(chunk.text)-1) flushChunk();
    return 1;
  }
  void flushChunk() {
    if(!length)return;
    chunk.text[length]=0;
    xSemaphoreTake(dbMutex,portMAX_DELAY);
    if(lastReportSize+length>=sizeof(lastReport)) {
      size_t remove=lastReportSize+length-(sizeof(lastReport)-1);
      memmove(lastReport,lastReport+remove,lastReportSize-remove);lastReportSize-=remove;
    }
    memcpy(lastReport+lastReportSize,chunk.text,length);lastReportSize+=length;lastReport[lastReportSize]=0;
    xSemaphoreGive(dbMutex);
    xQueueSend(outputQueue,&chunk,portMAX_DELAY);length=0;
  }
} reportOut;

static uint64_t nowUs() { return uint64_t(esp_timer_get_time()); }
static bool supportedBitrate(uint32_t bitrate) {
  switch(bitrate) {
    case 50000: case 100000: case 125000: case 250000: case 500000: case 1000000: return true;
    default: return false;
  }
}
static twai_timing_config_t timingFor(uint32_t bitrate) {
  switch(bitrate) {
    case 1000000: return TWAI_TIMING_CONFIG_1MBITS();
    case 500000: return TWAI_TIMING_CONFIG_500KBITS();
    case 250000: return TWAI_TIMING_CONFIG_250KBITS();
    case 125000: return TWAI_TIMING_CONFIG_125KBITS();
    case 100000: return TWAI_TIMING_CONFIG_100KBITS();
    default: return TWAI_TIMING_CONFIG_50KBITS();
  }
}
static void notice(const char *text, bool summary = false) {
  Notice n = {}; snprintf(n.text, sizeof(n.text), "%s", text); n.summary = summary;
  // Nonblocking. A full notice queue never stops analysis; STATUS remains authoritative.
  if (xQueueSend(noticeQueue, &n, 0) != pdTRUE) ++statusRequests;
}
static uint32_t lossTotal() { return analysisDrops + driverMissed + driverOverruns + dbFullFrames; }
static void highWater(std::atomic<uint32_t> &dest, QueueHandle_t q) {
  uint32_t n = uxQueueMessagesWaiting(q); if (n > dest.load()) dest.store(n);
}
static void rxTask(void *) {
  uint64_t lastHealth = 0;
  uint32_t previousMissed = 0, previousOverruns = 0;
  bool installed=false;
  for (;;) {
    const bool requested=driverRequested.load();
    if(requested && !installed) {
      const uint32_t requestedBitrate=configuredBitrate.load();
      driverState="STARTING";
      twai_general_config_t general=TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN,(gpio_num_t)CAN_RX_PIN,TWAI_MODE_LISTEN_ONLY);
      general.tx_queue_len=0;general.rx_queue_len=DRIVER_RX_DEPTH;
      twai_timing_config_t timing=timingFor(requestedBitrate);
      twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
      const esp_err_t installResult=twai_driver_install(&general,&timing,&filter);
      const esp_err_t startResult=installResult==ESP_OK ? twai_start() : installResult;
      if(installResult!=ESP_OK || startResult!=ESP_OK) {
        if(installResult==ESP_OK)twai_driver_uninstall();
        driverRequested=false;driverRunning=false;runningBitrate=0;driverState="START_FAILED";
        notice("CAN start failed; receiver remains disabled.");vTaskDelay(pdMS_TO_TICKS(100));continue;
      }
      installed=true;driverRunning=true;runningBitrate=requestedBitrate;driverState="LISTENING";
      previousMissed=previousOverruns=0;lastHealth=nowUs();++epoch;
      notice("CAN receiver enabled in LISTEN_ONLY mode.");
    } else if(!requested && installed) {
      driverState="STOPPING";twai_stop();twai_driver_uninstall();installed=false;
      driverRunning=false;runningBitrate=0;driverState="DISABLED";++epoch;
      notice("CAN receiver disabled.");
    }
    if(!installed) { vTaskDelay(pdMS_TO_TICKS(10));continue; }
    twai_message_t m = {};
    esp_err_t result = twai_receive(&m, pdMS_TO_TICKS(10));
    uint64_t workStart = nowUs();
    if (result == ESP_OK) {
      ++rxFrames;
      if (m.data_length_code > 8 || m.dlc_non_comp ||
          m.identifier > (m.extd ? 0x1FFFFFFFu : 0x7FFu)) ++invalidFrames;
      else if (acquiring.load()) {
        Frame f = {}; f.us = workStart; f.id = m.identifier;
        f.extended = m.extd; f.rtr = m.rtr; f.dlc = m.data_length_code; f.gap = epoch.load();
        if (!f.rtr) memcpy(f.data, m.data, f.dlc);
        if (xQueueSend(frameQueue, &f, 0) != pdTRUE) { ++analysisDrops; ++epoch; }
        highWater(rxHighWater, frameQueue);
        if (logEnabled.load()) {
          if (xSemaphoreTake(logGate, 0) == pdTRUE) {
            if (logEnabled.load()) {
              if (xQueueSend(logQueue, &f, 0) != pdTRUE) ++logDrops;
              highWater(logHighWater, logQueue);
            }
            xSemaphoreGive(logGate);
          } else ++logDrops;
        }
      }
    }
    if (workStart - lastHealth >= 100000) {
      twai_status_info_t s = {};
      if (twai_get_status_info(&s) == ESP_OK) {
        driverMissed = s.rx_missed_count; driverOverruns = s.rx_overrun_count;
        busErrors = s.bus_error_count;
        if (s.rx_missed_count != previousMissed || s.rx_overrun_count != previousOverruns) {
          ++epoch; previousMissed = s.rx_missed_count; previousOverruns = s.rx_overrun_count;
        }
      }
      lastHealth = workStart;
    }
    rxBusyUs.fetch_add(uint32_t(nowUs() - workStart));
  }
}
static bool secondsValue(const char *s, unsigned &seconds) {
  if (!s || !*s) return false;
  for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return false;
  char *end; unsigned long v = strtoul(s, &end, 10);
  if (*end || v < 1 || v > MAX_CAPTURE_SECONDS) return false;
  seconds = unsigned(v); return true;
}
static void finishPhase() {
  if (phase == Phase::Baseline) {
    baselineReady = true; baselineLoss = lossTotal() - phaseDropStart;
    notice("Baseline complete. Run EXPERIMENT <label> <seconds>.");
  } else if (phase == Phase::Action) {
    actionReady = true; actionLoss = lossTotal() - phaseDropStart;
    notice("Action capture complete; candidate comparison follows.", true);
  }
  phase = Phase::Idle;
}
static void startPhase(Phase next, unsigned seconds, const char *label, uint64_t start) {
  if (reportBusy) { notice("Candidate report in progress; retry capture after report completes."); return; }
  if (!acquiring) { notice("START acquisition before capturing."); return; }
  if (phase != Phase::Idle) { notice("Capture already running. STOP cancels it."); return; }
  if (next == Phase::Action && !baselineReady) { notice("Complete a BASELINE first."); return; }
  for (unsigned i = 0; i < recordCount; ++i) {
    if (next == Phase::Baseline) records[i]->baseline = Window{};
    records[i]->action = Window{};
  }
  actionReady = false;candidateCount=0;
  if (next == Phase::Baseline) { baselineReady = false; baselineDuration = uint64_t(seconds)*1000000; }
  else { actionDuration = uint64_t(seconds)*1000000; snprintf(experimentName, sizeof(experimentName), "%s", label); }
  phase = next; phaseStart = start; phaseEnd = start + uint64_t(seconds)*1000000;
  phaseDropStart = lossTotal();
  notice(next == Phase::Baseline ? "Baseline recording now." : "Experiment recording now; operate the labelled control safely.");
}
static void applyCommand(Command c) {
  char *save = nullptr;
  char *a = strtok_r(c.text, " \t", &save), *b = strtok_r(nullptr, " \t", &save);
  char *d = strtok_r(nullptr, " \t", &save), *extra = strtok_r(nullptr, " \t", &save);
  if (!a) return;
  if (!strcmp(a,"START") && !b) {
    if (!acquiring) { acceptAfter = c.us; activeSince = c.us; ++epoch; acquiring = true; }
    notice("Acquisition enabled; controller remains LISTEN_ONLY.");
  } else if (!strcmp(a,"STOP") && !b) {
    if (acquiring) { activeUs += c.us - activeSince; acquiring = false; }
    ++epoch; phase = Phase::Idle; notice("Acquisition paused; any active experiment cancelled. LOG STOP closes SD files.");
  } else if (!strcmp(a,"RESET") && !b) {
    if (reportBusy) { notice("Candidate report in progress; retry RESET afterward."); return; }
    for (unsigned i = 0; i < recordCount; ++i) *records[i] = Record{};
    recordCount = 0; analyzedFrames = payloadBytes = wireBits = activeUs = 0;
    activeSince = acceptAfter = c.us; ++epoch;
    phase = Phase::Idle; baselineReady = actionReady = false;candidateCount=0;
    notice("Database and captures reset. Lifetime reception/drop/log counters retained.");
  } else if (!strcmp(a,"BASELINE") && b && !d) {
    unsigned seconds; if (secondsValue(b, seconds)) startPhase(Phase::Baseline,seconds,"",c.us);
    else notice("Duration must be an integer from 1 to 3600 seconds.");
  } else if (!strcmp(a,"EXPERIMENT") && b && d && !extra) {
    unsigned seconds;
    if (strlen(b) >= sizeof(experimentName)) notice("Experiment label: maximum 23 characters, no spaces.");
    else if (secondsValue(d, seconds)) startPhase(Phase::Action,seconds,b,c.us);
    else notice("Duration must be an integer from 1 to 3600 seconds.");
  } else if (!strcmp(a,"CAPTURE") && b && !extra &&
             (!strcmp(b,"BASELINE") || !strcmp(b,"ACTION"))) {
    unsigned seconds = DEFAULT_CAPTURE_SECONDS;
    if (d && !secondsValue(d, seconds)) { notice("Invalid CAPTURE duration."); return; }
    startPhase(!strcmp(b,"BASELINE") ? Phase::Baseline : Phase::Action,seconds,"ACTION",c.us);
  } else notice("Invalid command; HELP lists commands.");
}
static void analysisTask(void *) {
  for (;;) {
    Command cmd;
    if (xQueueReceive(commandQueue,&cmd,0) == pdTRUE) {
      xSemaphoreTake(dbMutex,portMAX_DELAY); applyCommand(cmd); xSemaphoreGive(dbMutex);
    }
    Frame f;
    bool got = xQueueReceive(frameQueue,&f,pdMS_TO_TICKS(5)) == pdTRUE;
    uint64_t begin = nowUs();
    xSemaphoreTake(dbMutex,portMAX_DELAY);
    if (got && acquiring && f.us >= acceptAfter) {
      if (phase != Phase::Idle && f.us >= phaseEnd) finishPhase();
      ++analyzedFrames; payloadBytes += f.rtr ? 0 : f.dlc;
      // Classical frame + intermission, excluding stuff/error/overload bits.
      wireBits += (f.extended ? 67 : 47) + (f.rtr ? 0 : 8*f.dlc);
      unsigned i = 0;
      for (; i < recordCount; ++i)
        if (records[i]->id == f.id && records[i]->extended == bool(f.extended) && records[i]->rtr == bool(f.rtr)) break;
      if (i == recordCount && recordCount < MAX_IDS) ++recordCount;
      if (i == MAX_IDS) ++dbFullFrames;
      else {
        Record &r = *records[i]; r.add(f);
        if (phase != Phase::Idle && f.us >= phaseStart && f.us < phaseEnd) {
          Window &w = phase == Phase::Baseline ? r.baseline : r.action;
          w.add(f,phaseStart,phaseEnd-phaseStart);
        }
      }
    }
    // Drain queued pre-deadline frames before finalizing an idle bus capture.
    if (!got && phase != Phase::Idle && nowUs() >= phaseEnd) finishPhase();
    xSemaphoreGive(dbMutex);
    analysisBusyUs.fetch_add(uint32_t(nowUs()-begin));
    // Lower-priority SD/UI must run even when CAN backlog is sustained.
    static unsigned batch = 0; if (++batch == 32) { batch = 0; vTaskDelay(1); }
  }
}

// Only loggerTask accesses SPI, SD, and File objects.
static void loggerTask(void *) {
  File csv, meta; bool open = false; uint32_t session = 0;
  uint64_t lastFlush = 0; unsigned buffered = 0, bufferedRows = 0;
  static char buffer[4096];
  auto writeBuffer = [&]() {
    if (!buffered) return true;
    bool ok = csv.write(reinterpret_cast<const uint8_t *>(buffer),buffered) == buffered;
    buffered = 0; if (!ok) { ++logErrors; logDiscarded.fetch_add(bufferedRows); }
    bufferedRows = 0; return ok;
  };
  auto disable = [&]() {
    xSemaphoreTake(logGate,portMAX_DELAY); logEnabled = false; xSemaphoreGive(logGate);
  };
  auto writeFrame = [&](const Frame &f) {
    char row[180]; int n = snprintf(row,sizeof(row),"%llu,%08lX,%u,%u",
      (unsigned long long)f.us,(unsigned long)f.id,f.extended,f.dlc);
    for (unsigned i = 0; i < 8; ++i)
      n += snprintf(row+n,sizeof(row)-n,(!f.rtr && i < f.dlc) ? ",%u" : ",",f.data[i]);
    row[n++] = '\n';
    if (buffered + unsigned(n) > sizeof(buffer) && !writeBuffer()) return false;
    memcpy(buffer+buffered,row,n); buffered += n; ++bufferedRows; ++logWritten;
    if (f.rtr && meta.printf("RTR,%llu,%08lX,%u,%u\n",(unsigned long long)f.us,
        (unsigned long)f.id,f.extended,f.dlc) == 0) { ++logErrors; return false; }
    return true;
  };
  for (;;) {
    char command;
    if (xQueueReceive(logCommands,&command,0) == pdTRUE) {
      if (command == 'S' && !open) {
        logState="OPENING";
        xSemaphoreTake(storageMutex,portMAX_DELAY);
        SPI.begin(SD_SCK_PIN,SD_MISO_PIN,SD_MOSI_PIN,SD_CS_PIN);
        if (!SD.begin(SD_CS_PIN,SPI,10000000)) { ++logErrors; notice("SD mount failed; logging disabled."); logState="MOUNT_FAILED";xSemaphoreGive(storageMutex);continue; }
        char path[32], metadata[32];
        do {
          snprintf(path,sizeof(path),"/can_%06lu.csv",(unsigned long)++session);
          snprintf(metadata,sizeof(metadata),"/can_%06lu.meta",(unsigned long)session);
        } while (SD.exists(path) || SD.exists(metadata));
        csv = SD.open(path,FILE_WRITE); meta = SD.open(metadata,FILE_WRITE);
        if (!csv || !meta || !csv.println("timestamp_us,id,extended,dlc,d0,d1,d2,d3,d4,d5,d6,d7")) {
          ++logErrors; csv.close(); meta.close(); notice("SD open/header failed.");logState="OPEN_FAILED";xSemaphoreGive(storageMutex);continue;
        }
        if (!meta.printf("LISTEN_ONLY,bitrate=%lu,timestamp=task_dequeue_us\nSTART,rx=%lu,analysis_drop=%lu,log_drop=%lu\n",
          (unsigned long)configuredBitrate.load(),(unsigned long)rxFrames.load(),(unsigned long)analysisDrops.load(),(unsigned long)logDrops.load())) {
          ++logErrors; csv.close(); meta.close(); notice("SD metadata write failed.");logState="WRITE_FAILED";xSemaphoreGive(storageMutex);continue;
        }
        buffered = 0; bufferedRows = 0; open = true;
        xSemaphoreTake(dbMutex,portMAX_DELAY);snprintf(logPath,sizeof(logPath),"%s",path);xSemaphoreGive(dbMutex);logState="RECORDING"; lastFlush = nowUs();
        xSemaphoreTake(logGate,portMAX_DELAY); logEnabled = true; xSemaphoreGive(logGate);
        char message[96]; snprintf(message,sizeof(message),"Logging to %s; .meta contains RTR and loss information.",path); notice(message);
      } else if (command == 'T' && open) {
        logState="CLOSING";disable();
        Frame f; while (xQueueReceive(logQueue,&f,0) == pdTRUE)
          if (!writeFrame(f)) ++logDiscarded;
        writeBuffer();
        if (!meta.printf("STOP,analysis_drop=%lu,log_drop=%lu,driver_missed=%lu,driver_overrun=%lu,write_errors=%lu\n",
          (unsigned long)analysisDrops.load(),(unsigned long)logDrops.load(),(unsigned long)driverMissed.load(),
          (unsigned long)driverOverruns.load(),(unsigned long)logErrors.load())) ++logErrors;
        csv.flush(); meta.flush(); csv.close(); meta.close(); open = false; xSemaphoreGive(storageMutex);
        logState="STOPPED";notice("Logging stopped; queues drained and files closed. Check STATUS for errors.");
      } else notice(open ? "Logging is already active." : "Logging is already stopped.");
    }
    Frame f;
    if (xQueueReceive(logQueue,&f,pdMS_TO_TICKS(20)) == pdTRUE) {
      if (!open) ++logDiscarded;
      else if (!writeFrame(f)) {
        // Conservatively count all unconfirmed rows after a write/metadata error.
        logDiscarded.fetch_add(bufferedRows+1); bufferedRows=buffered=0;
        disable(); csv.close(); meta.close(); open = false; xSemaphoreGive(storageMutex);
        logState="WRITE_FAILED";notice("SD write failed; logging disabled. Buffered rows may be lost.");
      }
    }
    if (open && nowUs()-lastFlush > 1000000) {
      if (!writeBuffer()) {
        disable(); csv.close(); meta.close(); open = false; xSemaphoreGive(storageMutex); logState="WRITE_FAILED";notice("SD write failed; logging disabled.");
      } else { csv.flush(); meta.flush(); }
      lastFlush = nowUs();
    }
    vTaskDelay(1);
  }
}

static bool copyRecord(unsigned i) {
  xSemaphoreTake(dbMutex,portMAX_DELAY);
  bool ok = i < recordCount; if (ok) snapshot = *records[i];
  xSemaphoreGive(dbMutex); return ok;
}
static void printKey(const Record &r) {
  reportOut.printf("ID 0x%lX %s %s",(unsigned long)r.id,r.extended ? "EXT" : "STD",r.rtr ? "RTR" : "DATA");
}
static void printField(Field f) {
  reportOut.printf("start=%u width=%u %s",f.start,f.width,f.motorola ? "Motorola-MSB/sawtooth" : "Intel-LSB");
}
static void printIdLine(const Record &r) {
  printKey(r);
  reportOut.printf(" count=%llu DLC-mask=0x%03X first=%llu last=%llu period_us[min,max,avg]=",
    (unsigned long long)r.count,r.dlcMask,(unsigned long long)r.first,(unsigned long long)r.last);
  if (r.periodCount) reportOut.printf("[%llu,%llu,%.1f] estimated_Hz=%.3f\n",
      (unsigned long long)r.minPeriod,(unsigned long long)r.maxPeriod,r.meanPeriod,
      r.meanPeriod > 0 ? 1e6/r.meanPeriod : 0);
  else reportOut.println("unavailable (need two gap-free observations)");
}
struct Candidate { Field field; double score; Evidence evidence; Ranking ranking; };
static void insert(Candidate *top, unsigned size, Candidate c) {
  for (unsigned i = 0; i < size; ++i) if (c.score > top[i].score) {
    for (unsigned j = size-1; j > i; --j) top[j] = top[j-1]; top[i] = c; return;
  }
}
static void describeRecord(const Record &r) {
  printIdLine(r); if (r.rtr) return;
  for (unsigned i = 0; i < 8; ++i) {
    const ByteStats &b = r.bytes[i]; if (!b.value.n) continue;
    double p = b.transitions ? double(b.changes)/b.transitions : 0;
    const char *kind = b.value.n < MIN_EVIDENCE ? "insufficient evidence" :
      b.value.lo == b.value.hi ? "observed constant" :
      __builtin_popcount(unsigned(b.xorMask)) == 1 ? "candidate binary/state field" :
      p < .05 ? "slowly changing candidate" : p > .5 ? "rapidly changing candidate" : "varying candidate";
    reportOut.printf("byte %u n=%llu min=%lu max=%lu mean=%.4f variance=%.4f changes=%llu transitions=%llu p=%.5f xor=0x%02X %s\n",
      i,(unsigned long long)b.value.n,(unsigned long)b.value.lo,(unsigned long)b.value.hi,b.value.mean,
      b.value.variance(),(unsigned long long)b.changes,(unsigned long long)b.transitions,p,b.xorMask,kind);
    reportOut.print("  bits 0..7 toggle_probability / observed_toggles_per_second:");
    for (unsigned bit=0;bit<8;++bit) reportOut.printf(" %.4f/%.3f",b.transitions ? double(b.toggles[bit])/b.transitions : 0,
      r.last>r.first ? double(b.toggles[bit])*1e6/(r.last-r.first) : 0);
    reportOut.println();
    if (b.value.n>=MIN_EVIDENCE && b.xorMask) {
      reportOut.print("  candidate binary/state bit boundaries (LSB0):");
      for(unsigned bit=0;bit<8;++bit)if(b.xorMask&(1u<<bit))reportOut.printf(" %u",i*8+bit);
      reportOut.println("; toggling does not distinguish flags from packed numeric fields");
    }
  }
  Candidate continuous[8] = {}, counters[8] = {};
  const uint8_t widths[] = {1,2,4,8,16,24,32};
  for (uint8_t width : widths) for (unsigned endian = 0; endian < (width == 1 ? 1u : 2u); ++endian) {
    for (unsigned start=0;start<64;++start) {
      Field f{uint8_t(start),width,bool(endian)}; if (!validField(f)) continue;
      Evidence e = evaluate(r.recent,f,true); if (e.values.n < MIN_EVIDENCE || !e.changed) continue;
      Candidate c; c.field=f; c.evidence=e;
      if (width==2 || width==4 || width==8 || width==16) {
        c.score=e.counter(); if (c.score>=.8) insert(counters,8,c);
      }
      c.score=e.smoothness() * std::min(1.0,double(e.changed)/8);
      if (width != 2 && width != 4 && c.score >= .5) insert(continuous,8,c);
    }
    vTaskDelay(1);
  }
  reportOut.printf("Recent candidate search: last %u DATA frames; overlapping hypotheses, unsigned raw values. Scores are heuristics.\n",r.recent.count);
  for (auto &c : counters) if (c.score) {
    reportOut.print("Possible rolling counter candidate "); printField(c.field);
    reportOut.printf(" match=%.3f increments=%u/%u observed_wraps=%u%s\n",c.score,c.evidence.increment,c.evidence.transitions,
      c.evidence.wraps,c.evidence.wraps ? "" : " (modulus not demonstrated)");
  }
  for (auto &c : continuous) if (c.score) {
    reportOut.print("Candidate continuously varying/state signal "); printField(c.field);
    reportOut.printf(" heuristic_confidence=%.3f n=%llu range=[%lu,%lu] time_r=%.3f\n",c.score,
      (unsigned long long)c.evidence.values.n,(unsigned long)c.evidence.values.lo,(unsigned long)c.evidence.values.hi,c.evidence.correlation);
  }
  for (unsigned b=0;b<8;++b) {
    auto e=checksum(r.recent,b); if (e.samples < MIN_EVIDENCE || e.changes < 4) continue;
    if (e.sum == e.samples || e.complement == e.samples || e.xor8 == e.samples || (e.avalanche>.25 && e.avalanche<.75))
      reportOut.printf("byte %u checksum/CRC candidate n=%u sum_matches=%u twos_complement_matches=%u xor_matches=%u mean_changed_bits_fraction=%.3f; offline validation required\n",
        b,e.samples,e.sum,e.complement,e.xor8,e.avalanche);
    if (b==0 && (e.parityEven[0]==e.samples || e.parityOdd[0]==e.samples))
      reportOut.println("Payload parity invariant candidate; does not localize a parity bit and may be an XOR-checksum consequence.");
  }
}
static void summary() {
  struct Guard { Guard(){reportBusy=true;} ~Guard(){reportBusy=false;} } guard;
  xSemaphoreTake(dbMutex,portMAX_DELAY);
  bool ready=baselineReady && actionReady && phase==Phase::Idle;
  uint64_t bd=baselineDuration, ad=actionDuration;
  uint32_t bl=baselineLoss, al=actionLoss;
  char label[24]; snprintf(label,sizeof(label),"%s",experimentName);
  xSemaphoreGive(dbMutex);
  if (!ready) { reportOut.println("Complete baseline and action captures before SUMMARY."); return; }
  reportOut.printf("Experiment: %s — candidates only; known capture losses baseline=%lu action=%lu\n",label,(unsigned long)bl,(unsigned long)al);
  struct Global { Candidate c; uint32_t id; bool ext; } top[12] = {};
  struct IdRank { double score; uint32_t id; bool ext; } topIds[8] = {};
  for (unsigned i=0;copyRecord(i);++i) {
    const Record &r=snapshot; const Window &b=r.baseline, &a=r.action;
    double bestIdScore=0;
    if (a.frames && !b.frames) { printKey(r); reportOut.println(" newly observed in action candidate (absence is window-limited)"); }
    if (b.frames && !a.frames) { printKey(r); reportOut.println(" absent in action window"); }
    if (!a.frames || !b.frames) continue;
    printKey(r); reportOut.printf(" baseline_Hz=%.2f action_Hz=%.2f\n",b.frames*1e6/bd,a.frames*1e6/ad);
    for (unsigned j=0;j<8;++j) if (b.bytes[j].n && a.bytes[j].n) {
      unsigned mask = (b.bitOr[j]^a.bitOr[j]) | (b.bitAnd[j]^a.bitAnd[j]);
      if (mask || fabs(a.bytes[j].mean-b.bytes[j].mean)>.5 || a.bytes[j].variance()>b.bytes[j].variance()+1)
        reportOut.printf("  changed-byte candidate %u changed-observed-bit-set=0x%02X mean_delta=%.3f variance %.3f -> %.3f\n",
          j,mask,a.bytes[j].mean-b.bytes[j].mean,b.bytes[j].variance(),a.bytes[j].variance());
    }
    for (unsigned bit=0;bit<64;++bit) {
      Field f{uint8_t(bit),1,false};
      Evidence be=evaluate(b.samples,f,false),ae=evaluate(a.samples,f,false);
      if (be.values.n>=4 && ae.values.n>=4 && fabs(be.values.mean-ae.values.mean)>.1)
        reportOut.printf("  changed-bit candidate %u sampled_P1 %.3f -> %.3f\n",bit,be.values.mean,ae.values.mean);
    }
    const uint8_t widths[]={1,8,16,24,32};
    for (uint8_t width:widths) for (unsigned endian=0;endian<(width==1?1u:2u);++endian) {
      for (unsigned start=0;start<64;++start) {
        Field f{uint8_t(start),width,bool(endian)}; if (!validField(f)) continue;
        Evidence be=evaluate(b.samples,f,false), ae=evaluate(a.samples,f,false);
        Ranking rank=compare(be,ae); if (rank.score<.10) continue;
        if(rank.score>bestIdScore)bestIdScore=rank.score;
        Candidate c; c.field=f;c.score=rank.score;c.evidence=ae;c.ranking=rank;
        for (unsigned k=0;k<12;++k) if (c.score>top[k].c.score) {
          for (unsigned m=11;m>k;--m) top[m]=top[m-1]; top[k]={c,r.id,r.extended}; break;
        }
      }
      vTaskDelay(1);
    }
    for(unsigned k=0;k<8;++k)if(bestIdScore>topIds[k].score) {
      for(unsigned m=7;m>k;--m)topIds[m]=topIds[m-1];
      topIds[k]={bestIdScore,r.id,r.extended};break;
    }
  }
  for(unsigned i=0;i<8;++i)if(topIds[i].score)
    reportOut.printf("ID candidate rank #%u ID=0x%lX %s best_signal_score=%.3f\n",i+1,
      (unsigned long)topIds[i].id,topIds[i].ext?"EXT":"STD",topIds[i].score);
  for (unsigned i=0;i<12;++i) if (top[i].c.score) {
    auto &g=top[i]; auto &r=g.c.ranking;
    reportOut.printf("Candidate #%u ID=0x%lX %s ",i+1,(unsigned long)g.id,g.ext?"EXT":"STD");printField(g.c.field);
    reportOut.printf(" score=%.3f variance_gain=%.3f phase_association=%.3f activity_gain=%.3f monotonic=%.3f range_gain=%.3f action_time_r=%.3f sampled_n=%llu\n",
      g.c.score,r.varianceGain,r.phaseCorrelation,r.activityGain,r.monotonic,r.rangeGain,
      g.c.evidence.correlation,(unsigned long long)g.c.evidence.values.n);
  }
  xSemaphoreTake(dbMutex,portMAX_DELAY);
  candidateCount=0;snprintf(candidateLabel,sizeof(candidateLabel),"%s",label);
  for(unsigned i=0;i<12;++i)if(top[i].c.score) {
    auto &v=candidateCache[candidateCount++];v.id=top[i].id;v.extended=top[i].ext;
    v.start=top[i].c.field.start;v.width=top[i].c.field.width;v.motorola=top[i].c.field.motorola;v.score=top[i].c.score;
  }
  xSemaphoreGive(dbMutex);
  reportOut.println("No physical meaning established. Repeat controlled experiments; export CSV for deeper analysis.");
}
static void status() {
  xSemaphoreTake(dbMutex,portMAX_DELAY);
  unsigned ids=recordCount; uint64_t frames=analyzedFrames, bytes=payloadBytes, bits=wireBits;
  uint64_t elapsed=activeUs+(acquiring ? nowUs()-activeSince : 0);
  Phase p=phase; bool br=baselineReady, ar=actionReady;
  xSemaphoreGive(dbMutex);
  reportOut.printf("Mode=LISTEN_ONLY bitrate=%lu acquiring=%u log=%u IDs=%u/%u frames_analyzed=%llu rx_lifetime=%lu bytes=%llu\n",
    (unsigned long)configuredBitrate.load(),acquiring.load(),logEnabled.load(),ids,MAX_IDS,(unsigned long long)frames,
    (unsigned long)rxFrames.load(),(unsigned long long)bytes);
  reportOut.printf("observed_fps=%.2f unstuffed_observed_bus_load_estimate=%.2f%% active_seconds=%.2f\n",
    elapsed ? frames*1e6/elapsed : 0,elapsed ? bits*1e8/(double(elapsed)*configuredBitrate.load()) : 0,elapsed*1e-6);
  reportOut.printf("loss: analysis_queue=%lu logger_queue_or_gate=%lu driver_missed=%lu driver_overrun=%lu database_full_frames=%lu invalid=%lu bus_errors=%lu\n",
    (unsigned long)analysisDrops.load(),(unsigned long)logDrops.load(),(unsigned long)driverMissed.load(),
    (unsigned long)driverOverruns.load(),(unsigned long)dbFullFrames.load(),(unsigned long)invalidFrames.load(),(unsigned long)busErrors.load());
  reportOut.printf("SD buffered_or_written_rows=%lu write_errors=%lu discarded_rows=%lu; not a durability guarantee\n",
    (unsigned long)logWritten.load(),(unsigned long)logErrors.load(),(unsigned long)logDiscarded.load());
  reportOut.printf("queues current/high: analysis=%u/%lu log=%u/%lu free_heap=%u min_free_heap=%u DB_bytes=%u Record_bytes=%u Frame_bytes=%u\n",
    unsigned(uxQueueMessagesWaiting(frameQueue)),(unsigned long)rxHighWater.load(),unsigned(uxQueueMessagesWaiting(logQueue)),
    (unsigned long)logHighWater.load(),ESP.getFreeHeap(),ESP.getMinFreeHeap(),unsigned(sizeof(Record)*MAX_IDS),unsigned(sizeof(Record)),unsigned(sizeof(Frame)));
  reportOut.printf("phase=%u baseline_ready=%u action_ready=%u dropped_notices=%lu cumulative_busy_us_mod32 RX=%lu analysis=%lu\n",
    unsigned(p),br,ar,(unsigned long)statusRequests.load(),(unsigned long)rxBusyUs.load(),(unsigned long)analysisBusyUs.load());
}
static void reportCommand(char *line) {
  while (*line==' ' || *line=='\t') ++line;
  size_t length=strlen(line); while (length && (line[length-1]==' ' || line[length-1]=='\t')) line[--length]=0;
  if (!*line) return;
  for (char *p=line;*p;++p) *p=toupper(static_cast<unsigned char>(*p));
  if (!strcmp(line,"STATUS")) status();
  else if (!strcmp(line,"IDS")) { for (unsigned i=0;copyRecord(i);++i) printIdLine(snapshot); }
  else if (!strcmp(line,"SUMMARY")) summary();
  else if (!strcmp(line,"HELP")) reportOut.println("STATUS | IDS | ID <hex> [STD|EXT] | START | STOP | RESET | BASELINE <s> | EXPERIMENT <label> <s> | CAPTURE BASELINE|ACTION [s] | SUMMARY | LOG START|STOP");
  else if (!strncmp(line,"ID ",3)) {
    char *end; unsigned long id=strtoul(line+3,&end,16);
    if (end==line+3 || id>0x1FFFFFFF) { reportOut.println("Invalid hexadecimal CAN ID."); return; }
    while (*end==' ') ++end;
    int ext=-1;
    if (*end) { if (!strcmp(end,"STD")) ext=0; else if (!strcmp(end,"EXT")) ext=1; else { reportOut.println("Use ID <hex> [STD|EXT]."); return; } }
    bool found=false;
    for (unsigned i=0;copyRecord(i);++i) if (snapshot.id==id && (ext<0 || snapshot.extended==bool(ext))) { describeRecord(snapshot);found=true; }
    if (!found) reportOut.println("ID not observed in bounded database.");
  } else if (!strcmp(line,"LOG START") || !strcmp(line,"LOG STOP")) {
    char c=!strcmp(line,"LOG START")?'S':'T';
    if (xQueueSend(logCommands,&c,0)!=pdTRUE) reportOut.println("Logger control queue full; retry.");
  } else {
    Command c={}; snprintf(c.text,sizeof(c.text),"%s",line);c.us=nowUs();
    if (xQueueSend(commandQueue,&c,0)!=pdTRUE) reportOut.println("Control queue full; retry.");
  }
}
static void reportTask(void *) {
  for (;;) {
    Command c;
    if(xQueueReceive(reportQueue,&c,portMAX_DELAY)==pdTRUE) {
      xSemaphoreTake(dbMutex,portMAX_DELAY);lastReportSize=0;lastReport[0]=0;xSemaphoreGive(dbMutex);
      reportCommand(c.text); reportOut.flushChunk();
      xSemaphoreTake(dbMutex,portMAX_DELAY);++reportRevision;xSemaphoreGive(dbMutex);
    }
  }
}
static void fatal(const char *message) {
  acquiring=false; Serial.println(message); Serial.println("Startup halted; no active transmit mode exists.");
  for (;;) delay(1000);
}
bool begin(SemaphoreHandle_t sharedStorage) {
  storageMutex=sharedStorage;
  canPreferencesReady=canPreferences.begin("can-config",false);
  const uint32_t savedBitrate=canPreferencesReady ? canPreferences.getUInt("bitrate",DEFAULT_CAN_BITRATE) : DEFAULT_CAN_BITRATE;
  configuredBitrate=supportedBitrate(savedBitrate) ? savedBitrate : DEFAULT_CAN_BITRATE;
  for(unsigned i=0;i<MAX_IDS;++i) {
    records[i]=new(std::nothrow) Record;
    if(!records[i])fatal("ID record allocation failed; reduce MAX_IDS.");
  }
  dbMutex=xSemaphoreCreateMutex();logGate=xSemaphoreCreateMutex();
  frameQueue=xQueueCreate(RX_QUEUE_DEPTH,sizeof(Frame));logQueue=xQueueCreate(LOG_QUEUE_DEPTH,sizeof(Frame));
  commandQueue=xQueueCreate(8,sizeof(Command));noticeQueue=xQueueCreate(16,sizeof(Notice));logCommands=xQueueCreate(4,sizeof(char));
  reportQueue=xQueueCreate(4,sizeof(Command));outputQueue=xQueueCreate(16,sizeof(OutputChunk));
  if(!dbMutex || !logGate || !frameQueue || !logQueue || !commandQueue || !noticeQueue || !logCommands || !reportQueue || !outputQueue)fatal("Allocation failed; reduce MAX_IDS or queue capacity.");
  activeSince=acceptAfter=nowUs();
  const BaseType_t workerCore=portNUM_PROCESSORS>1?1:0;
  if(xTaskCreatePinnedToCore(rxTask,"can-rx",3072,nullptr,20,nullptr,0)!=pdPASS ||
     xTaskCreatePinnedToCore(analysisTask,"can-analysis",4096,nullptr,4,nullptr,workerCore)!=pdPASS ||
     xTaskCreatePinnedToCore(loggerTask,"can-sd",4096,nullptr,2,nullptr,workerCore)!=pdPASS ||
     xTaskCreatePinnedToCore(reportTask,"can-report",8192,nullptr,1,nullptr,workerCore)!=pdPASS)fatal("Task creation failed.");
  ready=true;return true;
}
bool active() { return modeActive.load(); }
bool enabled() { return driverRunning.load(); }
uint32_t bitrate() { return configuredBitrate.load(); }
bool command(const char *text) {
  if(!ready || !text || strlen(text)>=80)return false;
  Command c={};snprintf(c.text,sizeof(c.text),"%s",text);c.us=nowUs();
  for(char *p=c.text;*p;++p)*p=toupper(static_cast<unsigned char>(*p));
  if(!strcmp(c.text,"CAN ON"))return setEnabled(true);
  if(!strcmp(c.text,"CAN OFF"))return setEnabled(false);
  if(!strcmp(c.text,"LOG START") || !strcmp(c.text,"LOG STOP")) {
    if(!strcmp(c.text,"LOG START") && (!modeActive.load() || !driverRunning.load()))return false;
    char code=!strcmp(c.text,"LOG START")?'S':'T';
    return xQueueSend(logCommands,&code,0)==pdTRUE;
  }
  bool report=!strcmp(c.text,"STATUS") || !strcmp(c.text,"IDS") || !strcmp(c.text,"SUMMARY") ||
              !strcmp(c.text,"HELP") || !strncmp(c.text,"ID ",3);
  return xQueueSend(report?reportQueue:commandQueue,&c,0)==pdTRUE;
}
bool setMode(bool enabled) {
  if(!ready)return false;
  if(enabled==modeActive.load())return true;
  if(enabled) { modeActive=true;return true; }
  bool ok=setEnabled(false);modeActive=false;return ok;
}
bool setEnabled(bool enabled) {
  if(!ready || (enabled && !modeActive.load()))return false;
  if(enabled) {
    if(driverRequested.load())return true;
    if(!command("START"))return false;
    driverRequested=true;
  } else {
    const bool wasRequested=driverRequested.exchange(false);
    bool ok=true;
    if(wasRequested || acquiring.load())ok=command("STOP") && ok;
    if(strcmp(logState.load(),"STOPPED"))ok=command("LOG STOP") && ok;
    return ok;
  }
  return true;
}
bool setBitrate(uint32_t value) {
  if(!ready || !supportedBitrate(value) || driverRequested.load() || driverRunning.load())return false;
  if(value==configuredBitrate.load())return true;
  if(!canPreferencesReady || canPreferences.putUInt("bitrate",value)!=sizeof(uint32_t))return false;
  configuredBitrate=value;command("RESET");return true;
}
static char serialLine[80];static unsigned serialLength=0;
static bool serialLineActive=false,serialOverflow=false,serialRequested=false;
bool serialPending() { return serialLineActive; }
void pollConsole(bool quiet) {
  if(!ready)return;
  for(unsigned i=0;i<160 && Serial.available();++i) {
    if(!serialLineActive) {
      int c=Serial.peek();
      if(c!=':' && !(c>='A' && c<='Z' && strchr("SIBECRLH",c)))break;
      serialLineActive=true;serialRequested=true;
      if(c==':'){Serial.read();continue;}
    }
    char c=Serial.read();
    if(c=='\r' || c=='\n') {
      if(serialOverflow)Serial.println("CAN command too long; discarded.");
      else if(serialLength){serialLine[serialLength]=0;if(!command(serialLine))Serial.println("CAN queue full; retry.");}
      serialLength=0;serialOverflow=false;serialLineActive=false;
    } else if(c>=32 && c<=126) {
      if(serialLength<sizeof(serialLine)-1 && !serialOverflow)serialLine[serialLength++]=c;else serialOverflow=true;
    }
  }
  Notice n;
  while(xQueueReceive(noticeQueue,&n,0)==pdTRUE) {
    if(!quiet || serialRequested)Serial.println(n.text);
    if(n.summary)command("SUMMARY");
  }
  OutputChunk chunk;
  for(unsigned i=0;i<8 && xQueueReceive(outputQueue,&chunk,0)==pdTRUE;++i)
    if(!quiet || serialRequested)Serial.print(chunk.text);
}
class JsonWriter {
  char *buffer;size_t capacity,used=0;
public:
  bool ok=true;
  JsonWriter(char *b,size_t c):buffer(b),capacity(c){}
  void operator()(const char *format,...) {
    if(!ok)return;va_list args;va_start(args,format);
    int n=vsnprintf(buffer+used,capacity-used,format,args);va_end(args);
    if(n<0 || size_t(n)>=capacity-used){ok=false;return;}used+=size_t(n);
  }
};
bool statusJson(char *buffer,size_t capacity) {
  if(!ready || !capacity)return false;
  JsonWriter append(buffer,capacity);
  // A single bounded snapshot avoids large allocations and long network-held locks.
  if(xSemaphoreTake(dbMutex,pdMS_TO_TICKS(5))!=pdTRUE)return false;
  struct LiteRecord {
    uint32_t id;bool extended,rtr;uint64_t count;uint8_t lastDlc,previous[8];uint16_t dlcMask;double meanPeriod;
  } ids[MAX_IDS];
  unsigned idCount=recordCount,viewCount=candidateCount;
  CandidateView views[12];memcpy(views,candidateCache,sizeof(views));
  for(unsigned i=0;i<idCount;++i) {
    auto &r=*records[i];auto &v=ids[i];v.id=r.id;v.extended=r.extended;v.rtr=r.rtr;v.count=r.count;
    v.lastDlc=r.lastDlc;memcpy(v.previous,r.previous,8);v.dlcMask=r.dlcMask;v.meanPeriod=r.meanPeriod;
  }
  uint64_t frames=analyzedFrames,end=phaseEnd;Phase capturedPhase=phase;
  bool br=baselineReady,ar=actionReady;uint32_t revision=reportRevision;
  char path[32];snprintf(path,sizeof(path),"%s",logPath);
  char label[24];snprintf(label,sizeof(label),"%s",candidateLabel);
  xSemaphoreGive(dbMutex);
  const uint64_t current=nowUs();
  for(char *p=label;*p;++p)if(!isalnum(static_cast<unsigned char>(*p)) && *p!='_' && *p!='-')*p='_';
  append("{\"mode\":\"LISTEN_ONLY\",\"active\":%s,\"enabled\":%s,\"requested_enabled\":%s,\"driver_state\":\"%s\",\"acquiring\":%s,\"bitrate\":%lu,\"running_bitrate\":%lu,\"tx_gpio\":%d,\"rx_gpio\":%d,\"capacity\":%u,\"log\":%s,\"log_state\":\"%s\",\"file\":\"%s\",",
    modeActive?"true":"false",driverRunning?"true":"false",driverRequested?"true":"false",driverState.load(),
    acquiring?"true":"false",(unsigned long)configuredBitrate.load(),(unsigned long)runningBitrate.load(),CAN_TX_PIN,CAN_RX_PIN,MAX_IDS,
    logEnabled?"true":"false",logState.load(),path);
  append("\"frames\":%llu,\"analysis_drops\":%lu,\"log_drops\":%lu,\"write_errors\":%lu,\"db_full\":%lu,\"driver_missed\":%lu,\"driver_overruns\":%lu,\"bus_errors\":%lu,\"rows\":%lu,\"phase\":%u,\"remaining_s\":%u,\"baseline_ready\":%s,\"action_ready\":%s,",
    (unsigned long long)frames,(unsigned long)analysisDrops.load(),(unsigned long)logDrops.load(),(unsigned long)logErrors.load(),
    (unsigned long)dbFullFrames.load(),(unsigned long)driverMissed.load(),(unsigned long)driverOverruns.load(),(unsigned long)busErrors.load(),
    (unsigned long)logWritten.load(),unsigned(capturedPhase),capturedPhase!=Phase::Idle && end>current?unsigned((end-current)/1000000):0,
    br?"true":"false",ar?"true":"false");
  append("\"report_revision\":%lu,\"free_heap\":%u,\"min_heap\":%u,\"rx_queue_peak\":%lu,\"log_queue_peak\":%lu,\"ids\":[",(unsigned long)revision,ESP.getFreeHeap(),ESP.getMinFreeHeap(),(unsigned long)rxHighWater.load(),(unsigned long)logHighWater.load());
  for(unsigned i=0;i<idCount;++i) {
    auto &r=ids[i];
    append("%s{\"id\":%lu,\"extended\":%s,\"rtr\":%s,\"count\":%llu,\"dlc\":%u,\"dlc_mask\":%u,\"hz\":%.2f,\"data\":[",i?",":"",(unsigned long)r.id,r.extended?"true":"false",r.rtr?"true":"false",(unsigned long long)r.count,r.lastDlc,r.dlcMask,r.meanPeriod>0?1e6/r.meanPeriod:0);
    if(!r.rtr)for(unsigned j=0;j<r.lastDlc;++j)append("%s%u",j?",":"",r.previous[j]);
    append("]}");
  }
  append("],\"experiment\":\"%s\",\"candidates\":[",label);
  for(unsigned i=0;i<viewCount;++i) {
    auto &c=views[i];
    append("%s{\"id\":%lu,\"extended\":%s,\"start\":%u,\"width\":%u,\"motorola\":%s,\"score\":%.3f}",i?",":"",(unsigned long)c.id,c.extended?"true":"false",c.start,c.width,c.motorola?"true":"false",double(c.score));
  }
  append("]}");return append.ok;
}
bool reportText(char *buffer,size_t capacity) {
  if(!ready || capacity<sizeof(lastReport))return false;
  if(xSemaphoreTake(dbMutex,pdMS_TO_TICKS(5))!=pdTRUE)return false;
  memcpy(buffer,lastReport,lastReportSize+1);xSemaphoreGive(dbMutex);return true;
}
} // namespace canservice
