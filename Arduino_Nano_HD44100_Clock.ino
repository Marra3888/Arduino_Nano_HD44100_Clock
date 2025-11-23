#include <Arduino.h>
#include <string.h>

// Пины HD44100
#define PIN_DL1  11   // Data
#define PIN_CL2  13   // один из тактов (shift или latch)
#define PIN_CL1  12   // второй такт (latch или shift)
#define PIN_M    10   // переменная полярность (AC)

// Геометрия
#define HAVE_DP             1
const uint16_t NSEG = HAVE_DP ? 80 : 70;  // 10*(7+DP) или 10*7
const uint8_t  NCOM = 4;                  // чаще 4

// Настройки (оставьте как подобрали)
#define CL2_IS_SHIFT        1     // 1: CL2=shift, CL1=latch; 0 — наоборот
#define BIT_LSB_FIRST       1     // 1: LSB first, 0 — MSB first
#define REVERSE_DIGIT_ORDER 1     // 1: разряды 9..0; 0 — 0..9
#define M_PER_LINE          1     // 1: инвертировать M на каждой строке; 0 — на кадре
#define M_INVERT            0     // 1: инвертировать уровень M целиком
#define LINE_HOLD_US        60    // «время строки»
#define TCLK_US             2     // полупериод «пикания» CL1/CL2

// Шрифт (A..G, DP в бите7)
const uint8_t seg7_font[10] = {
  0b00111111, // 0
  0b00000110, // 1
  0b01011011, // 2
  0b01001111, // 3
  0b01100110, // 4
  0b01101101, // 5
  0b01111101, // 6
  0b00000111, // 7
  0b01111111, // 8
  0b01101111  // 9
};

// ===== карта: digit(0..9) × seg(0=A..6=G,7=DP) → индекс бита
int16_t mapBit[10][8];

// Буферы
uint8_t lineBits[8][(NSEG+7)/8];   // строки (битовый буфер)
static int8_t   digits10[10];      // -1 пусто, 0..9 — цифра
static uint16_t dpMask10 = 0;      // точки (если понадобятся)
static uint16_t gOnlyMask = 0;     // МАСКА разрядов, где выводим ТОЛЬКО сегмент G

// ===== утилиты низкого уровня =====
inline void setBit(uint8_t com, uint16_t bitIndex, bool val){
  uint16_t byteIdx = bitIndex >> 3;
  uint8_t  mask    = 1 << (bitIndex & 7);
  if(val) lineBits[com][byteIdx] |= mask;
  else    lineBits[com][byteIdx] &= ~mask;
}
inline void pulse(uint8_t pin){
  digitalWrite(pin, HIGH); delayMicroseconds(TCLK_US);
  digitalWrite(pin, LOW ); delayMicroseconds(TCLK_US);
}

void shiftLineFromBuf(uint8_t com){
  uint8_t pinShift = CL2_IS_SHIFT ? PIN_CL2 : PIN_CL1;
  uint8_t pinLatch = CL2_IS_SHIFT ? PIN_CL1 : PIN_CL2;

  uint16_t cnt = 0;
  for(uint16_t i=0; cnt < NSEG; i++){
    uint8_t b = lineBits[com][i];
    if (!BIT_LSB_FIRST) {
      for (uint8_t k=0; k<8 && cnt<NSEG; k++, cnt++){
        digitalWrite(PIN_DL1, (b & 0x80) ? HIGH : LOW);
        pulse(pinShift);
        b <<= 1;
      }
    } else {
      for (uint8_t k=0; k<8 && cnt<NSEG; k++, cnt++){
        digitalWrite(PIN_DL1, (b & 0x01) ? HIGH : LOW);
        pulse(pinShift);
        b >>= 1;
      }
    }
  }
  pulse(pinLatch);
  delayMicroseconds(LINE_HOLD_US);
}

// ===== сборка кадра =====
void buildFrame(const int8_t digits[10], uint16_t dpMask){
  for(uint8_t com=0; com<NCOM; com++)
    memset(lineBits[com], 0, (NSEG+7)/8);

  for(uint8_t d=0; d<10; d++){
    // если для позиции задан «G‑только», то рисуем только сегмент G
    if (gOnlyMask & (1U<<d)) {
      int16_t idxG = mapBit[d][6]; // сегмент G
      if (idxG >= 0) {
        for(uint8_t com=0; com<NCOM; com++) setBit(com, (uint16_t)idxG, true);
      }
      continue; // цифру не рисуем
    }

    int8_t val = digits[d];
    if(val < 0 || val > 9) continue;

    uint8_t pat = seg7_font[val];
#if HAVE_DP
    if (dpMask & (1U<<d)) pat |= 0x80;
#endif
    for(uint8_t s=0; s<(HAVE_DP ? 8 : 7); s++){
      if (!(pat & (1<<s))) continue;
      int16_t bitIndex = mapBit[d][s];
      if (bitIndex < 0) continue;
      for(uint8_t com=0; com<NCOM; com++)
        setBit(com, (uint16_t)bitIndex, true);
    }
  }
}

// ===== карта “линейно по разрядам”: 8 бит на каждый разряд =====
void fillLinearMap() {
  for (uint8_t d=0; d<10; d++){
    uint16_t base = (REVERSE_DIGIT_ORDER ? (9 - d) : d) * (HAVE_DP ? 8 : 7);
    for (uint8_t s=0; s<8; s++){
#if HAVE_DP
      mapBit[d][s] = base + s;
#else
      mapBit[d][s] = (s<7) ? (base + s) : -1;
#endif
    }
  }
}

// ===== API вывода =====
void clearDigits(){
  for (uint8_t i=0;i<10;i++) digits10[i] = -1;
  dpMask10 = 0;
  gOnlyMask = 0;
}

void clearDisplayNow(uint8_t frames = 1) {
// 1) очистить логический буфер
for (uint8_t i=0;i<10;i++) digits10[i] = -1;
dpMask10 = 0;
gOnlyMask = 0;
// 2) построить пустой кадр (lineBits = 0)
buildFrame(digits10, dpMask10);
// 3) «запушить» его в контроллер (1–2 кадра для уверенности)
bool mFrame = false;
for (uint8_t f=0; f<frames; ++f){
bool m = mFrame;
for (uint8_t com=0; com<NCOM; ++com){
digitalWrite(PIN_M, m);
shiftLineFromBuf(com);
if (M_PER_LINE) m = !m;
}
if (!M_PER_LINE) mFrame = !mFrame;
}
}

void setDigit(uint8_t pos, int8_t val, bool dp=false){
  if (pos > 9) return;
  digits10[pos] = (val>=0 && val<=9) ? val : -1;
  if (HAVE_DP){
    if (dp) dpMask10 |=  (1U<<pos);
    else    dpMask10 &= ~(1U<<pos);
  }
}
inline void setSeparatorG(uint8_t pos, bool on=true){
  if (pos > 9) return;
  if (on)  gOnlyMask |=  (1U<<pos);
  else     gOnlyMask &= ~(1U<<pos);
}

// Время “HH G MM G SS” начиная с позиции start (занимает 8 разрядов)
void setTimeG(uint8_t hh, uint8_t mm, uint8_t ss, uint8_t start=0){
  if (start > 9) return;
  if (start+7 > 9) return; // нужно 8 позиций: 2+1+2+1+2

  // clearDigits();
  clearDisplayNow();

  // Позиции: H1 H2 G M1 M2 G S1 S2
  uint8_t pH1 = start+0, pH2 = start+1;
  uint8_t pG1 = start+2;
  uint8_t pM1 = start+3, pM2 = start+4;
  uint8_t pG2 = start+5;
  uint8_t pS1 = start+6, pS2 = start+7;

  setDigit(pH1, (hh/10)%10);
  setDigit(pH2, (hh%10));
  setSeparatorG(pG1, true);
  setDigit(pM1, (mm/10)%10);
  setDigit(pM2, (mm%10));
  setSeparatorG(pG2, true);
  setDigit(pS1, (ss/10)%10);
  setDigit(pS2, (ss%10));
}

// ===== ОСНОВНОЙ КОД =====
void setup(){
  pinMode(PIN_DL1, OUTPUT);
  pinMode(PIN_CL1, OUTPUT);
  pinMode(PIN_CL2, OUTPUT);
  pinMode(PIN_M,   OUTPUT);
  digitalWrite(PIN_DL1, LOW);
  digitalWrite(PIN_CL1, LOW);
  digitalWrite(PIN_CL2, LOW);
  digitalWrite(PIN_M,   LOW);

  fillLinearMap();
  clearDigits();
}

void loop(){
  // Демонстрация: часы идут, формат “HH G MM G SS”
  static uint32_t t0=0;
  static uint8_t hh=12, mm=34, ss=50;
  if (millis()-t0 >= 1000){
    t0 += 1000;
    if (++ss==60){ ss=0; if (++mm==60){ mm=0; if (++hh==24) hh=0; } }
  }
  setTimeG(hh, mm, ss, 1);  // с позиции 0 (займёт 0..7)

  // Сборка и скан
  buildFrame(digits10, dpMask10);

  static bool mFrame = false;
  bool m = (M_INVERT ? !mFrame : mFrame);
  for(uint8_t com=0; com<NCOM; com++){
    digitalWrite(PIN_M, m);
    shiftLineFromBuf(com);
    if (M_PER_LINE) m = !m;
  }
  if (!M_PER_LINE) mFrame = !mFrame;
}