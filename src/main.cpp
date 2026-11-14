// Глаз на 6 сервах: ESP32-C3 SuperMini + PCA9685.
// Раздаёт Wi-Fi точку "Glaz" и страницу управления на http://192.168.4.1
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include "page.h"
#include "emotions.h"

// ---- железо и сеть ----
constexpr int SDA_PIN = 8, SCL_PIN = 9;
constexpr uint8_t PCA_ADDR = 0x40;
constexpr uint8_t CH[6] = {0, 1, 2, 3, 4, 5};  // канал PCA для S1..S6
const char *AP_SSID = "Glaz";
const char *AP_PASS = "glaz2026";  // минимум 8 символов
const IPAddress AP_IP(192, 168, 4, 1);

// Роли: что делает серва. Позы считаются в этих координатах.
enum { R_X, R_Y, RUL, RLL, RUR, RLR, NR };

struct Cfg {
  uint8_t ver;
  int16_t mn[6], mx[6];  // мкс в положении 0 и 1 (для зрачков: -1 и +1)
  uint8_t role[6];
  float speed;
  uint8_t live, follow;
};
const Cfg DEF = {4,
                 {400, 1800, 1600, 1500, 1400, 1150},
                 {1050, 550, 2150, 2520, 2100, 1950},
                 {RLR, RUR, R_Y, RUL, RLL, R_X},  // S1..S6
                 1.0f, 1, 1};
Cfg cfg;

Adafruit_PWMServoDriver pca(PCA_ADDR);
WebServer http(80);
WebSocketsServer ws(81);
DNSServer dns;
Preferences prefs;

// ---- состояние движения ----
const float NEUTRAL[NR] = {0, 0, .85f, .85f, .85f, .85f};
float tgt[NR], cur[NR], eff[NR];
float tauMul = 1;                       // >1 медленнее
float blinkL = 0, blinkR = 0, bTgtL = 0, bTgtR = 0;
uint32_t bOffL = 0, bOffR = 0, blink2At = 0;
int emo = -1, step = 0;
uint32_t nextAt = 0, lastManual = 0, liveNext = 0, liveBlinkNext = 0;
int testCh = -1;
uint32_t testUntil = 0;
int lastUs[6] = {-1, -1, -1, -1, -1, -1};
float usPerTick = 4.8889f;
bool pcaOk = false;

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float clamp11(float v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

void blink(uint8_t mask) {  // 1 левый, 2 правый, 3 оба
  uint32_t off = millis() + 110;
  if (mask & 1) { bTgtL = 1; bOffL = off; }
  if (mask & 2) { bTgtR = 1; bOffR = off; }
}

void neutral(float slow = 1.6f) {
  memcpy(tgt, NEUTRAL, sizeof tgt);
  tauMul = slow;
}

void startEmo(int i) { emo = i; step = 0; nextAt = 0; }

void runEmo(uint32_t now) {
  if (emo < 0 || now < nextAt) return;
  const Emo &e = EMOS[emo];
  if (step >= e.n) {
    emo = -1;
    liveNext = now + 1500;
    return;
  }
  const KF &k = e.kf[step++];
  const int8_t v[NR] = {k.x, k.y, k.ul, k.ll, k.ur, k.lr};
  for (int r = 0; r < NR; r++)
    if (v[r] != K) tgt[r] = r < 2 ? v[r] / 100.0f : v[r] / 100.0f;
  tauMul = k.slow / 10.0f;
  if (k.act == 1) blink(3);
  if (k.act == 2) blink(1);
  if (k.act == 3) blink(2);
  if (k.act == 4) { blink(3); blink2At = now + 260; }
  nextAt = now + k.ms;
}

// «Живой» режим: сам поглядывает по сторонам и моргает, когда его не трогают.
void runLive(uint32_t now) {
  if (!cfg.live || emo >= 0 || now - lastManual < 4000) return;
  if (now >= liveNext) {
    bool big = random(100) < 30;
    tgt[R_X] = random(big ? -85 : -35, big ? 86 : 36) / 100.0f;
    tgt[R_Y] = random(big ? -55 : -25, big ? 56 : 26) / 100.0f;
    float o = random(75, 96) / 100.0f;
    tgt[RUL] = tgt[RUR] = o;
    tgt[RLL] = tgt[RLR] = random(80, 96) / 100.0f;
    tauMul = 1;
    if (big && random(100) < 35) blink(3);
    liveNext = now + random(700, 3500);
  }
  if (now >= liveBlinkNext) {
    blink(3);
    if (random(100) < 15) blink2At = now + 260;
    liveBlinkNext = now + random(1800, 6000);
  }
}

void update(uint32_t now, float dt) {
  float s = cfg.speed;
  float aG = 1 - expf(-dt / (0.075f * tauMul / s));
  float aL = 1 - expf(-dt / (0.065f * tauMul / s));
  for (int r = 0; r < NR; r++) cur[r] += (tgt[r] - cur[r]) * (r < 2 ? aG : aL);

  if (blink2At && now >= blink2At) { blink(3); blink2At = 0; }
  if (now >= bOffL) bTgtL = 0;
  if (now >= bOffR) bTgtR = 0;
  float aB = 1 - expf(-dt / (0.025f / s));
  blinkL += (bTgtL - blinkL) * aB;
  blinkR += (bTgtR - blinkR) * aB;

  float k = cfg.follow ? 0.3f : 0;  // веки чуть следуют за взглядом вверх/вниз
  eff[R_X] = cur[R_X];
  eff[R_Y] = cur[R_Y];
  eff[RUL] = clamp01(cur[RUL] + k * cur[R_Y]) * (1 - blinkL);
  eff[RLL] = clamp01(cur[RLL] - k * cur[R_Y]) * (1 - blinkL);
  eff[RUR] = clamp01(cur[RUR] + k * cur[R_Y]) * (1 - blinkR);
  eff[RLR] = clamp01(cur[RLR] - k * cur[R_Y]) * (1 - blinkR);
}

void writeUs(int i, int us) {
  us = constrain(us, 400, 2600);
  if (us == lastUs[i]) return;
  lastUs[i] = us;
  if (pcaOk) pca.setPWM(CH[i], 0, (uint16_t)(us / usPerTick + 0.5f));
}

void output(uint32_t now) {
  for (int i = 0; i < 6; i++) {
    if (i == testCh && now < testUntil) continue;
    int r = cfg.role[i];
    float t = r < 2 ? (clamp11(eff[r]) + 1) * 0.5f : clamp01(eff[r]);
    writeUs(i, lroundf(cfg.mn[i] + t * (cfg.mx[i] - cfg.mn[i])));
  }
}

String cfgJson() {
  JsonDocument d;
  JsonObject c = d["cfg"].to<JsonObject>();
  for (int i = 0; i < 6; i++) {
    c["mn"].add(cfg.mn[i]);
    c["mx"].add(cfg.mx[i]);
    c["role"].add(cfg.role[i]);
  }
  c["sp"] = cfg.speed;
  c["lv"] = cfg.live;
  c["fo"] = cfg.follow;
  c["pca"] = pcaOk;
  String s;
  serializeJson(d, s);
  return s;
}

void broadcastState() {
  static char last[96];
  char buf[96];
  snprintf(buf, sizeof buf, "{\"s\":[%d,%d,%d,%d,%d,%d],\"e\":%d,\"lv\":%d}",
           (int)lroundf(eff[0] * 100), (int)lroundf(eff[1] * 100), (int)lroundf(eff[2] * 100),
           (int)lroundf(eff[3] * 100), (int)lroundf(eff[4] * 100), (int)lroundf(eff[5] * 100),
           emo, cfg.live);
  if (strcmp(buf, last) == 0) return;
  strcpy(last, buf);
  ws.broadcastTXT(buf);
}

void saveCfg() { prefs.putBytes("cfg", &cfg, sizeof cfg); }

void onWs(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
  if (type == WStype_CONNECTED) {
    String s = cfgJson();
    ws.sendTXT(num, s);
    return;
  }
  if (type != WStype_TEXT) return;
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  uint32_t now = millis();

  if (d["g"].is<JsonArray>()) {
    tgt[R_X] = clamp11(d["g"][0].as<float>());
    tgt[R_Y] = clamp11(d["g"][1].as<float>());
    emo = -1; tauMul = 1; lastManual = now;
  }
  if (d["l"].is<JsonArray>()) {
    for (int k = 0; k < 4; k++) tgt[RUL + k] = clamp01(d["l"][k].as<float>());
    emo = -1; tauMul = 1; lastManual = now;
  }
  if (d["b"].is<int>()) {
    int b = d["b"];
    blink(b == 1 ? 1 : b == 2 ? 2 : 3);
    lastManual = now;
  }
  if (d["e"].is<int>()) {
    int i = d["e"];
    if (i >= 0 && i < EMO_N) startEmo(i);
    else { emo = -1; neutral(); }
  }
  if (d["c"].is<int>()) { emo = -1; neutral(1); lastManual = now; }
  bool cfgChanged = false;
  if (d["lv"].is<int>()) { cfg.live = d["lv"].as<int>() ? 1 : 0; cfgChanged = true; }
  if (d["fo"].is<int>()) { cfg.follow = d["fo"].as<int>() ? 1 : 0; cfgChanged = true; }
  if (d["sp"].is<float>()) { cfg.speed = constrain(d["sp"].as<float>(), 0.3f, 2.5f); cfgChanged = true; }
  if (d["cal"].is<JsonObject>()) {
    JsonObject o = d["cal"];
    int i = o["i"] | -1;
    if (i >= 0 && i < 6) {
      if (o["mn"].is<int>()) cfg.mn[i] = constrain(o["mn"].as<int>(), 400, 2600);
      if (o["mx"].is<int>()) cfg.mx[i] = constrain(o["mx"].as<int>(), 400, 2600);
      if (o["r"].is<int>()) cfg.role[i] = constrain(o["r"].as<int>(), 0, NR - 1);
      lastUs[i] = -1;
      cfgChanged = true;
    }
  }
  if (d["t"].is<JsonObject>()) {  // поставить серву в точное положение на 2.5 с
    int i = d["t"]["i"] | -1, us = d["t"]["us"] | 1500;
    if (i >= 0 && i < 6) {
      testCh = i; testUntil = now + 2500;
      lastUs[i] = -1;
      writeUs(i, us);
      lastUs[i] = -1;
    }
  }
  if (d["def"].is<int>()) {
    float sp = cfg.speed;
    cfg = DEF;
    cfg.speed = sp;
    for (int i = 0; i < 6; i++) lastUs[i] = -1;
    cfgChanged = true;
  }
  if (d["save"].is<int>()) {
    saveCfg();
    ws.sendTXT(num, "{\"ok\":\"Сохранено в память платы\"}");
  }
  if (cfgChanged) {
    String s = cfgJson();
    ws.broadcastTXT(s);
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  prefs.begin("eye", false);
  cfg = DEF;
  if (prefs.getBytesLength("cfg") == sizeof(Cfg)) {
    Cfg c;
    prefs.getBytes("cfg", &c, sizeof c);
    if (c.ver == DEF.ver) cfg = c;
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  Wire.beginTransmission(PCA_ADDR);
  pcaOk = Wire.endTransmission() == 0;
  if (pcaOk) {
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);
    usPerTick = (pca.readPrescale() + 1) * 1e6f / 27e6f;
  }

  memcpy(tgt, NEUTRAL, sizeof tgt);
  memcpy(cur, NEUTRAL, sizeof cur);
  memcpy(eff, NEUTRAL, sizeof eff);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // у SuperMini на полной мощности Wi-Fi часто отваливается

  dns.start(53, "*", AP_IP);  // любое имя -> на страницу (окно входа в сеть)
  http.on("/", [] { http.send_P(200, "text/html; charset=utf-8", PAGE); });
  http.onNotFound([] {
    http.sendHeader("Location", "http://192.168.4.1/", true);
    http.send(302, "text/plain", "");
  });
  http.begin();
  ws.begin();
  ws.onEvent(onWs);

  Serial.printf("Глаз готов. Wi-Fi \"%s\" / %s -> http://192.168.4.1  PCA9685: %s\n",
                AP_SSID, AP_PASS, pcaOk ? "OK" : "НЕ НАЙДЕНА");
}

void loop() {
  dns.processNextRequest();
  http.handleClient();
  ws.loop();

  static uint32_t lastTick = millis(), lastSend = 0;
  uint32_t now = millis();
  if (now - lastTick >= 10) {
    float dt = (now - lastTick) / 1000.0f;
    lastTick = now;
    runEmo(now);
    runLive(now);
    update(now, dt);
    output(now);
  }
  if (now - lastSend >= 50) {
    lastSend = now;
    if (ws.connectedClients() > 0) broadcastState();
  }
}
