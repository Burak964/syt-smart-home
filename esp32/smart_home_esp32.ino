#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <math.h>

// Zugangsdaten nur in der lokalen Kopie eintragen, nicht mit abgeben.
const char* wlanName = "DEIN_WIFI_SSID";
const char* wlanPasswort = "DEIN_WIFI_PASSWORT";
const char* mqttServer = "172.20.10.3"; // IP der Ubuntu-VM im Hotspot
const char* mqttBenutzer = "esp32";
const char* mqttPasswort = "DEIN_MQTT_PASSWORT";

constexpr int PIN_DHT = 32;
constexpr int PIN_ROT = 25;
constexpr int PIN_GRUEN = 26;
constexpr int PIN_BLAU = 27;

// Nur diesen Wert fuer den LED-Schwellwerttest anpassen.
constexpr float TEMPERATUR_GRENZWERT_C = 26.0f;

constexpr unsigned long MESSINTERVALL_MS = 10000;
constexpr unsigned long MQTT_WIEDERVERSUCH_MS = 5000;
constexpr unsigned long WLAN_WIEDERVERSUCH_MS = 60000;

const char* TOPIC_JSON = "smartroom/sensor1/telemetry";

Adafruit_BMP280 bmp;
DHT dht(PIN_DHT, DHT11);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long letzteMessung = 0;
unsigned long letzterMqttVersuch = 0;
unsigned long letzterWlanVersuch = 0;
bool wlanWarVerbunden = false;

void warnLedSetzen(float temperatur, bool temperaturOk) {
  // RGB-LED mit gemeinsamer Kathode an GND:
  // Ueber Grenzwert nur Rot, sonst alle Farben aus.
  const bool alarm = temperaturOk && temperatur > TEMPERATUR_GRENZWERT_C;
  digitalWrite(PIN_ROT, alarm ? HIGH : LOW);
  digitalWrite(PIN_GRUEN, LOW);
  digitalWrite(PIN_BLAU, LOW);

  Serial.print("LED lokal: ");
  Serial.println(alarm ? "ROT (Grenzwert ueberschritten)" : "AUS");
}

void messungVerarbeiten() {
  const float temperatur = bmp.readTemperature();
  const float luftdruck = bmp.readPressure() / 100.0f; // Pa -> hPa
  const float feuchte = dht.readHumidity();

  const bool temperaturOk =
      isfinite(temperatur) && temperatur >= -40 && temperatur <= 85;
  const bool luftdruckOk =
      isfinite(luftdruck) && luftdruck >= 300 && luftdruck <= 1100;
  const bool feuchteOk =
      isfinite(feuchte) && feuchte >= 0 && feuchte <= 100;

  // Die LED funktioniert auch ohne WLAN, MQTT, Node-RED oder Server.
  warnLedSetzen(temperatur, temperaturOk);

  if (!temperaturOk) {
    Serial.println("BMP280: ungueltige Temperatur, nichts gesendet");
  }
  if (!luftdruckOk) {
    Serial.println("BMP280: ungueltiger Luftdruck, nichts gesendet");
  }
  if (!feuchteOk) {
    Serial.println("DHT11: Lesefehler, keine Feuchtigkeit gesendet");
  }

  if (!mqtt.connected()) {
    Serial.println("MQTT getrennt: LED arbeitet lokal, Daten nicht gesendet");
    return;
  }

  char wert[24];

  // Bestehende einzelne Topics bleiben fuer bisherige Node-RED-Flows erhalten.
  if (temperaturOk) {
    snprintf(wert, sizeof(wert), "%.2f", temperatur);
    if (mqtt.publish("smart-home/bmp280/temperatur", wert)) {
      Serial.print("Temperatur gesendet: ");
      Serial.println(wert);
    }
  }

  if (luftdruckOk) {
    snprintf(wert, sizeof(wert), "%.2f", luftdruck);
    if (mqtt.publish("smart-home/bmp280/luftdruck", wert)) {
      Serial.print("Luftdruck gesendet: ");
      Serial.println(wert);
    }
  }

  if (feuchteOk) {
    snprintf(wert, sizeof(wert), "%.1f", feuchte);
    if (mqtt.publish("smart-home/dht11/feuchte", wert)) {
      Serial.print("Luftfeuchtigkeit gesendet: ");
      Serial.println(wert);
    }
  }

  // JSON nur bei drei gueltigen Messwerten senden.
  if (temperaturOk && luftdruckOk && feuchteOk) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"temperature\":%.2f,\"humidity\":%.1f,\"pressure\":%.2f}",
             temperatur, feuchte, luftdruck);

    if (mqtt.publish(TOPIC_JSON, json)) {
      Serial.print("JSON gesendet: ");
      Serial.println(json);
    } else {
      Serial.println("JSON konnte nicht gesendet werden");
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_ROT, OUTPUT);
  pinMode(PIN_GRUEN, OUTPUT);
  pinMode(PIN_BLAU, OUTPUT);
  digitalWrite(PIN_ROT, LOW);
  digitalWrite(PIN_GRUEN, LOW);
  digitalWrite(PIN_BLAU, LOW);

  Wire.begin(21, 22);

  if (!bmp.begin(0x76) && !bmp.begin(0x77)) {
    Serial.println("BMP280 nicht gefunden! Verkabelung pruefen.");
    while (true) delay(1000);
  }
  Serial.println("BMP280 gefunden!");

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(wlanName, wlanPasswort);
  letzterWlanVersuch = millis();

  mqtt.setServer(mqttServer, 1883);
  letzterMqttVersuch = millis() - MQTT_WIEDERVERSUCH_MS;

  // Erste Messung nach 10 s; der DHT11 hat dann Zeit zum Starten.
  letzteMessung = millis();
  Serial.println("Verbinde mit WLAN...");
}

void loop() {
  const unsigned long jetzt = millis();

  // Vor der Netzpruefung messen, damit die LED auch ohne Netz reagiert.
  if (jetzt - letzteMessung >= MESSINTERVALL_MS) {
    letzteMessung = jetzt;
    messungVerarbeiten();
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) mqtt.disconnect();
    if (wlanWarVerbunden) Serial.println("WLAN getrennt");
    wlanWarVerbunden = false;

    if (jetzt - letzterWlanVersuch >= WLAN_WIEDERVERSUCH_MS) {
      Serial.println("Verbinde WLAN neu...");
      WiFi.disconnect();
      WiFi.begin(wlanName, wlanPasswort);
      letzterWlanVersuch = jetzt;
    }

    delay(10);
    return;
  }

  if (!wlanWarVerbunden) {
    wlanWarVerbunden = true;
    Serial.print("ESP32-IP: ");
    Serial.println(WiFi.localIP());
    letzterMqttVersuch = millis() - MQTT_WIEDERVERSUCH_MS;
  }

  if (!mqtt.connected()) {
    if (jetzt - letzterMqttVersuch >= MQTT_WIEDERVERSUCH_MS) {
      letzterMqttVersuch = jetzt;
      Serial.print("Verbinde mit MQTT... ");

      if (mqtt.connect("esp32-bmp280-dht11", mqttBenutzer, mqttPasswort)) {
        Serial.println("verbunden!");
      } else {
        Serial.print("fehlgeschlagen, Status: ");
        Serial.println(mqtt.state());
      }
    }

    delay(10);
    return;
  }

  mqtt.loop();
  delay(10);
}
