/*
 * Station de mesure horodatée : ESP32 + DHT22 + RTC DS1307 (Wokwi)
 *
 * - L'heure locale de Madagascar (GMT+3, sans heure d'été) est stockée
 *   directement dans le DS1307.
 * - Au démarrage : synchro NTP via WiFi, puis écriture dans le RTC.
 *   Ensuite, TOUTES les heures de mesure viennent du RTC.
 * - Une mesure par seconde RTC. Le DHT22 est lu toutes les 2 s (limite du
 *   capteur) ; entre deux lectures on réutilise la dernière valeur valide.
 * - Si la lecture du DHT22 échoue, la mesure est ignorée.
 * - Chaque mesure est affichée sur le moniteur série et publiée en MQTT.
 *
 * Format :
 * {"temperature":24.0,"humidity":40.0,"time":"2026-11-06 11:20:05"}
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <DHT.h>
#include <time.h>

// ---------- Brochage ----------
#define DHT_PIN   15
#define DHT_TYPE  DHT22
#define I2C_SDA   21
#define I2C_SCL   22

// ---------- WiFi (réseau virtuel de Wokwi) ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ---------- MQTT ----------
const char*    MQTT_HOST  = "broker.hivemq.com";
const uint16_t MQTT_PORT  = 1883;
// Change ce topic pour qu'il soit unique (le broker est public) :
const char*    MQTT_TOPIC = "wokwi/rtc-dht22/mesure";

// ---------- Temps ----------
const long GMT_OFFSET_SEC = 3 * 3600;   // Madagascar = GMT+3
const long DST_OFFSET_SEC = 0;          // pas d'heure d'été
const char* NTP_SERVER    = "pool.ntp.org";

// ---------- Intervalles ----------
const unsigned long DHT_INTERVAL_MS  = 2000;  // minimum imposé par le DHT22
const unsigned long MQTT_RETRY_MS    = 5000;

// ---------- Objets ----------
DHT          dht(DHT_PIN, DHT_TYPE);
RTC_DS1307   rtc;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- État ----------
float         lastTemp = NAN;
float         lastHum  = NAN;
bool          haveValid = false;        // dernière lecture DHT valide ?
bool          dhtReadOnce = false;
unsigned long lastDhtRead = 0;
unsigned long lastMqttTry = 0;
int           lastSecond  = -1;

// ------------------------------------------------------------------
// WiFi + NTP -> écrit l'heure locale dans le RTC. Renvoie true si OK.
// ------------------------------------------------------------------
bool syncRtcFromNtp() {
  Serial.print("[WiFi] Connexion");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // canal 6 = connexion rapide sur Wokwi
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Echec de connexion.");
    return false;
  }
  Serial.println("[WiFi] Connecté.");

  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  struct tm tmLocal;
  if (!getLocalTime(&tmLocal, 10000)) {
    Serial.println("[NTP] Echec de synchronisation.");
    return false;
  }

  rtc.adjust(DateTime(tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
                      tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec));
  Serial.println("[RTC] Réglé sur l'heure locale (GMT+3) via NTP.");
  return true;
}

// ------------------------------------------------------------------
// Heure du RTC en texte brut "YYYY-MM-DD hh:mm:ss"
// (on part du format ISO 8601 puis on remplace le 'T' par un espace)
// ------------------------------------------------------------------
String formatTime(const DateTime& dt) {
  String s = dt.timestamp(DateTime::TIMESTAMP_FULL);  // YYYY-MM-DDThh:mm:ss
  s.replace('T', ' ');
  return s;
}

// ------------------------------------------------------------------
// Lecture du DHT22 (au plus une fois toutes les 2 s)
// ------------------------------------------------------------------
void updateSensor() {
  if (dhtReadOnce && millis() - lastDhtRead < DHT_INTERVAL_MS) return;

  lastDhtRead = millis();
  dhtReadOnce = true;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    haveValid = false;                 // mesure ignorée jusqu'à la prochaine lecture OK
    Serial.println("[DHT22] Erreur de lecture : mesure ignorée.");
    return;
  }
  lastTemp = t;
  lastHum  = h;
  haveValid = true;
}

// ------------------------------------------------------------------
// MQTT : reconnexion non bloquante
// ------------------------------------------------------------------
void mqttMaintain() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if (millis() - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = millis();

  String clientId = "esp32-rtc-dht22-" + String((uint32_t)esp_random(), HEX);
  Serial.print("[MQTT] Connexion au broker... ");
  if (mqtt.connect(clientId.c_str())) {
    Serial.println("OK");
  } else {
    Serial.print("échec (rc=");
    Serial.print(mqtt.state());
    Serial.println(")");
  }
}

// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== ESP32 + DHT22 + DS1307 (GMT+3) ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) {
    Serial.println("[RTC] DS1307 introuvable. Arrêt.");
    while (true) delay(1000);
  }

  dht.begin();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(3);

  bool synced = syncRtcFromNtp();
  if (!synced) {
    // Pas de NTP : on n'accepte le RTC que s'il tourne et a une date plausible
    DateTime now = rtc.now();
    if (!rtc.isrunning() || now.year() < 2024) {
      Serial.println("[RTC] Heure non fiable et NTP indisponible. Arrêt.");
      while (true) delay(1000);
    }
    Serial.println("[RTC] NTP indisponible : on utilise l'heure déjà stockée.");
  }

  Serial.println("Démarrage des mesures (1 par seconde).");
}

void loop() {
  mqttMaintain();

  // Une mesure par seconde, alignée sur le changement de seconde du RTC
  DateTime now = rtc.now();
  if (now.second() == lastSecond) {
    delay(20);
    return;
  }
  lastSecond = now.second();

  updateSensor();
  if (!haveValid) return;              // lecture invalide : on ignore la mesure

  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"temperature\":%.1f,\"humidity\":%.1f,\"time\":\"%s\"}",
           lastTemp, lastHum, formatTime(now).c_str());

  Serial.println(payload);

  if (mqtt.connected()) {
    if (!mqtt.publish(MQTT_TOPIC, payload)) {
      Serial.println("[MQTT] Publication échouée.");
    }
  } else {
    Serial.println("[MQTT] Non connecté : mesure non publiée.");
  }
}
