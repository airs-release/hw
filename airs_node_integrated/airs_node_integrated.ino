/*
 * AIRS 노드 통합 스케치 — DHT22 + SCD41 + PIR/mmWave + IR(멀티브랜드 에어컨) + BLE + RSSI
 *   (2026-07 멀티브랜드 개정: IRac 범용 AC + 리모컨 1회 수신 브랜드 자동감지 + NVS 저장)
 *
 * 발행: airs/node/<id>/dht22 (호환) + airs/node/<id>/telemetry (통합)
 * 구독: airs/node/<id>/cmd — JSON 명령:
 *   {"cmd":"ac_learn"}                 리모컨 1회 수신 → 브랜드 자동감지·저장
 *   {"cmd":"ac_brand","name":"SAMSUNG_AC"}  브랜드 수동지정(LG/SAMSUNG_AC/CARRIER_AC/HAIER_AC/COOLIX/GREE/DAIKIN...)
 *   {"cmd":"ac_off"} / {"cmd":"ac_cool","temp":24} / {"cmd":"ac_heat","temp":24}
 *   {"cmd":"off"} / {"cmd":"on"}       노드 발행 중지/재개
 *
 * 라이브러리: DHT sensor library(Adafruit)+Adafruit Unified Sensor+PubSubClient+IRremoteESP8266
 * ⚠️ IRac(모든 AC 프로토콜)+BLE 라 용량 큼 → Partition = Huge APP 필수. BLE는 코어 2.0.17.
 *    (컴파일 용량 초과 시: 라이브러리 IRremoteESP8266.h에서 안 쓰는 프로토콜 SEND_/DECODE_ 주석)
 */
// #define USE_HARDCODED_WIFI     // info.h 의 ssid/password 로 접속(BLE 미사용)

#include <Adafruit_Sensor.h>
#include "DHT.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Preferences.h>          // 브랜드 설정 영구 저장(NVS)
// IR — 라이브러리: "IRremoteESP8266"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <IRac.h>                 // 범용 AC(멀티 브랜드) 송신
#ifndef USE_HARDCODED_WIFI
  #include "WiFiProv.h"
#endif
#ifdef USE_HARDCODED_WIFI
  #include "info.h"
#endif

// ─────────── 설정 ───────────
#define DHTPIN   4
#define DHTTYPE  DHT22
#define I2C_SDA  5
#define I2C_SCL  6
const char* mqtt_server = "sogang-airs.iptime.org";
const int   mqtt_port   = 1883;
const char* node_id     = "node_01";
const char* PROV_POP    = "airs-pop";
const unsigned long SEND_MS = 5000;

// 추가 센서/IR 핀
#define PIN_PIR     3     // HC-SR501 OUT
#define PIN_MMWAVE  10    // HLK-LD2410 OUT (재실)
#define PIN_IR_TX   7     // IR 송신(ELB030103 S핀 또는 TSAL6400 드라이버)
#define PIN_IR_RX   1     // TSOP38238 OUT (IR 수신/학습)

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);
char topicCmd[48];
volatile bool wifiUp = false;
volatile bool nodeActive = true;
unsigned long lastSend = 0;

bool pirLatched = false;
bool mmwaveNow  = false;

// ── IR: 범용 AC 송신 + 수신/학습 ──
IRac    ac(PIN_IR_TX);                      // 멀티 브랜드 AC 송신
IRrecv  irrecv(PIN_IR_RX, 1024, 15, true);  // IR 수신
decode_results irResults;
Preferences prefs;
decode_type_t acProtocol   = decode_type_t::LG;  // 현재 방의 에어컨 브랜드(기본 LG, 학습으로 갱신)
bool          acLearnBrand = false;              // ac_learn: 다음 수신을 브랜드로 저장
IRsend   irsend(PIN_IR_TX);                      // raw 재생용(IRac 미지원 브랜드: 위니아·파세코 등)
uint16_t rawBuf[256]; uint16_t rawLen = 0;       // 학습된 raw 타이밍(µs)
String   irLearnSlot = "";                       // 비면 학습 안 함, "off"/"cool" 등이면 다음 수신을 그 슬롯에 저장

String topic(String type) { return "airs/node/" + String(node_id) + "/" + type; }

// ═══════════ NVS: 브랜드 저장/복원 ═══════════
void saveBrand(decode_type_t p) { prefs.begin("airs", false); prefs.putInt("acproto", (int)p); prefs.end(); }
void loadBrand() { prefs.begin("airs", true); acProtocol = (decode_type_t)prefs.getInt("acproto", (int)decode_type_t::LG); prefs.end(); }
// raw 명령(미지원 브랜드용) — 슬롯별 NVS 블롭 저장/재생
void saveRawSlot(const String& slot) { prefs.begin("airs", false); prefs.putBytes(("raw_" + slot).c_str(), rawBuf, rawLen * 2); prefs.end(); }
uint16_t loadRawSlot(const String& slot) {
  prefs.begin("airs", true);
  size_t n = prefs.getBytesLength(("raw_" + slot).c_str());
  if (n > 0 && n <= sizeof(rawBuf)) { prefs.getBytes(("raw_" + slot).c_str(), rawBuf, n); prefs.end(); return n / 2; }
  prefs.end(); return 0;
}
void irRaw(const String& slot) {
  uint16_t len = loadRawSlot(slot);
  if (!len) { Serial.printf("⚠ raw slot '%s' 비어있음 (ir_learn 먼저)\n", slot.c_str()); return; }
  irsend.sendRaw(rawBuf, len, 38);   // 38kHz 재생
  Serial.printf("📡 raw 재생: '%s' (%d)\n", slot.c_str(), len);
}

// ═══════════ SCD41 (I2C) ═══════════
static const uint8_t SCD_ADDR = 0x62;
static uint8_t crc8(const uint8_t* d, int n) {
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
void scdInit() { Wire.begin(I2C_SDA, I2C_SCL); scdCmd(0x3F86); delay(500); }
void scdStartPeriodic() { scdCmd(0x21B1); delay(1); }
bool scdRead(uint16_t* co2, float* tC, float* rh) {
  if (!scdCmd(0xE4B8)) return false; delay(1);
  if (Wire.requestFrom((int)SCD_ADDR, 3) != 3) return false;
  uint8_t r[3]; for (int i = 0; i < 3; i++) r[i] = Wire.read();
  if ((((r[0] << 8) | r[1]) & 0x07FF) == 0) return false;
  if (!scdCmd(0xEC05)) return false; delay(1);
  if (Wire.requestFrom((int)SCD_ADDR, 9) != 9) return false;
  uint8_t d[9]; for (int i = 0; i < 9; i++) d[i] = Wire.read();
  if (crc8(&d[0],2)!=d[2] || crc8(&d[3],2)!=d[5] || crc8(&d[6],2)!=d[8]) return false;
  *co2 = (d[0] << 8) | d[1];
  *tC  = -45.0f + 175.0f * ((d[3] << 8) | d[4]) / 65535.0f;
  *rh  = 100.0f * ((d[6] << 8) | d[7]) / 65535.0f;
  return true;
}

// ═══════════ IR: 멀티 브랜드 에어컨 송신 ═══════════
bool acSupported() { return IRac::isProtocolSupported(acProtocol); }

// 공통 상태 채우기(전원/모드/온도) — 나머지는 무난한 기본값
void acFill(bool power, stdAc::opmode_t mode, uint8_t temp) {
  ac.next.protocol = acProtocol;
  ac.next.model    = -1;                         // 브랜드 기본 모델
  ac.next.power    = power;
  ac.next.mode     = mode;
  ac.next.celsius  = true;
  ac.next.degrees  = temp;
  ac.next.fanspeed = stdAc::fanspeed_t::kAuto;
  ac.next.swingv   = stdAc::swingv_t::kOff;
  ac.next.swingh   = stdAc::swingh_t::kOff;
  ac.next.light = false; ac.next.beep = false; ac.next.econo = false;
  ac.next.filter = false; ac.next.turbo = false; ac.next.quiet = false;
  ac.next.clean = false; ac.next.sleep = -1; ac.next.clock = -1;
}
void acOff() {
  if (!acSupported()) { Serial.printf("⚠ %s는 IRac 미지원 → 학습(raw) 방식 필요\n", typeToString(acProtocol).c_str()); return; }
  acFill(false, stdAc::opmode_t::kCool, 24); ac.sendAc();
  Serial.printf("📡 %s AC OFF 전송\n", typeToString(acProtocol).c_str());
}
void acCool(uint8_t t) {
  if (!acSupported()) { Serial.printf("⚠ %s 미지원 → raw 필요\n", typeToString(acProtocol).c_str()); return; }
  acFill(true, stdAc::opmode_t::kCool, t); ac.sendAc();
  Serial.printf("📡 %s COOL %d℃ 전송\n", typeToString(acProtocol).c_str(), t);
}
void acHeat(uint8_t t) {
  if (!acSupported()) { Serial.printf("⚠ %s 미지원 → raw 필요\n", typeToString(acProtocol).c_str()); return; }
  acFill(true, stdAc::opmode_t::kHeat, t); ac.sendAc();
  Serial.printf("📡 %s HEAT %d℃ 전송\n", typeToString(acProtocol).c_str(), t);
}

// ═══════════ IR 수신: 브랜드 자동감지 / 학습 로그 ═══════════
void irLearnPoll() {
  if (!irrecv.decode(&irResults)) return;
  if (irLearnSlot.length()) {                      // ① raw 학습(미지원 브랜드): 신호 통째로 저장
    uint16_t len = getCorrectedRawLength(&irResults);
    uint16_t* arr = resultToRawArray(&irResults);
    if (len > 0 && len <= 256) { memcpy(rawBuf, arr, len * 2); rawLen = len; saveRawSlot(irLearnSlot);
      Serial.printf("💾 raw 저장: slot='%s' (%d개) proto=%s\n", irLearnSlot.c_str(), len, typeToString(irResults.decode_type).c_str()); }
    else Serial.println("⚠ raw 길이 초과(256)");
    delete[] arr; irLearnSlot = "";
  } else if (acLearnBrand && irResults.decode_type != decode_type_t::UNKNOWN) {  // ② 브랜드 자동감지
    acProtocol = irResults.decode_type;
    saveBrand(acProtocol); acLearnBrand = false;
    Serial.printf("✅ 브랜드 자동감지: %s | 지원명령=%s\n", typeToString(acProtocol).c_str(),
                  IRac::isProtocolSupported(acProtocol) ? "off·cool·heat·온도·풍량" : "미지원 → ir_learn(raw) 권장");
  } else {
    Serial.print("── IR 수신: "); Serial.println(resultToHumanReadableBasic(&irResults));
  }
  irrecv.resume();
}

// ═══════════ MQTT ═══════════
static uint8_t parseTemp(const String& msg, uint8_t def) {
  int p = msg.indexOf("temp"); if (p < 0) return def;
  int c = msg.indexOf(':', p); if (c < 0) return def;
  int v = msg.substring(c + 1).toInt(); return (v >= 16 && v <= 30) ? (uint8_t)v : def;
}
static String parseStr(const String& msg, const char* key, const char* def) {  // {"...":"값"} 문자열 추출
  int p = msg.indexOf(key); if (p < 0) return def;
  int c = msg.indexOf(':', p); if (c < 0) return def;
  int a = msg.indexOf('"', c); if (a < 0) return def;
  int b = msg.indexOf('"', a + 1); if (b < 0) return def;
  return msg.substring(a + 1, b);
}
void mqttCallback(char* t, byte* payload, unsigned int len) {
  String msg; for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  if (String(t) != String(topicCmd)) return;
  if (msg.indexOf("ac_learn") >= 0) { acLearnBrand = true; Serial.println("👂 브랜드 학습: 리모컨을 IR 센서에 대고 아무 버튼 누르세요"); return; }
  if (msg.indexOf("ac_brand") >= 0) {                         // {"cmd":"ac_brand","name":"SAMSUNG_AC"}
    int a = msg.indexOf('"', msg.indexOf("name") + 4); a = msg.indexOf('"', a + 1);
    int b = msg.indexOf('"', a + 1);
    if (a >= 0 && b > a) {
      decode_type_t d = strToDecodeType(msg.substring(a + 1, b).c_str());
      if (d != decode_type_t::UNKNOWN) { acProtocol = d; saveBrand(d); Serial.printf("✅ 브랜드=%s 저장\n", typeToString(d).c_str()); }
      else Serial.println("❌ 알 수 없는 브랜드명");
    } return;
  }
  if (msg.indexOf("ir_learn") >= 0) { irLearnSlot = parseStr(msg, "slot", "off"); Serial.printf("👂 raw 학습: 리모컨의 '%s' 버튼을 IR 센서에 대고 누르세요\n", irLearnSlot.c_str()); return; }
  if (msg.indexOf("ir_raw")   >= 0) { irRaw(parseStr(msg, "slot", "off")); return; }   // 미지원 브랜드 재생
  if (msg.indexOf("ac_off")  >= 0) { acOff(); return; }
  if (msg.indexOf("ac_cool") >= 0) { acCool(parseTemp(msg, 24)); return; }
  if (msg.indexOf("ac_heat") >= 0) { acHeat(parseTemp(msg, 24)); return; }
  if (msg.indexOf("off") >= 0) { nodeActive = false; Serial.println("🛑 node-off"); }
  else if (msg.indexOf("on") >= 0) { nodeActive = true; Serial.println("▶️ node-on"); }
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
    case ARDUINO_EVENT_PROV_START:        Serial.println("🔵 BLE 프로비저닝 시작 — 앱에서 AIRS-node01 연결"); break;
    case ARDUINO_EVENT_PROV_CRED_RECV:    Serial.printf("🔵 Wi-Fi 자격 수신: SSID=%s\n", (const char*)e->event_info.prov_cred_recv.ssid); break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:    Serial.println("❌ Wi-Fi 접속 실패"); break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS: Serial.println("✅ Wi-Fi 접속 성공"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:   Serial.print("✅ IP: "); Serial.println(WiFi.localIP()); wifiUp = true; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: wifiUp = false; break;
    default: break;
  }
}
#endif

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("MQTT 연결 중...");
    if (client.connect(node_id)) { client.subscribe(topicCmd); Serial.println("✅ MQTT 연결 완료!"); }
    else { Serial.printf("❌ rc=%d\n", client.state()); delay(3000); }
  }
}

// ═══════════ setup / loop ═══════════
void setup() {
  Serial.begin(115200); delay(1000);
  snprintf(topicCmd, sizeof(topicCmd), "airs/node/%s/cmd", node_id);
  loadBrand();                                   // 저장된 에어컨 브랜드 복원
  dht.begin();
  scdInit(); scdStartPeriodic();
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_MMWAVE, INPUT_PULLDOWN);           // LD2410 미연결 시 0으로(연결 후엔 센서가 구동)
  irrecv.enableIRIn();                           // IR 수신
  irsend.begin();                                // IR 송신(raw 재생용)
  Serial.printf("🌡 센서 + IR 준비 | 현재 에어컨 브랜드=%s\n", typeToString(acProtocol).c_str());
  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(512);
  client.setCallback(mqttCallback);

#ifdef USE_HARDCODED_WIFI
  connectWiFi();
#else
  WiFi.onEvent(SysProvEvent);
  uint8_t uuid[16] = { 0xb4,0xdf,0x5a,0x1c,0x3f,0x6b,0xf4,0xbf,0xea,0x4a,0x82,0x03,0x04,0x90,0x1a,0x02 };
  Serial.println("🔵 BLE 프로비저닝 대기 (ESP BLE Provisioning 앱)");
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

  if (digitalRead(PIN_PIR) == HIGH) pirLatched = true;
  mmwaveNow = (digitalRead(PIN_MMWAVE) == HIGH);
  irLearnPoll();

  if (!(wifiUp && nodeActive) || millis() - lastSend < SEND_MS) return;
  lastSend = millis();

  float temp = dht.readTemperature();
  float humi = dht.readHumidity();
  bool dhtOK = !(isnan(temp) || isnan(humi));
  uint16_t co2 = 0; float sT = 0, sH = 0;
  bool scdOK = scdRead(&co2, &sT, &sH);
  if (!dhtOK && !scdOK) { Serial.println("❌ 두 센서 모두 읽기 실패"); return; }

  if (dhtOK) {
    char p[64];
    snprintf(p, sizeof(p), "{\"temperature\":%.1f,\"humidity\":%.1f}", temp, humi);
    client.publish(topic("dht22").c_str(), p);
  }

  float tc = dhtOK ? temp : sT;
  float hp = dhtOK ? humi : sH;
  int   pir  = pirLatched ? 1 : 0;
  int   mmw  = mmwaveNow  ? 1 : 0;
  long  rssi = WiFi.RSSI();
  pirLatched = false;
  char tp[320];
  snprintf(tp, sizeof(tp),
    "{\"node_id\":\"%s\",\"temperature_c\":%.1f,\"humidity_pct\":%.1f,\"co2_ppm\":%u,"
    "\"scd41_temperature_c\":%.1f,\"scd41_humidity_pct\":%.1f,"
    "\"pir_detected\":%d,\"mmwave_detected\":%d,\"wifi_signal_dbm\":%ld,"
    "\"sensor_status\":{\"dht22\":\"%s\",\"scd41\":\"%s\"}}",
    node_id, tc, hp, (unsigned)co2, sT, sH, pir, mmw, rssi,
    dhtOK ? "OK" : "NO_DATA", scdOK ? "OK" : "NO_DATA");
  client.publish(topic("telemetry").c_str(), tp);

  Serial.printf("📤 T=%.1f H=%.1f CO2=%u | 재실 PIR=%d mmW=%d | RSSI=%lddBm\n",
                tc, hp, co2, pir, mmw, rssi);
}
