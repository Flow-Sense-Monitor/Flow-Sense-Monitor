#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

#define WIFI_SSID      "Wokwi-GUEST"
#define WIFI_PASSWORD  ""
#define WIFI_CHANNEL   6

#define MQTT_HOST      "broker.hivemq.com"
#define MQTT_PORT      1883
#define MQTT_USER      ""
#define MQTT_PASS      ""

#define DEVICE_ID      "esp32-01"

#define PIN_TENSAO        34
#define PIN_CORRENTE      35
#define PIN_LED_VERDE     25
#define PIN_LED_VERMELHO  26
#define PIN_BUZZER        27
#define PIN_RELE          23

#define RELE_NIVEL_LIGADO HIGH

const float TENSAO_ESCALA_MAX   = 300.0;
const float CORRENTE_ESCALA_MAX = 30.0;

#define OLED_LARGURA  128
#define OLED_ALTURA    64
#define OLED_ENDERECO 0x3C
Adafruit_SSD1306 display(OLED_LARGURA, OLED_ALTURA, &Wire, -1);
bool oledOk = false;

struct Limites {
  float tensaoMin;
  float tensaoMax;
  float correnteMax;
};
Limites limites = {198.0, 242.0, 20.0};
// Tempo mínimo entre duas leituras publicadas quando os valores estão mudando (configurável pelo dashboard).
unsigned long intervaloEnvio = 2000;

const unsigned long TEMPO_DISPARO_MS = 5000;

// Envio por variação: só publica quando a energia oscila. Sem oscilação, publica
// mesmo assim a cada 1 min ("batimento"), para o backend saber que o ESP32 e a rede estão ok.
const unsigned long BATIMENTO_MS     = 60000;
const float VARIACAO_TENSAO_V        = 1.0;   // ~0,5% de 220 V; ignora ruído do ADC
const float VARIACAO_CORRENTE_A      = 0.10;

float tensao = 0, corrente = 0, potencia = 0;
double kwh = 0;
bool releLigado = true;
bool protecaoDisparada = false;
String motivoDisparo = "NORMAL";
String alertaAtual = "NORMAL";
bool foraDaFaixa = false;
unsigned long inicioFora = 0;
bool buzzerTocando = false;

// Valores da última leitura publicada, para saber se houve variação.
float tensaoEnviada = -1000, correnteEnviada = -1000;
bool releEnviado = true;
String alertaEnviado = "";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

String topicoLeituras, topicoAlertas, topicoComandos, topicoStatus;

unsigned long ultimoCalculo = 0;
unsigned long ultimoEnvio = 0;
unsigned long ultimaTentativaMqtt = 0;
unsigned long ultimoOled = 0;

unsigned long agoraEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (unsigned long)t : 0;
}

void aplicarRele() {
  digitalWrite(PIN_RELE, releLigado ? RELE_NIVEL_LIGADO : !RELE_NIVEL_LIGADO);
}

unsigned long segundosParaDesligar() {
  unsigned long passou = millis() - inicioFora;
  return passou >= TEMPO_DISPARO_MS ? 0 : (TEMPO_DISPARO_MS - passou + 999) / 1000;
}

void carregarConfig() {
  prefs.begin("flowsense", false);
  limites.tensaoMin   = prefs.getFloat("vMin", limites.tensaoMin);
  limites.tensaoMax   = prefs.getFloat("vMax", limites.tensaoMax);
  limites.correnteMax = prefs.getFloat("iMax", limites.correnteMax);
  intervaloEnvio      = prefs.getULong("intervalo", intervaloEnvio);
  prefs.end();
}

void salvarConfig() {
  prefs.begin("flowsense", false);
  prefs.putFloat("vMin", limites.tensaoMin);
  prefs.putFloat("vMax", limites.tensaoMax);
  prefs.putFloat("iMax", limites.correnteMax);
  prefs.putULong("intervalo", intervaloEnvio);
  prefs.end();
}

void lerSensores(unsigned long agora) {
  tensao = analogRead(PIN_TENSAO) * TENSAO_ESCALA_MAX / 4095.0;

  corrente = analogRead(PIN_CORRENTE) * CORRENTE_ESCALA_MAX / 4095.0;

  potencia = releLigado ? tensao * corrente : 0;
  kwh += potencia * (agora - ultimoCalculo) / 3600000.0 / 1000.0;
  ultimoCalculo = agora;
}

void publicarAlerta();

void avaliarRegraLocal(unsigned long agora) {
  String situacao = "NORMAL";
  if (corrente > limites.correnteMax)  situacao = "SOBRECORRENTE";
  else if (tensao > limites.tensaoMax) situacao = "SOBRETENSAO";
  else if (tensao < limites.tensaoMin) situacao = "SUBTENSAO";

  if (situacao != "NORMAL") {
    if (!foraDaFaixa) {
      foraDaFaixa = true;
      inicioFora = agora;
    }
  } else {
    foraDaFaixa = false;
  }

  bool desligouAgora = false;
  if (foraDaFaixa && releLigado && !protecaoDisparada && agora - inicioFora >= TEMPO_DISPARO_MS) {
    releLigado = false;
    protecaoDisparada = true;
    motivoDisparo = situacao;
    aplicarRele();
    desligouAgora = true;
    Serial.printf("[PROTECAO] %s por 5 s - rele desligado (%.1f V, %.2f A). Religue pelo dashboard.\n",
                  situacao.c_str(), tensao, corrente);
  }

  String novoAlerta = protecaoDisparada ? motivoDisparo : situacao;
  if (novoAlerta != alertaAtual || desligouAgora) {
    alertaAtual = novoAlerta;
    Serial.printf("[ALERTA] %s (%.1f V, %.2f A)\n", alertaAtual.c_str(), tensao, corrente);
    publicarAlerta();
  }
}

void atualizarSaidas() {
  bool emAlerta = alertaAtual != "NORMAL";
  digitalWrite(PIN_LED_VERDE, emAlerta ? LOW : HIGH);
  digitalWrite(PIN_LED_VERMELHO, emAlerta ? HIGH : LOW);

  bool deveTocar = protecaoDisparada || (emAlerta && (millis() / 300) % 2 == 0);
  if (deveTocar != buzzerTocando) {
    if (deveTocar) tone(PIN_BUZZER, 2000);
    else noTone(PIN_BUZZER);
    buzzerTocando = deveTocar;
  }
}

void publicarLeitura() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["dispositivo"] = DEVICE_ID;
  doc["tensao"]      = serialized(String(tensao, 2));
  doc["corrente"]    = serialized(String(corrente, 2));
  doc["potencia"]    = serialized(String(potencia, 2));
  doc["kwh"]         = serialized(String(kwh, 5));
  doc["rele"]        = releLigado ? "ON" : "OFF";
  doc["alerta"]      = alertaAtual;
  JsonObject lim = doc["limites"].to<JsonObject>();
  lim["tensaoMin"]   = limites.tensaoMin;
  lim["tensaoMax"]   = limites.tensaoMax;
  lim["correnteMax"] = limites.correnteMax;
  doc["intervalo"]   = intervaloEnvio;
  doc["timestamp"]   = agoraEpoch();

  char buffer[400];
  serializeJson(doc, buffer, sizeof(buffer));
  if (!mqtt.publish(topicoLeituras.c_str(), buffer)) {
    Serial.println("[MQTT] Falha ao publicar leitura");
    ultimoEnvio = millis();
    alertaEnviado = "";  // força nova tentativa depois de "intervaloEnvio", sem martelar o broker
    return;
  }
  ultimoEnvio     = millis();
  tensaoEnviada   = tensao;
  correnteEnviada = corrente;
  releEnviado     = releLigado;
  alertaEnviado   = alertaAtual;
  Serial.printf("[MQTT] %s -> %s\n", topicoLeituras.c_str(), buffer);
}

bool energiaOscilou() {
  return fabsf(tensao - tensaoEnviada) >= VARIACAO_TENSAO_V
      || fabsf(corrente - correnteEnviada) >= VARIACAO_CORRENTE_A
      || releLigado != releEnviado
      || alertaAtual != alertaEnviado;
}

void publicarAlerta() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["dispositivo"] = DEVICE_ID;
  doc["tipo"]        = alertaAtual;
  doc["tensao"]      = serialized(String(tensao, 2));
  doc["corrente"]    = serialized(String(corrente, 2));
  doc["rele"]        = releLigado ? "ON" : "OFF";
  doc["timestamp"]   = agoraEpoch();

  char buffer[256];
  serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(topicoAlertas.c_str(), buffer);
  Serial.printf("[MQTT] %s -> %s\n", topicoAlertas.c_str(), buffer);
}

void aoReceberComando(char* topico, byte* payload, unsigned int tamanho) {
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, payload, tamanho);
  if (erro) {
    Serial.printf("[CMD] JSON invalido: %s\n", erro.c_str());
    return;
  }

  bool mudouConfig = false;

  if (doc["rele"].is<const char*>()) {
    String cmd = doc["rele"].as<String>();
    cmd.toUpperCase();
    if (cmd == "ON") {
      releLigado = true;
      protecaoDisparada = false;
      motivoDisparo = "NORMAL";
      foraDaFaixa = false;
    } else if (cmd == "OFF") {
      releLigado = false;
      protecaoDisparada = false;
      motivoDisparo = "NORMAL";
    } else {
      Serial.printf("[CMD] Valor de rele invalido: %s\n", cmd.c_str());
    }
    aplicarRele();
  }

  JsonObject lim = doc["limites"];
  if (!lim.isNull()) {
    float vMin = lim["tensaoMin"]   | limites.tensaoMin;
    float vMax = lim["tensaoMax"]   | limites.tensaoMax;
    float iMax = lim["correnteMax"] | limites.correnteMax;
    if (vMin > 0 && vMin < vMax && vMax <= TENSAO_ESCALA_MAX && iMax > 0 && iMax <= CORRENTE_ESCALA_MAX) {
      limites = {vMin, vMax, iMax};
      mudouConfig = true;
    } else {
      Serial.println("[CMD] Limites rejeitados (valores fora da faixa)");
    }
  }

  if (doc["intervalo"].is<unsigned long>()) {
    unsigned long ms = doc["intervalo"];
    if (ms >= 500 && ms <= 60000) {
      intervaloEnvio = ms;
      mudouConfig = true;
    } else {
      Serial.println("[CMD] Intervalo rejeitado (use 500 a 60000 ms)");
    }
  }

  if (mudouConfig) salvarConfig();
  Serial.printf("[CMD] Recebido: %.*s\n", tamanho, (const char*)payload);
  avaliarRegraLocal(millis());
  publicarLeitura();
}

void conectarMqtt() {
  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  const char* usuario = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* senha   = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;

  Serial.printf("[MQTT] Conectando em %s:%d ... ", MQTT_HOST, MQTT_PORT);

  bool ok = mqtt.connect(clientId.c_str(), usuario, senha, topicoStatus.c_str(), 1, true, "offline");
  if (ok) {
    Serial.println("ok");
    mqtt.publish(topicoStatus.c_str(), "online", true);
    mqtt.subscribe(topicoComandos.c_str(), 1);
    if (alertaAtual != "NORMAL") publicarAlerta();
    publicarLeitura();
  } else {
    Serial.printf("falhou (estado %d)\n", mqtt.state());
  }
}

void atualizarOled() {
  if (!oledOk) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  display.printf("FlowSense  %s %s\n",
                 WiFi.status() == WL_CONNECTED ? "WiFi" : "----",
                 mqtt.connected() ? "MQTT" : "----");
  display.drawFastHLine(0, 9, OLED_LARGURA, SSD1306_WHITE);
  display.setCursor(0, 12);
  display.printf("Tensao:   %6.1f V\n", tensao);
  display.printf("Corrente: %6.2f A\n", corrente);
  display.printf("Potencia: %6.0f W\n", potencia);
  display.printf("Consumo: %.4f kWh\n", kwh);

  if (protecaoDisparada) {
    display.print("Rele:OFF religue site\n");
  } else {
    display.printf("Rele: %s\n", releLigado ? "LIGADO" : "DESLIGADO");
  }

  if (alertaAtual == "NORMAL") display.print("Status: NORMAL");
  else if (!protecaoDisparada && releLigado) display.printf("! %s %lus", alertaAtual.c_str(), segundosParaDesligar());
  else display.printf("! %s", alertaAtual.c_str());

  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELE, OUTPUT);
  analogReadResolution(12);

  carregarConfig();
  aplicarRele();

  Wire.begin(21, 22);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ENDERECO);
  if (!oledOk) Serial.println("OLED nao encontrado");

  String base = String("flowsense/") + DEVICE_ID;
  topicoLeituras = base + "/leituras";
  topicoAlertas  = base + "/alertas";
  topicoComandos = base + "/comandos";
  topicoStatus   = base + "/status";

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(aoReceberComando);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(3);

  ultimoCalculo = millis();
  Serial.printf("FlowSense iniciado. Dispositivo: %s\n", DEVICE_ID);
}

void loop() {
  unsigned long agora = millis();

  lerSensores(agora);
  avaliarRegraLocal(agora);
  atualizarSaidas();

  if (agora - ultimoOled >= 500) {
    ultimoOled = agora;
    atualizarOled();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (agora - ultimaTentativaMqtt >= 5000) {
        ultimaTentativaMqtt = agora;
        conectarMqtt();
      }
    } else {
      mqtt.loop();
      unsigned long desdeEnvio = millis() - ultimoEnvio;
      bool porVariacao = energiaOscilou() && desdeEnvio >= intervaloEnvio;
      bool porBatimento = desdeEnvio >= BATIMENTO_MS;
      if (porVariacao || porBatimento) {
        if (porBatimento && !porVariacao) Serial.println("[MQTT] Sem oscilacao ha 1 min - enviando batimento");
        publicarLeitura();
      }
    }
  }

  delay(10);
}
