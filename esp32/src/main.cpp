#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "lut.h"
#include "translations.h"

#define FIRMWARE_VERSION "1.1"

#define WIFI_SSID "your-network"
#define WIFI_PASS "your-password"

#define UART_RX_PIN 16
#define UART_BAUD   2400

// ── BLE RSC ───────────────────────────────────────────────────────────────────
static NimBLECharacteristic* charMeas;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* srv, ble_gap_conn_desc*) override {}
    void onDisconnect(NimBLEServer* srv) override {
        NimBLEDevice::getAdvertising()->start();
    }
};

// ── State ─────────────────────────────────────────────────────────────────────
static float         gSpeedKmh  = 0.0f;
static float         gDistM     = 0.0f;
static uint32_t      gPktTotal  = 0;
static uint32_t      gPktErr    = 0;
static unsigned long gLastPktMs = 0;
static unsigned long gStartMs   = 0;

static AsyncWebServer server(80);

static const char HTML_MAIN[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>Uvero U1</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:20px}
h1{margin:0 0 20px;font-size:1.4em;color:#aaa;display:flex;align-items:center;gap:12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px}
.card{background:#1e1e1e;border-radius:10px;padding:16px;text-align:center}
.label{font-size:.75em;color:#888;text-transform:uppercase;letter-spacing:.05em}
.value{font-size:2em;font-weight:bold;margin:6px 0 2px}
.unit{font-size:.8em;color:#888}
.ok{color:#4caf50}.timeout{color:#ff9800}.nosig{color:#f44336}
#st{margin-top:20px;font-size:.8em;color:#555}
.btn{border:none;border-radius:6px;padding:10px 20px;font-size:1em;cursor:pointer}
#langBtn{background:#333;color:#aaa;padding:4px 10px;font-size:.75em;border-radius:4px;border:1px solid #555;cursor:pointer}
</style></head><body>
<h1>Uvero U1 <span id="ver" style="font-size:.5em;color:#555;font-weight:normal"></span><button id="langBtn" onclick="toggleLang()">EN</button><small style="font-size:.4em;color:#555;margin-left:auto"><a href="https://github.com/Shhatrat/uvero" style="color:#555">github</a></small></h1>
<div class="grid">
  <div class="card"><div class="label" data-i18n="speed"></div><div class="value" id="spd">--</div><div class="unit">km/h</div></div>
  <div class="card"><div class="label" data-i18n="dist"></div><div class="value" id="dst">--</div><div class="unit">m</div></div>
  <div class="card"><div class="label">Uptime</div><div class="value" id="upt">--</div><div class="unit">hh:mm:ss</div></div>
  <div class="card"><div class="label" data-i18n="temp"></div><div class="value" id="tmp">--</div><div class="unit">°C</div></div>
  <div class="card"><div class="label">BLE</div><div class="value" id="ble">--</div><div class="unit" data-i18n="clients"></div></div>
  <div class="card"><div class="label">UART</div><div class="value" id="ust">--</div><div class="unit" id="uinf"></div></div>
  <div class="card"><div class="label" data-i18n="ram"></div><div class="value" id="ram">--</div><div class="unit">kB</div></div>
  <div class="card"><div class="label">CPU</div><div class="value" id="cpu">--</div><div class="unit">MHz</div></div>
</div>
<div style="margin-top:16px">
  <button class="btn" onclick="resetDist()" style="background:#c0392b;color:#fff" data-i18n="reset"></button>
</div>
<div id="st" data-i18n-status="connecting"></div>
<script src="/i18n.js"></script>
<script>
let lang=localStorage.getItem('lang')||'pl';
function applyLang(){
  document.querySelectorAll('[data-i18n]').forEach(el=>el.textContent=T[lang][el.dataset.i18n]);
  document.getElementById('langBtn').textContent=lang==='pl'?'EN':'PL';
  document.querySelector('[data-i18n-status]').textContent=T[lang].connecting;
}
function toggleLang(){lang=lang==='pl'?'en':'pl';localStorage.setItem('lang',lang);applyLang();}
applyLang();
async function r(){
  try{const d=await(await fetch('/json')).json();
  document.getElementById('ver').textContent='v'+d.version;
  document.getElementById('spd').textContent=d.speed_kmh.toFixed(1);
  document.getElementById('dst').textContent=d.distance_m.toFixed(0);
  const u=d.uptime_s;document.getElementById('upt').textContent=String(Math.floor(u/3600)).padStart(2,'0')+':'+String(Math.floor(u%3600/60)).padStart(2,'0')+':'+String(u%60).padStart(2,'0');
  document.getElementById('tmp').textContent=d.temp_c.toFixed(1);
  document.getElementById('ble').textContent=d.ble_clients;
  const s=document.getElementById('ust');
  s.textContent=d.uart.status;
  s.className='value '+(d.uart.status==='ok'?'ok':d.uart.status==='timeout'?'timeout':'nosig');
  document.getElementById('uinf').textContent='pkt:'+d.uart.packets_total+' err:'+d.uart.parse_errors;
  document.getElementById('ram').textContent=d.free_heap_kb.toFixed(1);
  document.getElementById('cpu').textContent=d.cpu_mhz;
  document.getElementById('st').textContent=T[lang].updated+' '+new Date().toLocaleTimeString();
  }catch(e){document.getElementById('st').textContent=T[lang].error;}
}
async function resetDist(){await fetch('/reset_dist');r();}
r();setInterval(r,1000);
</script></body></html>)rawhtml";

// ── UART parser ───────────────────────────────────────────────────────────────
static uint8_t rxBuf[32];
static int     rxLen = 0;

static const uint8_t HEADER[] = {0x80, 0x1E, 0x80, 0x00};

static bool tryParse() {
    int start = -1;
    for (int i = 0; i <= rxLen - 4; i++) {
        if (memcmp(rxBuf + i, HEADER, 4) == 0) { start = i; break; }
    }
    if (start < 0) {
        if (rxLen > 3) { memmove(rxBuf, rxBuf + rxLen - 3, 3); rxLen = 3; }
        return false;
    }
    if (start > 0) { memmove(rxBuf, rxBuf + start, rxLen - start); rxLen -= start; }

    if (rxLen < 15) return false;

    int pktLen = 0;
    if (rxBuf[4] == 0x00) {
        if (rxLen >= 15 && rxBuf[14] == 0xF8) pktLen = 15;
        else if (rxLen < 15) return false;
    } else {
        if      (rxLen >= 18 && rxBuf[17] == 0xF8) pktLen = 18;
        else if (rxLen >= 17 && rxBuf[16] == 0xF8) pktLen = 17;
        else if (rxLen >= 16 && rxBuf[15] == 0xF8) pktLen = 16;
        else if (rxLen < 18)                        return false;
    }
    if (pktLen == 0) {
        memmove(rxBuf, rxBuf + 1, rxLen - 1); rxLen--;
        gPktErr++;
        return false;
    }

    int keyStart = (pktLen == 18) ? 12 : (pktLen == 17) ? 8 : 10;
    int keyLen   = pktLen - keyStart;

    gPktTotal++;
    gLastPktMs = millis();

    if (rxBuf[4] == 0x00) {
        gSpeedKmh = 0.0f;
    } else {
        uint8_t speed10 = lookupSpeed(rxBuf + keyStart, keyLen);
        if (speed10 > 0) gSpeedKmh = speed10 / 10.0f;
        else             gPktErr++;
    }

    memmove(rxBuf, rxBuf + pktLen, rxLen - pktLen);
    rxLen -= pktLen;
    return true;
}

// ── BLE send ──────────────────────────────────────────────────────────────────
static void sendRsc() {
    float speedMs     = gSpeedKmh / 3.6f;
    uint16_t speedRaw = (uint16_t)(speedMs * 256.0f);
    float strideM     = 0.45f + speedMs * 0.12f;
    uint8_t cadence   = speedMs > 0.1f ? (uint8_t)(speedMs / strideM * 60.0f) : 0;
    uint8_t d[4]      = {0x00, (uint8_t)(speedRaw & 0xFF), (uint8_t)(speedRaw >> 8), cadence};
    charMeas->setValue(d, sizeof(d));
    charMeas->notify();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    gStartMs = millis();

    Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, -1);

    NimBLEDevice::init("Uvero U1");
    NimBLEServer* srv = NimBLEDevice::createServer();
    srv->setCallbacks(new ServerCallbacks());
    NimBLEService* svc = srv->createService(NimBLEUUID((uint16_t)0x1814));
    NimBLECharacteristic* feat = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A54), NIMBLE_PROPERTY::READ);
    uint16_t featVal = 0x0000; feat->setValue(featVal);
    NimBLECharacteristic* loc = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A5D), NIMBLE_PROPERTY::READ);
    uint8_t locVal = 7; loc->setValue(&locVal, 1);
    charMeas = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A53), NIMBLE_PROPERTY::NOTIFY);
    svc->start();
    NimBLEService* dis = srv->createService(NimBLEUUID((uint16_t)0x180A));
    dis->createCharacteristic(NimBLEUUID((uint16_t)0x2A29), NIMBLE_PROPERTY::READ)->setValue("Shhatrat");
    dis->createCharacteristic(NimBLEUUID((uint16_t)0x2A24), NIMBLE_PROPERTY::READ)->setValue("U1");
    dis->createCharacteristic(NimBLEUUID((uint16_t)0x2A25), NIMBLE_PROPERTY::READ)->setValue("001");
    dis->createCharacteristic(NimBLEUUID((uint16_t)0x2A26), NIMBLE_PROPERTY::READ)->setValue(FIRMWARE_VERSION);
    dis->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1814));
    adv->setAppearance(0x0481);
    adv->setScanResponse(true);
    adv->start();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi");
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("\nhttp://%s\n", WiFi.localIP().toString().c_str());

    server.on("/i18n.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "application/javascript", TRANSLATIONS_JS);
    });
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", HTML_MAIN);
    });
    server.on("/json", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["version"]      = FIRMWARE_VERSION;
        doc["speed_kmh"]    = round(gSpeedKmh * 10) / 10.0;
        doc["distance_m"]   = round(gDistM * 10) / 10.0;
        doc["uptime_s"]     = (millis() - gStartMs) / 1000;
        doc["temp_c"]       = round(temperatureRead() * 10) / 10.0;
        doc["ble_clients"]  = NimBLEDevice::getServer()->getConnectedCount();
        doc["free_heap_kb"] = ESP.getFreeHeap() / 1024.0;
        doc["cpu_mhz"]      = ESP.getCpuFreqMHz();
        JsonObject uart = doc["uart"].to<JsonObject>();
        unsigned long age = gLastPktMs > 0 ? (millis() - gLastPktMs) : 999999;
        uart["status"]        = age < 1000 ? "ok" : age < 10000 ? "timeout" : "no signal";
        uart["parse_errors"]  = gPktErr;
        uart["packets_total"] = gPktTotal;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });
    server.on("/reset_dist", HTTP_GET, [](AsyncWebServerRequest* req) {
        gDistM = 0.0f;
        req->send(200, "text/plain", "ok");
    });
    server.begin();
    Serial.println("HTTP OK");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static unsigned long lastDistMs = 0;
static unsigned long lastBleMs  = 0;
static unsigned long lastLogMs  = 0;

void loop() {
    unsigned long now = millis();

    while (Serial2.available() && rxLen < (int)sizeof(rxBuf))
        rxBuf[rxLen++] = Serial2.read();
    while (rxLen >= 15) {
        if (!tryParse()) break;
    }

    if (now - lastDistMs >= 100) {
        float dt = (now - lastDistMs) / 1000.0f;
        lastDistMs = now;
        if (gSpeedKmh > 0) gDistM += (gSpeedKmh / 3.6f) * dt;
    }

    if (now - lastBleMs >= 1000) {
        lastBleMs = now;
        if (NimBLEDevice::getServer()->getConnectedCount() > 0) sendRsc();
    }

    if (now - lastLogMs >= 5000) {
        lastLogMs = now;
        const char* uartStatus = "no signal";
        if (gLastPktMs > 0) {
            unsigned long age = now - gLastPktMs;
            if      (age < 1000)  uartStatus = "ok";
            else if (age < 10000) uartStatus = "timeout";
        }
        Serial.printf("speed=%.1f dist=%.0f uart=%s pkt=%lu err=%lu ble=%d\n",
            gSpeedKmh, gDistM, uartStatus, gPktTotal, gPktErr,
            NimBLEDevice::getServer()->getConnectedCount());
    }
}
