#include <Arduino.h>
#include <AzIoTSasToken.h>
#include <SerialLogger.h>
#include <WiFi.h>
#include <az_core.h>
#include <azure_ca.h>
#include <ctime>
#include "WiFiClientSecure.h"
#include "PubSubClient.h"
#include "ArduinoJson.h"
#include "secrets.h"
#include <HTTPClient.h>

/* Azure auth data */
// Device ID as specified in the list of devices on IoT Hub
const int tokenDuration = 60;

/* MQTT data for IoT Hub connection */
const char *mqttBroker = iotHubHost;                              // MQTT host = IoT Hub link
const int mqttPort = AZ_IOT_DEFAULT_MQTT_CONNECT_PORT;            // Secure MQTT port
const char *mqttC2DTopic = AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC; // Topic where we can receive cloud to device messages

// These three are just buffers - actual clientID/username/password is generated
// using the SDK functions in initIoTHub()
char mqttClientId[128];
char mqttUsername[128];
char mqttPasswordBuffer[200];
char publishTopic[200];

/* Auth token requirements */

uint8_t sasSignatureBuffer[256]; // Make sure it's of correct size, it will just freeze otherwise :/

az_iot_hub_client client;
AzIoTSasToken sasToken(
    &client, az_span_create_from_str(deviceKey),
    AZ_SPAN_FROM_BUFFER(sasSignatureBuffer),
    AZ_SPAN_FROM_BUFFER(
        mqttPasswordBuffer)); // Authentication token for our specific device

/* Pin definitions and library instance(s) */

#define PIR_PIN 14   // GPIO za PIR senzor
#define RED_PIN 18   // GPIO za crvenu LED
#define GREEN_PIN 19 // GPIO za zelenu LED

/* WiFi things */

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

void setupWiFi()
{
  Logger.Info("Connecting to WiFi");

  wifiClient.setCACert((const char *)ca_pem); // We are using TLS to secure the connection, therefore we need to supply a certificate (in the SDK)

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  short timeoutCounter = 0;
  while (WiFi.status() != WL_CONNECTED)
  { // Wait until we connect...
    Serial.print(".");
    delay(500);

    timeoutCounter++;
    if (timeoutCounter >= 20)
      ESP.restart(); // Or restart if we waited for too long, not much else can you do
  }

  Logger.Info("WiFi connected");
}

// Use pool pool.ntp.org to get the current time
// Define a date on 1.1.2023. and wait until the current time has the same year (by default it's 1.1.1970.)
void initializeTime()
{ // MANDATORY or SAS tokens won't generate
  Logger.Info("Setting time using SNTP");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(NULL);
  std::tm tm{};
  tm.tm_year = 2023; // Define a date on 1.1.2023. and wait until the current time has the same year (by default it's 1.1.1970.)

  while (now < std::mktime(&tm)) // Since we are using an Internet clock, it may take a moment for clocks to sychronize
  {
    delay(500);
    Serial.print(".");
    now = time(NULL);
  }
}

void setupPIRSensor()
{
  pinMode(PIR_PIN, INPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);

  // Početno stanje LED
  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, HIGH);
}

// MQTT is a publish-subscribe based, therefore a callback function is called whenever something is published on a topic that device is subscribed to
void callback(char *topic, byte *payload, unsigned int length)
{
  payload[length] = '\0';                   // It's also a binary-safe protocol, therefore instead of transfering text, bytes are transfered and they aren't null terminated - so we need ot add \0 to terminate the string
  String message = String((char *)payload); // After it's been terminated, it can be converted to String

  Logger.Info("Callback:" + String(topic) + ": " + message);
}

void connectMQTT()
{
  mqttClient.setBufferSize(2048); // Povećanje buffer-a za stabilnije slanje podataka
  mqttClient.setServer(mqttBroker, mqttPort);
  mqttClient.setCallback(callback);

  int attempt = 0;
  const int maxAttempts = 5; // Ograničavamo broj pokušaja

  while (!mqttClient.connected() && attempt < maxAttempts)
  {
    Logger.Info("Attempting MQTT connection... (Attempt " + String(attempt + 1) + "/" + String(maxAttempts) + ")");

    if (sasToken.Generate(tokenDuration) != 0)
    {
      Logger.Error("Failed generating SAS token");
      delay(2000); // Kratka pauza prije ponovnog pokušaja
      attempt++;
      continue;
    }

    const char *mqttPassword = (const char *)az_span_ptr(sasToken.Get());
    if (mqttClient.connect(mqttClientId, mqttUsername, mqttPassword))
    {
      Logger.Info("MQTT connected");
      mqttClient.subscribe(mqttC2DTopic);
      break;
    }
    else
    {
      Logger.Error("MQTT connection failed, retrying in 5 seconds...");
      delay(5000);
      attempt++;
    }
  }

  if (!mqttClient.connected())
  {
    Logger.Error("MQTT connection failed after multiple attempts, restarting ESP32...");
    ESP.restart(); // Resetiraj ESP ako ne uspije povezivanje
  }
}

String getISO8601Timestamp()
{
  time_t now = time(NULL);
  struct tm timeinfo;

  now += 3600;
  localtime_r(&now, &timeinfo);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo); // ISO 8601 format
  return String(buffer);
}

String getTelemetryData(bool status)
{
  StaticJsonDocument<256> doc;
  String output;

  doc["DeviceID"] = (String)deviceId;       // Jedinstveni identifikator uređaja
  doc["Status"] = status;                   // True (zauzeto) ili False (slobodno)
  doc["Timestamp"] = getISO8601Timestamp(); // Vrijeme promjene statusa

  serializeJson(doc, output);
  Logger.Info(output);
  return output;
}

void sendTelemetryData(String telemetryData)
{
  mqttClient.publish(publishTopic, telemetryData.c_str());
}

long lastTime, currentTime = 0;
unsigned long lastMotionTime = 0;
const unsigned long noMotionDelay = 10000; // Vrijeme neaktivnosti prije povratka u "slobodno" stanje
const unsigned long debounceDelay = 1000;  // Minimalni razmak između detekcija (debouncing)
unsigned long lastDebounceTime = 0;        // Vrijeme zadnje registracije pokreta
unsigned long lastCheckTime = 0;           // Vrijeme zadnje provjere u loop() petlji
const unsigned long checkInterval = 100;   // Interval između provjera                    // Brojač pokreta                // Indikator za prvu poruku
bool lastSentState = false;                // Zadnje poslano stanje (false = slobodno, true = zauzeto)

unsigned long lastInactiveTimeReported = 0; // Dodana varijabla za praćenje posljednje prijavljene sekunde

unsigned long startTime = millis(); // Dodana varijabla za praćenje početnog vremena

void sendDataToAzureFunction(const String &jsonPayload)
{
  if (WiFi.status() == WL_CONNECTED)
  { // Provjera WiFi konekcije
    HTTPClient http;

    // Postavljanje URL-a funkcije
    http.begin(functionUrl);

    // Postavljanje HTTP headera za JSON podatke
    http.addHeader("Content-Type", "application/json");

    // Slanje POST zahtjeva
    int httpResponseCode = http.POST(jsonPayload);

    // Provjera odgovora servera
    if (httpResponseCode > 0)
    {
      String response = http.getString(); // Odgovor servera
      Logger.Info("Response: " + response);
    }
    else
    {
      Logger.Error("Error sending data: " + String(httpResponseCode));
    }

    // Zatvaranje konekcije
    http.end();
  }
  else
  {
    Logger.Error("WiFi not connected!");
  }
}

void checkPIRSensor()
{
  unsigned long currentTime = millis();
  static bool isRoomOccupied = false; // Trenutni status zauzetosti

  // Provjera svakih 100 ms
  if (currentTime - lastCheckTime >= checkInterval)
  {
    lastCheckTime = currentTime;

    int motionDetected = digitalRead(PIR_PIN);

    // Ako je detektiran pokret i prostorija je bila slobodna
    if (motionDetected == HIGH && !isRoomOccupied)
    {
      isRoomOccupied = true;        // Označi prostoriju kao zauzetu
      lastMotionTime = currentTime; // Pohrani vrijeme zadnjeg pokreta

      digitalWrite(RED_PIN, HIGH); // Crvena LED = zauzeto
      digitalWrite(GREEN_PIN, LOW);

      if (!lastSentState)
      { // Pošalji samo ako zadnje poslano stanje nije bilo "zauzeto"
        Logger.Info("Pokret detektiran! Slanje zauzetosti na IoT Hub.");
        String telemetryData = getTelemetryData(true); // true = zauzeto
        sendTelemetryData(telemetryData);
        sendDataToAzureFunction(telemetryData); // Slanje na Azure Function
        lastSentState = true;                   // Oznaka da je zadnje poslano stanje "zauzeto"
      }
    }
    // Ako nije bilo pokreta dulje od noMotionDelay i prostorija je zauzeta
    else if (motionDetected == LOW && isRoomOccupied && (currentTime - lastMotionTime >= noMotionDelay))
    {
      isRoomOccupied = false; // Označi prostoriju kao slobodnu

      digitalWrite(RED_PIN, LOW);
      digitalWrite(GREEN_PIN, HIGH); // Zelena LED = slobodno

      if (lastSentState)
      { // Pošalji samo ako zadnje poslano stanje nije već bilo "slobodno"
        Logger.Info("Nema pokreta dulje od 10 sekundi. Prostorija sada slobodna.");
        String telemetryData = getTelemetryData(false); // false = slobodno
        sendTelemetryData(telemetryData);
        sendDataToAzureFunction(telemetryData); // Slanje na Azure Function
        lastSentState = false;                  // Oznaka da je zadnje poslano stanje "slobodno"
      }
    }
  }
}

void sendTestMessageToIoTHub()
{
  az_result res = az_iot_hub_client_telemetry_get_publish_topic(&client, NULL, publishTopic, 200, NULL); // The receive topic isn't hardcoded and depends on chosen properties, therefore we need to use az_iot_hub_client_telemetry_get_publish_topic()
  Logger.Info(String(publishTopic));

  mqttClient.publish(publishTopic, deviceId); // Use https://github.com/Azure/azure-iot-explorer/releases to read the telemetry
}

bool initIoTHub()
{
  az_iot_hub_client_options options = az_iot_hub_client_options_default();

  if (az_result_failed(az_iot_hub_client_init(
          &client,
          az_span_create((uint8_t *)iotHubHost, strlen(iotHubHost)),
          az_span_create((uint8_t *)deviceId, strlen(deviceId)),
          &options)))
  {
    Logger.Error("Failed initializing Azure IoT Hub client");
    return false;
  }

  size_t client_id_length;
  if (az_result_failed(az_iot_hub_client_get_client_id(
          &client, mqttClientId, sizeof(mqttClientId) - 1, &client_id_length)))
  {
    Logger.Error("Failed getting client ID");
    return false;
  }

  size_t mqttUsernameSize;
  if (az_result_failed(az_iot_hub_client_get_user_name(
          &client, mqttUsername, sizeof(mqttUsername), &mqttUsernameSize)))
  {
    Logger.Error("Failed to get MQTT username ");
    return false;
  }

  Logger.Info("Successfully initialized Azure IoT Hub");
  Logger.Info("Client ID: " + String(mqttClientId));
  Logger.Info("Username: " + String(mqttUsername));

  return true;
}

// Funkcija za slanje podataka na Azure Function

void setup()
{
  setupWiFi();
  initializeTime();

  if (initIoTHub())
  {
    connectMQTT();
  }

  sendTestMessageToIoTHub();

  setupPIRSensor();

  Logger.Info("Setup done");
}

void loop()
{
  if (!mqttClient.connected())
  {
    Logger.Error("MQTT connection failed after multiple attempts, waiting 1 min before retry...");
    delay(60000);  // Pričekaj 60 sekundi prije ponovnog pokušaja
    connectMQTT(); // Ponovni pokušaj povezivanja
  }

  mqttClient.loop(); // Drži MQTT vezu aktivnom
  checkPIRSensor();  // Provjera PIR senzora
}
