// ISOLATED BENCH ONLY. This is a separate transmitter, never an analyzer feature.
// Requires GPIO27 grounded AND the exact Serial command ENABLE BENCH.
#include <Arduino.h>
#include <driver/twai.h>
constexpr int TX_PIN=21, RX_PIN=22, ARM_PIN=27;
bool enabled=false, action=false;
uint32_t sequence=0, failures=0;
char input[48]; unsigned length=0; bool overflow=false;
void send(uint32_t id,bool ext,uint8_t *data,uint8_t dlc,bool rtr=false) {
  twai_message_t m={};m.identifier=id;m.extd=ext;m.rtr=rtr;m.ss=1;m.data_length_code=dlc;
  if(!rtr)memcpy(m.data,data,dlc);
  if(twai_transmit(&m,pdMS_TO_TICKS(5))!=ESP_OK)++failures;
}
void setup() {
  Serial.begin(115200);pinMode(ARM_PIN,INPUT_PULLUP);
  // No-ACK permits the two-board test when the analyzer correctly sends no ACK.
  twai_general_config_t general=TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)TX_PIN,(gpio_num_t)RX_PIN,TWAI_MODE_NO_ACK);
  twai_timing_config_t timing=TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if(twai_driver_install(&general,&timing,&filter)!=ESP_OK || twai_start()!=ESP_OK) {
    Serial.println("BENCH TWAI initialization failed");while(true)delay(1000);
  }
  Serial.println("ISOLATED BENCH TRANSMITTER. Ground GPIO27, then ENABLE BENCH. ACTION ON / ACTION OFF / DISABLE / STATUS.");
}
void loop() {
  while(Serial.available()) {
    char c=Serial.read();
    if(c=='\n'||c=='\r') {
      input[length]=0;
      if(!overflow && !strcmp(input,"ENABLE BENCH") && digitalRead(ARM_PIN)==LOW)enabled=true;
      else if(!strcmp(input,"DISABLE"))enabled=false;
      else if(!overflow && !strcmp(input,"ACTION ON"))action=true;
      else if(!overflow && !strcmp(input,"ACTION OFF"))action=false;
      else if(!overflow && !strcmp(input,"STATUS"))Serial.printf("enabled=%u action=%u sequence=%lu enqueue_failures=%lu\n",enabled,action,(unsigned long)sequence,(unsigned long)failures);
      length=0;overflow=false;
    } else if(length<sizeof(input)-1&&!overflow)input[length++]=c;else overflow=true;
  }
  if(digitalRead(ARM_PIN)!=LOW)enabled=false;
  if(!enabled){delay(10);return;}
  uint8_t d[8]={};uint32_t ramp=sequence%512;ramp=ramp<256?ramp:511-ramp;
  d[0]=((sequence&15)<<4)|(sequence&3);
  uint16_t le=action?uint16_t(1234+ramp*8):1234;d[1]=le;d[2]=le>>8;
  uint32_t be=action?100000+ramp*16:100000;d[3]=be>>16;d[4]=be>>8;d[5]=be;
  d[6]=sequence;for(unsigned i=0;i<7;++i)d[7]+=d[i];
  // All identifiers below are synthetic bench fixtures, with no vehicle meaning.
  send(0x321,false,d,8);send(0x321,true,d,8);
  uint8_t counter[8]={uint8_t(sequence),uint8_t(sequence>>8),0x55,0xAA,0,0,0,0};
  send(0x322,false,counter,8);
  if(sequence%10==0)send(0x345,false,d,(sequence/10)%9);
  if(sequence%50==0)send(0x456,false,d,8,true);
  if(action&&sequence%5==0)send(0x700,false,d,8);
  ++sequence;delay(10);
}
