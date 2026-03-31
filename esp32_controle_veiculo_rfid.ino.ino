#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal.h>
#include <ArduinoJson.h>
#include <time.h>

// =====================================================
// WIFI
// =====================================================
const char* WIFI_SSID = "IoT";
const char* WIFI_PASS = "12345678";

// =====================================================
// MQTT / AWS / MOSQUITTO
// =====================================================
const char* MQTT_BROKER = "3.143.217.29";
const int   MQTT_PORT   = 1883;
const char* MQTT_CLIENT_ID = "ESP32S3_CAR_01";
const char* MQTT_TOPIC_EVENTS = "veiculo/acesso/eventos";
const char* MQTT_TOPIC_STATUS = "veiculo/acesso/status";

// Se você configurar usuário/senha no Mosquitto, preencha aqui.
// Se não usar autenticação, deixe vazio.
const char* MQTT_USER = "";
const char* MQTT_PASS = "";

// =====================================================
// NTP
// =====================================================
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "a.st1.ntp.br";
const char* NTP_SERVER_3 = "b.st1.ntp.br";

// Brasil UTC-3
const long GMT_OFFSET_SEC = -3 * 3600;
const int  DAYLIGHT_OFFSET_SEC = 0;

// =====================================================
// PINOS
// =====================================================
// RFID RC522
#define RFID_SS_PIN         10
#define RFID_RST_PIN        9
#define RFID_SCK_PIN        12
#define RFID_MOSI_PIN       11
#define RFID_MISO_PIN       13

// Botão, buzzer e relé
#define START_BUTTON_PIN    4
#define BUZZER_PIN          5
#define ENGINE_ENABLE_PIN   6

// LCD 16x2 comum
#define LCD_RS              15
#define LCD_EN              16
#define LCD_D4              17
#define LCD_D5              18
#define LCD_D6              19
#define LCD_D7              20

// =====================================================
// CONFIGURAÇÕES DO RELÉ
// =====================================================
// true  = relé acionado com LOW
// false = relé acionado com HIGH
const bool RELAY_ACTIVE_LOW = false;

// =====================================================
// CONFIGURAÇÕES GERAIS
// =====================================================
const unsigned long AUTH_TIMEOUT_MS   = 60000;
const unsigned long DEBOUNCE_MS       = 80;
const unsigned long LCD_SHORT_MSG_MS  = 1800;
const unsigned long WIFI_RETRY_MS     = 10000;
const unsigned long MQTT_RETRY_MS     = 5000;

// =====================================================
// OBJETOS
// =====================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);

MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);
LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// =====================================================
// ESTADOS DO SISTEMA
// =====================================================
enum SystemState {
  IDLE,
  WAITING_CREDENTIAL,
  AUTHORIZED,
  TIMEOUT_BLOCKED
};

SystemState state = IDLE;

// =====================================================
// VARIÁVEIS DE CONTROLE
// =====================================================
unsigned long authStartMillis = 0;
unsigned long lastButtonChangeMillis = 0;
unsigned long lastWifiRetryMillis = 0;
unsigned long lastMqttRetryMillis = 0;

bool lastButtonReading = HIGH;

String currentDriverName = "";
String currentDriverUID  = "";

// =====================================================
// CARTÕES AUTORIZADOS
// =====================================================
struct AuthorizedCard {
  const char* uid;
  const char* name;
};

AuthorizedCard authorizedCards[] = {
  {"41D87E05", "Matheus"},
  {"0191AF9B", "Marina"}
};

const int authorizedCardsCount = sizeof(authorizedCards) / sizeof(authorizedCards[0]);

// =====================================================
// FUNÇÕES AUXILIARES
// =====================================================
void lcdShow2Lines(const String &line1, const String &line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void setBuzzer(bool on) {
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}

void setEngineEnabled(bool enabled) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(ENGINE_ENABLE_PIN, enabled ? LOW : HIGH);
  } else {
    digitalWrite(ENGINE_ENABLE_PIN, enabled ? HIGH : LOW);
  }
}

String normalizeUID(byte *buffer, byte bufferSize) {
  String uid = "";
  for (byte i = 0; i < bufferSize; i++) {
    if (buffer[i] < 0x10) uid += "0";
    uid += String(buffer[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

String getDriverNameByUID(const String &uid) {
  for (int i = 0; i < authorizedCardsCount; i++) {
    if (uid.equalsIgnoreCase(authorizedCards[i].uid)) {
      return String(authorizedCards[i].name);
    }
  }
  return "";
}

String getTimestampISO() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 2000)) {
    return "1970-01-01T00:00:00";
  }

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buffer);
}

bool ntpTimeIsValid() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) {
    return false;
  }
  return (timeinfo.tm_year > (2024 - 1900));
}

// =====================================================
// WIFI
// =====================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  lcdShow2Lines("Conectando WiFi", "Aguarde...");
  Serial.print("Conectando Wi-Fi");

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi conectado");
    Serial.print("IP local: ");
    Serial.println(WiFi.localIP());

    lcdShow2Lines("WiFi conectado", WiFi.localIP().toString());
    delay(1200);
  } else {
    Serial.println("\nFalha no Wi-Fi");
    lcdShow2Lines("Falha WiFi", "Modo local");
    delay(1200);
  }
}

// =====================================================
// NTP
// =====================================================
void initNTP() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  lcdShow2Lines("Sincronizando", "horario NTP");
  Serial.println("Sincronizando horario via NTP...");

  int tentativas = 0;
  while (!ntpTimeIsValid() && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  Serial.println();

  if (ntpTimeIsValid()) {
    String ts = getTimestampISO();
    Serial.println("Horario sincronizado: " + ts);
    lcdShow2Lines("Hora sincronizada", ts.substring(11, 16));
    delay(1200);
  } else {
    Serial.println("Nao foi possivel sincronizar NTP");
    lcdShow2Lines("Falha no NTP", "Sem hora real");
    delay(1200);
  }
}

// =====================================================
// MQTT
// =====================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Não usado por enquanto, mas mantido para expansão futura
  Serial.print("Mensagem recebida no topico: ");
  Serial.println(topic);
}

void setupMQTT() {
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

bool connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.print("Conectando ao broker MQTT... ");

  bool connected;
  if (strlen(MQTT_USER) > 0) {
    connected = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
  } else {
    connected = mqttClient.connect(MQTT_CLIENT_ID);
  }

  if (connected) {
    Serial.println("OK");
    mqttClient.publish(MQTT_TOPIC_STATUS, "online", true);
    lcdShow2Lines("MQTT conectado", "Broker AWS OK");
    delay(1000);
    return true;
  } else {
    Serial.print("falhou, rc=");
    Serial.println(mqttClient.state());
    lcdShow2Lines("Falha MQTT", "Ver broker");
    delay(1000);
    return false;
  }
}

bool publishEvent(const String &eventType,
                  const String &driverName,
                  const String &uid,
                  const String &statusText) {
  if (!mqttClient.connected()) {
    Serial.println("MQTT offline. Evento nao publicado.");
    return false;
  }

  JsonDocument doc;
  doc["device"] = "ESP32S3-CAR-01";
  doc["event_type"] = eventType;
  doc["driver_name"] = driverName;
  doc["uid"] = uid;
  doc["status"] = statusText;
  doc["timestamp"] = getTimestampISO();
  doc["timestamp_valid"] = ntpTimeIsValid();
  doc["ip"] = WiFi.localIP().toString();
  doc["state"] = (int)state;

  String payload;
  serializeJson(doc, payload);

  bool ok = mqttClient.publish(MQTT_TOPIC_EVENTS, payload.c_str(), false);

  Serial.println("=================================");
  Serial.println("PUBLICACAO MQTT");
  Serial.print("Topico: ");
  Serial.println(MQTT_TOPIC_EVENTS);
  Serial.println(payload);
  Serial.print("Resultado: ");
  Serial.println(ok ? "OK" : "FALHOU");
  Serial.println("=================================");


  return ok;
}

// =====================================================
// CONTROLE DO SISTEMA
// =====================================================
void enterIdleState() {
  state = IDLE;
  currentDriverName = "";
  currentDriverUID = "";

  setBuzzer(false);
  setEngineEnabled(false);

  lcdShow2Lines("Sistema pronto", "Aguardando...");
  Serial.println("Estado -> IDLE");
}

void startAuthenticationProcess() {
  state = WAITING_CREDENTIAL;
  authStartMillis = millis();
  currentDriverName = "";
  currentDriverUID = "";

  setEngineEnabled(false);
  setBuzzer(true);

  lcdShow2Lines("Para seguir use", "credencial RFID");

  Serial.println("Estado -> WAITING_CREDENTIAL");
  publishEvent("start_requested", "", "", "Aguardando credencial");
}

void authorizeDriver(const String &uid, const String &name) {
  state = AUTHORIZED;
  currentDriverUID = uid;
  currentDriverName = name;

  setBuzzer(false);
  setEngineEnabled(true);

  lcdShow2Lines("Ligando o motor", name);
  Serial.println("Credencial valida: " + name + " | UID: " + uid);

  publishEvent("authorized", name, uid, "Motor liberado");

  delay(LCD_SHORT_MSG_MS);
  lcdShow2Lines("Motor Ligado", name);
}

void denyUnknownCard(const String &uid) {
  setEngineEnabled(false);
  setBuzzer(true);

  lcdShow2Lines("Cartao invalido", "Acesso negado");
  Serial.println("Cartao nao autorizado: " + uid);

  publishEvent("unauthorized_card", "", uid, "Cartao nao autorizado");

  delay(LCD_SHORT_MSG_MS);
  lcdShow2Lines("Para seguir use", "credencial RFID");
}

void handleTimeout() {
  state = TIMEOUT_BLOCKED;

  setBuzzer(false);
  setEngineEnabled(false);

  lcdShow2Lines("Tempo esgotado", "Motor bloqueado");
  Serial.println("Timeout sem credencial");

  publishEvent("timeout", "", "", "Tempo expirado sem credencial");

  delay(2500);
  enterIdleState();
}

bool startButtonPressed() {
  bool reading = digitalRead(START_BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastButtonChangeMillis = millis();
    lastButtonReading = reading;
  }

  if ((millis() - lastButtonChangeMillis) > DEBOUNCE_MS) {
    if (reading == LOW) {
      delay(20);
      while (digitalRead(START_BUTTON_PIN) == LOW) {
        delay(5);
      }
      return true;
    }
  }
  return false;
}

bool readRFIDCard(String &uidOut, String &nameOut) {
  if (!mfrc522.PICC_IsNewCardPresent()) return false;
  if (!mfrc522.PICC_ReadCardSerial()) return false;

  String uid = normalizeUID(mfrc522.uid.uidByte, mfrc522.uid.size);
  String name = getDriverNameByUID(uid);

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  uidOut = uid;
  nameOut = name;
  return true;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(ENGINE_ENABLE_PIN, OUTPUT);

  setBuzzer(false);
  setEngineEnabled(false);

  lcd.begin(16, 2);
  lcdShow2Lines("Inicializando", "Sistema...");

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  mfrc522.PCD_Init();
  Serial.println("RC522 inicializado");

  connectWiFi();
  initNTP();
  setupMQTT();
  connectMQTT();

  enterIdleState();
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetryMillis >= WIFI_RETRY_MS) {
      lastWifiRetryMillis = millis();
      connectWiFi();
      initNTP();
    }
  }

  if (!mqttClient.connected()) {
    if (millis() - lastMqttRetryMillis >= MQTT_RETRY_MS) {
      lastMqttRetryMillis = millis();
      connectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  switch (state) {
    case IDLE:
      if (startButtonPressed()) {
        startAuthenticationProcess();
      }
      break;

    case WAITING_CREDENTIAL: {
      setBuzzer(true);

      if (millis() - authStartMillis >= AUTH_TIMEOUT_MS) {
        handleTimeout();
        break;
      }

      String uid, name;
      if (readRFIDCard(uid, name)) {
        Serial.println("UID lido: " + uid);

        if (name.length() > 0) {
          authorizeDriver(uid, name);
        } else {
          denyUnknownCard(uid);
        }
      }
      break;
    }

    case AUTHORIZED:
      if (startButtonPressed()) {
        publishEvent("manual_reset", currentDriverName, currentDriverUID, "Sistema reiniciado");
        enterIdleState();
      }
      break;

    case TIMEOUT_BLOCKED:
      enterIdleState();
      break;
  }
}