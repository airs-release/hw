/*
 * AIRS BLE 프로비저닝 PoC — NimBLE GATT + LESC 본딩/암호화 + MAC 기반 ID
 * ----------------------------------------------------------------------------
 * 목적: WiFiProv 대신 "자체 GATT 서비스"(B안)로 프로비저닝. 표준 Device Information
 *       Service(0x180A)로 노드 정보(명함)를 노출하고, 커스텀 서비스로 Wi-Fi 자격을
 *       "암호화+인증(MITM)" 상태에서만 받아 접속한다. 노드 ID는 MAC(칩 고유)에서 파생.
 *
 * 라이브러리: NimBLE-Arduino 2.x  (Arduino 라이브러리 매니저에서 "NimBLE-Arduino")
 * 보드: ESP32-C3 SuperMini · Partition Scheme = "Huge APP" · USB CDC On Boot = Enabled
 * 참고: 자체 GATT라 WiFiProv를 안 쓰므로 코어 2.0.17 제약(3.x 크래시)에서 자유롭다.
 *       (이 스케치는 2.0.17에서도, 3.x에서도 컴파일된다)
 *
 * 흐름:  스캔 → 연결 → (페어링: 노드가 6자리 표시, 앱이 입력)
 *        → DIS 읽기(펌웨어/HW/모델/시리얼) → creds 쓰기 → 노드 Wi-Fi 접속 → status notify
 * 테스트: nRF Connect(모바일)로 위 흐름을 그대로 재현 가능(아래 지시서 참고).
 *
 * 보안 요약:
 *   - LESC(Secure Connections) + 본딩 + MITM(passkey) 강제.
 *   - Wi-Fi 자격(creds) 특성은 WRITE_ENC|WRITE_AUTHEN → 페어링 전엔 쓰기 불가.
 *   - status 특성은 READ_ENC|NOTIFY → 페어링된 앱만 상태 열람.
 *   - Wi-Fi 비밀번호는 로그/BLE에 절대 평문 노출하지 않는다(길이만 출력).
 */
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <Preferences.h>

// ── 표준 Device Information Service (0x180A) UUID ──
#define UUID_DIS      "180A"
#define UUID_MANUF    "2A29"   // Manufacturer Name
#define UUID_MODEL    "2A24"   // Model Number
#define UUID_SERIAL   "2A25"   // Serial Number  ← MAC 기반
#define UUID_FW_REV   "2A26"   // Firmware Revision
#define UUID_HW_REV   "2A27"   // Hardware Revision

// ── AIRS 커스텀 프로비저닝 서비스 (임의 고정 128-bit UUID) ──
#define UUID_PROV_SVC    "8f1d0001-a125-4e10-9c00-a1250000ba5e"
#define UUID_PROV_CREDS  "8f1d0002-a125-4e10-9c00-a1250000ba5e"   // write(enc+authen): {"ssid":"..","pw":".."}
#define UUID_PROV_STATUS "8f1d0003-a125-4e10-9c00-a1250000ba5e"   // read(enc)+notify: {"state":..,"ip":..,"rssi":..}

// ── 제품 상수(펌웨어/HW 버전은 빌드 시 여기서 정해 박는다) ──
#define MANUFACTURER "Sogang Release / AIRS"
#define MODEL_NAME   "AIRS Node C3"
#define FW_VERSION   "0.1.0-ble-poc"
#define HW_VERSION   "perfboard-v1"

// ⚠ MQTT node_id — 현재 Grafana/Spring/브리지가 "node_01" 기준으로 필터링 중.
//   기존 노드를 이 스킴으로 바꾸면 파이프라인 필터가 깨질 수 있다. 기존 노드는 아래 주석을 해제해
//   node_01로 고정하고, "신규 노드"만 MAC 기반 자동 ID를 쓰는 걸 권장.
// #define NODE_ID_FIXED "node_01"

// #define FORCE_PROV        // 정의하면 저장된 Wi-Fi 무시하고 항상 프로비저닝 대기

// ── 전역 ──
Preferences prefs;
NimBLECharacteristic* pStatus = nullptr;
uint32_t g_passkey = 0;
char g_serial[13], g_devCode[16], g_nodeId[24];
volatile bool g_startConnect = false, g_wifiPending = false;
unsigned long g_t0 = 0;
String g_ssid, g_pw;

// {"key":"값"} 에서 문자열 추출(경량 파서)
static String jsonStr(const String& msg, const char* key) {
  int p = msg.indexOf(key); if (p < 0) return "";
  int c = msg.indexOf(':', p); if (c < 0) return "";
  int a = msg.indexOf('"', c); if (a < 0) return "";
  int b = msg.indexOf('"', a + 1); if (b < 0) return "";
  return msg.substring(a + 1, b);
}

// status 특성 갱신 + notify (앱에 진행상태 전달)
void setStatus(const char* state) {
  char buf[96];
  if (WiFi.status() == WL_CONNECTED)
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"ip\":\"%s\",\"rssi\":%ld}",
             state, WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", state);
  if (pStatus) { pStatus->setValue((uint8_t*)buf, strlen(buf)); pStatus->notify(); }
  Serial.printf("[STATUS] %s\n", buf);
}

// ── BLE 서버 콜백(연결/페어링) ──
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    Serial.printf("[BLE] 연결됨: %s\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    Serial.printf("[BLE] 연결 해제(reason=%d) → 광고 재시작\n", reason);
    NimBLEDevice::startAdvertising();
  }
  // DISPLAY_ONLY: 노드가 표시할 passkey를 반환 → 앱이 이 숫자를 입력해야 페어링(MITM 방지)
  uint32_t onPassKeyDisplay() override {
    Serial.printf("\n========================\n[페어링] 앱에 이 6자리 입력 → %06u\n========================\n", g_passkey);
    // TODO(통합): SSD1306/ST7789 화면에 %06u 로 g_passkey 표시
    return g_passkey;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (info.isEncrypted())
      Serial.printf("[BLE] ✅ 페어링 성공 (암호화=%d 인증=%d 본딩=%d)\n",
                    info.isEncrypted(), info.isAuthenticated(), info.isBonded());
    else
      Serial.println("[BLE] ❌ 페어링/암호화 실패");
  }
};

// ── creds 쓰기 콜백(Wi-Fi 자격 수신) ──
class CredsCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (!info.isEncrypted()) { Serial.println("[PROV] 비암호화 쓰기 거부"); return; }   // 이중 방어
    String s(c->getValue().c_str());
    g_ssid = jsonStr(s, "ssid");
    g_pw   = jsonStr(s, "pw");
    if (!g_ssid.length()) { setStatus("FAIL_BAD_INPUT"); return; }
    prefs.begin("airs", false);
    prefs.putString("ssid", g_ssid);
    prefs.putString("pw",   g_pw);
    prefs.end();
    Serial.printf("[PROV] creds 수신: ssid=%s (pw 숨김, 길이 %d)\n", g_ssid.c_str(), g_pw.length());
    g_startConnect = true;          // 실제 접속은 loop에서(콜백 안에서 blocking 접속 회피)
    setStatus("CONNECTING");
  }
};

void setup() {
  Serial.begin(115200); delay(300);

  // ── MAC(칩 고유 ID) 기반 식별자 ──
  uint64_t chipid = ESP.getEfuseMac();                                  // 공장에서 박힌 전 세계 유일 값
  snprintf(g_serial,  sizeof(g_serial),  "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  snprintf(g_devCode, sizeof(g_devCode), "AIRS-%s", g_serial + 8);      // 사람이 읽는 코드(마지막 4 hex)
  snprintf(g_nodeId,  sizeof(g_nodeId),  "node_%s", g_serial);          // MQTT node_id 후보
#ifdef NODE_ID_FIXED
  strncpy(g_nodeId, NODE_ID_FIXED, sizeof(g_nodeId) - 1);
#endif
  Serial.printf("\n[AIRS] serial=%s  devCode=%s  nodeId=%s\n", g_serial, g_devCode, g_nodeId);

  // 저장된 Wi-Fi 있으면 바로 접속(프로비저닝 스킵)
  prefs.begin("airs", true);
  String ss = prefs.getString("ssid", ""), pw = prefs.getString("pw", "");
  prefs.end();
#ifndef FORCE_PROV
  if (ss.length()) {
    Serial.printf("[WiFi] 저장된 SSID=%s 로 접속\n", ss.c_str());
    g_ssid = ss; g_pw = pw; WiFi.begin(ss.c_str(), pw.c_str());
    g_wifiPending = true; g_t0 = millis();
  }
#endif

  g_passkey = 100000 + (esp_random() % 900000);   // 부팅마다 랜덤 6자리(HW RNG)

  // ── NimBLE 초기화 + 보안 ──
  NimBLEDevice::init(g_devCode);
  NimBLEDevice::setSecurityAuth(true, true, true);                       // 본딩 + MITM + LESC(SC)
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);               // 노드가 passkey 표시
  NimBLEDevice::setSecurityPasskey(g_passkey);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  // 표준 Device Information Service — 읽기 전용 "명함"
  NimBLEService* dis = pServer->createService(UUID_DIS);
  dis->createCharacteristic(UUID_MANUF,  NIMBLE_PROPERTY::READ)->setValue(MANUFACTURER);
  dis->createCharacteristic(UUID_MODEL,  NIMBLE_PROPERTY::READ)->setValue(MODEL_NAME);
  dis->createCharacteristic(UUID_SERIAL, NIMBLE_PROPERTY::READ)->setValue(g_serial);   // MAC 기반
  dis->createCharacteristic(UUID_FW_REV, NIMBLE_PROPERTY::READ)->setValue(FW_VERSION);
  dis->createCharacteristic(UUID_HW_REV, NIMBLE_PROPERTY::READ)->setValue(HW_VERSION);
  dis->start();
  // (원하면 위 5개를 READ→READ_ENC 로 바꿔 "페어링 후에만" 정보 열람하도록 강화 가능)

  // AIRS 프로비저닝 서비스 — 쓰기=암호화+인증 필수
  NimBLEService* prov = pServer->createService(UUID_PROV_SVC);
  NimBLECharacteristic* creds = prov->createCharacteristic(
      UUID_PROV_CREDS,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  creds->setCallbacks(new CredsCB());
  pStatus = prov->createCharacteristic(
      UUID_PROV_STATUS,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  pStatus->setValue("{\"state\":\"IDLE\"}");
  prov->start();

  // 광고 시작(서비스 UUID + 이름)
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(UUID_PROV_SVC);
  adv->setName(g_devCode);
  adv->start();
  Serial.printf("[BLE] 광고 시작: %s (프로비저닝 대기)\n", g_devCode);
}

void loop() {
  if (g_startConnect) {
    g_startConnect = false;
    WiFi.begin(g_ssid.c_str(), g_pw.c_str());
    g_wifiPending = true; g_t0 = millis();
    Serial.println("[WiFi] 접속 시도...");
  }
  if (g_wifiPending) {
    if (WiFi.status() == WL_CONNECTED) {
      g_wifiPending = false;
      setStatus("CONNECTED");
      Serial.printf("[WiFi] ✅ 접속 성공 IP=%s RSSI=%ld\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      // 통합 시: 여기서 NimBLEDevice::deinit(true) 로 BLE 종료 → RAM 확보 후 센서/MQTT 상주
    } else if (millis() - g_t0 > 20000) {
      g_wifiPending = false;
      setStatus("FAIL_TIMEOUT");
      Serial.println("[WiFi] ❌ 접속 타임아웃(20s)");
    }
  }
  delay(50);
}
