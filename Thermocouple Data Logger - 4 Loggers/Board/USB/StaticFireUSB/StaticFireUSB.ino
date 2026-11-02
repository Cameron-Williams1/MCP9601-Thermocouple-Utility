// Feather RP2040 Adalogger: stream new MCP9601 conversions over USB.
// K-type, fastest 12-bit conversion, unfiltered. No SD card required.
#include <Wire.h>
#include <pico/time.h>
bool configured[4]; uint32_t retry[4], lastReport[4], seq;
bool get(uint8_t a,uint8_t r,uint8_t *out,int n){
  Wire.beginTransmission(a);Wire.write(r);
  if(Wire.endTransmission(false)||Wire.requestFrom(a,(uint8_t)n)!=n)return false;
  while(n--)*out++=Wire.read();return true;
}
bool put(uint8_t a,uint8_t r,uint8_t x){
  Wire.beginTransmission(a);Wire.write(r);Wire.write(x);return !Wire.endTransmission();
}
void emit(uint8_t a,const char* state,int raw=0){
  if(!Serial)return;
  Serial.printf("T,%lu,%llu,0x%02X,%s,", (unsigned long)++seq,(unsigned long long)time_us_64(),a,state);
  if(!strcmp(state,"OK"))Serial.print(raw/16.0,4);
  Serial.println();
}
void setup(){Serial.begin(115200);Wire.setSDA(2);Wire.setSCL(3);Wire.begin();Wire.setClock(100000);Wire.setTimeout(20);}
void loop(){
  for(int i=0;i<4;i++){
    uint8_t a=0x64+i,id[2],s,raw[2];
    if(!configured[i]){
      if((int32_t)(millis()-retry[i])<0)continue;
      retry[i]=millis()+1000;
      configured[i]=get(a,0x20,id,2)&&id[0]==0x41&&put(a,5,0)&&put(a,6,0x60);
      if(!configured[i])emit(a,"I2C_ERROR");
      continue;
    }
    if(!get(a,4,&s,1)){configured[i]=false;emit(a,"I2C_ERROR");continue;}
    if(!(s&0x40)){
      if((s&0x30) && millis()-lastReport[i]>=1000){
        emit(a,(s&0x10)?"OPEN":"SHORT");lastReport[i]=millis();
      }
      continue;
    } // Temperature update flag: no duplicate polls.
    if(!get(a,0,raw,2)||!put(a,4,s&~0x40)){configured[i]=false;emit(a,"I2C_ERROR");continue;}
    lastReport[i]=millis();
    emit(a,(s&0x10)?"OPEN":(s&0x20)?"SHORT":"OK",(int16_t)((raw[0]<<8)|raw[1]));
  }
}
