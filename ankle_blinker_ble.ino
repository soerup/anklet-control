// ankle_blinker_ble_v4.ino
// WEMOS LOLIN32 Lite (ESP32, core 3.x) — 6 main LED channels + 1 independent side LED, BLE (Bluefy).
// No button, no piezo. Control is entirely over BLE; the slide switch is the real on/off.
// Power-up starts on a RANDOM main effect and a RANDOM (active) side pattern.
//
// Main LEDs : GPIO 32,33,25,26,27,14 (indices 0..5). Effects 0..7, plus "Off" = 8 (main dark).
// Side LED  : GPIO 19 (purple). Patterns: 0 Off, 1 50/50, 2 Fast-fast-slow, 3 Strobe, 4 Breathe, 5 SOS.
// Shared    : brightness + speed apply to both.  Status LED: GPIO22 (active-low) = BLE connected.
// Power     : 3–4× Ladda (NiMH) -> JST BAT (+ header GND). Slide switch for off.
//             NEVER plug USB while the pack is connected.
//
// Toolchain: ESP32 Arduino core 3.x + NimBLE-Arduino 2.x. Board: "WEMOS LOLIN32 Lite".

#include <NimBLEDevice.h>
#include <math.h>

// ---------- pins ----------
const uint8_t LED[]    = {32, 33, 25, 26, 27, 14};   // main effect channels
const uint8_t N        = sizeof(LED);                // 6 channels
const uint8_t SIDE     = 19;                         // purple side LED (independent)
const uint8_t LED_STAT = 22;                         // onboard, active-low

// ---------- shared state ----------
uint8_t       pattern      = 0;                      // 0..7 effects, 8 = off (random in setup)
const uint8_t N_EFFECTS    = 8;                      // effects available to random start
const uint8_t N_PATTERNS   = 9;                      // effects + off
uint8_t       sidePattern  = 1;                      // 0 off, 1..5 rhythms (random in setup)
const uint8_t N_SIDE       = 6;
uint8_t       brightness   = 120;                    // 0..255  (shared)
uint8_t       speed        = 128;                    // 8..255, 128 = 1x (shared)
bool          powerOn      = true;
unsigned long t0 = 0, tSide = 0;

// ---------- control kinds (before any function for the .ino auto-prototype) ----------
enum Ctrl { C_PATTERN, C_BRIGHT, C_POWER, C_SPEED, C_SIDE };

// ---------- BLE ----------
#define SERVICE_UUID "19b10000-e8f2-537e-4f6c-d104768a1214"
#define UUID_PATTERN "19b10001-e8f2-537e-4f6c-d104768a1214"
#define UUID_BRIGHT  "19b10002-e8f2-537e-4f6c-d104768a1214"
#define UUID_POWER   "19b10003-e8f2-537e-4f6c-d104768a1214"
#define UUID_SPEED   "19b10004-e8f2-537e-4f6c-d104768a1214"
#define UUID_SIDE    "19b10005-e8f2-537e-4f6c-d104768a1214"

NimBLECharacteristic *chPattern, *chBright, *chPower, *chSpeed, *chSide;

// ---------- LED helpers ----------
void writePin(uint8_t pin, uint8_t v) { ledcWrite(pin, (uint16_t)v * brightness / 255); }
void writeLed(uint8_t i, uint8_t v)   { writePin(LED[i], v); }
void setAll(uint8_t v)  { for (uint8_t i = 0; i < N; i++) writeLed(i, v); }
void clearMain()        { for (uint8_t i = 0; i < N; i++) ledcWrite(LED[i], 0); }
void clearAll()         { clearMain(); ledcWrite(SIDE, 0); }

// ---------- state setters ----------
void notifyByte(NimBLECharacteristic* c, uint8_t v) { c->setValue(&v, 1); c->notify(); }
void setPower(bool on) {
  powerOn = on;
  if (!on) clearAll(); else { t0 = millis(); tSide = millis(); }
  notifyByte(chPower, on ? 1 : 0);
}
void setPattern(uint8_t p) { pattern = p % N_PATTERNS; t0 = millis(); notifyByte(chPattern, pattern); }
void setSide(uint8_t p)    { sidePattern = p % N_SIDE; tSide = millis(); notifyByte(chSide, sidePattern); }

// ---------- BLE callbacks ----------
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { digitalWrite(LED_STAT, LOW); }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    digitalWrite(LED_STAT, HIGH);
    NimBLEDevice::startAdvertising();
  }
};
class CtrlCB : public NimBLECharacteristicCallbacks {
  Ctrl kind;
 public:
  explicit CtrlCB(Ctrl k) : kind(k) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() < 1) return;
    const uint8_t* d = v.data();
    switch (kind) {
      case C_PATTERN:  setPattern(d[0]);              break;
      case C_BRIGHT:   brightness = d[0];             break;
      case C_POWER:    setPower(d[0] != 0);            break;
      case C_SPEED:    speed = (d[0] < 8) ? 8 : d[0];  break;
      case C_SIDE:     setSide(d[0]);                 break;
    }
  }
};

// ---------- main effects (es = speed-scaled elapsed ms) ----------
void patAllBlink(unsigned long es) { setAll(((es / 400) % 2) ? 255 : 0); }
void patChase(unsigned long es) {
  uint8_t step = (es / 150) % N;
  for (uint8_t i = 0; i < N; i++) writeLed(i, (i == step) ? 255 : 0);
}
void patComet(unsigned long es) {
  uint8_t head = (es / 120) % N;
  for (uint8_t i = 0; i < N; i++) {
    uint8_t dist = (uint8_t)((head + N - i) % N);
    writeLed(i, dist <= 3 ? (uint8_t)(255 >> dist) : 0);
  }
}
void patBounce(unsigned long es) {
  uint8_t span = 2 * (N - 1);
  uint8_t step = (es / 120) % span;
  uint8_t pos  = step < N ? step : (uint8_t)(span - step);
  for (uint8_t i = 0; i < N; i++) {
    uint8_t d = i > pos ? i - pos : pos - i;
    writeLed(i, d == 0 ? 255 : (d == 1 ? 70 : 0));
  }
}
void patSparkle(unsigned long es) {
  uint32_t slot = es / 90;
  for (uint8_t i = 0; i < N; i++) {
    uint32_t h = (slot * 2654435761u) ^ ((uint32_t)(i + 1) * 40503u);
    h ^= h >> 13;
    writeLed(i, (h & 3) == 0 ? 255 : 0);
  }
}
void patAlternate(unsigned long es) {
  uint8_t phase = (es / 300) % 2;
  for (uint8_t i = 0; i < N; i++) writeLed(i, ((i & 1) == phase) ? 255 : 0);
}
void patWave(unsigned long es) {
  for (uint8_t i = 0; i < N; i++) {
    float a = es * 0.006f + i * 0.9f;
    writeLed(i, (uint8_t)(127.5f * (1.0f + sinf(a))));
  }
}
void patBuildUp(unsigned long es) {
  uint8_t step = (es / 150) % (N + 1);
  for (uint8_t i = 0; i < N; i++) writeLed(i, (i < step) ? 255 : 0);
}
void runPattern() {
  unsigned long es = (millis() - t0) * speed / 128;
  switch (pattern) {
    case 0: patAllBlink(es);  break;
    case 1: patChase(es);     break;
    case 2: patComet(es);     break;
    case 3: patBounce(es);    break;
    case 4: patSparkle(es);   break;
    case 5: patAlternate(es); break;
    case 6: patWave(es);      break;
    case 7: patBuildUp(es);   break;
    case 8: clearMain();      break;   // main 6 off (side keeps running)
  }
}

// ---------- side LED patterns (single channel) ----------
void patSide5050(unsigned long es)  { writePin(SIDE, ((es / 500) % 2) ? 255 : 0); }
void patSideFFS(unsigned long es) {                 // blink, blink, pause
  const unsigned long u = 150;
  unsigned long c = es % (u * 6);
  bool on = (c < u) || (c >= 2 * u && c < 3 * u);
  writePin(SIDE, on ? 255 : 0);
}
void patSideStrobe(unsigned long es) { writePin(SIDE, (es % 160) < 40 ? 255 : 0); }
void patSideBreathe(unsigned long es) {
  unsigned long p = es % 2000;
  writePin(SIDE, (p < 1000) ? (p * 255 / 1000) : ((2000 - p) * 255 / 1000));
}
void patSideSOS(unsigned long es) {                 // Morse: · · ·  — — —  · · ·
  const uint16_t U = 200;
  static const uint8_t on_[18]  = {1,0,1,0,1,0, 1,0,1,0,1,0, 1,0,1,0,1,0};
  static const uint8_t dur_[18] = {1,1,1,1,1,3, 3,1,3,1,3,3, 1,1,1,1,1,7};
  unsigned long total = 0; for (uint8_t i = 0; i < 18; i++) total += dur_[i];
  unsigned long tms = es % (total * U), acc = 0;
  for (uint8_t i = 0; i < 18; i++) {
    acc += (unsigned long)dur_[i] * U;
    if (tms < acc) { writePin(SIDE, on_[i] ? 255 : 0); return; }
  }
}
void runSide() {
  unsigned long es = (millis() - tSide) * speed / 128;
  switch (sidePattern) {
    case 0: ledcWrite(SIDE, 0);  break;   // off
    case 1: patSide5050(es);     break;
    case 2: patSideFFS(es);      break;
    case 3: patSideStrobe(es);   break;
    case 4: patSideBreathe(es);  break;
    case 5: patSideSOS(es);      break;
  }
}

// ---------- BLE setup helper ----------
NimBLECharacteristic* mkChar(NimBLEService* s, const char* uuid, Ctrl kind) {
  const uint32_t RWN = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY;
  NimBLECharacteristic* c = s->createCharacteristic(uuid, RWN);
  c->setCallbacks(new CtrlCB(kind));
  return c;
}

void setup() {
  pinMode(LED_STAT, OUTPUT); digitalWrite(LED_STAT, HIGH);
  for (uint8_t i = 0; i < N; i++) { ledcAttach(LED[i], 5000, 8); ledcWrite(LED[i], 0); }
  ledcAttach(SIDE, 5000, 8); ledcWrite(SIDE, 0);

  NimBLEDevice::init("Anklet");
  pattern     = esp_random() % N_EFFECTS;              // random effect (never "off")
  sidePattern = 1 + (esp_random() % (N_SIDE - 1));     // random side rhythm (never "off")

  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCB());
  NimBLEService* svc = srv->createService(SERVICE_UUID);

  chPattern = mkChar(svc, UUID_PATTERN, C_PATTERN);
  chBright  = mkChar(svc, UUID_BRIGHT,  C_BRIGHT);
  chPower   = mkChar(svc, UUID_POWER,   C_POWER);
  chSpeed   = mkChar(svc, UUID_SPEED,   C_SPEED);
  chSide    = mkChar(svc, UUID_SIDE,    C_SIDE);

  uint8_t vp = pattern, vb = brightness, vo = powerOn ? 1 : 0, vs = speed, vsd = sidePattern;
  chPattern->setValue(&vp, 1); chBright->setValue(&vb, 1);
  chPower->setValue(&vo, 1);   chSpeed->setValue(&vs, 1); chSide->setValue(&vsd, 1);

  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  NimBLEDevice::startAdvertising();

  t0 = tSide = millis();
}

void loop() {
  if (powerOn) { runPattern(); runSide(); } else clearAll();
  delay(2);
}
