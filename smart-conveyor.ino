/*
  Smart Conveyor - prototipe Wokwi (ESP32)
  - IR barrier  : dihitung sebagai barang masuk (disimulasikan tombol)
  - HC-SR04     : ukur tinggi barang (QC)
  - LDR + LED   : baca kecerahan/warna kemasan (QC)
  - 2x servo    : penggerak conveyor (continuous: 90 = berhenti)
  - Buzzer      : alarm saat barang reject
  - MQTT        : publish data, subscribe kontrol dari dashboard
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ====== WiFi & MQTT ======
const char* WIFI_SSID   = "MosoZydraGyrottaZao";
const char* WIFI_PASS   = "stone park";
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;

// Broker publik dipakai banyak orang: ganti PREFIX dengan nama unik
// (dashboard harus memakai prefix yang sama).
const String PREFIX       = "conveyortesst";
const String TOPIC_DATA   = PREFIX + "/data";
const String TOPIC_CTRL   = PREFIX + "/control"; //{"cmd": "conveyor","state": "off","speed": 45}
const String TOPIC_STATUS = PREFIX + "/status";

// ====== Pin ======
const int PIN_IR     = 27;  // tombol ke GND (simulasi sensor IR barrier)
const int PIN_TRIG   = 5;
const int PIN_ECHO   = 18;
const int PIN_LDR    = 34;  // ADC1 (aman dipakai saat WiFi aktif)
const int PIN_LED    = 4;
const int PIN_SERVO1 = 16;
const int PIN_SERVO2 = 17;
const int PIN_BUZZER = 25;

// ====== Parameter QC (sesuaikan saat kalibrasi) ======
const float SENSOR_HEIGHT_CM = 30.0;  // jarak sensor ke permukaan belt
const float TINGGI_MIN_CM    = 8.0;   // tinggi barang standar
const float TINGGI_MAX_CM    = 12.0;
const int   LDR_MIN          = 1500;  // rentang kecerahan yang dianggap sesuai (0-4095)
const int   LDR_MAX          = 3000;

// ====== Objek & state ======
WiFiClient espClient;
PubSubClient client(espClient);
Servo servo1, servo2;

bool conveyorOn = true;
int  conveyorSpeed = 30;  // 0-90, selisih dari titik berhenti 90 derajat

unsigned long totalMasuk = 0, totalNormal = 0, totalReject = 0;
float lastTinggi = 0;
int   lastLdr = 0;
const char* lastHasil = "-";

int lastIrState = HIGH;
unsigned long lastIrMs = 0;
unsigned long lastReconnectMs = 0;

// ---------- Servo conveyor ----------
void applyConveyor() {
  if (conveyorOn && conveyorSpeed > 0) {
    if (!servo1.attached()) servo1.attach(PIN_SERVO1, 500, 2400);
    if (!servo2.attached()) servo2.attach(PIN_SERVO2, 500, 2400);
    servo1.write(90 + conveyorSpeed);
    servo2.write(90 - conveyorSpeed);  // dibalik agar belt searah
  } else {
    // servo continuous: 90 = berhenti, kirim dulu sebelum sinyal dilepas
    if (servo1.attached()) servo1.write(90);
    if (servo2.attached()) servo2.write(90);
    delay(50);
    servo1.detach();
    servo2.detach();
  }
}

// ---------- MQTT ----------
void publishStatus() {
  StaticJsonDocument<96> doc;
  doc["state"] = conveyorOn ? "on" : "off";
  doc["speed"] = conveyorSpeed;
  char buf[96];
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
    conveyorOn    = (doc["state"] == "on");
    conveyorSpeed = constrain((int)(doc["speed"] | 30), 0, 90);
    applyConveyor();
    publishStatus();
    Serial.printf("Kontrol: conveyor %s, speed %d\n",
                  conveyorOn ? "ON" : "OFF", conveyorSpeed);
  }
}

void connectWifi() {
  Serial.print("Menghubungkan WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" terhubung");
}

void reconnectMqtt() {
  if (client.connected()) return;
  if (millis() - lastReconnectMs < 5000) return;
  lastReconnectMs = millis();

  String id = "conveyor-" + String((uint32_t)esp_random(), HEX);
  Serial.print("Menghubungkan MQTT...");
  if (client.connect(id.c_str())) {
    Serial.println(" ok");
    client.subscribe(TOPIC_CTRL.c_str());
    publishStatus();
    publishData();
  } else {
    Serial.printf(" gagal (rc=%d)\n", client.state());
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
  for (int i = 0; i < n; i++) { sum += readDistanceCm(); delay(10); }
  return sum / n;
}

int avgLdr(int n = 8) {
  long sum = 0;
  for (int i = 0; i < n; i++) { sum += analogRead(PIN_LDR); delay(5); }
  return sum / n;
}

void beepReject() {
  // buzzer pasif: bangkitkan nada ~1 kHz selama ~0,3 detik
  for (int i = 0; i < 300; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delayMicroseconds(500);
    digitalWrite(PIN_BUZZER, LOW);  delayMicroseconds(500);
  }
}

// ---------- Proses satu barang ----------
void processItem() {
  totalMasuk++;

  // QC tinggi
  float jarak = avgDistanceCm();
  lastTinggi = SENSOR_HEIGHT_CM - jarak;
  bool tinggiOK = (lastTinggi >= TINGGI_MIN_CM && lastTinggi <= TINGGI_MAX_CM);

  // QC warna/kecerahan (LED menyinari barang, LDR membaca pantulan)
  digitalWrite(PIN_LED, HIGH);
  delay(50);
  lastLdr = avgLdr();
  digitalWrite(PIN_LED, LOW);
  bool warnaOK = (lastLdr >= LDR_MIN && lastLdr <= LDR_MAX);

  if (tinggiOK && warnaOK) {
    totalNormal++;
    lastHasil = "normal";
  } else {
    totalReject++;
    lastHasil = "reject";
    beepReject();
  }

  Serial.printf("Masuk=%lu Normal=%lu Reject=%lu | tinggi=%.1f cm (%s) | ldr=%d (%s) -> %s\n",
                totalMasuk, totalNormal, totalReject,
                lastTinggi, tinggiOK ? "OK" : "X",
                lastLdr, warnaOK ? "OK" : "X", lastHasil);

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

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servo1.setPeriodHertz(50);
  servo2.setPeriodHertz(50);
  applyConveyor();

  connectWifi();
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(onMessage);
}

void loop() {
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
}
