// ESP32-C3 Super Mini, 9 strands × 20 WS2812/GRB LEDs.
// Arduino: ESP32C3 Dev Module; USB CDC On Boot: Enabled; Flash: 4MB;
// Partition Scheme: Default 4MB with spiffs (two OTA slots).
// GPIO20/21 are LED data here, NOT UART. Serial uses native USB (GPIO18/19).
// GPIO8/9 are boot strapping pins: LED inputs must not force their boot levels.
// Strand order: 20,10,9,7,8,21,5,0,6. Each strand starts at row 0 (top).
#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "This firmware is ONLY for ESP32-C3 Super Mini 9x20"
#endif
#if !ARDUINO_USB_CDC_ON_BOOT
#error "Enable USB CDC On Boot: GPIO20 and GPIO21 are used for LEDs"
#endif
#include <WiFi.h>
#include <AsyncUDP.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <sys/time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/md.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <LittleFS.h>

// версия прошивки — приложение сравнивает её с последней и предлагает обновиться
#define FW_VERSION 19
const char *DEVICE_MODEL = "esp32c3-9x20";
String deviceIdentity;
String discoveryMessage;

// обновления прошивки — только отсюда (чужую прошивку подсунуть нельзя)
const char *OTA_PREFIX = "https://raw.githubusercontent.com/nf97700-glitch/LED-Matrix-Updates/";

const char *AP_SSID     = "Matrix-9x20";
const char *AP_PASSWORD = "12345678";
const char *DEVICE_NAME = "Matrix 9x20";

const uint16_t DISCOVERY_PORT = 45454;
const uint16_t COMMAND_PORT   = 3333;

const uint32_t ANNOUNCE_INTERVAL_MS    = 3000;
const uint32_t HOME_CONNECT_TIMEOUT_MS = 6000;
const uint32_t RESET_HOLD_MS           = 5000;   // кнопку сброса держать 5 с без перерыва
const uint32_t RECONNECT_CHECK_MS      = 2000;
const uint32_t RECONNECT_TIMEOUT_MS    = 15000;

#define COLS 9
#define ROWS 20
#define LANES_PER_FRAME 5
#define SNOW_FLAKES 57

uint8_t STRAND_PINS[COLS] = {
  20, 10, 9, 7, 8, 21, 5, 0, 6
};

Adafruit_NeoPixel *strips[COLS] = { nullptr };
bool strandDirty[COLS] = { false };

Preferences prefs;
// UDP принимается в отдельной задаче (AsyncUDP) и складывается в очередь:
// пока loop() занят (TCP, кадр режима), пиксели не теряются
AsyncUDP    udp;
WiFiServer  commandServer(COMMAND_PORT);
WiFiClient  commandClient;
bool     tcpActive  = false;   // есть клиент
bool     tcpReplied = false;   // ответ ушёл, ждём, пока клиент сам закроет соединение
uint32_t tcpAt      = 0;
String   tcpLine;

#define UDP_MSG_MAX 1000
#define UDP_QUEUE_LEN 64
struct UdpMsg { uint16_t len; char data[UDP_MSG_MAX + 1]; };
QueueHandle_t udpQueue = nullptr;

String homeSsid, homePass;
uint32_t lastAnnounce = 0;
uint32_t lastShowAt   = 0;
uint8_t  globalBrightness = 191;

const uint32_t SHOW_INTERVAL_MS = 2;
bool wifiLoggedOn = false;

enum Mode { MODE_OFF = 0, MODE_SNOW, MODE_FIRE, MODE_RAINBOW, MODE_RAIN,
           MODE_FIREWORKS, MODE_LAVA, MODE_STARS, MODE_GALAXY, MODE_TUNNEL, MODE_AURORA, MODE_PLASMA, MODE_KALEIDOSCOPE };
Mode currentMode = MODE_OFF;
// СЛУЧАЙНЫЙ режим: режимы сменяют друг друга каждые randEvery минут (0 = выключен)
const int MODE_COUNT = 12;   // snow..galaxy по порядку enum
uint16_t randEvery = 0;
uint32_t randNextAt = 0;
uint8_t  modeSpeeds[MODE_COUNT + 1];   // своя скорость у каждого режима (для случайного)
bool picsOk = false;      // хранилище картинок во флеше
uint32_t picsRev = 0;     // меняется при каждом изменении набора картинок
int   modeSpeed  = 65;
uint32_t lastModeTick = 0;

float fb[ROWS][COLS][3];

struct Flake {
  float x, y, depth, sway, phase;
};
Flake flakes[SNOW_FLAKES];

// ===== данные ОГНЯ (структуры должны стоять до первой функции) =====
#define FIRE_SMALL    2
#define FIRE_SPARKS   12
#define FIRE_BASE_ROW (ROWS - 2)   // ряд ROWS-1 — угли

struct FireFlame {
  float cx, h, w, amp, freq, phase, seed, age, life, power;
};

struct FireSpark {
  float x, y, vx, vy, age, life, wob;
  bool alive;
};

FireFlame fireMain;
FireFlame fireSmall[FIRE_SMALL];
FireSpark fireSparks[FIRE_SPARKS];
float fireT = 0.0f;
float fireHeat[ROWS][COLS];

const float FIRE_TAU = 6.2831853f;

// палитра подобрана под реальные WS2812
const float FIRE_PAL[7][4] = {
  {0.00f,   0,   0,  0},
  {0.20f,   6,   0,  0},
  {0.40f,  40,   1,  0},
  {0.60f, 140,   6,  0},
  {0.78f, 255,  22,  0},
  {0.90f, 255,  55,  0},
  {1.00f, 255,  95,  4},
};

// ===== данные РАДУГИ =====
float rainbowT = 0.0f;

// ===== данные ДОЖДЯ =====
#define RAIN_DROPS 8
const float RAIN_SPLASH_LIFE = 0.12f;

struct RainDrop {
  int c; int len;
  float y, v, b;
};

struct RainSplash {
  int c; float age; bool alive;
};

RainDrop   rainDrops[RAIN_DROPS];
RainSplash rainSplashes[RAIN_DROPS];

// ===== ВРЕМЯ и СЦЕНАРИИ =====
#define MAX_SCHED 8

struct Sched {
  bool en;          // сценарий включён
  uint8_t days;     // дни недели битами: бит 0 = понедельник ... бит 6 = воскресенье
  int16_t on;       // минута включения (0..1439)
  int16_t off;      // минута выключения, -1 = не выключать
  String mode;      // snow / fire / rainbow / rain
  uint8_t speed;
  uint16_t every;   // для «random»: смена режима каждые N минут
};

Sched scheds[MAX_SCHED];
int  schedCount = 0;
int  lastSchedMinute = -1;
bool schedBootChecked = false;
int  tzOffsetHours = 3;
String schedJoined = "";   // сценарии строками — отдаём приложению в state
uint8_t brightPct = 75;     // яркость в процентах

int laneCursor = 0;

String otaUrl = "";
String otaSha256 = "";
String otaStatus = "";

unsigned long rnd32(){ return esp_random(); }
float rndf(float a, float b){ return a + (b - a) * (rnd32() / 4294967295.0f); }

float rowsPerSecond(int speed){
  if(speed < 1) speed = 1;
  if(speed > 100) speed = 100;
  return 1.1f + (speed - 1) * (4.0f - 1.1f) / 99.0f;
}

const uint32_t FRAME_MS = 20;
const uint32_t FIRE_FRAME_MS = 25;   // огонь, радуга и дождь рисуют все 19 лент за кадр

void newFlake(Flake& f, bool spread){
  f.depth = rndf(0.05f, 1.0f);
  f.x     = rndf(-0.5f, COLS - 0.5f);
  f.y     = spread ? rndf(-4.0f, ROWS) : rndf(-3.0f, -0.5f);
  f.sway  = rndf(0.25f, 1.1f);
  f.phase = rndf(0.0f, 6.2831853f);
}

void initSnow(){
  for(int i = 0; i < SNOW_FLAKES; i++) newFlake(flakes[i], true);
}

void tickSnow(){
  float dt = (float)FRAME_MS / 1000.0f;
  float rps = rowsPerSecond(modeSpeed);

  for(int i = 0; i < SNOW_FLAKES; i++){
    Flake& f = flakes[i];
    f.y     += rps * dt * (0.35f + 1.3f * f.depth);
    f.phase += dt * 2.0f * f.sway;
    if(f.y >= (float)ROWS + 1.0f) newFlake(f, false);
  }

  for(int r = 0; r < ROWS; r++){
    for(int c = 0; c < COLS; c++) fb[r][c][0] = 0.0f;
  }

  for(int i = 0; i < SNOW_FLAKES; i++){
    Flake& f = flakes[i];
    float x = f.x + sinf(f.phase) * f.sway;
    int pc = (int)lroundf(x);
    int pr = (int)lroundf(f.y);
    float base = 0.18f + 0.82f * f.depth;
    for(int r = pr - 1; r <= pr + 1; r++){
      for(int c = pc - 1; c <= pc + 1; c++){
        if(r < 0 || r >= ROWS || c < 0 || c >= COLS) continue;
        float d = sqrtf((c - x) * (c - x) + (r - f.y) * (r - f.y));
        float add = (1.0f - d / 1.5f) * base;
        if(add > fb[r][c][0]) fb[r][c][0] = add;
      }
    }
  }

  for(int r = 0; r < ROWS; r++){
    for(int c = 0; c < COLS; c++){
      float w = fb[r][c][0] * 255.0f;
      fb[r][c][0] = w;
      fb[r][c][1] = w;
      fb[r][c][2] = w;
    }
  }
}

// ==================== ОГОНЬ ====================
float fireHash(int32_t x, int32_t y){
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (float)h / 4294967295.0f;
}

float fireNoise(float x, float y){
  float xf = floorf(x), yf = floorf(y);
  int32_t xi = (int32_t)xf, yi = (int32_t)yf;
  float fx = x - xf, fy = y - yf;
  float u = fx * fx * (3.0f - 2.0f * fx);
  float v = fy * fy * (3.0f - 2.0f * fy);
  float a = fireHash(xi, yi),     b = fireHash(xi + 1, yi);
  float c = fireHash(xi, yi + 1), d = fireHash(xi + 1, yi + 1);
  float top = a + (b - a) * u;
  float bottom = c + (d - c) * u;
  return top + (bottom - top) * v;
}

void firePalette(float heat, float& r, float& g, float& b){
  if(heat <= 0.0f){ r = g = b = 0.0f; return; }
  if(heat >= 1.0f){ r = FIRE_PAL[6][1]; g = FIRE_PAL[6][2]; b = FIRE_PAL[6][3]; return; }
  for(int i = 1; i < 7; i++){
    if(heat <= FIRE_PAL[i][0]){
      float k = (heat - FIRE_PAL[i-1][0]) / (FIRE_PAL[i][0] - FIRE_PAL[i-1][0]);
      r = FIRE_PAL[i-1][1] + (FIRE_PAL[i][1] - FIRE_PAL[i-1][1]) * k;
      g = FIRE_PAL[i-1][2] + (FIRE_PAL[i][2] - FIRE_PAL[i-1][2]) * k;
      b = FIRE_PAL[i-1][3] + (FIRE_PAL[i][3] - FIRE_PAL[i-1][3]) * k;
      return;
    }
  }
  r = FIRE_PAL[6][1]; g = FIRE_PAL[6][2]; b = FIRE_PAL[6][3];
}

// ползунок 1..100 -> множитель времени, 65 = 1.0 (огонь, радуга, дождь)
float fireSpeedFactor(int speed){
  if(speed < 1) speed = 1;
  if(speed > 100) speed = 100;
  if(speed <= 65) return 0.3f + 0.7f * (speed - 1) / 64.0f;
  return 1.0f + (speed - 65) / 35.0f;
}

void newSmallFlame(FireFlame& f, bool spread){
  if(rndf(0.0f, 1.0f) < 0.5f) f.cx = rndf(1.0f, 2.8f);
  else                        f.cx = rndf(COLS - 3.5f, COLS - 1.5f);
  f.h     = rndf(5.0f, 10.0f);
  f.w     = rndf(0.9f, 1.5f);
  f.amp   = rndf(0.5f, 1.1f);
  f.freq  = rndf(0.8f, 1.3f);
  f.phase = rndf(0.0f, FIRE_TAU);
  f.seed  = rndf(2.0f, 100.0f);
  f.life  = rndf(3.0f, 7.0f);
  f.age   = spread ? rndf(0.0f, f.life) : 0.0f;
  f.power = rndf(0.55f, 0.72f);
}

float flameGrowth(const FireFlame& f){
  if(f.life < 0.0f) return 1.0f;
  float g = f.age / 0.8f;
  float out = (f.life - f.age) / 1.0f;
  if(out < g) g = out;
  if(g > 1.0f) g = 1.0f;
  if(g < 0.0f) g = 0.0f;
  return g;
}

float flameHeight(const FireFlame& f, float g){
  return f.h * g * (0.85f + 0.15f * fireNoise(fireT * 2.0f, f.seed * 7.1f));
}

float flameCenter(const FireFlame& f, int h, float y){
  float w2 = FIRE_TAU * f.freq * fireT + f.phase;
  float sway = f.amp * (powf(y, 1.5f) * sinf(w2) + 0.45f * y * y * sinf(w2 * 2.3f + 1.3f));
  sway += (fireNoise(fireT * 1.7f, h * 0.35f + f.seed) - 0.5f) * 1.2f * y;
  return f.cx + sway;
}

void initFire(){
  fireT = 0.0f;
  fireMain.cx = (COLS - 1) / 2.0f;
  fireMain.h = ROWS + 0.5f;
  fireMain.w = 2.5f;
  fireMain.amp = 1.0f;
  fireMain.freq = 0.55f;
  fireMain.phase = rndf(0.0f, FIRE_TAU);
  fireMain.seed = 1.0f;
  fireMain.age = 0.0f;
  fireMain.life = -1.0f;          // главное пламя живёт всегда
  fireMain.power = 1.0f;
  for(int i = 0; i < FIRE_SMALL; i++) newSmallFlame(fireSmall[i], true);
  for(int i = 0; i < FIRE_SPARKS; i++) fireSparks[i].alive = false;
}

void addFlameHeat(const FireFlame& f){
  float g = flameGrowth(f);
  if(g <= 0.01f) return;
  float flick  = 0.88f + 0.24f * fireNoise(fireT * 3.5f, f.seed * 13.7f);
  float height = flameHeight(f, g);
  float width  = f.w * (0.55f + 0.45f * g);
  for(int h = 0; h <= FIRE_BASE_ROW; h++){
    float y = h / height;
    if(y >= 1.12f) break;
    float center = flameCenter(f, h, y);
    float rest = 1.0f - y; if(rest < 0.0f) rest = 0.0f;
    float half = width * powf(rest, 0.6f) * (1.0f + 0.45f * y * (1.0f - y)) + 0.6f;
    float tip = (1.12f - y) / 0.32f; if(tip > 1.0f) tip = 1.0f; if(tip < 0.0f) tip = 0.0f;
    float vert = (0.5f + 0.42f * (y < 0.7f ? y : 0.7f) / 0.7f) * tip;
    int r = FIRE_BASE_ROW - h;
    int c0 = (int)(center - half) - 1; if(c0 < 0) c0 = 0;
    int c1 = (int)(center + half) + 1; if(c1 > COLS - 1) c1 = COLS - 1;
    for(int c = c0; c <= c1; c++){
      float d = fabsf(c - center) / half;
      if(d >= 1.0f) continue;
      float val = powf(1.0f - d * d, 1.4f) * vert * flick * f.power;
      float old = fireHeat[r][c];
      fireHeat[r][c] = old + val - old * val;
    }
  }
}

void tickFire(){
  float dt = (float)FIRE_FRAME_MS / 1000.0f * fireSpeedFactor(modeSpeed);
  fireT += dt;

  for(int i = 0; i < FIRE_SMALL; i++){
    fireSmall[i].age += dt;
    if(fireSmall[i].age >= fireSmall[i].life) newSmallFlame(fireSmall[i], false);
  }

  for(int r = 0; r < ROWS; r++)
    for(int c = 0; c < COLS; c++) fireHeat[r][c] = 0.0f;

  addFlameHeat(fireMain);
  for(int i = 0; i < FIRE_SMALL; i++) addFlameHeat(fireSmall[i]);

  for(int r = 0; r < ROWS; r++){
    float yRel = (float)(FIRE_BASE_ROW - r) / FIRE_BASE_ROW;
    for(int c = 0; c < COLS; c++){
      float v = (r <= FIRE_BASE_ROW) ? fireHeat[r][c] : 0.0f;
      if(v > 0.0f){
        float n = fireNoise(c * 0.55f, r * 0.45f + fireT * 3.2f);
        v *= 1.0f + (n - 0.5f) * (0.7f + 0.9f * yRel);
        if(v > 1.0f) v = 1.0f;
      }
      firePalette(v, fb[r][c][0], fb[r][c][1], fb[r][c][2]);
    }
  }

  for(int c = 0; c < COLS; c++){
    float n1 = fireNoise(c * 0.8f, fireT * 0.7f + 40.0f);
    float n2 = fireNoise(c * 2.1f + 11.0f, fireT * 1.9f);
    float glow = 0.35f + 0.45f * n1 + 0.2f * n2;
    float near = 1.0f - fabsf(c - fireMain.cx) / 11.0f; if(near < 0.0f) near = 0.0f;
    glow *= 0.65f + 0.45f * near;
    if(glow > 1.0f) glow = 1.0f;
    glow = powf(glow, 1.4f);
    float cr = 8.0f + 50.0f * glow, cg = 1.0f + 7.0f * glow, cb = 0.0f;
    fb[ROWS-1][c][0] = cr; fb[ROWS-1][c][1] = cg; fb[ROWS-1][c][2] = cb;
    float *p = fb[FIRE_BASE_ROW][c];
    if(cr * 0.45f > p[0]) p[0] = cr * 0.45f;
    if(cg * 0.45f > p[1]) p[1] = cg * 0.45f;
    if(cb * 0.45f > p[2]) p[2] = cb * 0.45f;
  }

  if(rndf(0.0f, 1.0f) < 7.5f * dt){
    int slot = -1;
    for(int i = 0; i < FIRE_SPARKS; i++) if(!fireSparks[i].alive){ slot = i; break; }
    if(slot >= 0){
      const FireFlame *f = &fireMain;
      if(rndf(0.0f, 1.0f) >= 0.65f){
        int alive[FIRE_SMALL]; int n = 0;
        for(int i = 0; i < FIRE_SMALL; i++) if(flameGrowth(fireSmall[i]) > 0.5f) alive[n++] = i;
        if(n > 0) f = &fireSmall[alive[rnd32() % n]];
      }
      float height = flameHeight(*f, flameGrowth(*f));
      float hy = rndf(0.45f, 0.9f) * height;
      float yRel = hy / height; if(yRel > 1.0f) yRel = 1.0f;
      FireSpark& s = fireSparks[slot];
      s.x = flameCenter(*f, (int)hy, yRel) + rndf(-1.0f, 1.0f);
      s.y = FIRE_BASE_ROW - hy;
      s.vx = rndf(-1.2f, 1.2f);
      s.vy = rndf(5.0f, 9.0f);
      s.age = 0.0f;
      s.life = rndf(0.5f, 1.2f);
      s.wob = rndf(0.0f, FIRE_TAU);
      s.alive = true;
    }
  }

  for(int i = 0; i < FIRE_SPARKS; i++){
    FireSpark& s = fireSparks[i];
    if(!s.alive) continue;
    s.age += dt;
    s.x += (s.vx + sinf(s.age * 7.0f + s.wob)) * dt;
    s.y -= s.vy * dt;
    s.vy *= 1.0f - 0.6f * dt;
    if(s.age >= s.life || s.y < -1.0f){ s.alive = false; continue; }
    int rr = (int)lroundf(s.y), cc = (int)lroundf(s.x);
    if(rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) continue;
    float a = 1.0f - s.age / s.life;
    float m = 0.35f + 0.65f * a;
    float sr = (120.0f + 135.0f * a) * m, sg = (12.0f + 100.0f * a) * m, sb = 10.0f * a * m;
    float *p = fb[rr][cc];
    if(sr > p[0]) p[0] = sr;
    if(sg > p[1]) p[1] = sg;
    if(sb > p[2]) p[2] = sb;
  }
}
// ================== конец ОГНЯ ==================

// ================ РАДУЖНАЯ ВОЛНА ================
void initRainbow(){ rainbowT = 0.0f; }

void tickRainbow(){
  rainbowT += (float)FIRE_FRAME_MS / 1000.0f * fireSpeedFactor(modeSpeed);
  for(int r = 0; r < ROWS; r++){
    for(int c = 0; c < COLS; c++){
      float h = c * 0.03f + r * 0.025f - rainbowT * 0.25f + 0.08f * sinf(rainbowT + r * 0.3f);
      h -= floorf(h);
      const float v = 230.0f;
      int i = ((int)(h * 6.0f)) % 6;
      float f = h * 6.0f - (int)(h * 6.0f);
      float q = v * (1.0f - f), w = v * f;
      float rr, gg, bb;
      switch(i){
        case 0:  rr = v; gg = w; bb = 0; break;
        case 1:  rr = q; gg = v; bb = 0; break;
        case 2:  rr = 0; gg = v; bb = w; break;
        case 3:  rr = 0; gg = q; bb = v; break;
        case 4:  rr = w; gg = 0; bb = v; break;
        default: rr = v; gg = 0; bb = q; break;
      }
      fb[r][c][0] = rr; fb[r][c][1] = gg; fb[r][c][2] = bb;
    }
  }
}
// ============= конец РАДУЖНОЙ ВОЛНЫ =============

// ===================== ДОЖДЬ =====================
// дождь в стиле Minecraft: капли падают вниз, брызги, земля
void newRainDrop(RainDrop& d, bool spread){
  d.c   = rnd32() % COLS;
  d.len = 2 + rnd32() % 3;
  d.y   = spread ? rndf(-(float)ROWS, (float)ROWS) : rndf(-6.0f, -1.0f);
  d.v   = rndf(16.0f, 22.0f);
  d.b   = rndf(0.55f, 1.0f);
}

void initRain(){
  for(int i = 0; i < RAIN_DROPS; i++){
    newRainDrop(rainDrops[i], true);
    rainSplashes[i].alive = false;
  }
}

void tickRain(){
  float dt = (float)FIRE_FRAME_MS / 1000.0f * fireSpeedFactor(modeSpeed);
  const int ground = ROWS - 1;

  // ночное небо
  for(int r = 0; r < ROWS; r++)
    for(int c = 0; c < COLS; c++){ fb[r][c][0] = 0; fb[r][c][1] = 1; fb[r][c][2] = 4; }

  // земля «блоками»
  for(int c = 0; c < COLS; c++){
    bool dark = (c % 3) != 0;
    fb[ground][c][0] = dark ? 14 : 18;
    fb[ground][c][1] = dark ? 5 : 7;
    fb[ground][c][2] = 0;
  }

  // капли
  for(int i = 0; i < RAIN_DROPS; i++){
    RainDrop& d = rainDrops[i];
    d.y += d.v * dt;
    int head = (int)floorf(d.y);
    if(head >= ground){
      for(int j = 0; j < RAIN_DROPS; j++){
        if(!rainSplashes[j].alive){
          rainSplashes[j].c = d.c; rainSplashes[j].age = 0.0f; rainSplashes[j].alive = true;
          break;
        }
      }
      newRainDrop(d, false);
      continue;
    }
    for(int k = 0; k < d.len; k++){
      int r = head - k;
      if(r < 0 || r >= ground) continue;
      float a = d.b * (1.0f - k / (d.len + 0.5f));
      fb[r][d.c][0] = 12.0f * a;
      fb[r][d.c][1] = 30.0f * a + 1.0f;
      fb[r][d.c][2] = 150.0f * a + 4.0f;
    }
  }

  // брызги
  for(int j = 0; j < RAIN_DROPS; j++){
    RainSplash& s = rainSplashes[j];
    if(!s.alive) continue;
    s.age += dt;
    if(s.age > RAIN_SPLASH_LIFE){ s.alive = false; continue; }
    float a = 1.0f - s.age / RAIN_SPLASH_LIFE;
    for(int dc = -1; dc <= 1; dc += 2){
      int cc = s.c + dc;
      if(cc < 0 || cc >= COLS) continue;
      fb[ground-1][cc][0] = 16.0f * a;
      fb[ground-1][cc][1] = 40.0f * a + 1.0f;
      fb[ground-1][cc][2] = 170.0f * a + 4.0f;
    }
    fb[ground][s.c][0] = 20; fb[ground][s.c][1] = 40; fb[ground][s.c][2] = 150;
  }
}
// ================== конец ДОЖДЯ ==================

// ================ НОВЫЕ РЕЖИМЫ (v8) ================
void hsvf(float h, float s, float v, float *o){
  h -= floorf(h);
  int i = (int)(h * 6.0f); float f = h * 6.0f - i;
  float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  switch(i % 6){
    case 0: o[0]=v; o[1]=t; o[2]=p; break;
    case 1: o[0]=q; o[1]=v; o[2]=p; break;
    case 2: o[0]=p; o[1]=v; o[2]=t; break;
    case 3: o[0]=p; o[1]=q; o[2]=v; break;
    case 4: o[0]=t; o[1]=p; o[2]=v; break;
    default: o[0]=v; o[1]=p; o[2]=q; break;
  }
}

void fbAdd(int r, int c, float R, float G, float B, float a){
  if(r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
  float *p = fb[r][c];
  p[0] += R * a; if(p[0] > 255) p[0] = 255;
  p[1] += G * a; if(p[1] > 255) p[1] = 255;
  p[2] += B * a; if(p[2] > 255) p[2] = 255;
}

void fbSplat(float y, float x, float R, float G, float B, float a, float rad){
  for(int r = (int)y - 2; r <= (int)y + 2; r++)
    for(int c = (int)x - 2; c <= (int)x + 2; c++){
      float d = sqrtf((r - y) * (r - y) + (c - x) * (c - x));
      if(d < rad + 0.5f) fbAdd(r, c, R, G, B, a * (1.0f - d / (rad + 0.5f)));
    }
}

void fbFill(float R, float G, float B){
  for(int r = 0; r < ROWS; r++)
    for(int c = 0; c < COLS; c++){ fb[r][c][0] = R; fb[r][c][1] = G; fb[r][c][2] = B; }
}

// ---------- ФЕЙЕРВЕРК ----------
#define FW_ROCKETS 4
#define FW_PARTS   140
struct FwRocket { float x, y, vy, top, hue; bool alive; };
struct FwPart   { float x, y, vx, vy, age, life, hue; bool alive; };
FwRocket fwRockets[FW_ROCKETS];
FwPart   fwParts[FW_PARTS];

void launchRocket(){
  for(int i = 0; i < FW_ROCKETS; i++){
    FwRocket& k = fwRockets[i];
    if(k.alive) continue;
    k.x = rndf(3.0f, COLS - 4.0f); k.y = ROWS - 1; k.vy = -rndf(13.0f, 17.0f);
    k.top = rndf(3.0f, 8.0f); k.hue = rndf(0.0f, 1.0f); k.alive = true;
    return;
  }
}

void initFireworks(){
  fbFill(0, 0, 0);
  for(int i = 0; i < FW_ROCKETS; i++) fwRockets[i].alive = false;
  for(int i = 0; i < FW_PARTS; i++) fwParts[i].alive = false;
  launchRocket();
}

void tickFireworks(){
  float sf = fireSpeedFactor(modeSpeed);
  float dt = (float)FIRE_FRAME_MS / 1000.0f * sf;
  bool anyR = false, anyP = false;
  for(int i = 0; i < FW_ROCKETS; i++) if(fwRockets[i].alive) anyR = true;
  for(int i = 0; i < FW_PARTS; i++) if(fwParts[i].alive){ anyP = true; break; }
  if(rndf(0.0f, 1.0f) < 1.125f * dt || (!anyR && !anyP)) launchRocket();

  // коэффициенты заданы на кадр 40 мс — пересчёт под реальный шаг
  float fade = powf(0.72f, dt / 0.04f), damp = powf(0.93f, dt / 0.04f);
  for(int r = 0; r < ROWS; r++)
    for(int c = 0; c < COLS; c++){ fb[r][c][0] *= fade; fb[r][c][1] *= fade; fb[r][c][2] *= fade; }

  for(int i = 0; i < FW_ROCKETS; i++){
    FwRocket& k = fwRockets[i];
    if(!k.alive) continue;
    k.y += k.vy * dt; k.vy += 6.0f * dt;
    fbSplat(k.y, k.x, 255, 190, 120, 0.9f, 0.6f);
    if(k.y <= k.top || k.vy >= -2.0f){
      k.alive = false;
      int n = 26 + rnd32() % 15;
      float add[3] = {0.0f, 0.5f, 0.15f};
      float h2 = k.hue + add[rnd32() % 3];
      int made = 0;
      for(int j = 0; j < FW_PARTS && made < n; j++){
        FwPart& p = fwParts[j];
        if(p.alive) continue;
        float a = FIRE_TAU * made / n + rndf(-0.1f, 0.1f), sp = rndf(5.0f, 9.0f);
        p.x = k.x; p.y = k.y; p.vx = cosf(a) * sp; p.vy = sinf(a) * sp;
        p.age = 0; p.life = rndf(1.0f, 1.6f); p.hue = (made % 3) ? k.hue : h2; p.alive = true;
        made++;
      }
      fbSplat(k.y, k.x, 255, 255, 255, 1.0f, 1.8f);
    }
  }

  for(int j = 0; j < FW_PARTS; j++){
    FwPart& p = fwParts[j];
    if(!p.alive) continue;
    p.age += dt; p.x += p.vx * dt; p.y += p.vy * dt;
    p.vx *= damp; p.vy = p.vy * damp + 5.0f * dt;
    if(p.age > p.life){ p.alive = false; continue; }
    float k = 1.0f - p.age / p.life;
    float s = 0.85f * fminf(1.0f, p.age * 3.0f);
    float col[3]; hsvf(p.hue, s, 1.0f, col);
    float tw = k > 0.35f ? 1.0f : 0.4f + 0.6f * rndf(0.0f, 1.0f);
    fbAdd((int)roundf(p.y), (int)roundf(p.x), 255 * col[0], 255 * col[1], 255 * col[2], k * tw);
  }
}

// ---------- ЛАВА-ЛАМПА ----------
#define LAVA_BLOBS 5
struct LavaBlob { float x, ph, sp, r; };
LavaBlob lavaBlobs[LAVA_BLOBS];
float lavaT = 0.0f;

void initLava(){
  lavaT = 0.0f;
  for(int k = 0; k < LAVA_BLOBS; k++){
    lavaBlobs[k].x = 3.0f + 3.2f * k + rndf(-0.5f, 0.5f);
    lavaBlobs[k].ph = rndf(0.0f, 6.28f);
    lavaBlobs[k].sp = rndf(0.25f, 0.45f);
    lavaBlobs[k].r = rndf(1.7f, 2.3f);
  }
}

void tickLava(){
  lavaT += (float)FIRE_FRAME_MS / 1000.0f * fireSpeedFactor(modeSpeed);
  float px[LAVA_BLOBS], py[LAVA_BLOBS], pr[LAVA_BLOBS];
  for(int k = 0; k < LAVA_BLOBS; k++){
    px[k] = lavaBlobs[k].x + 1.5f * sinf(lavaT * 0.6f + lavaBlobs[k].ph);
    py[k] = ROWS / 2.0f + (ROWS / 2.0f - 2.0f) * sinf(lavaT * lavaBlobs[k].sp * 2.0f + lavaBlobs[k].ph);
    pr[k] = lavaBlobs[k].r * lavaBlobs[k].r;
  }
  float hs = 0.03f * sinf(lavaT);
  for(int r = 0; r < ROWS; r++)
    for(int c = 0; c < COLS; c++){
      float v = 0;
      for(int k = 0; k < LAVA_BLOBS; k++){
        float dx = c - px[k], dy = r - py[k];
        v += pr[k] / (dx * dx + dy * dy + 0.5f);
      }
      float *p = fb[r][c];
      if(v > 1.0f){
        float k = fminf(1.0f, (v - 1.0f) / 1.5f);
        float col[3]; hsvf(0.95f + 0.1f * r / ROWS + hs, 1.0f - 0.35f * k, 0.55f + 0.45f * k, col);
        p[0] = 255 * col[0]; p[1] = 255 * col[1] * 0.8f; p[2] = 255 * col[2];
      } else if(v > 0.7f){
        float a = (v - 0.7f) / 0.3f;
        p[0] = 70 * a; p[1] = 8 * a; p[2] = 40 * a;
        if(p[0] < 6) p[0] = 6; if(p[2] < 10) p[2] = 10;
      } else { p[0] = 6; p[1] = 0; p[2] = 10; }
    }
}

// ---------- ЗВЁЗДНОЕ НЕБО ----------
#define SKY_STARS 18
#define SKY_SHOOT 3
struct SkyStar { uint8_t r, c; float ph, f, b; bool warm; };
struct SkyShoot { float x, y, age; bool alive; };
SkyStar skyStars[SKY_STARS];
SkyShoot skyShoot[SKY_SHOOT];
float skyT = 0.0f;

void initStars(){
  skyT = 0.0f;
  for(int i = 0; i < SKY_STARS; i++){
    SkyStar& s = skyStars[i];
    s.r = rnd32() % (ROWS - 3); s.c = rnd32() % COLS;
    s.ph = rndf(0.0f, 6.28f); s.f = rndf(0.6f, 2.2f); s.b = rndf(0.25f, 0.9f);
    s.warm = rndf(0.0f, 1.0f) < 0.3f;
  }
  for(int i = 0; i < SKY_SHOOT; i++) skyShoot[i].alive = false;
}

void tickStars(){
  float sf = fireSpeedFactor(modeSpeed);
  float dt = (float)FIRE_FRAME_MS / 1000.0f * sf;
  skyT += dt;
  for(int r = 0; r < ROWS; r++){
    float g = (float)r / ROWS;
    for(int c = 0; c < COLS; c++){ fb[r][c][0] = 1 + 2 * g; fb[r][c][1] = 1 + g; fb[r][c][2] = 4 + 5 * g; }
  }
  for(int c = 0; c < COLS; c++){
    float h = 2.0f + 1.2f * sinf(c * 0.6f) + 0.8f * sinf(c * 1.7f);
    for(int r = ROWS - (int)h; r < ROWS; r++){ fb[r][c][0] = 0; fb[r][c][1] = 0; fb[r][c][2] = 1; }
  }
  for(int i = 0; i < SKY_STARS; i++){
    SkyStar& s = skyStars[i];
    float w = 0.55f + 0.45f * sinf(skyT * s.f * 3.0f + s.ph);
    float v = s.b * w * w;
    if(s.warm) fbAdd(s.r, s.c, 255, 210, 150, v);
    else       fbAdd(s.r, s.c, 200, 220, 255, v);
  }
  if(rndf(0.0f, 1.0f) < 0.625f * dt){
    for(int i = 0; i < SKY_SHOOT; i++) if(!skyShoot[i].alive){
      skyShoot[i].x = rndf(4.0f, COLS); skyShoot[i].y = rndf(0.0f, 6.0f);
      skyShoot[i].age = 0; skyShoot[i].alive = true; break;
    }
  }
  for(int i = 0; i < SKY_SHOOT; i++){
    SkyShoot& s = skyShoot[i];
    if(!s.alive) continue;
    s.age += dt; s.x -= 22.0f * dt; s.y += 11.0f * dt;
    if(s.age > 1.2f || s.x < -6.0f){ s.alive = false; continue; }
    for(int k = 0; k < 6; k++)
      fbAdd((int)roundf(s.y - k * 0.5f), (int)roundf(s.x + k), 255, 255, 255, (1.0f - k / 6.0f) * (1.0f - s.age / 1.2f));
  }
}

// ---------- ГАЛАКТИКА ----------
float galT = 0.0f;
void initGalaxy(){ galT = 0.0f; }

void tickGalaxy(){
  galT += (float)FIRE_FRAME_MS / 1000.0f * fireSpeedFactor(modeSpeed);
  const float cx = (COLS - 1) / 2.0f, cy = (ROWS - 1) / 2.0f;
  float hs = 0.05f * sinf(galT);
  for(int r = 0; r < ROWS; r++)
    for(int c = 0; c < COLS; c++){
      float dx = c - cx, dy = r - cy;
      float d = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
      float v = cosf(2.0f * (a - galT * 0.9f) - d * 0.55f);
      v = v > 0 ? v * v * v * expf(-d / 9.0f) : 0.0f;
      float core = expf(-d * d / 5.0f);
      float col[3]; hsvf(0.62f + 0.25f * d / 12.0f + hs, 0.8f, 1.0f, col);
      float *p = fb[r][c];
      p[0] = fminf(255, 255 * (col[0] * v + core));
      p[1] = fminf(255, 255 * (col[1] * v + core * 0.85f));
      p[2] = fminf(255, 255 * (col[2] * v + core * 0.6f));
      if((r * 31 + c * 17) % 23 == 0) fbAdd(r, c, 120, 120, 140, 0.5f + 0.5f * sinf(galT * 3.0f + r + c));
    }
}
// ============= конец НОВЫХ РЕЖИМОВ =============


// ---------- ЧЕТЫРЕ УТВЕРЖДЁННЫХ РЕЖИМА ----------
float fieldT=0.0f;
float fieldRadius[ROWS][COLS],fieldAngle[ROWS][COLS];
void initField(){
 fieldT=0.0f;
 for(int r=0;r<ROWS;r++)for(int c=0;c<COLS;c++){
  float x=(c-(COLS-1)/2.0f)/((COLS-1)/2.0f),y=(r-(ROWS-1)/2.0f)/((ROWS-1)/2.0f);
  fieldRadius[r][c]=sqrtf(x*x+y*y);fieldAngle[r][c]=atan2f(y,x);
 }
}
void fieldPixel(Mode kind,int c,int r,float x,float y,float radius,float angle,float t,float *out){
 if(kind==MODE_TUNNEL){
  float a=.1f*sinf(t*.65f),u=x*cosf(a)-y*sinf(a),v=x*sinf(a)+y*cosf(a),q=fmaxf(fabsf(u),fabsf(v));
  float p=q*2.6f-t*.66f;p-=floorf(p);
  float z=(p-.5f)/.15f;
  hsvf(.54f+.29f*q+.055f*sinf(t*.4f),.84f,.045f+.955f*expf(-z*z),out);
  for(int i=0;i<3;i++)out[i]*=255;
 }else if(kind==MODE_AURORA){
  out[0]=out[1]=out[2]=0;
  for(int band=0;band<3;band++){
   float center=5+band*4+2.8f*sinf(c*.27f+t*.7f+band*1.7f)+1.2f*sinf(c*.61f-t*.5f+band),dy=r-center;
   float w=sinf(c*.35f+t*.55f+band);
   float v=expf(-dy*dy/(dy>0?22.0f:3.4f))*(.52f+.48f*w*w),col[3];
   hsvf(.34f+band*.075f+.045f*sinf(c*.2f+t*.3f),.85f,v*.74f,col);
   for(int i=0;i<3;i++)out[i]+=floorf(255*col[i]);
  }for(int i=0;i<3;i++)out[i]=fminf(255,out[i]);
 }else if(kind==MODE_PLASMA){
  float z=(sinf(c*.44f+t*1.6f)+sinf(r*.36f-t*1.3f)+sinf((c+r)*.27f+t*.9f)+sinf(radius*5-t*1.4f))/4;
  hsvf(.72f+z*.28f+t*.06f,.98f,.16f+.76f*(.5f+.5f*sinf(z*6+t)),out);
  for(int i=0;i<3;i++)out[i]*=255;
 }else{
  float facet=cosf(angle*6+t*.25f),wave=radius*9+facet*2.6f-t*1.6f,w=.5f+.5f*sinf(wave);
  hsvf(.03f+radius*.22f+facet*.09f+.07f*sinf(t*.3f),.92f,.025f+.975f*w*w,out);
  for(int i=0;i<3;i++)out[i]*=255;
 }
}
void tickField(){
 fieldT+=(float)FIRE_FRAME_MS/1000.0f*fireSpeedFactor(modeSpeed);
 for(int r=0;r<ROWS;r++)for(int c=0;c<COLS;c++){
  float x=(c-(COLS-1)/2.0f)/((COLS-1)/2.0f),y=(r-(ROWS-1)/2.0f)/((ROWS-1)/2.0f);
  fieldPixel(currentMode,c,r,x,y,fieldRadius[r][c],fieldAngle[r][c],fieldT,fb[r][c]);
 }
}

void initCurrentMode(){
  if(currentMode == MODE_SNOW) initSnow();
  else if(currentMode == MODE_FIRE) initFire();
  else if(currentMode == MODE_RAINBOW) initRainbow();
  else if(currentMode == MODE_RAIN) initRain();
  else if(currentMode == MODE_FIREWORKS) initFireworks();
  else if(currentMode == MODE_LAVA) initLava();
  else if(currentMode == MODE_STARS) initStars();
  else if(currentMode == MODE_GALAXY) initGalaxy();
  else if(currentMode >= MODE_TUNNEL && currentMode <= MODE_KALEIDOSCOPE) initField();
}

void tickCurrentMode(){
  if(currentMode == MODE_SNOW) tickSnow();
  else if(currentMode == MODE_FIRE) tickFire();
  else if(currentMode == MODE_RAINBOW) tickRainbow();
  else if(currentMode == MODE_RAIN) tickRain();
  else if(currentMode == MODE_FIREWORKS) tickFireworks();
  else if(currentMode == MODE_LAVA) tickLava();
  else if(currentMode == MODE_STARS) tickStars();
  else if(currentMode == MODE_GALAXY) tickGalaxy();
  else if(currentMode >= MODE_TUNNEL && currentMode <= MODE_KALEIDOSCOPE) tickField();
}

void markDirty(int n){ if(n >= 0 && n < COLS) strandDirty[n] = true; }

// немедленный показ всех грязных лент, без троттла
void updateLedsNow(){
  for(int i = 0; i < COLS; i++){
    if(strandDirty[i]){
      strips[i]->show();
      strandDirty[i] = false;
    }
  }
  lastShowAt = millis();
}

// обычный показ с троттлом
void updateLeds(){
  bool any = false;
  for(int i = 0; i < COLS; i++){
    if(strandDirty[i]){ any = true; break; }
  }
  if(!any) return;
  uint32_t now = millis();
  if(now - lastShowAt < SHOW_INTERVAL_MS) return;
  updateLedsNow();
}

void renderModeFrame(){
  // снег — по 7 лент за кадр как раньше, остальные режимы — весь кадр сразу
  int lanes = (currentMode == MODE_SNOW) ? LANES_PER_FRAME : COLS;
  for(int n = 0; n < lanes; n++){
    int c = laneCursor;
    if(c >= COLS) c = COLS - 1;
    for(int r = 0; r < ROWS; r++){
      float *px = fb[r][c];
      strips[c]->setPixelColor(r, (uint8_t)(px[0] + 0.5f), (uint8_t)(px[1] + 0.5f), (uint8_t)(px[2] + 0.5f));
    }
    markDirty(c);
    laneCursor++;
    if(laneCursor >= COLS) laneCursor = 0;
  }
}

void clearFrame(){
  for(int c = 0; c < COLS; c++){
    strips[c]->clear();
    markDirty(c);
  }
}

void stopModeNow(){
  currentMode = MODE_OFF;
  randEvery = 0;
  clearFrame();
  updateLedsNow();
  Serial.println("[Режим] выключен + очистка");
}

void runMode(Mode want, int speed);
void startRandom(int every);

void startMode(const String& name, int speed){
  if(name == "random"){ startRandom(speed); return; }
  if(name == "off" || name.length() == 0){
    // режим и так выключен — рисунок не трогаем (раньше гирлянда гасла целиком)
    if(currentMode != MODE_OFF) stopModeNow();
    return;
  }

  Mode want = MODE_OFF;
  if(name == "snow") want = MODE_SNOW;
  else if(name == "fire") want = MODE_FIRE;
  else if(name == "rainbow") want = MODE_RAINBOW;
  else if(name == "rain") want = MODE_RAIN;
  else if(name == "fireworks") want = MODE_FIREWORKS;
  else if(name == "lava") want = MODE_LAVA;
  else if(name == "stars") want = MODE_STARS;
  else if(name == "galaxy") want = MODE_GALAXY;

  else if(name == "tunnel") want = MODE_TUNNEL;

  else if(name == "aurora") want = MODE_AURORA;

  else if(name == "plasma") want = MODE_PLASMA;

  else if(name == "kaleidoscope") want = MODE_KALEIDOSCOPE;

  if(want == MODE_OFF){
    stopModeNow();
    return;
  }
  randEvery = 0;   // выбрали конкретный режим — случайный выключается
  modeSpeeds[want] = speed;
  runMode(want, speed);
}

const char* modeNameOf(Mode m);

void runMode(Mode want, int speed){
  // режим уже этот идёт — только скорость, без перезапуска
  if(want == currentMode){
    modeSpeed = speed;
    lastModeTick = millis();
    Serial.printf("[Режим] %s уже идёт, скорость %d\n", modeNameOf(want), speed);
    return;
  }

  currentMode = want;
  initCurrentMode();

  modeSpeed = speed;
  laneCursor = 0;

  // гасим и сразу рисуем первый кадр — запуск без задержки в 1-2 кадра
  clearFrame();
  updateLedsNow();

  lastModeTick = millis();
  tickCurrentMode();
  renderModeFrame();
  updateLedsNow();

  Serial.printf("[Режим] %s speed %d\n", modeNameOf(want), speed);
}

void randomNext(){
  int m;
  do { m = 1 + esp_random() % MODE_COUNT; } while(m == (int)currentMode);
  runMode((Mode)m, modeSpeeds[m]);
  randNextAt = millis() + (uint32_t)randEvery * 60000UL;
}

void startRandom(int every){
  if(every < 1) every = 1;
  if(every > 1440) every = 1440;
  randEvery = (uint16_t)every;
  randomNext();
  Serial.printf("[Режим] случайный, смена каждые %d мин\n", every);
}

// скорости режимов от приложения: "65,40,..." в порядке snow..galaxy
void setModeSpeeds(const String& list){
  int idx = 1, st = 0;
  bool changed = false;
  for(int i = 0; i <= (int)list.length() && idx <= MODE_COUNT; i++){
    if(i == (int)list.length() || list[i] == ','){
      int v = list.substring(st, i).toInt();
      if(v >= 1 && v <= 100 && modeSpeeds[idx] != v){ modeSpeeds[idx] = v; changed = true; }
      idx++; st = i + 1;
    }
  }
  if(changed){
    prefs.begin("sched", false);
    prefs.putBytes("speeds", modeSpeeds, sizeof(modeSpeeds));
    prefs.end();
  }
}

void loadModeSpeeds(){
  for(int i = 0; i <= MODE_COUNT; i++) modeSpeeds[i] = 65;
  prefs.begin("sched", true);
  size_t savedLength=prefs.getBytesLength("speeds");
  // Старые 8 скоростей сохраняются: новые четыре получают значение по умолчанию.
  if(savedLength>0 && savedLength<=sizeof(modeSpeeds)) prefs.getBytes("speeds",modeSpeeds,savedLength);
  for(int i=1;i<=MODE_COUNT;i++) if(modeSpeeds[i]<1||modeSpeeds[i]>100) modeSpeeds[i]=65;
  prefs.end();
}

void updateRandom(){
  if(randEvery && currentMode != MODE_OFF && (int32_t)(millis() - randNextAt) >= 0) randomNext();
}

void updateMode(){
  if(currentMode == MODE_OFF) return;
  uint32_t now = millis();
  uint32_t frame = (currentMode == MODE_SNOW) ? FRAME_MS : FIRE_FRAME_MS;
  if(now - lastModeTick < frame) return;
  lastModeTick = now;
  tickCurrentMode();
  renderModeFrame();
}

// ==================== ВРЕМЯ ====================
bool timeValid(){
  return time(nullptr) > 1700000000;   // время уже установлено (после 2023 года)
}

void setClock(long epoch){
  if(epoch < 1700000000) return;
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.printf("[Время] установлено из программы: %ld\n", epoch);
}

void saveTz(int hours){
  if(hours < -12) hours = -12;
  if(hours > 14) hours = 14;
  if(hours == tzOffsetHours) return;   // не трём флеш лишний раз
  tzOffsetHours = hours;
  prefs.begin("sched", false);
  prefs.putInt("tz", hours);
  prefs.end();
  Serial.printf("[Время] часовой пояс UTC%+d\n", hours);
}

// ================== СЦЕНАРИИ ==================
// строка сценария: "вкл,дни,начало,конец,режим,скорость", например "1,127,1080,1380,fire,65"
bool parseSchedLine(const String& line, Sched& s){
  String parts[6];
  int n = 0, st = 0;
  int len = line.length();
  for(int i = 0; i <= len && n < 6; i++){
    if(i == len || line[i] == ','){
      parts[n++] = line.substring(st, i);
      st = i + 1;
    }
  }
  if(n < 6) return false;
  s.en   = parts[0].toInt() != 0;
  s.days = (uint8_t)(parts[1].toInt() & 127);
  s.on   = (int16_t)parts[2].toInt();
  s.off  = (int16_t)parts[3].toInt();
  s.mode = parts[4];
  s.mode.trim();
  int sp = parts[5].toInt();
  if(sp < 1) sp = 1;
  if(sp > 100) sp = 100;
  s.speed = (uint8_t)sp;
  int ev = parts[5].toInt();
  if(ev < 1) ev = 1;
  if(ev > 1440) ev = 1440;
  s.every = (uint16_t)ev;
  if(s.on < 0 || s.on >= 1440) return false;
  if(s.off >= 1440) s.off = -1;
  return true;
}

void applyScheduleLines(const String *lines, int n, bool save){
  schedCount = 0;
  String joined = "";
  for(int i = 0; i < n && schedCount < MAX_SCHED; i++){
    if(parseSchedLine(lines[i], scheds[schedCount])){
      if(joined.length()) joined += ";";
      joined += lines[i];
      schedCount++;
    }
  }
  schedJoined = joined;
  if(save){
    prefs.begin("sched", false);
    prefs.putString("list", joined);
    prefs.end();
  }
  Serial.printf("[Сценарии] загружено: %d\n", schedCount);
}

void loadSchedules(){
  prefs.begin("sched", false);
  tzOffsetHours = prefs.getInt("tz", 3);
  String joined = prefs.getString("list", "");
  prefs.end();
  String lines[MAX_SCHED];
  int n = 0, st = 0;
  int len = joined.length();
  for(int i = 0; i <= len && n < MAX_SCHED; i++){
    if(i == len || joined[i] == ';'){
      if(i > st) lines[n++] = joined.substring(st, i);
      st = i + 1;
    }
  }
  applyScheduleLines(lines, n, false);
}

bool dayOn(const Sched& s, int weekday){ return (s.days >> weekday) & 1; }

void checkSchedules(){
  static uint32_t lastCheck = 0;
  if(millis() - lastCheck < 1000) return;
  lastCheck = millis();
  if(!timeValid()) return;

  time_t local = time(nullptr) + (time_t)tzOffsetHours * 3600;
  struct tm t;
  gmtime_r(&local, &t);
  int minute = t.tm_hour * 60 + t.tm_min;
  int wd = (t.tm_wday + 6) % 7;   // 0 = понедельник
  int yd = (wd + 6) % 7;          // вчера
  if(minute == lastSchedMinute) return;
  lastSchedMinute = minute;

  // раз в минуту — время гирлянды в мониторе порта, удобно проверять
  Serial.printf("[Время] %02d:%02d UTC%+d, день недели %d, сценариев %d\n",
                t.tm_hour, t.tm_min, tzOffsetHours, wd + 1, schedCount);
  if(schedCount == 0) return;

  // гирлянду включили посреди сценария — сразу запускаем его режим
  if(!schedBootChecked){
    schedBootChecked = true;
    for(int i = 0; i < schedCount; i++){
      Sched& s = scheds[i];
      if(!s.en || s.off < 0 || s.on == s.off) continue;
      bool inside;
      if(s.on < s.off) inside = dayOn(s, wd) && minute >= s.on && minute < s.off;
      else inside = (dayOn(s, wd) && minute >= s.on) || (dayOn(s, yd) && minute < s.off);
      if(inside){
        startMode(s.mode, s.mode == "random" ? s.every : s.speed);
        Serial.printf("[Сценарии] догоняю сценарий %d\n", i + 1);
        return;
      }
    }
  }

  // ВЫКЛЮЧЕНИЕ: в минуту «ВЫКЛ» гирлянда гаснет всегда — неважно,
  // кто включил режим (сценарий, программа или повторная отправка)
  for(int i = 0; i < schedCount; i++){
    Sched& s = scheds[i];
    if(!s.en || s.off < 0 || minute != s.off) continue;
    // сценарий через полночь (22:00–02:00) выключается уже «завтра»
    bool day = (s.on < s.off) ? dayOn(s, wd) : dayOn(s, yd);
    if(s.on == s.off) day = dayOn(s, wd);
    if(day){
      stopModeNow();
      Serial.printf("[Сценарии] выключение по сценарию %d\n", i + 1);
    }
  }

  // ВКЛЮЧЕНИЕ
  for(int i = 0; i < schedCount; i++){
    Sched& s = scheds[i];
    if(s.en && s.on == minute && dayOn(s, wd)){
      startMode(s.mode, s.mode == "random" ? s.every : s.speed);
      Serial.printf("[Сценарии] включение по сценарию %d: %s\n", i + 1, s.mode.c_str());
    }
  }
}

void initLeds(){
  for(int i = 0; i < COLS; i++){
    strips[i] = new Adafruit_NeoPixel(ROWS, STRAND_PINS[i], NEO_GRB + NEO_KHZ800);
    strips[i]->begin();
    strips[i]->setBrightness(globalBrightness);
    strips[i]->clear();
    strips[i]->show();
    strandDirty[i] = false;
  }
  Serial.println("[LED] Adafruit_NeoPixel C3 9x20");
}

bool hasSavedWifi(){ return homeSsid.length() > 0; }

bool apOn = false;            // точка доступа Matrix включена
uint32_t wifiLostAt = 0;      // когда пропала домашняя сеть (0 — сеть есть)
uint32_t wifiTryAt  = 0;      // последняя попытка подключиться
uint32_t wifiUpAt   = 0;      // когда сеть снова появилась

const uint32_t WIFI_RETRY_MS    = 20000;    // повтор подключения
const uint32_t AP_FALLBACK_MS   = 90000;    // нет сети 1,5 мин — включаем запасную точку доступа
const uint32_t WIFI_RESTART_MS  = 600000;   // нет сети 10 мин — перезапуск Wi-Fi модуля целиком

// энергосбережение Wi-Fi копит пакеты и отдаёт их пачкой — рисование «зависает».
// Отключаем его после каждого переключения режима Wi-Fi
void wifiNoSleep(){
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void wifiBegin(){
  WiFi.disconnect(false, false);   // пароль не стираем
  delay(20);
  WiFi.begin(homeSsid.c_str(), homePass.c_str());
  wifiTryAt = millis();
}

bool connectToHome(const String& ssid, const String& pass, uint32_t timeoutMs){
  if(ssid.isEmpty()) return false;
  Serial.printf("[WiFi] %s ...\n", ssid.c_str());
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiTryAt = millis();
  uint32_t s = millis();
  while(millis() - s < timeoutMs){
    if(WiFi.status() == WL_CONNECTED){
      Serial.printf("[WiFi] OK %s, сигнал %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      wifiLoggedOn = true;
      wifiNoSleep();
      return true;
    }
    delay(100);
  }
  wifiLoggedOn = false;
  return false;
}

// точка доступа Matrix: при первой настройке или как запасной вход,
// если домашняя сеть долго не находится (пароль при этом НЕ стирается)
void startAP(){
  if(apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  wifiNoSleep();
  apOn = true;
  Serial.printf("[AP] %s / %s  IP %s\n", AP_SSID, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void startProvisioningAP(){ startAP(); }

void stopProvisioningAP(){
  if(!apOn) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  wifiNoSleep();
  apOn = false;
  Serial.println("[AP] выключена");
}

void forgetCredentials(){
  prefs.begin("matrix-c3", false);
  prefs.clear();
  prefs.end();
  homeSsid = "";
  homePass = "";
  Serial.println("[NVS] стёрто");
}

void sendAnnounce();
String stateJson();
String handleJsonCommand(const String& cmd);

// причина обрыва — в монитор порта (201 — сеть не найдена, 15/204 — неверный пароль и т.п.)
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info){
  if(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED){
    Serial.printf("[WiFi] обрыв, причина %d\n", info.wifi_sta_disconnected.reason);
  } else if(event == ARDUINO_EVENT_WIFI_STA_GOT_IP){
    Serial.printf("[WiFi] IP %s\n", WiFi.localIP().toString().c_str());
  }
}

// следит за сетью без блокировки: сама переподключается сколько угодно раз,
// через 1,5 мин без сети включает запасную точку доступа, через 10 мин перезапускает Wi-Fi
void maintainWifi(){
  static uint32_t last = 0;
  if(millis() - last < 1000) return;
  last = millis();
  if(!hasSavedWifi()) return;

  if(WiFi.status() == WL_CONNECTED){
    if(wifiLostAt){
      wifiLostAt = 0;
      wifiUpAt = millis();
      wifiNoSleep();
      Serial.printf("[WiFi] снова в сети %s\n", WiFi.localIP().toString().c_str());
      sendAnnounce();
    }
    if(!wifiUpAt) wifiUpAt = millis();
    // сеть стабильно есть 10 с — запасная точка доступа больше не нужна
    if(apOn && millis() - wifiUpAt > 10000 && WiFi.softAPgetStationNum() == 0) stopProvisioningAP();
    return;
  }

  if(!wifiLostAt){
    wifiLostAt = millis();
    wifiUpAt = 0;
    Serial.println("[WiFi] сети нет — переподключаюсь");
  }
  uint32_t lost = millis() - wifiLostAt;
  // если кто-то подключён к точке доступа Matrix — пробуем реже, чтобы поиск сети не мешал ему
  uint32_t retry = (apOn && WiFi.softAPgetStationNum() > 0) ? 60000 : WIFI_RETRY_MS;
  if(millis() - wifiTryAt > retry) wifiBegin();
  if(!apOn && lost > AP_FALLBACK_MS) startAP();
  if(lost > WIFI_RESTART_MS && WiFi.softAPgetStationNum() == 0){
    // драйвер Wi-Fi иногда «залипает» — перезапускаем его целиком
    Serial.println("[WiFi] перезапуск Wi-Fi модуля");
    bool ap = apOn;
    apOn = false;
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    wifiNoSleep();
    if(ap) startAP();
    wifiBegin();
    wifiLostAt = millis() - AP_FALLBACK_MS;   // точку доступа держим, пока сети нет
  }
}

IPAddress subnetBroadcast(){
  IPAddress ip, mask;
  if(WiFi.status() == WL_CONNECTED){
    ip = WiFi.localIP();
    mask = WiFi.subnetMask();
  } else {
    ip = WiFi.softAPIP();
    mask = IPAddress(255,255,255,0);
  }
  IPAddress b;
  for(int i = 0; i < 4; i++) b[i] = ip[i] | ~mask[i];
  return b;
}

void sendAnnounce(){
  udp.writeTo((const uint8_t*)discoveryMessage.c_str(), discoveryMessage.length(), subnetBroadcast(), DISCOVERY_PORT);
}

// ==================== ОБНОВЛЕНИЕ ПО ВОЗДУХУ ====================
void otaProgress(int done, int total){
  if(total <= 0) return;
  int k = (int)((long)done * COLS / total);
  for(int c = 0; c < COLS; c++){
    strips[c]->setPixelColor(ROWS - 1, c < k ? strips[c]->Color(0, 120, 30) : strips[c]->Color(8, 8, 8));
    markDirty(c);
  }
  updateLedsNow();
}

// Stable mbedTLS API on Arduino-ESP32 2.x and 3.x. Hash bytes before OTA commit.
class OtaSha256 {
  mbedtls_md_context_t ctx;
public:
  OtaSha256(){ mbedtls_md_init(&ctx); }
  ~OtaSha256(){ mbedtls_md_free(&ctx); }
  bool begin(){
    const mbedtls_md_info_t* info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_setup(&ctx,info,0)==0 && mbedtls_md_starts(&ctx)==0;
  }
  bool update(const uint8_t* data,size_t size){ return mbedtls_md_update(&ctx,data,size)==0; }
  bool finish(uint8_t out[32]){ return mbedtls_md_finish(&ctx,out)==0; }
};
int otaHexNibble(char c){
  if(c>='0' && c<='9') return c-'0';
  if(c>='a' && c<='f') return c-'a'+10;
  if(c>='A' && c<='F') return c-'A'+10;
  return -1;
}

// A failed attempt only writes the inactive slot. Boot selection changes only at Update.end().
bool otaAttempt(const String& url,const String& sha,String& error){
  uint8_t expectedSha[32];
  const bool verifySha=sha.length()!=0;
  if(verifySha){
    if(sha.length()!=64){error="invalid SHA256 length";return false;}
    for(int i=0;i<32;i++){
      int hi=otaHexNibble(sha[i*2]),lo=otaHexNibble(sha[i*2+1]);
      if(hi<0||lo<0){error="invalid SHA256 hex";return false;}
      expectedSha[i]=(uint8_t)((hi<<4)|lo);
    }
  }
  OtaSha256 checksum;
  if(!checksum.begin()){error="SHA256 init failed";return false;}
  WiFiClientSecure client;
  client.setHandshakeTimeout(8);
  client.setInsecure(); // same repository trust model as v10; expected digest supplied by client
  HTTPClient http;
  http.setConnectTimeout(7000);http.setTimeout(8000);http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if(!http.begin(client,url)){error="HTTP begin failed";return false;}
  int code=http.GET();
  if(code!=200){error="HTTP "+String(code)+" "+http.errorToString(code);http.end();return false;}
  int total=http.getSize();
  const esp_partition_t* slot=esp_ota_get_next_update_partition(NULL);
  if(total<=0||slot==NULL||(uint32_t)total>slot->size){error="invalid image size / OTA partition";http.end();return false;}
  Update.abort();
  if(!Update.begin((size_t)total,U_FLASH)){error=Update.errorString();http.end();return false;}
  auto* stream=http.getStreamPtr();uint8_t buffer[4096];int done=0,lastCol=-1;
  uint32_t start=millis(),lastData=start;
  while(done<total){
    uint32_t now=millis();
    if(now-lastData>10000||now-start>90000){error="download timed out at "+String(done)+"/"+String(total);break;}
    if(WiFi.status()!=WL_CONNECTED){error="WiFi disconnected during download";break;}
    int available=stream->available();
    if(available>0){
      int want=min((int)sizeof(buffer),min(available,total-done));int n=stream->read(buffer,want);
      if(n>0){if(!checksum.update(buffer,(size_t)n)){error="SHA256 update failed";break;}if(Update.write(buffer,n)!=(size_t)n){error=Update.errorString();break;}done+=n;lastData=millis();int col=(int)((long)done*COLS/total);if(col!=lastCol){otaProgress(done,total);lastCol=col;}}
    }else if(!http.connected()){error="connection closed at "+String(done)+"/"+String(total);break;}
    delay(1);
  }
  bool ok=false;
  if(done==total){
    uint8_t digest[32];
    if(!checksum.finish(digest)) error="SHA256 finish failed";
    else {
      uint8_t difference=0;
      if(verifySha) for(int i=0;i<32;i++) difference|=digest[i]^expectedSha[i];
      if(difference) error="SHA256 mismatch";
      else {ok=Update.end();if(!ok)error=Update.errorString();}
    }
  }
  if(!ok)Update.abort();
  http.end();
  Serial.printf("[OTA] received %d/%d bytes: %s\n",done,total,ok?"verified":error.c_str());
  return ok;
}

void runOta(){
  String url=otaUrl,sha=otaSha256;otaUrl="";otaSha256="";otaStatus="run";stopModeNow();
  if(esp_ota_get_next_update_partition(NULL)==NULL){otaStatus="fail: нет OTA-раздела (Partition)";Serial.println("[OTA] "+otaStatus);return;}
  String error;
  for(int attempt=1;attempt<=3;attempt++){
    Serial.printf("[OTA] attempt %d/3\n",attempt);Serial.println("[OTA] "+url);
    if(otaAttempt(url,sha,error)){
      Serial.println("[OTA] verified; rebooting");delay(150);ESP.restart();return;
    }
    Serial.println("[OTA] attempt failed: "+error);
    if(error.indexOf("SHA")>=0||error.indexOf("MD5")>=0||error.indexOf("invalid")>=0||error.indexOf("partition")>=0||error.startsWith("HTTP 404")||error.startsWith("HTTP 403")) break;
    if(attempt<3){for(int i=0;i<20;i++)delay(50);}
  }
  otaStatus="fail: "+error;Serial.println("[OTA] "+otaStatus);
  for(int c=0;c<COLS;c++){strips[c]->setPixelColor(ROWS-1,strips[c]->Color(150,0,0));markDirty(c);}updateLedsNow();
}

String jsonGetString(const String& j, const String& key, bool& found){
  found = false;
  String p = "\"" + key + "\"";
  int i = j.indexOf(p);
  if(i < 0) return "";
  i = j.indexOf(':', i + p.length());
  if(i < 0) return "";
  i++;
  while(i < (int)j.length() && j[i] == ' ') i++;
  if(i >= (int)j.length() || j[i] != '"') return "";
  i++;
  String v = "";
  while(i < (int)j.length()){
    char c = j[i++];
    if(c == 0x5C && i < (int)j.length()){
      char n = j[i++];
      if(n == 'n') v += '\n';
      else if(n == 't') v += '\t';
      else v += n;
    } else if(c == '"'){ found = true; return v; }
    else v += c;
  }
  return "";
}

long jsonGetInt(const String& j, const String& key, bool& found){
  found = false;
  String p = "\"" + key + "\"";
  int i = j.indexOf(p);
  if(i < 0) return 0;
  i = j.indexOf(':', i + p.length());
  if(i < 0) return 0;
  i++;
  while(i < (int)j.length() && (j[i] == ' ' || j[i] == '"')) i++;
  bool neg = false;
  if(i < (int)j.length() && j[i] == '-'){ neg = true; i++; }
  long v = 0;
  bool any = false;
  while(i < (int)j.length() && isDigit((uint8_t)j[i])){
    v = v * 10 + (j[i] - '0');
    i++;
    any = true;
  }
  if(!any) return 0;
  found = true;
  return neg ? -v : v;
}

int jsonGetStringArray(const String& j, const String& key, String *out, int maxRows){
  String p = "\"" + key + "\"";
  int i = j.indexOf(p);
  if(i < 0) return 0;
  i = j.indexOf('[', i + p.length());
  if(i < 0) return 0;
  int count = 0;
  int len = j.length();
  while(i < len && count < maxRows){
    char c = j[i];
    if(c == '"'){
      int st = ++i;
      while(i < len && j[i] != '"') i++;
      out[count++] = j.substring(st, i);
      i++;
    } else if(c == ']') break;
    else i++;
  }
  return count;
}

void hexToRgb(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b){
  if(hex.length() < 6){ r = 0; g = 0; b = 0; return; }
  auto byteAt = [&](int p) -> uint8_t {
    return (uint8_t)strtol(hex.substring(p, p + 2).c_str(), NULL, 16);
  };
  r = byteAt(0); g = byteAt(2); b = byteAt(4);
}

void setPixel(uint8_t col, uint8_t row, uint8_t r, uint8_t g, uint8_t b){
  if(col >= COLS || row >= ROWS) return;
  strips[col]->setPixelColor(row, r, g, b);
  markDirty(col);
}

void setPixelHex(uint8_t col, uint8_t row, const String& hex){
  uint8_t r, g, b;
  hexToRgb(hex, r, g, b);
  setPixel(col, row, r, g, b);
}

void applyBrightness(long v){
  if(v < 0) v = 0;
  if(v > 100) v = 100;
  brightPct = (uint8_t)v;
  globalBrightness = (uint8_t)(v * 255 / 100);
  for(int i = 0; i < COLS; i++){
    strips[i]->setBrightness(globalBrightness);
    markDirty(i);
  }
}

void applyPicture(const String *rows, int n){
  int rn = n < ROWS ? n : ROWS;
  for(int r = 0; r < rn; r++){
    const String& row = rows[r];
    for(int c = 0; c < COLS; c++){
      if(row.length() >= (c + 1) * 6){
        setPixelHex((uint8_t)c, (uint8_t)r, row.substring(c * 6, c * 6 + 6));
      }
    }
  }
  for(int i = 0; i < COLS; i++) markDirty(i);
}

const char* modeName(){ return modeNameOf(currentMode); }

const char* modeNameOf(Mode m){
  switch(m){
    case MODE_SNOW: return "snow";
    case MODE_FIRE: return "fire";
    case MODE_RAINBOW: return "rainbow";
    case MODE_RAIN: return "rain";
    case MODE_FIREWORKS: return "fireworks";
    case MODE_LAVA: return "lava";
    case MODE_STARS: return "stars";
    case MODE_GALAXY: return "galaxy";
    case MODE_KALEIDOSCOPE: return "kaleidoscope";
    case MODE_PLASMA: return "plasma";
    case MODE_AURORA: return "aurora";
    case MODE_TUNNEL: return "tunnel";
    default: return "off";
  }
}

// состояние гирлянды одной строкой JSON — для приложения на телефоне
String stateJson(){
  bool sta = WiFi.status() == WL_CONNECTED;
  String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String ssid = sta ? WiFi.SSID() : String("");
  ssid.replace("\\", "\\\\");
  ssid.replace("\"", "\\\"");
  String j = "{\"name\":\"";
  j += DEVICE_NAME;
  j += "\",\"model\":\"" + String(DEVICE_MODEL) + "\",\"id\":\"" + deviceIdentity;
  j += "\",\"cols\":" + String(COLS) + ",\"rows\":" + String(ROWS);
  j += ",\"mode\":\"";
  j += modeName();
  j += "\",\"speed\":" + String(modeSpeed);
  j += ",\"rnd\":" + String(randEvery);
  j += ",\"brightness\":" + String(brightPct);
  j += ",\"tz\":" + String(tzOffsetHours);
  j += ",\"time\":" + String(timeValid() ? (long)time(nullptr) : 0L);
  j += ",\"ip\":\"" + ip + "\"";
  j += ",\"ssid\":\"" + ssid + "\"";
  j += String(",\"ap\":") + (sta ? "false" : "true");
  j += ",\"fw\":" + String(FW_VERSION);
  j += ",\"picsRev\":" + (picsOk ? String(picsRev) : String("-1"));
  j += ",\"ota\":\"" + otaStatus + "\"";
  j += ",\"sched\":[";
  int st = 0, n = 0, len = schedJoined.length();
  for(int i = 0; i <= len; i++){
    if(i == len || schedJoined[i] == ';'){
      if(i > st){
        if(n++) j += ",";
        j += "\"" + schedJoined.substring(st, i) + "\"";
      }
      st = i + 1;
    }
  }
  j += "]}";
  return j;
}


// ============ КАРТИНКИ (общие для телефона и ПК) ============
// хранятся во флеше гирлянды (LittleFS), клиенты сверяются по picsRev
#define PICS_MAX 60

bool picIdOk(const String& id){
  if(id.length() < 1 || id.length() > 16) return false;
  for(int i = 0; i < (int)id.length(); i++) if(!isalnum((unsigned char)id[i])) return false;
  return true;
}

// строка как есть (с JSON-экранированием) — имя отдаём обратно без перекодирования
String jsonGetRaw(const String& j, const String& key, bool& found){
  found = false;
  String p = "\"" + key + "\"";
  int i = j.indexOf(p);
  if(i < 0) return "";
  i = j.indexOf(':', i + p.length());
  if(i < 0) return "";
  i++;
  while(i < (int)j.length() && j[i] == ' ') i++;
  if(i >= (int)j.length() || j[i] != '"') return "";
  int st = ++i;
  while(i < (int)j.length()){
    char c = j[i];
    if(c == 0x5C){ i += 2; continue; }
    if(c == '"'){ found = true; return j.substring(st, i); }
    i++;
  }
  return "";
}

void initPics(){
  picsRev = esp_random() & 0x3FFFFFFF;   // после перезагрузки клиенты перечитают список
  picsOk = LittleFS.begin(true);
  if(picsOk && !LittleFS.exists("/p")) LittleFS.mkdir("/p");
  Serial.printf("[Картинки] хранилище %s\n", picsOk ? "есть" : "НЕТ (нужна схема Default 4MB with spiffs)");
}

int picsCount(){
  if(!picsOk) return 0;
  File d = LittleFS.open("/p");
  if(!d) return 0;
  int n = 0;
  File f = d.openNextFile();
  while(f){ n++; f.close(); f = d.openNextFile(); }
  d.close();
  return n;
}

String picsList(){
  String out = "{\"rev\":" + String(picsRev) + ",\"items\":[";
  if(picsOk){
    File d = LittleFS.open("/p");
    int n = 0;
    File f = d ? d.openNextFile() : File();
    while(f){
      String id = String(f.name());
      int sl = id.lastIndexOf('/');
      if(sl >= 0) id = id.substring(sl + 1);
      String name = f.readStringUntil('\n');
      String t = f.readStringUntil('\n');
      f.close();
      if(n++) out += ",";
      out += "{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"t\":" + (t.length() ? t : String("0")) + "}";
      f = d.openNextFile();
    }
    if(d) d.close();
  }
  out += "]}";
  return out;
}

String picsGet(const String& id){
  if(!picsOk || !picIdOk(id)) return "ERROR";
  File f = LittleFS.open("/p/" + id, "r");
  if(!f) return "ERROR";
  String name = f.readStringUntil('\n');
  String t = f.readStringUntil('\n');
  String rows = f.readStringUntil('\n');
  f.close();
  String out = "{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"t\":" + (t.length() ? t : String("0")) + ",\"rows\":[";
  for(int r = 0; r < ROWS; r++){
    if(r) out += ",";
    out += "\"" + rows.substring(r * COLS * 6, (r + 1) * COLS * 6) + "\"";
  }
  out += "]}";
  return out;
}

String picsPut(const String& cmd){
  if(!picsOk) return "ERROR";
  bool found = false;
  String id = jsonGetString(cmd, "id", found);
  if(!found || !picIdOk(id)) return "ERROR";
  String name = jsonGetRaw(cmd, "name", found);
  if(name.length() > 200) name = name.substring(0, 200);
  name.replace("\n", ""); name.replace("\r", "");
  long t = jsonGetInt(cmd, "t", found);
  String rows[ROWS];
  int n = jsonGetStringArray(cmd, "rows", rows, ROWS);
  if(n < 1) return "ERROR";
  String path = "/p/" + id;
  if(!LittleFS.exists(path) && picsCount() >= PICS_MAX) return "FULL";
  String all = "";
  for(int r = 0; r < ROWS; r++){
    String row = r < n ? rows[r] : String("");
    row.toUpperCase();
    while((int)row.length() < COLS * 6) row += "000000";
    all += row.substring(0, COLS * 6);
  }
  File f = LittleFS.open(path, "w");
  if(!f) return "ERROR";
  f.print(name); f.print('\n');
  f.print(String(t)); f.print('\n');
  f.print(all); f.print('\n');
  f.close();
  picsRev++;
  return "OK";
}

String picsDel(const String& cmd){
  if(!picsOk) return "ERROR";
  bool found = false;
  String id = jsonGetString(cmd, "id", found);
  if(!found || !picIdOk(id)) return "ERROR";
  if(LittleFS.exists("/p/" + id)){ LittleFS.remove("/p/" + id); picsRev++; }
  return "OK";
}
// ============ конец КАРТИНОК ============

String handleJsonCommand(const String& cmd){
  bool found = false;
  String type = jsonGetString(cmd, "type", found);

  if(type == "state") return stateJson();
  if(type == "pics_list") return picsList();
  if(type == "pic_get"){ String id = jsonGetString(cmd, "id", found); return picsGet(id); }
  if(type == "pic_put") return picsPut(cmd);
  if(type == "pic_del") return picsDel(cmd);

  if(type == "ota"){
    String url = jsonGetString(cmd, "url", found);
    if(!found || !url.startsWith(OTA_PREFIX) || !url.endsWith("/firmware/esp32c3-9x20/LEDMatrix.bin")) return "ERROR: wrong hardware firmware";   // прошивку берём только из своего репозитория
    String sha = jsonGetString(cmd, "sha256", found);
    if(sha.length()){
      if(sha.length()!=64) return "ERROR: invalid SHA256";
      for(int i=0;i<64;i++) if(!isxdigit((unsigned char)sha[i])) return "ERROR: invalid SHA256";
    }
    otaSha256=sha;
    otaUrl = url;   // запускаем из loop(), когда ответ уже ушёл
    return "OK";
  }

  if(type == "time"){
    long epoch = jsonGetInt(cmd, "epoch", found);
    if(found) setClock(epoch);
    long offset = jsonGetInt(cmd, "offset", found);
    if(found) saveTz((int)offset);
    return "OK";
  }

  if(type == "timezone"){
    long offset = jsonGetInt(cmd, "offset", found);
    if(found) saveTz((int)offset);
    return "OK";
  }

  if(type == "schedule"){
    String items[MAX_SCHED];
    int n = jsonGetStringArray(cmd, "items", items, MAX_SCHED);
    applyScheduleLines(items, n, true);
    return "OK";
  }

  if(type == "speed"){
    long v = jsonGetInt(cmd, "value", found);
    if(!found) return "ERROR";
    if(v < 1) v = 1;
    if(v > 100) v = 100;
    modeSpeed = (int)v;
    if(currentMode != MODE_OFF) modeSpeeds[currentMode] = (uint8_t)v;
    lastModeTick = millis();
    return "OK";
  }

  if(type == "mode"){
    String name = jsonGetString(cmd, "mode", found);
    if(name == "random"){
      bool f2 = false;
      String speeds = jsonGetString(cmd, "speeds", f2);
      if(f2) setModeSpeeds(speeds);
      long ev = jsonGetInt(cmd, "every", f2);
      startRandom(f2 ? (int)ev : 15);
      return "OK";
    }
    long sp = jsonGetInt(cmd, "speed", found);
    int speed = found ? (int)sp : 65;
    if(speed < 1) speed = 1;
    if(speed > 100) speed = 100;
    startMode(name, speed);
    return "OK";
  }

  if(type == "clear"){
    stopModeNow();
    return "OK";
  }

  if(type == "pixels" || type == "pixel" || type == "picture"){
    if(currentMode != MODE_OFF){
      // режим шёл — гасим его ВМЕСТЕ с последним кадром,
      // иначе анимация «замирает» и рисунок ложится поверх неё
      stopModeNow();
      Serial.println("[Режим] выключен (ручное рисование)");
    }
  }

  if(type == "pixels"){
    // ищем внешний массив после ключа "pixels" и начинаем ВНУТРИ него,
    // иначе первый элемент читался как "[0" и уходил в колонку 0
    int k = cmd.indexOf("\"pixels\"");
    int i = cmd.indexOf('[', k < 0 ? 0 : k);
    if(i < 0) return "ERROR";
    i++;
    int count = 0;
    int len = cmd.length();
    while(i < len){
      if(cmd[i] != '['){ i++; continue; }
      int p = cmd.indexOf(']', i);
      if(p < 0) break;
      String el = cmd.substring(i + 1, p);
      int c1 = el.indexOf(',');
      int c2 = c1 < 0 ? -1 : el.indexOf(',', c1 + 1);
      if(c1 > 0 && c2 > 0){
        long col = el.substring(0, c1).toInt();
        long row = el.substring(c1 + 1, c2).toInt();
        String hex = el.substring(c2 + 1);
        hex.replace("\"", "");
        hex.trim();
        if(col >= 0 && col < COLS && row >= 0 && row < ROWS && hex.length() >= 6){
          setPixelHex((uint8_t)col, (uint8_t)row, hex);
          count++;
        }
      }
      i = p + 1;
    }
    return count > 0 ? "" : "ERROR";
  }

  if(type == "pixel"){
    long col = jsonGetInt(cmd, "col", found);
    if(!found) return "ERROR";
    long row = jsonGetInt(cmd, "row", found);
    if(!found) return "ERROR";
    String color = jsonGetString(cmd, "color", found);
    if(!found) return "ERROR";
    setPixelHex((uint8_t)col, (uint8_t)row, color);
    return "";
  }

  if(type == "brightness"){
    long v = jsonGetInt(cmd, "value", found);
    if(!found) return "ERROR";
    applyBrightness(v);
    return "OK";
  }

  if(type == "picture"){
    String rows[ROWS];
    int n = jsonGetStringArray(cmd, "rows", rows, ROWS);
    if(n == 0) return "ERROR";
    applyPicture(rows, n);
    return "OK";
  }

  return "OK";
}

void handleResetButton(){} // No external reset/forget-WiFi button on C3.

void processCommand(const String& cmd, WiFiClient& client){
  // весь текст команды не печатаем: картинка — 2,5 КБ, это 0,2 с простоя на Serial
  { bool f = false; String t = jsonGetString(cmd, "type", f); if(t != "state") Serial.println("[TCP] " + (f ? t : String("wifi"))); }

  bool hasSsid = false, hasPass = false;
  String ssid = jsonGetString(cmd, "ssid", hasSsid);
  String pass = jsonGetString(cmd, "password", hasPass);

  if(hasSsid && hasPass){
    if(ssid.isEmpty()){
      client.print("ERROR: пустой SSID\n");
      return;
    }
    if(connectToHome(ssid, pass, HOME_CONNECT_TIMEOUT_MS)){
      client.print("OK\n");
      client.flush();
      delay(400);   // ответ успевает уйти, пока точка доступа ещё работает
      prefs.begin("matrix-c3", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
      homeSsid = ssid;
      homePass = pass;
      stopProvisioningAP();
      sendAnnounce();
    } else {
      client.print("ERROR: не удалось подключиться\n");
    }
    return;
  }

  String reply = handleJsonCommand(cmd);
  if(reply.length() == 0) reply = "OK";
  client.print(reply + "\n");
}

// вызывается в задаче AsyncUDP — только кладём пакет в очередь
void onUdpPacket(AsyncUDPPacket& p){
  static UdpMsg m;   // задача одна, статический буфер не мешает
  size_t n = p.length();
  const char *d = (const char*)p.data();
  while(n && (d[n-1] == '\n' || d[n-1] == '\r' || d[n-1] == ' ')) n--;
  if(n >= 4 && memcmp(d, "PING", 4) == 0){
    p.print(discoveryMessage);   // отвечаем сразу, даже если loop() занят
    return;
  }
  if(n == 0 || n > UDP_MSG_MAX || d[0] != '{' || !udpQueue) return;
  m.len = (uint16_t)n;
  memcpy(m.data, d, n);
  m.data[n] = 0;
  xQueueSend(udpQueue, &m, 0);
}

void handleUdpQueue(){
  static UdpMsg m;
  for(int k = 0; k < UDP_QUEUE_LEN && xQueueReceive(udpQueue, &m, 0) == pdTRUE; k++){
    handleJsonCommand(String(m.data));
  }
}

// TCP без ожидания: читаем строку, отвечаем и ждём, пока клиент закроет
// соединение сам — тогда stop() не тормозит loop()
void handleCommands(){
  if(!tcpActive){
    commandClient = commandServer.available();
    if(!commandClient) return;
    commandClient.setNoDelay(true);
    tcpActive = true;
    tcpReplied = false;
    tcpLine = "";
    tcpAt = millis();
  }
  if(tcpReplied){
    if(!commandClient.connected() || millis() - tcpAt > 1500){
      commandClient.stop();
      tcpActive = false;
    }
    return;
  }
  int budget = 4096;
  while(budget-- > 0 && commandClient.available()){
    char c = (char)commandClient.read();
    if(c == '\n'){
      String cmd = tcpLine;
      tcpLine = "";
      processCommand(cmd, commandClient);
      tcpReplied = true;
      tcpAt = millis();
      return;
    }
    if(c != '\r'){
      tcpLine += c;
      if(tcpLine.length() > 6000) tcpLine = "";
    }
  }
  if(!commandClient.connected() || millis() - tcpAt > 4000){
    commandClient.stop();
    tcpActive = false;
    tcpLine = "";
  }
}

void setup(){
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  Serial.printf("\n[Boot] LED Matrix, прошивка v%d\n", FW_VERSION);
  Serial.printf("[Boot] OTA-раздел: %s\n", esp_ota_get_next_update_partition(NULL) ? "есть" : "НЕТ (выбери Partition Scheme: Default 4MB with spiffs)");

  initLeds();


  // сценарии и часовой пояс хранятся отдельно от Wi-Fi:
  // сброс сети кнопкой их не стирает
  loadSchedules();
  loadModeSpeeds();
  initPics();

  Serial.println("[C3] external WiFi reset button disabled");

  WiFi.persistent(false);   // пароль храним сами, флеш Wi-Fi не трогаем
  WiFi.mode(WIFI_STA);
  deviceIdentity=WiFi.macAddress();
  discoveryMessage=String("MATRIX2|")+DEVICE_NAME+"|"+DEVICE_MODEL+"|"+deviceIdentity+"|9|20";
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);          // ищем по всем каналам
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);      // при нескольких роутерах — самый сильный
  WiFi.onEvent(onWifiEvent);
  wifiNoSleep();

  prefs.begin("matrix-c3", true);
  homeSsid = prefs.getString("ssid", "");
  homePass = prefs.getString("pass", "");
  prefs.end();

  if(hasSavedWifi()){
    // ждём сеть до 20 с; не нашли — дальше maintainWifi() пробует сама, без блокировки
    if(!connectToHome(homeSsid, homePass, 20000)){
      wifiLostAt = millis();
      Serial.println("[WiFi] пока не подключилась — буду пробовать дальше");
    }
  } else {
    startAP();
  }

  // время из интернета (если есть домашний Wi-Fi) — синхронизируется само
  // в фоне; без интернета время приходит из программы
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  udpQueue = xQueueCreate(UDP_QUEUE_LEN, sizeof(UdpMsg));
  if(udp.listen(DISCOVERY_PORT)) udp.onPacket(onUdpPacket);
  else Serial.println("[UDP] не удалось открыть порт");
  commandServer.begin();
  sendAnnounce();
  lastAnnounce = millis();
}

void loop(){
  handleResetButton();
  maintainWifi();
  handleUdpQueue();
  handleCommands();
  handleUdpQueue();
  checkSchedules();
  if(otaUrl.length()) runOta();
  updateRandom();
  updateMode();
  updateLeds();

  if(millis() - lastAnnounce >= ANNOUNCE_INTERVAL_MS){
    lastAnnounce = millis();
    sendAnnounce();
  }
}
