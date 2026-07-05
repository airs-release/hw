/*
 * AIRS 노드 통합 스케치 — 백업(dht22.ino) 기반 + SCD41 + BLE 프로비저닝
 *
 * 백업 원본(백업/dht22/dht22.ino) 구조를 그대로 살리고 다음을 추가:
 *   - SCD41(I2C): CO2 + 온도 + 습도
 *   - Wi-Fi: 기본 BLE 프로비저닝(8차 회의 결정). 앱이 SSID/PW를 BLE로 전달 → 접속.
 *   - node-off: airs/node/<id>/cmd 구독
 *   - 발행: airs/node/<id>/dht22 (기존 호환) + airs/node/<id>/telemetry (DHT22+SCD41 통합)
 *
 * 대상: Arduino IDE + Arduino-ESP32 코어 v3.x, Board "ESP32C3 Dev Module"
 * 라이브러리(Library Manager): "DHT sensor library"(Adafruit) + "Adafruit Unified Sensor" + "PubSubClient"
 * ※ 실물 플래시/실측은 직접.
 *
 * ── Wi-Fi 방식 선택 ─────────────────────────────────────────────
 *  기본: BLE 프로비저닝. 빠른 센서 테스트만 하려면 아래 한 줄 주석 해제(하드코딩 Wi-Fi):
 */
// #define USE_HARDCODED_WIFI     // info.h 의 ssid/password 로 접속(BLE 미사용)

#include <Adafruit_Sensor.h>
#include "DHT.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#ifndef USE_HARDCODED_WIFI
  #include "WiFiProv.h"
#endif
#ifdef USE_HARDCODED_WIFI
  #include "info.h"               // const char* ssid, password  (info_sample.h 복사해서 만들 것)
#endif

// ─────────── 설정 ───────────
#define DHTPIN   4                // 백업과 동일
#define DHTTYPE  DHT22
#define I2C_SDA  5                // SCD41용. 부트 스트랩(2/8/9)·DHT(4) 회피
#define I2C_SCL  6
const char* mqtt_server = "sogang-airs.iptime.org";   // 백업과 동일(라즈베리파이 DDNS)
const int   mqtt_port   = 1883;
const char* node_id     = "node_01";
const char* PROV_POP    = "airs-pop";                  // BLE 프로비저닝 PoP
const unsigned long SEND_MS = 5000;

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);
char topicCmd[48];
volatile bool wifiUp = false;
volatile bool nodeActive = true;
unsigned long lastSend = 0;

String topic(String type) { return "airs/node/" + String(node_id) + "/" + type; }

// ═══════════ SCD41 (I2C / Wire) ═══════════
static const uint8_t SCD_ADDR = 0x62;
static uint8_t crc8(const uint8_t* d, int n) {          // Sensirion CRC8
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) { c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1); }
  return c;
}
static bool scdCmd(uint16_t cmd) {
  Wire.beginTransmission(SCD_ADDR);
  Wire.write((uint8_t)(cmd >> 8)); Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}
void scdInit() { Wire.begin(I2C_SDA, I2C_SCL); scdCmd(0x3F86); delay(500); }   // stop periodic
void scdStartPeriodic() { scdCmd(0x21B1); delay(1); }                          // start periodic
bool scdRead(uint16_t* co2, float* tC, float* rh) {
  if (!scdCmd(0xE4B8)) return false; delay(1);                                 // data ready?
  if (Wire.requestFrom((int)SCD_ADDR, 3) != 3) return false;
  uint8_t r[3]; for (int i = 0; i < 3; i++) r[i] = Wire.read();
  if ((((r[0] << 8) | r[1]) & 0x07FF) == 0) return false;                      // 아직 준비 안 됨
  if (!scdCmd(0xEC05)) return false; delay(1);                                 // read measurement
  if (Wire.requestFrom((int)SCD_ADDR, 9) != 9) return false;
  uint8_t d[9]; for (int i = 0; i < 9; i++) d[i] = Wire.read();
  if (crc8(&d[0],2)!=d[2] || crc8(&d[3],2)!=d[5] || crc8(&d[6],2)!=d[8]) return false;
  *co2 = (d[0] << 8) | d[1];
  *tC  = -45.0f + 175.0f * ((d[3] << 8) | d[4]) / 65535.0f;
  *rh  = 100.0f * ((d[6] << 8) | d[7]) / 65535.0f;
  return true;
}

// ═══════════ MQTT node-off ═══════════
void mqttCallback(char* t, byte* payload, unsigned int len) {
  String msg; for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  if (String(t) == String(topicCmd)) {
    if (msg.indexOf("off") >= 0) { nodeActive = false; Serial.println("🛑 node-off (발행 중지; TODO: IR/전원)"); }
    else if (msg.indexOf("on") >= 0) { nodeActive = true; Serial.println("▶️ node-on"); }
  }
}

// ═══════════ Wi-Fi ═══════════
#ifdef USE_HARDCODED_WIFI
void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("WiFi 연결 중");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n✅ WiFi: " + WiFi.localIP().toString());
  wifiUp = true;
}
#else
void SysProvEvent(arduino_event_t* e) {
  switch (e->event_id) {
    case ARDUINO_EVENT_PROV_START:
      Serial.println("🔵 BLE 프로비저닝 시작 — 앱에서 AIRS-node01 연결 후 Wi-Fi 전달"); break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      Serial.printf("🔵 Wi-Fi 자격 수신: SSID=%s\n", (const char*)e->event_info.prov_cred_recv.ssid); break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      Serial.println("❌ Wi-Fi 접속 실패(자격 오류)"); break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("✅ Wi-Fi 접속 성공"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("✅ IP: "); Serial.println(WiFi.localIP()); wifiUp = true; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiUp = false; break;
    default: break;
  }
}
#endif

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("MQTT 연결 중...");
    if (client.connect(node_id)) {
      client.subscribe(topicCmd);
      Serial.println("✅ MQTT 연결 완료!");
    } else { Serial.printf("❌ rc=%d\n", client.state()); delay(3000); }
  }
}

// ═══════════ setup / loop ═══════════
void setup() {
  Serial.begin(115200); delay(1000);
  snprintf(topicCmd, sizeof(topicCmd), "airs/node/%s/cmd", node_id);
  dht.begin();
  scdInit(); scdStartPeriodic();
  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(384);          // 통합 telemetry payload 여유
  client.setCallback(mqttCallback);

#ifdef USE_HARDCODED_WIFI
  connectWiFi();
#else
  WiFi.onEvent(SysProvEvent);
  uint8_t uuid[16] = { 0xb4,0xdf,0x5a,0x1c,0x3f,0x6b,0xf4,0xbf,0xea,0x4a,0x82,0x03,0x04,0x90,0x1a,0x02 };
  Serial.println("🔵 BLE 프로비저닝 대기 (ESP BLE Provisioning 앱)");
  // 코어 2.x는 WIFI_PROV_*, 3.x는 NETWORK_PROV_* — 버전 자동 대응
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  WiFiProv.beginProvision(NETWORK_PROV_SCHEME_BLE, NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM,
                          NETWORK_PROV_SECURITY_1, PROV_POP, "AIRS-node01", NULL, uuid);
#else
  WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
                          WIFI_PROV_SECURITY_1, PROV_POP, "AIRS-node01", NULL, uuid);
#endif
#endif
}

void loop() {
  if (wifiUp) { if (!client.connected()) connectMQTT(); client.loop(); }
  if (!(wifiUp && nodeActive) || millis() - lastSend < SEND_MS) return;
  lastSend = millis();

  // DHT22
  float temp = dht.readTemperature();
  float humi = dht.readHumidity();
  bool dhtOK = !(isnan(temp) || isnan(humi));
  // SCD41
  uint16_t co2 = 0; float sT = 0, sH = 0;
  bool scdOK = scdRead(&co2, &sT, &sH);

  if (!dhtOK && !scdOK) { Serial.println("❌ 두 센서 모두 읽기 실패"); return; }

  // 1) 기존 호환: dht22 토픽 {"temperature","humidity"}
  if (dhtOK) {
    char p[64];
    snprintf(p, sizeof(p), "{\"temperature\":%.1f,\"humidity\":%.1f}", temp, humi);
    client.publish(topic("dht22").c_str(), p);
  }

  // 2) 통합 telemetry (DHT22 우선, 없으면 SCD41 값으로 대체)
  float tc = dhtOK ? temp : sT;
  float hp = dhtOK ? humi : sH;
  char tp[256];
  snprintf(tp, sizeof(tp),
    "{\"node_id\":\"%s\",\"temperature_c\":%.1f,\"humidity_pct\":%.1f,\"co2_ppm\":%u,"
    "\"scd41_temperature_c\":%.1f,\"scd41_humidity_pct\":%.1f,"
    "\"sensor_status\":{\"dht22\":\"%s\",\"scd41\":\"%s\"}}",
    node_id, tc, hp, (unsigned)co2, sT, sH,
    dhtOK ? "OK" : "NO_DATA", scdOK ? "OK" : "NO_DATA");
  client.publish(topic("telemetry").c_str(), tp);

  Serial.printf("📤 dht(%s) T=%.1f H=%.1f | scd(%s) CO2=%u T=%.1f H=%.1f\n",
                dhtOK ? "OK" : "X", temp, humi, scdOK ? "OK" : "X", co2, sT, sH);
}
