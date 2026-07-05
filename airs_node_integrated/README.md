# AIRS 노드 통합 스케치 (백업 dht22 + SCD41 + BLE)

백업 원본 `백업/dht22/dht22.ino`를 분석해, 그 구조를 살리면서 SCD41과 BLE 프로비저닝을 합친 **테스트용 단일 스케치**. (백업 원본은 그대로 보존)

> ⚠️ **BLE 프로비저닝은 Arduino-ESP32 코어 2.0.x(2.0.17에서 검증)에서만 동작.**
> 3.x는 ESP32-C3에서 `WiFiProv`가 부팅 시 `HLI Magic mismatch` → `Instruction access fault`로 크래시(부트루프)한다. 스톡 `WiFiProv` 예제도 똑같이 죽고, raw BLE(`예제 → BLE → Server`)는 3.x에서도 정상 → 컨트롤러/하드웨어가 아니라 **3.x WiFiProv 회귀 버그**.
> → BLE를 쓰려면 **보드 매니저에서 esp32 코어를 2.0.17로 다운그레이드.** 코드는 `#if ESP_ARDUINO_VERSION_MAJOR`로 2.x(`WIFI_PROV_*`)/3.x(`NETWORK_PROV_*`) 상수를 자동 대응한다.
> 다운그레이드 시 리셋되니 재확인할 설정: **Partition Scheme = Huge APP (3MB No OTA/1MB SPIFFS)**, **USB CDC On Boot = Enabled**(시리얼 로그).
> BLE 대신 하드코딩 Wi-Fi(`USE_HARDCODED_WIFI`)만 쓸 거면 코어 3.x여도 무방(WiFiProv를 안 켜므로).
> 검증 완료(2.0.17): 앱으로 `AIRS-node01` 프로비저닝 → Wi-Fi 접속 → MQTT → DHT22+SCD41(CO2) 발행까지 정상.

## 백업 원본 분석 (dht22.ino)
- `WiFi.begin(ssid, password)` — **하드코딩 Wi-Fi**(`info.h`) → MQTT `sogang-airs.iptime.org:1883`
- 5초마다 DHT22(GPIO4) 온·습도를 읽어 `airs/node/node_01/dht22` 로 `{"temperature","humidity"}` 발행
- 라이브러리: Adafruit DHT + PubSubClient

## 통합본이 추가한 것
| 항목 | 내용 |
| --- | --- |
| SCD41 | I2C(SDA=5, SCL=6)로 CO2·온도·습도 측정 |
| Wi-Fi | **기본 BLE 프로비저닝**(8차 회의 결정). 하드코딩도 토글로 지원 |
| node-off | `airs/node/node_01/cmd` 구독(`{"cmd":"off"}`) |
| 발행 | 기존 `.../dht22`(호환) **+** `.../telemetry`(DHT22+SCD41 통합) |

telemetry payload:
```json
{"node_id":"node_01","temperature_c":24.3,"humidity_pct":53.2,"co2_ppm":842,
 "scd41_temperature_c":24.8,"scd41_humidity_pct":51.0,
 "sensor_status":{"dht22":"OK","scd41":"OK"}}
```
→ DHT22와 SCD41 값이 **분리되어** 들어가므로 둘 다 잘 읽히는지 한 번에 확인 가능.

## 배선
- DHT22: DATA=GPIO4, VCC=3.3V, GND (백업과 동일)
- SCD41: SDA=GPIO5, SCL=GPIO6, VCC=3.3V, GND (I2C 0x62). ⚠️ GPIO2/8/9(부트 스트랩)·4(DHT) 회피
- 핀을 바꾸려면 스케치 상단 `I2C_SDA/I2C_SCL` 수정

## 라이브러리 (Arduino IDE Library Manager)
- Arduino-ESP32 코어 v3.x, Board `ESP32C3 Dev Module`, Partition `Large APPS`(BLE 때문)
- `DHT sensor library`(Adafruit) + `Adafruit Unified Sensor` + `PubSubClient`

## Wi-Fi 두 가지 모드
- **기본(BLE)**: 그대로 업로드 → BLE 광고 `AIRS-node01` → 앱으로 Wi-Fi 전달(아래 테스트 4)
- **하드코딩(빠른 센서 테스트)**: 스케치 상단 `// #define USE_HARDCODED_WIFI` 주석 해제 + `info_sample.h`를 `info.h`로 복사해 SSID/PW 입력

## 테스트 절차
1. 업로드 후 **시리얼 모니터 115200**.
2. (BLE 모드) 폰 **ESP BLE Provisioning** 앱 → `AIRS-node01` → PoP `airs-pop` → Wi-Fi 선택. 시리얼에 `✅ IP` → `✅ MQTT 연결` 확인.
3. 서버에서 수신 확인:
   ```bash
   mosquitto_sub -h localhost -t 'airs/node/+/telemetry' -v   # CO2 포함 통합
   mosquitto_sub -h localhost -t 'airs/node/+/dht22' -v       # 기존 호환
   ```
4. InfluxDB 삽입 + Grafana: `../../server_test/README.md` 참고
   - `python3 mqtt_to_influx_test.py --influx-selftest ...` 로 쓰기 확인 → 브리지 모드 실행 → Grafana에 습도·CO2 패널 추가.
   - 스크립트는 이 통합 payload(`co2_ppm`, `scd41_*` 포함)를 자동으로 InfluxDB에 저장한다.

## 참고
- 컴파일 시 `WIFI_PROV_SCHEME_BLE` 미정의 에러면 v3.x 신규 명칭으로 교체(스케치 주석/`../firmware/arduino/README.md` 참고).
- `info.h`(실제 SSID/PW)는 비밀 — git/슬랙 금지.
