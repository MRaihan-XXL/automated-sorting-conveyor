/*
  Smart Conveyor - ESP32 + 1 stepper 28BYJ-48 (driver ULN2003)
  - IR barrier  : hitung barang masuk
  - HC-SR04     : ukur tinggi barang (QC)
  - LDR + LED   : baca kecerahan/warna kemasan (QC)
  - Stepper     : penggerak conveyor (kecepatan tetap 800 langkah/detik)
  - Buzzer      : alarm saat barang reject
  - MQTT        : publish data, subscribe kontrol dari dashboard

  Library: PubSubClient, ArduinoJson (v6.x), AccelStepper
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <AccelStepper.h>
#include <stdio.h>

// ====== WiFi & MQTT ======
const char* WIFI_SSID   = "MASUKKAN_SSID_WIFI";
const char* WIFI_PASS   = "MASUKKAN_PASSWORD_WIFI";
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;

// Broker publik
const String PREFIX       = "conveyortesst";
const String TOPIC_DATA   = PREFIX + "/data";
const String TOPIC_CTRL   = PREFIX + "/control"; //{"cmd": "conveyor","state": "off"}
const String TOPIC_STATUS = PREFIX + "/status";
const String TOPIC_SERIAL = PREFIX + "/serial";

// ====== Pin ======
const int PIN_IR     = 27;  // sensor IR barrier (aktif LOW saat ada barang)
const int PIN_TRIG   = 5;
const int PIN_ECHO   = 18;  // pakai pembagi tegangan 1k/2k dari Echo
const int PIN_LDR    = 34;  // ADC1 (aman dipakai saat WiFi aktif)
const int PIN_LED    = 4;
const int PIN_BUZZER = 25;

// Driver ULN2003 (stepper 28BYJ-48)
const int PIN_IN1 = 16;
const int PIN_IN2 = 17;
const int PIN_IN3 = 19;
const int PIN_IN4 = 21;

// ====== Parameter stepper ======
const float STEPPER_SPEED = 800.0;  // langkah/detik

// ====== Parameter QC (sesuaikan saat kalibrasi) ======
const float SENSOR_HEIGHT_CM = 30.0;  // jarak sensor ke permukaan belt
const float TINGGI_MIN_CM    = 8.0;   // tinggi barang standar
const float TINGGI_MAX_CM    = 12.0;
const int   LDR_MIN          = 1500;  // rentang kecerahan yang dianggap sesuai (0-4095)
const int   LDR_MAX          = 3000;

// ====== Objek & state ======
WiFiClient espClient;
PubSubClient client(espClient);

// Urutan pin untuk 28BYJ-48 via ULN2003: IN1, IN3, IN2, IN4
AccelStepper m1(AccelStepper::HALF4WIRE, PIN_IN1, PIN_IN3, PIN_IN2, PIN_IN4);

void logSerial(const char* message) {
  Serial.println(message);
  if (client.connected()) {
    client.publish(TOPIC_SERIAL.c_str(), message);
  }
}

bool conveyorOn = true;

unsigned long totalMasuk = 0, totalNormal = 0, totalReject = 0;
float lastTinggi = 0;
int   lastLdr = 0;
const char* lastHasil = "-";

int lastIrState = HIGH;
unsigned long lastIrMs = 0;
unsigned long lastReconnectMs = 0;

// ---------- Delay yang tetap memutar stepper ----------
// Menjaga stepper tetap bergerak selama delay saat conveyor aktif.
void stepDelay(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    m1.runSpeed();
  }
}

// ---------- Stepper conveyor ----------
void applyConveyor() {
  if (conveyorOn) {
    m1.enableOutputs();
    m1.setSpeed(STEPPER_SPEED);
  } else {
    m1.setSpeed(0);
    m1.disableOutputs();  // matikan kumparan agar tidak panas
  }
}

// ---------- MQTT ----------
void publishStatus() {
  StaticJsonDocument<48> doc;
  doc["state"] = conveyorOn ? "on" : "off";
  char buf[48];
  serializeJson(doc, buf);
  client.publish(TOPIC_STATUS.c_str(), buf, true);
}

void publishData() {
  StaticJsonDocument<192> doc;
  doc["masuk"]  = totalMasuk;
  doc["normal"] = totalNormal;
  doc["reject"] = totalReject;
  doc["tinggi"] = lastTinggi;
  doc["ldr"]    = lastLdr;
  doc["hasil"]  = lastHasil;
  char buf[192];
  size_t n = serializeJson(doc, buf);
  client.publish(TOPIC_DATA.c_str(), buf, n);
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, len)) return;

  if (doc["cmd"] == "conveyor") {
    conveyorOn = (doc["state"] == "on");
    applyConveyor();
    publishStatus();
    char message[40];
    snprintf(message, sizeof(message), "Kontrol: conveyor %s", conveyorOn ? "ON" : "OFF");
    logSerial(message);
  }
}

void connectWifi() {
  logSerial("Menghubungkan WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    stepDelay(300);   // conveyor tetap jalan selagi menunggu WiFi
    Serial.print(".");
  }
  logSerial("WiFi terhubung");
}

void reconnectMqtt() {
  if (client.connected()) return;
  if (millis() - lastReconnectMs < 5000) return;
  lastReconnectMs = millis();

  String id = "conveyor-" + String((uint32_t)esp_random(), HEX);
  logSerial("Menghubungkan MQTT...");
  if (client.connect(id.c_str())) {
    logSerial("Menghubungkan MQTT... ok");
    client.subscribe(TOPIC_CTRL.c_str());
    publishStatus();
    publishData();
  } else {
    char message[48];
    snprintf(message, sizeof(message), "Menghubungkan MQTT gagal (rc=%d)", client.state());
    logSerial(message);
  }
}

// ---------- Sensor ----------
float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long dur = pulseIn(PIN_ECHO, HIGH, 30000);
  if (dur == 0) return SENSOR_HEIGHT_CM;  // tidak ada pantulan: anggap belt kosong
  return dur * 0.0343 / 2.0;
}

float avgDistanceCm(int n = 5) {
  float sum = 0;
  for (int i = 0; i < n; i++) { sum += readDistanceCm(); stepDelay(10); }
  return sum / n;
}

int avgLdr(int n = 8) {
  long sum = 0;
  for (int i = 0; i < n; i++) { sum += analogRead(PIN_LDR); stepDelay(5); }
  return sum / n;
}

void beepReject() {
  // buzzer pasif: nada ~1 kHz selama ~0,3 detik, stepper tetap dijalankan
  for (int i = 0; i < 300; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delayMicroseconds(500); m1.runSpeed();
    digitalWrite(PIN_BUZZER, LOW);  delayMicroseconds(500); m1.runSpeed();
  }
}

// ---------- Proses satu barang ----------
void processItem() {
  totalMasuk++;

  stepDelay(1000);
  m1.setSpeed(0);
  m1.disableOutputs();

  // QC tinggi
  float jarak = avgDistanceCm();
  lastTinggi = SENSOR_HEIGHT_CM - jarak;
  bool tinggiOK = (lastTinggi >= TINGGI_MIN_CM && lastTinggi <= TINGGI_MAX_CM);

  // QC warna/kecerahan (LED menyinari barang, LDR membaca pantulan)
  digitalWrite(PIN_LED, HIGH);
  stepDelay(50);
  lastLdr = avgLdr();
  digitalWrite(PIN_LED, LOW);
  applyConveyor();
  bool warnaOK = (lastLdr >= LDR_MIN && lastLdr <= LDR_MAX);

  if (tinggiOK && warnaOK) {
    totalNormal++;
    lastHasil = "normal";
  } else {
    totalReject++;
    lastHasil = "reject";
    beepReject();
  }

  char message[160];
  snprintf(message, sizeof(message),
           "Masuk=%lu Normal=%lu Reject=%lu | tinggi=%.1f cm (%s) | ldr=%d (%s) -> %s",
           totalMasuk, totalNormal, totalReject,
           lastTinggi, tinggiOK ? "OK" : "X",
           lastLdr, warnaOK ? "OK" : "X", lastHasil);
  logSerial(message);

  publishData();
}

// ---------- Setup & loop ----------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_IR, INPUT_PULLUP);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  analogReadResolution(12);

  // setMaxSpeed wajib: bawaan AccelStepper hanya 1 langkah/detik,
  // sehingga setSpeed() akan dibatasi ke nilai itu jika tidak diatur.
  m1.setMaxSpeed(STEPPER_SPEED);
  applyConveyor();   // conveyor langsung jalan, sebelum WiFi tersambung

  connectWifi();
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(onMessage);
}

void loop() {
  m1.runSpeed();   // putar stepper (harus dipanggil sesering mungkin)

  if (WiFi.status() != WL_CONNECTED) connectWifi();
  reconnectMqtt();
  client.loop();

  // Deteksi barang masuk: sisi turun (HIGH -> LOW) + debounce
  int ir = digitalRead(PIN_IR);
  if (lastIrState == HIGH && ir == LOW && millis() - lastIrMs > 300) {
    lastIrMs = millis();
    if (conveyorOn) processItem();   // barang hanya dihitung saat conveyor jalan
  }
  lastIrState = ir;

  m1.runSpeed();
}
