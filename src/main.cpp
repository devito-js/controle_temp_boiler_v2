#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <math.h>
#include "config.h"

WebServer server(80);
Preferences prefs;
TFT_eSPI screen;
TFT_eSprite dashboard(&screen);
float temperature = NAN, target = 50;
bool enabled = false, relay = false, fault = false;
bool manualMode = false;
bool autoEnabledBeforeManual = false;
void output(bool on);
int startMinute = 360, stopMinute = 480;
unsigned goodSamples = 0;
uint32_t sampled = 0, painted = 0, lastOff = 0;
String state = "Inicializando";
constexpr uint8_t BACKLIGHT_CHANNEL = 0;
uint32_t displayActivity = 0, targetChangedAt = 0;
bool displayIdle = false, targetDirty = false;

struct Button {
  int pin;
  explicit Button(int gpio) : pin(gpio) {}
  bool raw = HIGH, stable = HIGH;
  uint32_t changedAt = 0, heldSince = 0;
  bool tracking = false, consumed = false;
  void begin() {
    // GPIO35 usa o pull-up externo existente na placa.
    pinMode(pin, pin == 35 ? INPUT : INPUT_PULLUP);
    raw = stable = digitalRead(pin);
    changedAt = millis();
  }
  // 1 = pressionou, 2 = toque curto ao soltar, 3 = toque longo.
  int event(uint32_t now) {
    bool reading = digitalRead(pin);
    if (reading != raw) { raw = reading; changedAt = now; }
    if (raw != stable && now - changedAt >= 40) {
      stable = raw;
      if (stable == LOW) {
        tracking = true; consumed = false; heldSince = now; return 1;
      }
      bool shortPress = tracking && !consumed;
      tracking = false;
      return shortPress ? 2 : 0;
    }
    if (tracking && !consumed && raw == LOW && now - heldSince >= MODE_HOLD_MS) {
      consumed = true; return 3;
    }
    return 0;
  }
};
Button upButton{TEMP_UP_PIN}, downButton{TEMP_DOWN_PIN};

void wakeDisplay() {
  displayActivity = millis();
  displayIdle = false;
  ledcWrite(BACKLIGHT_CHANNEL, DISPLAY_BRIGHTNESS);
  painted = millis() - 1000;
}

void handleButtonsAndBacklight() {
  uint32_t now = millis();
  static bool suppressUp = false, suppressDown = false;
  int up = upButton.event(now), down = downButton.event(now);
  if (up == 1 || down == 1) {
    if (up == 1) suppressUp = displayIdle;
    if (down == 1) suppressDown = displayIdle;
    wakeDisplay();
  }
  if (upButton.raw == LOW && downButton.raw == LOW) {
    suppressUp = suppressDown = true;
    upButton.consumed = downButton.consumed = true;
  }
  if (up == 3 && downButton.raw == HIGH) {
    output(false);
    if (!manualMode) {
      autoEnabledBeforeManual = enabled;
      manualMode = true;
      enabled = true;
    } else {
      manualMode = false;
      enabled = autoEnabledBeforeManual;
    }
    wakeDisplay();
    sampled = now - 500; // Reavaliar imediatamente a agenda e a temperatura.
  }
  bool increase = up == 2 && !suppressUp;
  bool decrease = down == 2 && !suppressDown;
  if (increase != decrease) {
    float next = constrain(target + (increase ? TEMP_STEP : -TEMP_STEP), 20.0f, 70.0f);
    if (next != target) { target = next; targetDirty = true; targetChangedAt = now; }
    wakeDisplay();
    sampled = now - 500;
  }
  // Agrupa ajustes para nao gravar a flash a cada toque.
  if (targetDirty && now - targetChangedAt >= 1500) {
    prefs.putFloat("target", target);
    targetDirty = false;
  }
  if (!displayIdle && now - displayActivity >= DISPLAY_TIMEOUT_MS) {
    displayIdle = true;
    ledcWrite(BACKLIGHT_CHANNEL, DISPLAY_IDLE_BRIGHTNESS);
  }
}

void output(bool on) {
  bool changed = relay != on;
  if (relay && !on) lastOff = millis();
  relay = on;
  const int level = on ? (RELAY_ACTIVE_LOW ? LOW : HIGH)
                       : (RELAY_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(RELAY_PIN, level);
  if (changed) {
    Serial.printf("Rele COMANDO=%s GPIO%d=%s modo=%s\n", on ? "ON" : "OFF",
                  RELAY_PIN, level == HIGH ? "HIGH" : "LOW", manualMode ? "MANUAL" : "AUTO");
  }
}
bool clockReady() { return time(nullptr) > 1700000000; }
bool scheduled(int minute) {
  if (startMinute == stopMinute) return false;
  return startMinute < stopMinute ? minute >= startMinute && minute < stopMinute
                                 : minute >= startMinute || minute < stopMinute;
}
String hhmm(int minute) {
  char s[6]; snprintf(s, sizeof(s), "%02d:%02d", minute / 60, minute % 60); return String(s);
}
int parseTime(const String &s) {
  if (s.length() != 5 || s[2] != ':' || !isDigit(s[0]) || !isDigit(s[1]) || !isDigit(s[3]) || !isDigit(s[4])) return -1;
  int h = s.substring(0, 2).toInt(), m = s.substring(3).toInt();
  return h < 24 && m < 60 ? h * 60 + m : -1;
}
void control() {
  float mv = 0;
  for (int i = 0; i < 16; ++i) mv += analogReadMilliVolts(NTC_PIN);
  mv /= 16;
  bool valid = mv > 100 && mv < 3000 && mv < SUPPLY_MV;
  temperature = NAN;
  if (valid) {
    float r = SERIES_OHMS * mv / (SUPPLY_MV - mv);
    temperature = 1 / (1 / 298.15f + logf(r / NTC_OHMS) / NTC_BETA) - 273.15f;
    valid = isfinite(temperature) && temperature >= 0 && temperature <= 110;
  }
  if (!valid || temperature >= MAX_TEMP) fault = true;
  goodSamples = valid ? min(goodSamples + 1, 10u) : 0;
  bool demand = false;
  if (fault) state = "FALHA: sensor/temperatura";
  else if (goodSamples < 5) state = "Validando sensor";
  else if (!enabled) state = "Controle desabilitado";
  else if (!manualMode && !clockReady()) state = "Aguardando horario NTP";
  else {
    time_t now = time(nullptr); tm local; localtime_r(&now, &local);
    if (!manualMode && !scheduled(local.tm_hour * 60 + local.tm_min)) state = "Fora do horario";
    else {
      demand = relay ? temperature < target : temperature <= target - HYSTERESIS;
      state = demand ? "Aquecendo" : "Temperatura atingida";
    }
  }
  if (demand && !relay && millis() - lastOff < 10000) { demand = false; state = "Aguardando religamento"; }
  output(demand);
}
void redirect() { server.sendHeader("Location", "/"); server.send(303); }
void page() {
  String h = F("<!doctype html><html lang='pt-BR'><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>Boiler</title><style>body{font:18px sans-serif;max-width:520px;margin:25px auto;padding:16px;background:#eef3f6;color:#123}label{display:block}input,button{font:inherit;padding:10px;margin:8px 0;max-width:95%}</style><h1>Controle do boiler</h1>");
  h += "<p>Temperatura: <b>" + (isfinite(temperature) ? String(temperature, 1) + " &deg;C" : String("indisponivel")) + "</b></p><p>" + state + "</p><p>Rele: " + String(relay ? "LIGADO" : "DESLIGADO") + "</p>";
  if (clockReady()) { time_t now = time(nullptr); tm local; localtime_r(&now, &local); h += "<p>Horario: " + hhmm(local.tm_hour * 60 + local.tm_min) + " (UTC-3)</p>"; }
  h += "<p>Modo: <b>" + String(manualMode ? "MANUAL (sem agenda)" : "AUTOMATICO (com agenda)") + "</b></p>";
  h += "<p>Saida GPIO" + String(RELAY_PIN) + ": " + String(digitalRead(RELAY_PIN) == HIGH ? "HIGH" : "LOW")
     + " | Acionamento configurado: " + String(RELAY_ACTIVE_LOW ? "ativo em LOW" : "ativo em HIGH")
     + ". Estado fisico do rele nao monitorado.</p>";
  h += "<form method='post' action='/save'><label>Temperatura (&deg;C)<br><input name='target' type='number' min='20' max='70' step='0.5' required value='" + String(target, 1) + "'></label>";
  h += "<label>Inicio diario<br><input name='start' type='time' required value='" + hhmm(startMinute) + "'></label><label>Fim diario<br><input name='stop' type='time' required value='" + hhmm(stopMinute) + "'></label>";
  h += "<label><input name='enabled' type='checkbox' value='1' " + String(enabled ? "checked" : "") + "> Habilitar aquecimento</label><p>Inicio igual ao fim desabilita a janela. Pode atravessar a meia-noite.</p><button>Salvar</button></form>";
  h += F("<form method='post' action='/off'><button>Desligar controle</button></form><form method='post' action='/reset'><button>Reconhecer falha</button></form><h2>Wi-Fi para sincronizar o horario</h2><form method='post' action='/wifi'><label>Nome da rede<br><input name='ssid' maxlength='32' required></label><label>Senha<br><input name='password' type='password' maxlength='63'></label><button>Conectar</button></form><p>Atualize a pagina para consultar o estado.</p></html>");
  server.sendHeader("Cache-Control", "no-store"); server.send(200, "text/html; charset=utf-8", h);
}

void drawDashboard() {
  if (!dashboard.created()) return;
  const uint16_t background = screen.color565(9, 17, 29);
  const uint16_t panel = screen.color565(22, 36, 53);
  const uint16_t muted = screen.color565(151, 173, 193);
  const uint16_t cyan = screen.color565(67, 214, 240);
  const uint16_t orange = screen.color565(255, 163, 58);
  const uint16_t green = screen.color565(95, 224, 159);
  uint16_t accent = fault ? TFT_RED : relay ? orange : enabled ? green : muted;

  dashboard.fillSprite(background);
  dashboard.setTextWrap(false);
  dashboard.setTextSize(1);
  dashboard.setTextDatum(TL_DATUM);
  dashboard.setTextColor(TFT_WHITE, background);
  dashboard.drawString(manualMode ? "MANUAL" : "AUTO", 8, 5, 2);

  String clock = "--:--";
  if (clockReady()) {
    time_t now = time(nullptr); tm local; localtime_r(&now, &local);
    clock = hhmm(local.tm_hour * 60 + local.tm_min);
  }
  dashboard.setTextDatum(TR_DATUM);
  dashboard.setTextColor(muted, background);
  dashboard.drawString(clock, 232, 5, 2);
  dashboard.drawFastHLine(8, 24, 224, panel);

  dashboard.setTextDatum(TL_DATUM);
  dashboard.drawString("TEMPERATURA / C", 8, 29, 1);
  dashboard.setTextColor(fault ? TFT_RED : cyan, background);
  // Fonte numerica grande; valores de tres digitos usam fonte menor para caber.
  String reading = isfinite(temperature) ? String(temperature, 1) : "--.-";
  dashboard.drawString(reading, 6, isfinite(temperature) && temperature >= 100 ? 48 : 41,
                       isfinite(temperature) && temperature >= 100 ? 4 : 7);

  dashboard.fillRoundRect(176, 29, 56, 60, 5, panel);
  dashboard.setTextDatum(TC_DATUM);
  dashboard.setTextColor(muted, panel);
  dashboard.drawString("META", 204, 35, 1);
  dashboard.setTextColor(TFT_WHITE, panel);
  dashboard.drawString(String(target, 1), 204, 49, 2);
  dashboard.setTextColor(muted, panel);
  dashboard.drawString("graus C", 204, 74, 1);

  dashboard.fillRoundRect(8, 94, 224, 3, 1, panel);
  if (isfinite(temperature)) {
    int width = constrain(int(224 * temperature / target), 0, 224);
    if (width > 0) dashboard.fillRect(8, 94, width, 3, accent);
  }
  dashboard.fillCircle(12, 108, 3, accent);
  dashboard.setTextDatum(TL_DATUM);
  dashboard.setTextColor(accent, background);
  String label = state;
  if (fault) label = "FALHA - verifique no painel";
  else if (!enabled && goodSamples >= 5) label = "Controle desligado";
  else if (!manualMode && !clockReady() && enabled) label = "Sincronizando horario";
  else if (state == "Aguardando religamento") label = "Pausa para religar";
  dashboard.drawString(label, 22, 100, 2);

  // Rodape alterna conexao e programacao, sem apertar o painel principal.
  dashboard.setTextColor(muted, background);
  String footer;
  if (millis() - displayActivity < 5000) footer = "Segure + 2s: MANUAL / AUTO";
  else if (manualMode && (millis() / 5000) % 2 == 0) footer = "Manual: temperatura sem agenda";
  else if ((millis() / 5000) % 2 == 0) footer = "Agenda " + hhmm(startMinute) + " - " + hhmm(stopMinute);
  else if (WiFi.status() == WL_CONNECTED) footer = "Wi-Fi OK | " + WiFi.localIP().toString();
  else footer = "Config Wi-Fi: 192.168.4.1";
  dashboard.drawString(footer, 8, 124, 1);
  dashboard.pushSprite(0, 0);
}

void setup() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW); pinMode(RELAY_PIN, OUTPUT); output(false);
  Serial.begin(115200); analogReadResolution(12); analogSetPinAttenuation(NTC_PIN, ADC_11db);
  prefs.begin("boiler", false);
  target = prefs.getFloat("target", 50); startMinute = prefs.getInt("start", 360); stopMinute = prefs.getInt("stop", 480);
  if (!isfinite(target) || target < 20 || target > 70) target = 50;
  if (startMinute < 0 || startMinute > 1439) startMinute = 360;
  if (stopMinute < 0 || stopMinute > 1439) stopMinute = 480;
  screen.init(); screen.setRotation(1); screen.fillScreen(TFT_BLACK);
  ledcSetup(BACKLIGHT_CHANNEL, 5000, 8);
  ledcAttachPin(TFT_BL, BACKLIGHT_CHANNEL);
  upButton.begin(); downButton.begin();
  wakeDisplay();
  dashboard.setColorDepth(16);
  if (!dashboard.createSprite(240, 135)) {
    screen.setTextColor(TFT_RED, TFT_BLACK);
    screen.drawString("Sem memoria para tela", 8, 30, 2);
    Serial.println("Falha ao alocar painel da tela");
  }
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_NAME, AP_PASSWORD);
  String ssid = prefs.getString("ssid", "");
  if (ssid.length()) WiFi.begin(ssid.c_str(), prefs.getString("password", "").c_str());
  configTime(-10800, 0, "pool.ntp.org", "time.google.com");
  server.on("/", HTTP_GET, page);
  server.on("/save", HTTP_POST, [] {
    String raw = server.arg("target"); char *end = nullptr; float value = strtof(raw.c_str(), &end);
    int start = parseTime(server.arg("start")), stop = parseTime(server.arg("stop"));
    if (!raw.length() || *end || !isfinite(value) || value < 20 || value > 70 || start < 0 || stop < 0) { server.send(400, "text/plain", "Configuracao invalida"); return; }
    output(false); target = value; startMinute = start; stopMinute = stop; enabled = server.arg("enabled") == "1";
    prefs.putFloat("target", target); targetDirty = false;
    prefs.putInt("start", start); prefs.putInt("stop", stop); redirect();
  });
  server.on("/off", HTTP_POST, [] { enabled = false; output(false); redirect(); });
  server.on("/reset", HTTP_POST, [] {
    if (goodSamples >= 5 && isfinite(temperature) && temperature < MAX_TEMP - HYSTERESIS) { fault = false; enabled = false; output(false); redirect(); }
    else server.send(409, "text/plain", "Verifique sensor e temperatura antes de reconhecer");
  });
  server.on("/wifi", HTTP_POST, [] {
    String ssid = server.arg("ssid"), password = server.arg("password");
    if (!ssid.length() || ssid.length() > 32 || password.length() > 63) { server.send(400, "text/plain", "Wi-Fi invalido"); return; }
    enabled = false; output(false); prefs.putString("ssid", ssid); prefs.putString("password", password);
    WiFi.begin(ssid.c_str(), password.c_str()); redirect();
  });
  server.begin(); Serial.println("Config: Wi-Fi Boiler-Config, http://192.168.4.1");
}
void loop() {
  handleButtonsAndBacklight();
  if (millis() - sampled >= 500) { sampled = millis(); control(); }
  server.handleClient();
  if (millis() - painted >= 1000) {
    painted = millis(); drawDashboard();
  }
  delay(2);
}
