#include "DHT.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "info.h"

#define DHTPIN 4
#define DHTTYPE DHT22

// ✅ 본인 환경에 맞게 수정
const char* mqtt_server = "sogang-airs.iptime.org";
const int   mqtt_port   = 1883;
const char* node_id     = "node_01";

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

String topic(String type) {
  return "airs/node/" + String(node_id) + "/" + type;
}

void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("WiFi 연결 중");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi 연결: " + WiFi.localIP().toString());
}

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("MQTT 연결 중...");
    if (client.connect(node_id)) {
      Serial.println("✅ MQTT 연결 완료!");
    } else {
      Serial.print("❌ 실패 rc=");
      Serial.println(client.state());
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  dht.begin();
  connectWiFi();
  client.setServer(mqtt_server, mqtt_port);
}

void loop() {
  if (!client.connected()) connectMQTT();
  client.loop();

  static unsigned long lastSend = 0;
  if (millis() - lastSend > 5000) {
    lastSend = millis();

    float temp = dht.readTemperature();
    float humi = dht.readHumidity();

    if (isnan(temp) || isnan(humi)) {
      Serial.println("❌ DHT22 읽기 실패");
      return;
    }

    char payload[64];
    snprintf(payload, sizeof(payload),
      "{\"temperature\":%.1f,\"humidity\":%.1f}", temp, humi);

    client.publish(topic("dht22").c_str(), payload);
    Serial.println("📤 발행: " + String(payload));
  }
}