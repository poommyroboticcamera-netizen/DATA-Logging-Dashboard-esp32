#include "../../src/can/Analysis.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace canlab;
static void encode(Sample &s,Field f,uint32_t v) {
  int p=f.start;
  for(unsigned i=0;i<f.width;++i) {
    unsigned bit=f.motorola ? f.width-1-i:i;
    uint8_t mask=1u<<(p%8);
    s.data[p/8]=(s.data[p/8]&~mask)|(((v>>bit)&1)<<(p%8));
    if(f.motorola) p=p%8 ? p-1:p+15; else ++p;
  }
}
static Frame frame(unsigned n,uint8_t v=0) {
  Frame f={};f.id=0x321;f.us=1000ull*n;f.dlc=8;f.data[0]=v;return f;
}
static void extraction() {
  Sample s;s.dlc=8;uint32_t out;
  s.data[0]=0x12;s.data[1]=0x34;s.data[2]=0x56;s.data[3]=0x78;
  assert(extract(s,Field{0,32,false},out)&&out==0x78563412);
  assert(extract(s,Field{7,32,true},out)&&out==0x12345678);
  assert(extract(s,Field{4,8,false},out)&&out==0x41);
  assert(extract(s,Field{3,8,true},out)&&out==0x23);
  unsigned tested=0;
  for(unsigned width: {1u,2u,4u,8u,16u,24u,32u})for(unsigned endian=0;endian<2;++endian)for(unsigned start=0;start<64;++start) {
    Field f{uint8_t(start),uint8_t(width),bool(endian)};if(!validField(f))continue;
    s=Sample{};s.dlc=8;uint32_t mask=width==32?UINT32_MAX:((1u<<width)-1);
    uint32_t value=0xBA765432&mask;encode(s,f,value);assert(extract(s,f,out)&&out==value);++tested;
  }
  assert(tested>600);s.dlc=1;assert(!extract(s,Field{7,16,true},out));
  assert(!validField(Field{63,2,false}));assert(!validField(Field{0,0,false}));
}
static void statistics() {
  Moments m;for(unsigned v:{1u,2u,3u,4u})m.add(v);
  assert(m.mean==2.5&&fabs(m.variance()-5.0/3)<1e-10);
  Record r;r.add(frame(1,0));r.add(frame(2,1));r.add(frame(3,3));
  assert(r.count==3&&r.bytes[0].changes==2&&r.bytes[0].transitions==2);
  assert(r.bytes[0].xorMask==3&&r.bytes[0].toggles[0]==1&&r.bytes[0].toggles[1]==1);
  assert(r.meanPeriod==1000&&r.minPeriod==1000&&r.maxPeriod==1000);
  Frame f=frame(10,0);f.gap=1;r.add(f);assert(r.periodCount==2&&r.bytes[0].transitions==2);
  f=frame(11,9);f.gap=1;f.dlc=1;r.add(f);assert(r.bytes[0].transitions==2&&r.bytes[1].value.n==4);
  Record remote;f.rtr=1;remote.add(f);assert(remote.count==1&&remote.recent.count==0&&remote.bytes[0].value.n==0);
  for(unsigned n=0;n<100;++n)r.recent.append(frame(n));
  assert(r.recent.count==SAMPLE_COUNT&&r.recent.at(0).us==(100-SAMPLE_COUNT)*1000ull&&r.recent.at(SAMPLE_COUNT-1).us==99000);
}
static void counters() {
  for(unsigned width:{2u,4u,8u,16u})for(bool motorola:{false,true}) {
    Samples s;Field field{uint8_t(motorola?7:0),uint8_t(width),motorola};
    uint32_t mask=(1u<<width)-1;
    for(unsigned i=0;i<SAMPLE_COUNT;++i) {
      Sample p;p.dlc=8;encode(p,field,(mask-10+i)&mask);
      Frame f=frame(i);memcpy(f.data,p.data,8);s.append(f);
    }
    auto e=evaluate(s,field,true);assert(e.counter()==1&&e.wraps>0);
    assert(evaluate(s,field,false).counter()==0);
  }
  Samples gap;for(unsigned i=0;i<SAMPLE_COUNT;++i){Frame f=frame(i,i);f.gap=i;gap.append(f);}
  assert(evaluate(gap,Field{0,8,false},true).transitions==0);
}
static void windows() {
  Window w;for(unsigned i=0;i<1000;++i)w.add(frame(i,i%256),0,1000000);
  assert(w.frames==1000&&w.bytes[0].n==1000&&w.samples.count==SAMPLE_COUNT);
  assert(w.samples.at(0).us<32000&&w.samples.at(SAMPLE_COUNT-1).us==999000);
  Samples base, action, held, same;
  for(unsigned i=0;i<SAMPLE_COUNT;++i){base.append(frame(i,10));action.append(frame(i,10+i*3));held.append(frame(i,200));same.append(frame(i,10));}
  Field f{0,8,false};auto b=evaluate(base,f,false),a=evaluate(action,f,false);
  assert(compare(b,a).score>.5);assert(compare(b,evaluate(held,f,false)).score>.2);
  assert(compare(b,evaluate(same,f,false)).score==0);
  assert(fabs(a.correlation-1)<1e-9);assert(compare(a,a).score==0);
}
static void checksums() {
  Samples s;
  for(unsigned i=0;i<SAMPLE_COUNT;++i) {
    Frame f=frame(i,i);f.data[1]=i*3;f.data[2]=i*7;
    for(unsigned j=0;j<7;++j)f.data[7]+=f.data[j];s.append(f);
  }
  auto c=checksum(s,7);assert(c.samples==SAMPLE_COUNT&&c.sum==SAMPLE_COUNT&&c.changes>4);
}
int main() {
  extraction();statistics();counters();windows();checksums();
  printf("PASS: extraction, statistics, DLC/RTR/gaps, rings, modulo counters, windows, held-state/ramp ranking, checksum.\n");
  printf("host sizes Frame=%zu Sample=%zu Record=%zu database=%zu\n",sizeof(Frame),sizeof(Sample),sizeof(Record),sizeof(Record)*MAX_IDS);
}
