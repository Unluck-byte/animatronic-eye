// Калибровка рабочего хода 6 серв глаза: ESP32-C3 SuperMini + PCA9685.
// Управление из pio device monitor одиночными клавишами (см. help()).
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

// ---- настройки ----
constexpr int SDA_PIN = 8;          // SuperMini: SDA=GPIO8, SCL=GPIO9
constexpr int SCL_PIN = 9;
constexpr uint8_t PCA_ADDR = 0x40;
constexpr uint8_t NUM_SERVOS = 6;
constexpr uint8_t CHANNEL[NUM_SERVOS] = {0, 1, 2, 3, 4, 5};  // канал PCA9685 для серво 1..6
constexpr int HARD_MIN_US = 400;    // абсолютные пределы, дальше не пустит
constexpr int HARD_MAX_US = 2600;
constexpr int START_US = 1500;      // куда серва встаёт при первом выборе

Adafruit_PWMServoDriver pca(PCA_ADDR);

int posUs[NUM_SERVOS];
int minUs[NUM_SERVOS];
int maxUs[NUM_SERVOS];
bool active[NUM_SERVOS];
int sel = -1;

void writeUs(int i, int us) {
  us = constrain(us, HARD_MIN_US, HARD_MAX_US);
  posUs[i] = us;
  active[i] = true;
  pca.writeMicroseconds(CHANNEL[i], us);
  Serial.printf("S%d  %4d us   (min=%s  max=%s)\n", i + 1, us,
                minUs[i] ? String(minUs[i]).c_str() : "-",
                maxUs[i] ? String(maxUs[i]).c_str() : "-");
}

void release(int i) {
  pca.setPWM(CHANNEL[i], 0, 4096);  // полностью выкл — серва не держит
  active[i] = false;
}

void printTable() {
  Serial.println("\n#  кан   min   max   ход");
  for (int i = 0; i < NUM_SERVOS; i++) {
    Serial.printf("S%d  %2d  %4d  %4d  %4d\n", i + 1, CHANNEL[i], minUs[i], maxUs[i],
                  (minUs[i] && maxUs[i]) ? maxUs[i] - minUs[i] : 0);
  }
  Serial.print("\nconst int SERVO_MIN_US[6] = {");
  for (int i = 0; i < NUM_SERVOS; i++) Serial.printf("%d%s", minUs[i], i < 5 ? ", " : "};\n");
  Serial.print("const int SERVO_MAX_US[6] = {");
  for (int i = 0; i < NUM_SERVOS; i++) Serial.printf("%d%s", maxUs[i], i < 5 ? ", " : "};\n");
  Serial.println();
}

void sweep(int i) {
  if (!minUs[i] || !maxUs[i]) { Serial.println("сначала задай min и max"); return; }
  Serial.printf("S%d sweep %d -> %d -> %d (любая клавиша — стоп)\n", i + 1, minUs[i], maxUs[i], minUs[i]);
  int lo = min(minUs[i], maxUs[i]), hi = max(minUs[i], maxUs[i]);
  for (int pass = 0; pass < 2; pass++) {
    for (int k = 0; k <= hi - lo; k += 5) {
      int us = pass == 0 ? lo + k : hi - k;
      pca.writeMicroseconds(CHANNEL[i], us);
      posUs[i] = us;
      delay(15);
      if (Serial.available()) { Serial.read(); Serial.println("стоп"); return; }
    }
  }
  Serial.println("готово");
}

void scanI2C() {
  Serial.printf("I2C скан (SDA=%d SCL=%d):", SDA_PIN, SCL_PIN);
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
  }
  Serial.println(n ? "" : " ничего не найдено");
  Wire.beginTransmission(PCA_ADDR);
  Serial.println(Wire.endTransmission() == 0 ? "PCA9685 OK (0x40)" : "PCA9685 НЕ найдена на 0x40");
}

void help() {
  Serial.println(F(
    "\n=== Калибровка серв глаза ===\n"
    " 1..6    выбрать серву (встанет в 1500 us, если ещё не двигалась)\n"
    " a / d   -10 / +10 us\n"
    " A / D   -1  / +1  us  (точная подстройка)\n"
    " z / c   -50 / +50 us\n"
    " [       запомнить текущее как НАЧАЛО хода (min)\n"
    " ]       запомнить текущее как КОНЕЦ хода (max)\n"
    " m       в середину (1500, или между min/max если заданы)\n"
    " g       плавный прогон min->max->min для проверки\n"
    " o       отпустить выбранную серву (без сигнала)\n"
    " O       отпустить все\n"
    " p       таблица результатов + готовые массивы для кода\n"
    " i       скан I2C (проверка связи с PCA9685)\n"
    " h       эта справка\n"));
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.beginTransmission(PCA_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.printf("PCA9685 не найдена на 0x%02X (SDA=%d SCL=%d)! Проверь проводку.\n", PCA_ADDR, SDA_PIN, SCL_PIN);
  }
  pca.begin();
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(50);
  for (int i = 0; i < NUM_SERVOS; i++) { posUs[i] = START_US; minUs[i] = maxUs[i] = 0; release(i); }
  help();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c >= '1' && c < '1' + NUM_SERVOS) {
    sel = c - '1';
    Serial.printf(">> выбрана S%d (канал %d)\n", sel + 1, CHANNEL[sel]);
    writeUs(sel, posUs[sel]);
    return;
  }
  if (c == 'h') { help(); return; }
  if (c == 'p') { printTable(); return; }
  if (c == 'i') { scanI2C(); return; }
  if (c == 'O') { for (int i = 0; i < NUM_SERVOS; i++) release(i); Serial.println("все отпущены"); return; }
  if (c == '\r' || c == '\n' || c == ' ') return;
  if (sel < 0) { Serial.println("сначала выбери серву 1..6"); return; }

  switch (c) {
    case 'a': writeUs(sel, posUs[sel] - 10); break;
    case 'd': writeUs(sel, posUs[sel] + 10); break;
    case 'A': writeUs(sel, posUs[sel] - 1);  break;
    case 'D': writeUs(sel, posUs[sel] + 1);  break;
    case 'z': writeUs(sel, posUs[sel] - 50); break;
    case 'c': writeUs(sel, posUs[sel] + 50); break;
    case '[': minUs[sel] = posUs[sel]; Serial.printf("S%d НАЧАЛО = %d us\n", sel + 1, minUs[sel]); break;
    case ']': maxUs[sel] = posUs[sel]; Serial.printf("S%d КОНЕЦ  = %d us\n", sel + 1, maxUs[sel]); break;
    case 'm': writeUs(sel, (minUs[sel] && maxUs[sel]) ? (minUs[sel] + maxUs[sel]) / 2 : 1500); break;
    case 'g': sweep(sel); break;
    case 'o': release(sel); Serial.printf("S%d отпущена\n", sel + 1); break;
    default: Serial.printf("? '%c' — h для справки\n", c);
  }
}
