# Station de mesure horodatée : ESP32 + DHT22 + RTC DS1307 (GMT+3)

Projet Wokwi : mesurer la température et l'humidité, et **connaître l'heure exacte de chaque mesure** grâce à un module RTC réglé sur l'heure de Madagascar (GMT+3). Chaque mesure est envoyée en JSON par MQTT.

## 1. Objectif

- Mesurer température et humidité avec un **DHT22**.
- Horodater chaque mesure avec un **RTC DS1307**, en **heure locale de Madagascar (GMT+3)**.
- Produire **une mesure par seconde** dès le lancement de l'ESP32.
- Publier chaque mesure en **MQTT**, au format JSON :

```json
{
  "temperature": 24.0,
  "humidity": 40.0,
  "time": "2026-11-06 11:20:05"
}
```

## 2. Matériel

| Composant | Rôle | Bus / protocole |
|---|---|---|
| ESP32 DevKit | Microcontrôleur : lit les capteurs, gère WiFi et MQTT | - |
| DHT22 | Capteur de température et d'humidité | 1 fil (protocole propriétaire) |
| DS1307 | Horloge temps réel (RTC), garde la date et l'heure | I2C (adresse 0x68) |

### Branchements

| Composant | Broche | ESP32 |
|---|---|---|
| DHT22 | VCC | 3V3 |
| DHT22 | GND | GND |
| DHT22 | SDA (données) | GPIO 15 |
| DS1307 | 5V | VIN |
| DS1307 | GND | GND |
| DS1307 | SDA | GPIO 21 |
| DS1307 | SCL | GPIO 22 |

## 3. Architecture

```
          NTP (internet, via WiFi)
                   |
                   v  (une seule fois, au démarrage)
 DHT22 --> ESP32 <--I2C--> DS1307 (heure locale GMT+3)
              |
              +--> Moniteur série (une ligne JSON par mesure)
              |
              +--> Broker MQTT (topic MQTT_TOPIC)
```

Principe : **le WiFi/NTP sert uniquement à régler le RTC au démarrage**. Ensuite, toutes les heures de mesure viennent du RTC, pas d'internet.

## 4. Déroulement du programme

### Au démarrage (`setup()`)

1. Ouverture du port série (115200 bauds).
2. Initialisation du bus I2C (SDA = 21, SCL = 22) et détection du DS1307 (`rtc.begin()`). S'il est absent, le programme s'arrête avec un message.
3. Initialisation du DHT22.
4. Configuration du client MQTT (broker, port, délai de connexion de 3 s).
5. Synchronisation : WiFi, puis NTP, puis écriture de l'heure locale dans le RTC (`syncRtcFromNtp()`).
6. Si NTP échoue, le RTC est utilisé tel quel, **seulement** s'il tourne (`isrunning()`) et si son année est >= 2024. Sinon, arrêt avec un message.

### En boucle (`loop()`)

À chaque passage :

1. `mqttMaintain()` : maintient ou rétablit la connexion MQTT (sans bloquer plus de quelques secondes).
2. Lecture de l'heure du RTC (`rtc.now()`).
3. Si la **seconde du RTC n'a pas changé**, on attend 20 ms et on repart.
4. Sinon, c'est une nouvelle seconde : `updateSensor()` met à jour les valeurs si nécessaire.
5. Si la dernière lecture du DHT22 est invalide, la mesure est **ignorée**.
6. Sinon, construction du JSON, affichage sur le moniteur série, publication MQTT.

## 5. Les fonctions du code

| Fonction | Rôle |
|---|---|
| `syncRtcFromNtp()` | Se connecte au WiFi `Wokwi-GUEST`, interroge `pool.ntp.org`, écrit l'heure locale dans le RTC avec `rtc.adjust()`. Renvoie `true` si tout a réussi. |
| `formatTime(DateTime)` | Prend l'heure du RTC au format ISO 8601 (`2026-11-06T11:20:05`) et remplace le `T` par un espace pour obtenir un texte brut (`2026-11-06 11:20:05`). |
| `updateSensor()` | Lit le DHT22 **au plus une fois toutes les 2 s**. En cas d'erreur (`NaN`), marque la mesure comme invalide. |
| `mqttMaintain()` | Si le WiFi est connecté : appelle `mqtt.loop()` ou tente une reconnexion toutes les 5 s, avec un identifiant client aléatoire. |
| `setup()` / `loop()` | Initialisation et boucle principale décrites ci-dessus. |

### Variables d'état importantes

| Variable | Rôle |
|---|---|
| `lastTemp`, `lastHum` | Dernière température et humidité valides. |
| `haveValid` | `true` si la dernière lecture du DHT22 est valide. |
| `lastDhtRead` / `dhtReadOnce` | Instant (en ms) de la dernière lecture du DHT22, et indicateur de première lecture. |
| `lastSecond` | Dernière seconde du RTC traitée : sert à déclencher une seule mesure par seconde. |
| `lastMqttTry` | Instant de la dernière tentative de connexion MQTT. |

## 6. Choix techniques et justifications

**Heure locale stockée directement dans le RTC.**
Madagascar est en GMT+3 toute l'année, sans heure d'été. Il n'y a donc aucun cas ambigu : le RTC contient directement l'heure locale. Le DS1307 ne gère de toute façon aucun fuseau horaire.

**Réglage par NTP.**
Le WiFi est déjà nécessaire pour MQTT. NTP donne une heure exacte sans la coder en dur. `configTime()` applique le décalage de +3 h, donc l'heure obtenue est déjà locale.

**Une mesure par seconde, alignée sur le RTC.**
On déclenche la mesure au **changement de seconde du RTC**, et non avec un `delay(1000)`. Ainsi, les horodatages sont consécutifs, sans dérive, sans doublon et sans seconde sautée.

**Lecture du DHT22 toutes les 2 s seulement.**
Le DHT22 ne peut pas être lu plus d'une fois toutes les 2 secondes (0,5 Hz). Lire plus vite donne des erreurs ou des valeurs répétées. On garde donc une boucle à 1 s : à la seconde où le capteur n'est pas relu, on **réutilise la dernière valeur valide** avec l'heure actuelle du RTC. Les valeurs ne changent donc qu'une fois sur deux, mais chaque mesure a son horodatage exact.

**Erreur de lecture : mesure ignorée.**
Si le DHT22 renvoie `NaN`, aucune mesure n'est affichée ni publiée jusqu'à la prochaine lecture réussie (au plus 2 s). On évite ainsi d'envoyer de fausses données.

**Chaîne JSON construite avec `snprintf`.**
Pas de bibliothèque JSON : le format est simple et fixe, donc c'est plus léger. Les nombres ont une décimale (`%.1f`).

**Format de `time` en texte brut.**
Le RTC fournit l'heure en ISO 8601, que l'on transforme en `YYYY-MM-DD hh:mm:ss`. Le fuseau (+03:00) n'est pas écrit : il est implicite, car c'est toujours l'heure de Madagascar.

**MQTT non bloquant.**
Si le broker est injoignable, le programme continue d'afficher les mesures sur le moniteur série et retente la connexion toutes les 5 s.

## 7. Messages MQTT

| Paramètre | Valeur |
|---|---|
| Broker | `broker.hivemq.com` (public) |
| Port | 1883 (sans chiffrement) |
| Topic | défini par `MQTT_TOPIC` (à rendre unique, par exemple `madagascar/station-7k3q/rtc-dht22/mesure`) |
| Qualité de service | QoS 0 (par défaut de `PubSubClient`) |
| Taille d'un message | environ 70 octets (la limite de la bibliothèque est de 256) |

Pour vérifier la réception, il faut s'abonner au même topic avec un client MQTT (MQTTX, client web HiveMQ, `mosquitto_sub`...).

## 8. Bibliothèques (`libraries.txt`)

| Bibliothèque | Utilité |
|---|---|
| DHT sensor library + Adafruit Unified Sensor | Lecture du DHT22 |
| RTClib + Adafruit BusIO | Pilotage du DS1307 (`DateTime`, `adjust`, `timestamp`) |
| PubSubClient | Client MQTT |
| WiFi, Wire, time | Fournies avec le noyau ESP32 (WiFi, I2C, NTP via `configTime`) |

## 9. Lancer le projet

1. Créer un projet ESP32 sur https://wokwi.com/projects/new/esp32.
2. Remplacer `sketch.ino`, `diagram.json` et `libraries.txt` par les fichiers du projet.
3. Changer `MQTT_TOPIC` pour un topic unique.
4. Lancer la simulation : le moniteur série affiche la connexion WiFi, le réglage du RTC, puis une ligne JSON par seconde.

Exemple de sortie :

```
[WiFi] Connecté.
[RTC] Réglé sur l'heure locale (GMT+3) via NTP.
Démarrage des mesures (1 par seconde).
[MQTT] Connexion au broker... OK
{"temperature":24.0,"humidity":40.0,"time":"2026-11-06 11:20:05"}
{"temperature":24.0,"humidity":40.0,"time":"2026-11-06 11:20:06"}
```

## 10. Limites et améliorations possibles

- **Dérive du DS1307** : il utilise un quartz simple et peut dériver de quelques minutes par mois. Un **DS3231** (oscillateur compensé en température) est plus précis. On pourrait aussi **resynchroniser par NTP** périodiquement (par exemple toutes les heures).
- **Une valeur sur deux répétée** : conséquence de la limite de 2 s du DHT22. Le champ `time` reste exact, mais la valeur ne change que toutes les 2 s.
- **Connexion MQTT** : une tentative de connexion peut bloquer jusqu'à 3 s, ce qui peut faire sauter une ou deux secondes de mesure quand le broker est injoignable.
- **Pas de stockage local** : si MQTT est coupé, les mesures ne sont pas conservées. On pourrait ajouter un tampon, un fichier (LittleFS) ou une carte SD.
- **Broker public sans chiffrement ni authentification** : adapté à un test ; en production, utiliser TLS et des identifiants.
- **Pas de chauffage ni d'étalonnage** : le DHT22 a une précision d'environ ±0,5 °C et ±2 à 5 % d'humidité relative.

