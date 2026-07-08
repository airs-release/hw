# AIRS 노드 펌웨어 (ESP32-C3)

AIRS 센서 노드의 Arduino 펌웨어. Mosquitto(MQTT)로 telemetry를 발행하고, IR로 에어컨을 제어한다.
GitHub `airs-release/hw` 레포의 루트 내용.

## 구조

```
firmware/
  airs_node_integrated/            # 메인 통합 스케치 (현재 사용)
    airs_node_integrated.ino
    info_sample.h                  # Wi-Fi 하드코딩 샘플 → info.h 로 복사(실값은 비밀, git 금지)
    airs-node-wiring-v1.svg        # 배선도
    airs-node-breadboard-v2.svg    # 브레드보드 배치
  poc/
    airs_ble_provisioning_poc/     # App 참고용 BLE 프로비저닝 PoC (NimBLE GATT, 별도 보관)
      airs_ble_provisioning_poc.ino
```

> Arduino IDE는 `.ino`가 **폴더명과 같은 폴더** 안에 있어야 연다. 각 스케치는 자기 이름 폴더에 둔다.

## 1. 메인 스케치 — `airs_node_integrated`

한 노드의 모든 기능을 담은 통합 스케치.

| 기능 | 내용 |
| --- | --- |
| 센서 | DHT22(온·습도) + SCD41(CO2, I2C) + HC-SR501 PIR + HLK-LD2410C mmWave |
| 재실 | PIR/mmWave 원시값을 telemetry로 발행(융합·판정은 서버 Spring `OccupancyFusionService`) |
| IR 에어컨 | IRremoteESP8266 `IRac` 멀티 브랜드 송신 + 리모컨 1회 수신 브랜드 자동감지 + NVS 저장 + 미지원 브랜드 raw 학습/재생 |
| Wi-Fi | BLE 프로비저닝(WiFiProv) 또는 하드코딩(`USE_HARDCODED_WIFI`) 토글 |
| 발행 | `airs/node/<id>/telemetry`(통합) + `.../dht22`(호환), ~5초 |
| 제어 | `airs/node/<id>/cmd` 구독 — `ac_learn/ac_brand/ac_cool/ac_heat/ac_off/ir_learn/ir_raw/off/on` |

**핀맵(ESP32-C3):** DHT22=GPIO4 · I2C SDA=5/SCL=6 · PIR=3 · mmWave OUT=10 · IR TX=7 · IR RX=1. ⚠ 부트 스트랩 핀(2/8/9) 회피.

**telemetry payload:**
```json
{"node_id":"node_01","temperature_c":24.3,"humidity_pct":53.2,"co2_ppm":842,
 "scd41_temperature_c":24.1,"scd41_humidity_pct":52.0,
 "pir_detected":1,"mmwave_detected":0,"wifi_signal_dbm":-58,
 "sensor_status":{"dht22":"OK","scd41":"OK"}}
```

**빌드 설정:** Board `ESP32C3 Dev Module` · Partition **Huge APP** · USB CDC On Boot **Enabled** · 라이브러리 `DHT sensor library`(Adafruit)+`Adafruit Unified Sensor`+`PubSubClient`+`IRremoteESP8266`.
> ⚠ WiFiProv BLE 프로비저닝은 **Arduino-ESP32 코어 2.0.17**에서만 동작(3.x는 C3에서 크래시). 하드코딩 Wi-Fi만 쓰면 3.x도 무방.

## 2. BLE 프로비저닝 PoC — `poc/airs_ble_provisioning_poc`

App(진호)이 참고할 **자체 GATT 프로비저닝 레퍼런스**. WiFiProv 대신 NimBLE로 표준 GATT를 열어 웹/RN 어디서든 붙일 수 있게 한 PoC.
- 표준 Device Information Service(0x180A) + 커스텀 프로비저닝 서비스(creds write·status notify)
- LESC 본딩·MITM passkey, MAC 기반 ID
- 빌드: **코어 3.x + NimBLE-Arduino 2.x**, Partition Huge APP
- 스펙: `문서/BLE_GATT_프로비저닝_스펙.md`

## 3. 보안
- `info.h`(실제 Wi-Fi SSID/PW)는 **비밀**. git·Slack 금지. 샘플 `info_sample.h`만 커밋.
