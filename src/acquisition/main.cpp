#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <math.h>
#include "config.h"
#include "telemetry.h"

namespace {
constexpr uint32_t SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t FLOW_INTERVAL_MS = 2000;
constexpr uint32_t SEND_INTERVAL_MS = 250;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t FLOW_SAVE_INTERVAL_MS = 300000;
constexpr uint8_t ADC_SAMPLES = 5;
constexpr uint8_t FILTER_SAMPLES = 3;
constexpr float FILTER_ALPHA = 0.4f;

struct Sensor {
  Sensor(int gpio, float series, float nominal, float betaValue, float offset,
         uint8_t mask)
      : pin(gpio), seriesOhms(series), nominalOhms(nominal), beta(betaValue),
        offsetC(offset), validBit(mask) {}

  int pin;
  float seriesOhms;
  float nominalOhms;
  float beta;
  float offsetC;
  uint8_t validBit;
  float temperatures[FILTER_SAMPLES] = {};
  uint8_t count = 0;
  uint8_t next = 0;
  float celsius = NAN;
  float millivolts = NAN;
  bool valid = false;
};

Sensor sensors[] = {
  {NTC_PIN, SERIES_OHMS, NTC_OHMS, NTC_BETA, BOILER_TEMP_OFFSET_C, BoilerTelemetry::BOILER_VALID},
  {FLOW_INLET_TEMP_PIN, FLOW_TEMP_SERIES_OHMS, FLOW_TEMP_NTC_OHMS,
   FLOW_TEMP_NTC_BETA, INLET_TEMP_OFFSET_C, BoilerTelemetry::INLET_VALID},
  {FLOW_OUTLET_TEMP_PIN, FLOW_TEMP_SERIES_OHMS, FLOW_TEMP_NTC_OHMS,
   FLOW_TEMP_NTC_BETA, OUTLET_TEMP_OFFSET_C, BoilerTelemetry::OUTLET_VALID}
};

volatile uint32_t inletPulseCount = 0;
volatile uint32_t outletPulseCount = 0;
uint64_t inletPulses = 0;
uint64_t outletPulses = 0;
uint32_t sequenceNumber = 0;
uint32_t bootId = 0;
uint32_t sampledAt = 0;
uint32_t flowSampledAt = 0;
uint32_t sentAt = 0;
uint32_t flowSavedAt = 0;
uint32_t wifiStartedAt = 0;
uint32_t lastWifiRetryAt = 0;
bool provisioningActive = false;
Preferences preferences;
WebServer provisioningServer(80);
WiFiUDP udp;

void IRAM_ATTR countInletPulse() { ++inletPulseCount; }
void IRAM_ATTR countOutletPulse() { ++outletPulseCount; }

float medianMillivolts(int pin) {
  float values[ADC_SAMPLES];
  for (uint8_t i = 0; i < ADC_SAMPLES; ++i)
    values[i] = static_cast<float>(analogReadMilliVolts(pin));
  for (uint8_t i = 1; i < ADC_SAMPLES; ++i) {
    const float value = values[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && values[j] > value) {
      values[j + 1] = values[j];
      --j;
    }
    values[j + 1] = value;
  }
  return values[ADC_SAMPLES / 2];
}

void sampleSensor(Sensor &sensor) {
  sensor.millivolts = medianMillivolts(sensor.pin);
  if (!isfinite(sensor.millivolts) || sensor.millivolts <= 100
      || sensor.millivolts >= 3000 || sensor.millivolts >= SUPPLY_MV) {
    sensor.valid = false;
    sensor.count = 0;
    sensor.next = 0;
    sensor.celsius = NAN;
    return;
  }

  const float resistance = sensor.seriesOhms * sensor.millivolts
      / (SUPPLY_MV - sensor.millivolts);
  const float kelvin = 1.0f / (1.0f / 298.15f
      + logf(resistance / sensor.nominalOhms) / sensor.beta);
  const float celsius = kelvin - 273.15f + sensor.offsetC;
  if (!isfinite(celsius) || celsius < 0 || celsius > 110) {
    sensor.valid = false;
    sensor.count = 0;
    sensor.next = 0;
    sensor.celsius = NAN;
    return;
  }

  sensor.temperatures[sensor.next] = celsius;
  sensor.next = (sensor.next + 1) % FILTER_SAMPLES;
  if (sensor.count < FILTER_SAMPLES) ++sensor.count;

  float total = 0;
  for (uint8_t i = 0; i < sensor.count; ++i) total += sensor.temperatures[i];
  const float average = total / sensor.count;
  sensor.celsius = sensor.valid
      ? FILTER_ALPHA * average + (1.0f - FILTER_ALPHA) * sensor.celsius
      : average;
  sensor.valid = true;
}

void takeFlowSnapshot() {
  const uint32_t now = millis();
  if (now - flowSampledAt < FLOW_INTERVAL_MS) return;
  noInterrupts();
  const uint32_t inlet = inletPulseCount;
  const uint32_t outlet = outletPulseCount;
  inletPulseCount = 0;
  outletPulseCount = 0;
  interrupts();
  inletPulses += inlet;
  outletPulses += outlet;
  flowSampledAt = now;
}

void startProvisioningPortal() {
  if (provisioningActive) return;
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP("Boiler-Aquisicao", AP_PASSWORD))
    Serial.println("Erro ao iniciar ponto de acesso para configurar Wi-Fi");

  provisioningServer.on("/", HTTP_GET, [] {
    provisioningServer.send(200, "text/html; charset=utf-8",
      "<!doctype html><html lang='pt-BR'><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width'>"
      "<title>Wi-Fi da aquisicao</title><h1>Configurar Wi-Fi</h1>"
      "<form method='post' action='/wifi'>"
      "<label>Rede 2,4 GHz <input name='ssid' maxlength='32' required></label><br>"
      "<label>Senha <input name='password' type='password' maxlength='63'></label><br>"
      "<button>Salvar e conectar</button></form></html>");
  });
  provisioningServer.on("/wifi", HTTP_POST, [] {
    const String ssid = provisioningServer.arg("ssid");
    const String password = provisioningServer.arg("password");
    if (!ssid.length() || ssid.length() > 32 || password.length() > 63) {
      provisioningServer.send(400, "text/plain", "Credenciais Wi-Fi invalidas");
      return;
    }
    const size_t ssidBytes = preferences.putString("ssid", ssid);
    const size_t passwordBytes = preferences.putString("password", password);
    if (ssidBytes != ssid.length() + 1 || passwordBytes != password.length() + 1) {
      provisioningServer.send(500, "text/plain", "Falha ao salvar credenciais");
      Serial.println("Erro ao salvar credenciais Wi-Fi da aquisicao");
      return;
    }
    provisioningServer.send(200, "text/plain",
      "Credenciais salvas. A unidade de aquisicao vai reiniciar.");
    delay(500);
    ESP.restart();
  });
  provisioningServer.begin();
  provisioningActive = true;
  Serial.printf("Configuracao Wi-Fi: conecte em Boiler-Aquisicao, http://%s\n",
                WiFi.softAPIP().toString().c_str());
}

void connectToSavedWifi() {
  const String ssid = preferences.getString("ssid", "");
  if (!ssid.length()) {
    startProvisioningPortal();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), preferences.getString("password", "").c_str());
  wifiStartedAt = millis();
  Serial.printf("Conectando a rede Wi-Fi %s\n", ssid.c_str());
}

void sendFrame() {
  if (WiFi.status() != WL_CONNECTED || millis() - sentAt < SEND_INTERVAL_MS) return;
  sentAt = millis();

  BoilerTelemetry::Frame frame{};
  frame.magic = BoilerTelemetry::MAGIC;
  frame.version = BoilerTelemetry::VERSION;
  frame.sequence = ++sequenceNumber;
  frame.bootId = bootId;
  for (uint8_t i = 0; i < 3; ++i) {
    if (sensors[i].valid) {
      frame.validMask |= sensors[i].validBit;
      frame.temperatureCentiC[i] = static_cast<int16_t>(lroundf(sensors[i].celsius * 100));
    } else {
      frame.temperatureCentiC[i] = INT16_MIN;
    }
    frame.millivolts[i] = isfinite(sensors[i].millivolts)
        ? static_cast<uint16_t>(constrain(lroundf(sensors[i].millivolts), 0L, 65535L))
        : 0;
  }
  frame.inletPulses = inletPulses;
  frame.outletPulses = outletPulses;
  frame.crc = BoilerTelemetry::crc16(reinterpret_cast<const uint8_t *>(&frame),
      sizeof(frame) - sizeof(frame.crc));

  const IPAddress broadcast(255, 255, 255, 255);
  if (!udp.beginPacket(broadcast, BoilerTelemetry::UDP_PORT)) {
    Serial.println("Falha ao iniciar pacote de telemetria UDP");
    return;
  }
  if (udp.write(reinterpret_cast<const uint8_t *>(&frame), sizeof(frame)) != sizeof(frame)
      || !udp.endPacket())
    Serial.println("Falha ao enviar pacote de telemetria UDP");
}

void saveFlowTotalsIfDue() {
  const uint32_t now = millis();
  if (now - flowSavedAt < FLOW_SAVE_INTERVAL_MS) return;
  flowSavedAt = now;
  if (preferences.putULong64("inPulses", inletPulses) != sizeof(inletPulses))
    Serial.println("Erro ao salvar pulsos acumulados da entrada");
  if (preferences.putULong64("outPulses", outletPulses) != sizeof(outletPulses))
    Serial.println("Erro ao salvar pulsos acumulados da saida");
}
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  for (Sensor &sensor : sensors)
    analogSetPinAttenuation(sensor.pin, ADC_11db);

  pinMode(FLOW_INLET_PIN, INPUT);
  pinMode(FLOW_OUTLET_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_INLET_PIN), countInletPulse, RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW_OUTLET_PIN), countOutletPulse, RISING);

  preferences.begin("boiler-acq", false);
  inletPulses = preferences.getULong64("inPulses", 0);
  outletPulses = preferences.getULong64("outPulses", 0);
  bootId = esp_random();
  flowSampledAt = millis();
  flowSavedAt = millis();
  udp.begin(BoilerTelemetry::UDP_PORT);
  connectToSavedWifi();
}

void loop() {
  const uint32_t now = millis();
  if (provisioningActive) provisioningServer.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    if (!provisioningActive && now - wifiStartedAt >= WIFI_CONNECT_TIMEOUT_MS)
      startProvisioningPortal();
    if (provisioningActive && now - lastWifiRetryAt >= 10000) {
      lastWifiRetryAt = now;
      const String ssid = preferences.getString("ssid", "");
      if (ssid.length()) WiFi.begin(ssid.c_str(), preferences.getString("password", "").c_str());
    }
  } else if (provisioningActive) {
    provisioningServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    provisioningActive = false;
    Serial.printf("Wi-Fi conectado; IP da aquisicao: %s\n",
                  WiFi.localIP().toString().c_str());
  }

  if (now - sampledAt >= SAMPLE_INTERVAL_MS) {
    sampledAt = now;
    for (Sensor &sensor : sensors) sampleSensor(sensor);
  }
  takeFlowSnapshot();
  saveFlowTotalsIfDue();
  sendFrame();
  delay(2);
}
