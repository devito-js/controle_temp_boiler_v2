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
struct MeterReading {
  float temperatureC = NAN;
  float flowLpm = NAN;
  float totalLiters = 0;
  uint64_t totalPulses = 0;
  float temperatureMv = NAN;
  float temperatureOhms = NAN;
};
MeterReading meter1, meter2;
volatile uint32_t inletPulseCount = 0, outletPulseCount = 0;
uint32_t flowSampled = 0;
uint32_t flowTotalsSavedAt = 0;
constexpr uint32_t FLOW_TOTALS_SAVE_INTERVAL_MS = 300000;
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

void IRAM_ATTR countInletPulse() { ++inletPulseCount; }
void IRAM_ATTR countOutletPulse() { ++outletPulseCount; }

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
float readFlowTemperature(int pin, float &millivolts, float &resistanceOhms) {
  millivolts = 0;
  for (int i = 0; i < 16; ++i) millivolts += analogReadMilliVolts(pin);
  millivolts /= 16;
  if (millivolts <= 100 || millivolts >= 3000 || millivolts >= SUPPLY_MV) {
    resistanceOhms = NAN;
    return NAN;
  }

  resistanceOhms = FLOW_TEMP_SERIES_OHMS * millivolts / (SUPPLY_MV - millivolts);
  const float kelvin = 1.0f / (1.0f / 298.15f
      + logf(resistanceOhms / FLOW_TEMP_NTC_OHMS) / FLOW_TEMP_NTC_BETA);
  const float celsius = kelvin - 273.15f;
  return isfinite(celsius) && celsius >= 0 && celsius <= 110 ? celsius : NAN;
}
void updateFlowMeters() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - flowSampled;
  if (elapsed < FLOW_SAMPLE_INTERVAL_MS) return;

  noInterrupts();
  const uint32_t inletPulses = inletPulseCount;
  const uint32_t outletPulses = outletPulseCount;
  inletPulseCount = 0;
  outletPulseCount = 0;
  interrupts();

  const float litersPerPulse = 1.0f / FLOW_PULSES_PER_LITER;
  const float minutesPerMillisecond = 60000.0f / elapsed;
  meter1.flowLpm = inletPulses * litersPerPulse * minutesPerMillisecond;
  meter2.flowLpm = outletPulses * litersPerPulse * minutesPerMillisecond;
  meter1.totalPulses += inletPulses;
  meter2.totalPulses += outletPulses;
  meter1.totalLiters = meter1.totalPulses * litersPerPulse;
  meter2.totalLiters = meter2.totalPulses * litersPerPulse;
  meter1.temperatureC = readFlowTemperature(FLOW_INLET_TEMP_PIN,
      meter1.temperatureMv, meter1.temperatureOhms);
  meter2.temperatureC = readFlowTemperature(FLOW_OUTLET_TEMP_PIN,
      meter2.temperatureMv, meter2.temperatureOhms);
  flowSampled = now;
}
void saveFlowTotalsIfDue() {
  const uint32_t now = millis();
  if (now - flowTotalsSavedAt < FLOW_TOTALS_SAVE_INTERVAL_MS) return;
  flowTotalsSavedAt = now;
  if (meter1.totalPulses != prefs.getULong64("inPulses", 0)
      && prefs.putULong64("inPulses", meter1.totalPulses) != sizeof(meter1.totalPulses))
    Serial.println("Erro ao salvar volume acumulado da entrada");
  if (meter2.totalPulses != prefs.getULong64("outPulses", 0)
      && prefs.putULong64("outPulses", meter2.totalPulses) != sizeof(meter2.totalPulses))
    Serial.println("Erro ao salvar volume acumulado da saida");
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
  String h = F(R"rawliteral(<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>Controle do boiler</title><style>
*{box-sizing:border-box}body{font:16px system-ui,sans-serif;max-width:760px;margin:20px auto;padding:16px;background:#eef3f6;color:#123}
h1,h2{margin:.5em 0}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.card{background:white;border-radius:12px;padding:14px;box-shadow:0 2px 8px #1232}.wide{grid-column:1/-1}.value{font-size:1.6em;font-weight:700}.muted{color:#567}.status{font-weight:700;color:#087c58}canvas{width:100%;height:230px;display:block}label{display:block;margin:10px 0}input,button{font:inherit;padding:10px;margin:6px 0;max-width:100%}button{cursor:pointer}.actions{display:flex;gap:8px;flex-wrap:wrap}
@media(max-width:480px){body{margin:0;padding:12px}.grid{gap:8px}.card{padding:10px}.value{font-size:1.3em}canvas{height:190px}}
</style></head><body><h1>Controle do boiler</h1><div class='grid'>
<section class='card'><div class='muted'>Temperatura do boiler</div><div class='value'><span id='boilerTemp'>--</span> &deg;C</div><div class='muted'>Meta: <span id='targetRead'>--</span> &deg;C</div></section>
<section class='card'><div class='muted'>Estado / modo</div><div class='status' id='state'>Carregando...</div><div id='mode'>--</div><div class='muted'>Rele: <span id='relay'>--</span> | Horario: <span id='clock'>--</span><br>Agenda: <span id='schedule'>--</span><br>Rede: <span id='wifi'>--</span></div></section>
<section class='card'><div class='muted'>Entrada das placas</div><div class='value'><span id='inTemp'>--.-</span> &deg;C</div><div>Fluxo: <span id='inFlow'>--.-</span> L/min</div><div class='muted'>Volume acumulado: <span id='inLiters'>0.00</span> L</div><div class='muted'>ADC: <span id='inTempDiag'>--</span></div></section>
<section class='card'><div class='muted'>Saida das placas</div><div class='value'><span id='outTemp'>--.-</span> &deg;C</div><div>Fluxo: <span id='outFlow'>--.-</span> L/min</div><div class='muted'>Volume acumulado: <span id='outLiters'>0.00</span> L</div><div class='muted'>ADC: <span id='outTempDiag'>--</span></div></section>
<section class='card wide'><h2>Temperatura do boiler</h2><div class='muted'>Historico ao vivo: ultimos 4 minutos, enquanto esta pagina permanecer aberta.</div><canvas id='chart' aria-label='Grafico de temperatura'></canvas></section>
<section class='card wide'><h2>Fluxo de agua</h2><div class='muted'>Entrada (ciano) e saida (laranja), ultimos 4 minutos.</div><canvas id='flowChart' aria-label='Grafico de fluxo de agua'></canvas></section>
<section class='card wide'><h2>Configuracao</h2>)rawliteral");
  h += "<p id='outputInfo'>Saida GPIO" + String(RELAY_PIN) + ": " + String(digitalRead(RELAY_PIN) == HIGH ? "HIGH" : "LOW")
     + " | Rele configurado ativo em " + String(RELAY_ACTIVE_LOW ? "LOW" : "HIGH")
     + ". O estado fisico do rele nao e monitorado.</p>";
  h += "<p>Fluxo: entrada GPIO" + String(FLOW_INLET_PIN) + ", saida GPIO" + String(FLOW_OUTLET_PIN)
     + ", nominal " + String(FLOW_PULSES_PER_LITER) + " pulsos/L. Leitura 0.0 significa que nao foram contados pulsos; nao distingue ausencia de fluxo de problema de ligacao.</p>";
  h += "<form method='post' action='/save'><label>Temperatura meta (&deg;C)<br><input name='target' type='number' min='20' max='70' step='0.5' required value='" + String(target, 1) + "'></label>";
  h += "<label>Inicio diario<br><input name='start' type='time' required value='" + hhmm(startMinute) + "'></label><label>Fim diario<br><input name='stop' type='time' required value='" + hhmm(stopMinute) + "'></label>";
  h += "<label><input name='enabled' type='checkbox' value='1' " + String(enabled ? "checked" : "") + "> Habilitar aquecimento</label><p>Inicio igual ao fim desabilita a janela. Pode atravessar a meia-noite.</p><button>Salvar</button></form>";
  h += F(R"rawliteral(<div class='actions'><form method='post' action='/off'><button>Desligar controle</button></form><form method='post' action='/reset'><button>Reconhecer falha</button></form></div><h2>Wi-Fi para sincronizar o horario</h2><form method='post' action='/wifi'><label>Nome da rede<br><input name='ssid' maxlength='32' required></label><label>Senha<br><input name='password' type='password' maxlength='63'></label><button>Conectar</button></form></section></div>
<script>
const samples=[];const canvas=document.getElementById('chart');const ctx=canvas.getContext('2d');const flowCanvas=document.getElementById('flowChart');const flowCtx=flowCanvas.getContext('2d');
function drawChart(target){const dpr=window.devicePixelRatio||1;const w=canvas.clientWidth,h=canvas.clientHeight;if(!w||!h)return;canvas.width=w*dpr;canvas.height=h*dpr;ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,w,h);const pad={l:42,r:12,t:12,b:24},cw=w-pad.l-pad.r,ch=h-pad.t-pad.b;const vals=samples.map(p=>p.t).filter(Number.isFinite);if(Number.isFinite(target))vals.push(target);if(!vals.length){ctx.fillStyle='#567';ctx.fillText('Aguardando leituras validas',pad.l,35);return}let lo=Math.floor(Math.min(...vals)-2),hi=Math.ceil(Math.max(...vals)+2);if(hi-lo<5){lo-=2;hi+=2}const x=i=>pad.l+(samples.length<2?0:i/(samples.length-1))*cw;const y=v=>pad.t+(hi-v)/(hi-lo)*ch;ctx.font='12px system-ui';ctx.strokeStyle='#d5e0e8';ctx.fillStyle='#567';for(let i=0;i<=4;i++){const v=lo+(hi-lo)*i/4,yy=y(v);ctx.beginPath();ctx.moveTo(pad.l,yy);ctx.lineTo(w-pad.r,yy);ctx.stroke();ctx.fillText(v.toFixed(1),2,yy+4)}if(Number.isFinite(target)){ctx.save();ctx.setLineDash([5,4]);ctx.strokeStyle='#e49a32';ctx.beginPath();ctx.moveTo(pad.l,y(target));ctx.lineTo(w-pad.r,y(target));ctx.stroke();ctx.restore()}if(samples.length>1){ctx.strokeStyle='#08a9c2';ctx.lineWidth=2;ctx.beginPath();samples.forEach((p,i)=>{const xx=x(i),yy=y(p.t);if(i===0)ctx.moveTo(xx,yy);else ctx.lineTo(xx,yy)});ctx.stroke()}ctx.fillStyle='#567';ctx.fillText('agora',w-44,h-4);ctx.fillText('4 min',pad.l,h-4)}
function fmt(v){return Number.isFinite(v)?v.toFixed(1):'--.-'}
function drawFlowChart(){const dpr=window.devicePixelRatio||1;const w=flowCanvas.clientWidth,h=flowCanvas.clientHeight;if(!w||!h)return;flowCanvas.width=w*dpr;flowCanvas.height=h*dpr;flowCtx.setTransform(dpr,0,0,dpr,0,0);flowCtx.clearRect(0,0,w,h);const pad={l:42,r:12,t:12,b:24},cw=w-pad.l-pad.r,ch=h-pad.t-pad.b;const vals=samples.flatMap(p=>[p.fi,p.fo]).filter(Number.isFinite);if(!vals.length){flowCtx.fillStyle='#567';flowCtx.fillText('Aguardando pulsos dos medidores',pad.l,35);return}const hi=Math.max(1,Math.ceil(Math.max(...vals))),x=i=>pad.l+(samples.length<2?0:i/(samples.length-1))*cw,y=v=>pad.t+(hi-v)/hi*ch;flowCtx.font='12px system-ui';flowCtx.strokeStyle='#d5e0e8';flowCtx.fillStyle='#567';for(let i=0;i<=4;i++){const v=hi*i/4,yy=y(v);flowCtx.beginPath();flowCtx.moveTo(pad.l,yy);flowCtx.lineTo(w-pad.r,yy);flowCtx.stroke();flowCtx.fillText(v.toFixed(1),2,yy+4)}for(const [key,color] of [['fi','#08a9c2'],['fo','#e49a32']]){flowCtx.strokeStyle=color;flowCtx.lineWidth=2;flowCtx.beginPath();let started=false;samples.forEach((p,i)=>{if(!Number.isFinite(p[key]))return;const xx=x(i),yy=y(p[key]);if(!started){flowCtx.moveTo(xx,yy);started=true}else flowCtx.lineTo(xx,yy)});flowCtx.stroke()}flowCtx.fillStyle='#567';flowCtx.fillText('agora',w-44,h-4);flowCtx.fillText('4 min',pad.l,h-4)}
async function update(){try{const r=await fetch('/data',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();document.getElementById('boilerTemp').textContent=fmt(d.boiler);document.getElementById('targetRead').textContent=fmt(d.target);document.getElementById('inTemp').textContent=fmt(d.inTemp);document.getElementById('outTemp').textContent=fmt(d.outTemp);document.getElementById('inFlow').textContent=fmt(d.inFlow);document.getElementById('outFlow').textContent=fmt(d.outFlow);document.getElementById('inLiters').textContent=fmt(d.inLiters);document.getElementById('outLiters').textContent=fmt(d.outLiters);document.getElementById('inTempDiag').textContent=fmt(d.inTempMv)+' mV / '+fmt(d.inTempOhms/1000)+' kΩ';document.getElementById('outTempDiag').textContent=fmt(d.outTempMv)+' mV / '+fmt(d.outTempOhms/1000)+' kΩ';document.getElementById('state').textContent=d.state;document.getElementById('state').style.color=d.fault?'#b42318':d.relay?'#b85c00':'#087c58';document.getElementById('mode').textContent=d.mode;document.getElementById('relay').textContent=d.relay?'LIGADO':'DESLIGADO';document.getElementById('clock').textContent=d.clock||'sem NTP';document.getElementById('schedule').textContent=d.start+' - '+d.stop;document.getElementById('wifi').textContent=d.wifi?'Conectado: '+d.ip:'Ponto de acesso: '+d.ip;samples.push({t:d.boiler,fi:d.inFlow,fo:d.outFlow});if(samples.length>120)samples.shift();drawChart(d.target);drawFlowChart()}catch(e){document.getElementById('state').textContent='Sem comunicacao com o controlador';document.getElementById('state').style.color='#b42318'}}
window.addEventListener('resize',()=>{drawChart(NaN);drawFlowChart()});update();setInterval(update,2000);
</script></body></html>)rawliteral");
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
  dashboard.drawString("TEMPERATURA DO BOILER / C", 8, 29, 1);
  dashboard.setTextColor(fault ? TFT_RED : cyan, background);
  // Fonte numerica grande; valores de tres digitos usam fonte menor para caber.
  String boilerReading = isfinite(temperature) ? String(temperature, 1) : "--.-";
  dashboard.drawString(boilerReading, 6, isfinite(temperature) && temperature >= 100 ? 48 : 41,
                       isfinite(temperature) && temperature >= 100 ? 4 : 7);

  dashboard.fillRoundRect(176, 29, 56, 56, 5, panel);
  dashboard.setTextDatum(TC_DATUM);
  dashboard.setTextColor(muted, panel);
  dashboard.drawString("META", 204, 35, 1);
  dashboard.setTextColor(TFT_WHITE, panel);
  dashboard.drawString(String(target, 1), 204, 49, 2);
  dashboard.setTextColor(muted, panel);
  dashboard.drawString("graus C", 204, 68, 1);

  dashboard.drawFastHLine(8, 87, 224, panel);
  dashboard.setTextDatum(TL_DATUM);
  dashboard.setTextColor(muted, background);
  dashboard.drawString("ENTRADA PLACAS", 8, 90, 1);
  dashboard.drawString("SAIDA PLACAS", 128, 90, 1);
  dashboard.setTextColor(cyan, background);
  dashboard.drawString("T " + (isfinite(meter1.temperatureC) ? String(meter1.temperatureC, 1) : String("--.-")) + " C", 8, 100, 2);
  dashboard.drawString("T " + (isfinite(meter2.temperatureC) ? String(meter2.temperatureC, 1) : String("--.-")) + " C", 128, 100, 2);
  dashboard.setTextColor(muted, background);
  dashboard.drawString("F " + (isfinite(meter1.flowLpm) ? String(meter1.flowLpm, 1) : String("--.-")) + " L/m", 8, 115, 1);
  dashboard.drawString("F " + (isfinite(meter2.flowLpm) ? String(meter2.flowLpm, 1) : String("--.-")) + " L/m", 128, 115, 1);

  String label = state;
  if (fault) label = "FALHA - verifique no painel";
  else if (!enabled && goodSamples >= 5) label = "Controle desligado";
  else if (!manualMode && !clockReady() && enabled) label = "Sincronizando horario";
  else if (state == "Aguardando religamento") label = "Pausa para religar";
  if (!relay && !fault) {
    uint32_t infoPage = (millis() / 5000) % 3;
    if (infoPage == 1) {
      if (millis() - displayActivity < 5000) label = "Segure + 2s: MANUAL / AUTO";
      else if (manualMode) label = "Manual: controle sem agenda";
      else label = "Agenda " + hhmm(startMinute) + " - " + hhmm(stopMinute);
    } else if (infoPage == 2) {
      if (WiFi.status() == WL_CONNECTED) label = "Wi-Fi OK | " + WiFi.localIP().toString();
      else label = "Wi-Fi offline | AP 192.168.4.1";
    }
  }
  dashboard.setTextColor((relay || fault || (millis() / 5000) % 3 == 0) ? accent : muted, background);
  dashboard.drawString(label, 8, 126, 1);
  dashboard.pushSprite(0, 0);
}

void setup() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW); pinMode(RELAY_PIN, OUTPUT); output(false);
  Serial.begin(115200); analogReadResolution(12); analogSetPinAttenuation(NTC_PIN, ADC_11db);
  analogSetPinAttenuation(FLOW_INLET_TEMP_PIN, ADC_11db);
  analogSetPinAttenuation(FLOW_OUTLET_TEMP_PIN, ADC_11db);
  prefs.begin("boiler", false);
  target = prefs.getFloat("target", 50); startMinute = prefs.getInt("start", 360); stopMinute = prefs.getInt("stop", 480);
  meter1.totalPulses = prefs.getULong64("inPulses", 0);
  meter2.totalPulses = prefs.getULong64("outPulses", 0);
  meter1.totalLiters = meter1.totalPulses / static_cast<float>(FLOW_PULSES_PER_LITER);
  meter2.totalLiters = meter2.totalPulses / static_cast<float>(FLOW_PULSES_PER_LITER);
  if (!isfinite(target) || target < 20 || target > 70) target = 50;
  if (startMinute < 0 || startMinute > 1439) startMinute = 360;
  if (stopMinute < 0 || stopMinute > 1439) stopMinute = 480;
  screen.init(); screen.setRotation(1); screen.fillScreen(TFT_BLACK);
  ledcSetup(BACKLIGHT_CHANNEL, 5000, 8);
  ledcAttachPin(TFT_BL, BACKLIGHT_CHANNEL);
  upButton.begin(); downButton.begin();
  pinMode(FLOW_INLET_PIN, INPUT);
  pinMode(FLOW_OUTLET_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_INLET_PIN), countInletPulse, RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW_OUTLET_PIN), countOutletPulse, RISING);
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
  server.on("/data", HTTP_GET, [] {
    auto jsonNumber = [](float value) { return isfinite(value) ? String(value, 2) : String("null"); };
    String clock;
    if (clockReady()) {
      time_t now = time(nullptr); tm local; localtime_r(&now, &local);
      clock = hhmm(local.tm_hour * 60 + local.tm_min);
    }
    String json = "{\"boiler\":" + jsonNumber(temperature)
        + ",\"target\":" + jsonNumber(target)
        + ",\"inTemp\":" + jsonNumber(meter1.temperatureC)
        + ",\"inTempMv\":" + jsonNumber(meter1.temperatureMv)
        + ",\"inTempOhms\":" + jsonNumber(meter1.temperatureOhms)
        + ",\"inFlow\":" + jsonNumber(meter1.flowLpm)
        + ",\"inLiters\":" + jsonNumber(meter1.totalLiters)
        + ",\"outTemp\":" + jsonNumber(meter2.temperatureC)
        + ",\"outTempMv\":" + jsonNumber(meter2.temperatureMv)
        + ",\"outTempOhms\":" + jsonNumber(meter2.temperatureOhms)
        + ",\"outFlow\":" + jsonNumber(meter2.flowLpm)
        + ",\"outLiters\":" + jsonNumber(meter2.totalLiters)
        + ",\"state\":\"" + state + "\",\"mode\":\"" + (manualMode ? "MANUAL" : "AUTO")
        + "\",\"relay\":" + (relay ? "true" : "false")
        + ",\"fault\":" + (fault ? "true" : "false")
        + ",\"clock\":\"" + clock + "\""
        + ",\"start\":\"" + hhmm(startMinute) + "\",\"stop\":\"" + hhmm(stopMinute) + "\""
        + ",\"wifi\":" + (WiFi.status() == WL_CONNECTED ? "true" : "false")
        + ",\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", json);
  });
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
  flowTotalsSavedAt = millis();
}
void loop() {
  handleButtonsAndBacklight();
  updateFlowMeters();
  saveFlowTotalsIfDue();
  if (millis() - sampled >= 500) { sampled = millis(); control(); }
  server.handleClient();
  if (millis() - painted >= 1000) {
    painted = millis(); drawDashboard();
  }
  delay(2);
}
