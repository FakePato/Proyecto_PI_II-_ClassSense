// ClassSense Alerts 2.4 Plus - ESP32 DevKit / NodeMCU ESP32-WROOM-32 (30 o 38 pines).
// Arduino-ESP32 3.x. Sin camara. El KY-038 solo detecta si hay ruido: no graba audio.
//
// Novedades frente a 1.0:
//   - Volumen del buzzer en 10 niveles + silencio (pulsador 2 o panel): el 10 es el maximo que
//     puede dar el buzzer. Tono de 0,1 a 10 s, y esa duracion rige tambien los avisos locales.
//   - Sensibilidad del sensor de ruido en 10 niveles.
//   - Colores RGB libres (0-255 por canal) para cada estado y pruebas de color en vivo.
//   - Pantalla OLED SSD1306 con estado, medidor de ruido, volumen y Wi-Fi.
//   - Tres pulsadores con pulsacion corta y larga.
//   - Modo local: funciona sin Wi-Fi ni panel usando el sensor de ruido.
//   - Los ajustes se guardan y sobreviven a un reinicio.
//
// Librerias (Gestor de librerias de Arduino IDE):
//   "Adafruit SSD1306" y "Adafruit GFX Library".
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Preferences.h>
#include <cJSON.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <atomic>
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "wifi_retry.h"
#include "secrets.h"
#include "classsense_types.h"
#include "reaction_logic.h"
#include "controller_page.h"
#include <esp_arduino_version.h>

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Selecciona ESP32 Dev Module: este cableado requiere ESP32-WROOM-32."
#endif
#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "Instala esp32 by Espressif Systems 3.x en el Gestor de placas."
#endif

#ifndef LOCAL_MODE
#define LOCAL_MODE true  // false: sin panel las salidas quedan apagadas, como en 1.0
#endif

#define FIRMWARE_NAME "ClassSense Alerts 2.4 Plus"

// ----------------------------------------------------------------- Pines
constexpr uint8_t OLED_SDA_PIN = 21, OLED_SCL_PIN = 22;
constexpr uint8_t MIC_PIN = 34;                          // KY-038, salida D0
constexpr uint8_t BUZZER_PIN = 4;                        // HW-508, pin S
constexpr uint8_t RED_PIN = 25, GREEN_PIN = 26, BLUE_PIN = 27;
// Pulsador 1 = encender/apagar (debe ser GPIO 32: powerOff() lo usa para despertar).
constexpr uint8_t BTN_RESET_PIN = 32, BTN_BUZZER_PIN = 33, BTN_LED_PIN = 14;

// ----------------------------------------------------------------- Ajustes
constexpr uint8_t OLED_ADDRESS = 0x3C;                   // prueba 0x3D si la pantalla queda negra
constexpr uint32_t WATCHDOG_MS = 3500;                   // sin ordenes del panel -> modo local
constexpr uint16_t TONE_MAX_MS = 10000;                  // duracion maxima de un tono
constexpr uint8_t VOLUME_MAX = 10;
// Ciclo de trabajo del PWM (de 1024) por nivel. 512 = 50 % = el maximo que puede dar un buzzer
// pasivo con esta onda: el nivel 10 es siempre el sonido maximo. Los niveles bajan de 2,7 dB en
// 2,7 dB (el oido oye en escala logaritmica), asi cada paso se nota igual y el 1 sigue audible.
// Valores: duty = asin(10^(-(10-nivel)*2,7/20)) / pi * 1024.
const uint16_t VOLUME_DUTY[VOLUME_MAX + 1] = {0, 20, 27, 37, 51, 69, 95, 132, 185, 268, 512};
// Sensibilidad del ruido (1 = poco sensible, 10 = muy sensible). Cada columna es un porcentaje
// de actividad del sensor: WARN en 1 s (amarillo), ALERT sostenido en 3 s (alerta) y CLEAR
// para salir de la alerta. El nivel 5 equivale a los valores de la version 2.0.
const uint8_t SENS_WARN[11] = {0, 50, 45, 40, 35, 30, 25, 20, 15, 12, 8};
const uint8_t SENS_ALERT[11] = {0, 80, 74, 68, 62, 56, 50, 44, 38, 32, 26};
const uint8_t SENS_CLEAR[11] = {0, 60, 55, 50, 45, 40, 35, 30, 26, 22, 18};
constexpr uint32_t ALERT_REPEAT_MS = 8000;               // repite el aviso si el ruido sigue
constexpr uint32_t DIM_AFTER_MS = 60000;                 // atenua la pantalla en reposo

// Colores de cada estado (rojo, verde, azul de 0 a 255). El panel puede cambiarlos.
constexpr uint8_t PAL_NORMAL = 0, PAL_EVAL = 1, PAL_EVENT = 2, PAL_INSUF = 3, PAL_CALM = 4, PAL_COUNT = 5;
const char* const PAL_KEY[PAL_COUNT] = {"normal", "evaluating", "event", "insufficient", "calm"};
const uint8_t PAL_DEFAULT[PAL_COUNT][3] = {{0, 255, 0}, {255, 255, 0}, {255, 0, 0}, {0, 0, 255}, {0, 255, 0}};
// Colores de reposo del pulsador 3 en modo local.
constexpr uint8_t CALM_COLORS = 5;
const uint8_t CALM_RGB[CALM_COLORS][3] = {{0, 255, 0}, {0, 0, 255}, {0, 255, 255}, {255, 0, 255}, {255, 255, 255}};
const char* const CALM_NAME[CALM_COLORS] = {"Color: verde", "Color: azul", "Color: cian", "Color: morado", "Color: blanco"};

// ----------------------------------------------------------------- Estado
WebServer server(80);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Preferences prefs;
Button btnReset{BTN_RESET_PIN}, btnBuzzer{BTN_BUZZER_PIN}, btnLed{BTN_LED_PIN};

bool ready = false, oledOk = false, wifiEnabled = false, wifiUp = false;
bool hasHeartbeat = false, testActive = false, hasTest = false;
uint32_t heartbeatAt = 0, testUntil = 0, lastTestAt = 0;
// Wi-Fi: un intento a la vez, con espera creciente entre intentos (ver wifi_retry.h).
WifiRetry wifiRetry;
std::atomic<unsigned> wifiFailure{0};
unsigned lastWifiReason = 0;
String currentState = "off", testMode = "";
uint8_t testRgb[3] = {0, 0, 0};
ReactionLogic reaction;
uint8_t currentPatterns = 0;

// Ajustes guardados
uint8_t volume = 6, brightness = 70, noiseSens = 5, calmIndex = 0;
uint16_t toneMs = 500;                                   // duracion real de los avisos y pruebas
bool muted = false, ledEnabled = true;
uint8_t palette[PAL_COUNT][3];
int8_t volumeStep = 1;                                   // el pulsador sube y luego baja
uint32_t volumeRepeatAt = 0;
bool powerGuard = false;
bool settingsDirty = false;
uint32_t settingsSaveAt = 0;
// Ultimos valores recibidos del panel: solo se aplican cuando cambian, para que lo ajustado
// con los pulsadores no se pierda un segundo despues.
int lastPanelBrightness = -1, lastPanelSound = -1, lastPanelVolume = -1, lastPanelSens = -1, lastPanelTone = -1;
uint8_t lastPanelPalette[PAL_COUNT][3];
bool havePanelPalette = false;

// Buzzer
Note melody[4];
uint8_t melodyLength = 0, melodyIndex = 0;
bool playing = false;
uint32_t noteUntil = 0, micBlockedUntil = 0;

// Sensor de ruido: 60 tramos de 50 ms = 3 s de historia
volatile uint32_t micEdges = 0;
uint32_t micEdgesSeen = 0, micSlotAt = 0;
uint8_t micRing[60] = {0}, micCursor = 0;
uint8_t noiseNow = 0, noiseSustained = 0, localState = 0;  // 0 calma, 1 ruido, 2 alerta
uint16_t alertCount = 0;
uint32_t lastAlertBeepAt = 0;

// Pantalla
Overlay overlay = OV_NONE;
const char* overlayText = "";
uint32_t overlayUntil = 0, lastDrawAt = 0, lastActivityAt = 0;
bool infoScreen = false, dimmed = false;

// ----------------------------------------------------------------- Utilidades
bool before(uint32_t now, uint32_t deadline) { return (int32_t)(deadline - now) > 0; }
void markDirty() { settingsDirty = true; settingsSaveAt = millis() + 2000; }
void wake() { lastActivityAt = millis(); }
char ipText[20] = "";                                    // la IP se muestra en pantalla al conectar
void showOverlayFor(Overlay kind, const char* text, uint32_t ms) {
  overlay = kind; overlayText = text; overlayUntil = millis() + ms; wake();
}
void showOverlay(Overlay kind, const char* text) { showOverlayFor(kind, text, 1500); }
// El panel habla en porcentaje (0-100); la placa en niveles (0-10).
uint8_t levelFromPercent(int percent) { return (percent * VOLUME_MAX + 50) / 100; }
uint8_t percentFromLevel(uint8_t level) { return (level * 100 + VOLUME_MAX / 2) / VOLUME_MAX; }

bool validToken() {
  size_t n = strlen(DEVICE_TOKEN);
  if (n < 16 || n > 119) return false;
  for (size_t i = 0; i < n; i++) {
    char c = DEVICE_TOKEN[i];
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  }
  return true;
}
bool authorized() {
  String supplied = server.header("X-ClassSense-Key");
  size_t n = strlen(DEVICE_TOKEN);
  uint32_t difference = supplied.length() ^ n;
  for (size_t i = 0; i < n; i++)
    difference |= (uint8_t)DEVICE_TOKEN[i] ^ (i < supplied.length() ? (uint8_t)supplied[i] : 0);
  if (!validToken() || difference) {
    server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
    return false;
  }
  return true;
}

// ----------------------------------------------------------------- Wi-Fi
void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  // Arduino llama a esto desde otra tarea: el hardware y Serial se tocan solo en loop().
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    unsigned expected = 0, reason = info.wifi_sta_disconnected.reason;
    wifiFailure.compare_exchange_strong(expected, reason ? reason : 1);
  }
}
void beginWifi() {
  wifiFailure.store(0);
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(false); WiFi.setSleep(false);
  Serial.println("Wi-Fi: iniciando un intento; espera hasta 30 s.");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}
void serviceWifi(uint32_t now) {
  unsigned reason = wifiFailure.exchange(0);
  WifiRetry::Action action = wifiRetry.tick(now, WiFi.status() == WL_CONNECTED, reason != 0);
  if (action == WifiRetry::Action::Start) { beginWifi(); return; }
  if (action != WifiRetry::Action::Stop) return;
  lastWifiReason = reason;
  if (reason) {
    Serial.printf("Wi-Fi: intento terminado; codigo %u. ", reason);
    if (reason == WIFI_REASON_NO_AP_FOUND) Serial.println("Red no encontrada: revisa 2,4 GHz, alcance y punto de acceso encendido.");
    else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)
      Serial.println("No se completo la autenticacion: revisa seguridad del punto de acceso y contrasena.");
    else Serial.println("Conexion interrumpida; el codigo no demuestra por si solo una contrasena incorrecta.");
  } else Serial.println("Wi-Fi: no hay conexion con IP; se reiniciara el intento tras la espera.");
  // Cancela el intento anterior por completo, incluido el primer reintento automatico del nucleo.
  // Las credenciales de secrets.h y de la memoria no se borran.
  hasHeartbeat = false; testActive = false; currentState = "off";
  server.close();
  WiFi.mode(WIFI_OFF);
  Serial.printf("Nuevo intento en %lu s.\n", (unsigned long)(wifiRetry.waitMilliseconds() / 1000));
}

// ----------------------------------------------------------------- LED RGB
// Cada canal pasa por una curva gamma 2: los valores bajos se ven mas parecidos a como
// se mueve el control (un 128 se ve a media luz, no casi al maximo).
uint32_t channel(uint8_t value) {
  uint32_t scale = ledEnabled ? brightness : 0;
  uint32_t shaped = (uint32_t)value * value / 255U;
  uint32_t level = shaped * scale / 100U;
  return RGB_COMMON_ANODE ? 255U - level : level;
}
void rgb8(uint8_t red, uint8_t green, uint8_t blue) {
  if (!ready) return;
  ledcWrite(RED_PIN, channel(red));
  ledcWrite(GREEN_PIN, channel(green));
  ledcWrite(BLUE_PIN, channel(blue));
}
void rgbOf(const uint8_t* color) { rgb8(color[0], color[1], color[2]); }

// ----------------------------------------------------------------- Buzzer con volumen
// El volumen se regula con el ciclo de trabajo del PWM: no hace falta ningun componente extra.
void buzzerWrite(uint16_t freq) {
  static uint16_t activeFreq = 0;
  static uint8_t activeVolume = 0;
  if (!ready) return;
  uint8_t level = muted ? 0 : volume;
  if (freq == 0 || level == 0) {
    if (activeFreq != 0) { ledcWrite(BUZZER_PIN, 0); activeFreq = 0; }
    return;
  }
  if (freq != activeFreq) ledcChangeFrequency(BUZZER_PIN, freq, 10);
  if (freq != activeFreq || level != activeVolume) ledcWrite(BUZZER_PIN, VOLUME_DUTY[level]);
  activeFreq = freq; activeVolume = level;
}
void stopMelody() {
  if (playing) micBlockedUntil = millis() + 250;
  playing = false; buzzerWrite(0);
}
void play(const Note* notes, uint8_t count) {
  if (!ready || muted || volume == 0) return;
  melodyLength = count > 4 ? 4 : count;
  for (uint8_t i = 0; i < melodyLength; i++) melody[i] = notes[i];
  melodyIndex = 0; playing = true;
  noteUntil = millis() + melody[0].ms;
  buzzerWrite(melody[0].freq);
}
void playTone(uint16_t freq, uint16_t ms) { Note n[1] = {{freq, ms}}; play(n, 1); }
void serviceBuzzer(uint32_t now) {
  if (!playing || before(now, noteUntil)) return;
  if (++melodyIndex >= melodyLength) { stopMelody(); return; }
  noteUntil = now + melody[melodyIndex].ms;
  buzzerWrite(melody[melodyIndex].freq);
}

// ----------------------------------------------------------------- Sensor de ruido
// Cuenta los cambios de D0 en vez de leer su nivel: asi funciona igual si el modulo
// entrega ALTO o BAJO al detectar sonido. La sensibilidad de "hardware" es el tornillo azul
// del modulo; la de aqui decide cuanta actividad hace falta para avisar.
void IRAM_ATTR onMicEdge() { micEdges = micEdges + 1; }

void resetNoise() {
  memset(micRing, 0, sizeof(micRing));
  noiseNow = noiseSustained = 0; localState = 0;
}
void serviceMic(uint32_t now, bool panelLive) {
  if (now - micSlotAt < 50) return;
  micSlotAt = now;
  uint32_t edges = micEdges;
  bool heard = edges != micEdgesSeen;
  micEdgesSeen = edges;
  // El propio buzzer dispara el sensor: se ignora mientras suena y un instante despues.
  if (playing || before(now, micBlockedUntil)) heard = false;
  micRing[micCursor] = heard ? 1 : 0;
  micCursor = (micCursor + 1) % 60;

  uint8_t recent = 0, total = 0;
  for (uint8_t i = 0; i < 60; i++) {
    total += micRing[i];
    if (i < 20) recent += micRing[(micCursor + 59 - i) % 60];
  }
  noiseNow = recent * 5;
  noiseSustained = (uint16_t)total * 100 / 60;

  uint8_t previous = localState;
  if (localState == 2) { if (noiseSustained < SENS_CLEAR[noiseSens]) localState = noiseNow >= SENS_WARN[noiseSens] ? 1 : 0; }
  else if (noiseSustained >= SENS_ALERT[noiseSens]) localState = 2;
  else localState = noiseNow >= SENS_WARN[noiseSens] ? 1 : 0;

  bool localOutputs = LOCAL_MODE && !panelLive && !testActive;
  if (localState == 2 && previous != 2) {
    if (alertCount < 999) alertCount++;
    lastAlertBeepAt = now; wake();
    if (localOutputs) playTone(2000, toneMs);
  } else if (localState == 2 && localOutputs && now - lastAlertBeepAt >= (uint32_t)(toneMs + ALERT_REPEAT_MS)) {
    lastAlertBeepAt = now;
    playTone(2000, toneMs);                                // dura lo que se configuro, ni mas ni menos
  }
}

// ----------------------------------------------------------------- Ajustes
void setVolume(uint8_t level, bool preview) {
  volume = level > VOLUME_MAX ? VOLUME_MAX : level;
  if (volume == 0) stopMelody();
  markDirty(); showOverlay(OV_VOLUME, "");
  if (preview && !muted && volume > 0) playTone(2000, 90);
}
void setMuted(bool value) {
  muted = value;
  if (muted) stopMelody();
  markDirty(); showOverlay(OV_VOLUME, "");
}
void loadSettings() {
  prefs.begin("classsense", false);
  // Clave nueva "vol10": las versiones 2.0 y 2.1 guardaban 5 y 8 niveles y no se pueden reutilizar.
  volume = prefs.getUChar("vol10", 6); if (volume > VOLUME_MAX) volume = VOLUME_MAX;
  toneMs = prefs.getUShort("tms", 500); if (toneMs < 100 || toneMs > TONE_MAX_MS) toneMs = 500;
  brightness = prefs.getUChar("bri", 70); if (brightness > 100) brightness = 100;
  noiseSens = prefs.getUChar("sens", 5); if (noiseSens < 1 || noiseSens > 10) noiseSens = 5;
  muted = prefs.getBool("mute", false);
  ledEnabled = prefs.getBool("led", true);
  memcpy(palette, PAL_DEFAULT, sizeof(palette));
  if (prefs.getBytesLength("pal") == sizeof(palette)) prefs.getBytes("pal", palette, sizeof(palette));
}
void saveSettings(uint32_t now) {
  if (!settingsDirty || before(now, settingsSaveAt)) return;
  settingsDirty = false;
  prefs.putUChar("vol10", volume); prefs.putUShort("tms", toneMs); prefs.putUChar("bri", brightness); prefs.putUChar("sens", noiseSens);
  prefs.putBool("mute", muted); prefs.putBool("led", ledEnabled);
  prefs.putBytes("pal", palette, sizeof(palette));
}
// "Apagar" pone la placa en sueno profundo: pantalla, LED, buzzer y Wi-Fi quedan apagados
// y solo se vigila el pulsador 1. Al pulsarlo, la placa arranca de nuevo.
void centered(const char* text, int16_t y, uint8_t size);
void powerOff() {
  stopMelody();
  settingsDirty = true; settingsSaveAt = millis(); saveSettings(millis());
  if (oledOk) {
    display.dim(false); display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
    centered("Apagado", 12, 2);
    centered("Pulsa el boton 1", 40, 1);
    centered("para encender", 50, 1);
    display.display();
  }
  while (digitalRead(BTN_RESET_PIN) == LOW) delay(10);   // espera a que se suelte
  delay(700);
  if (oledOk) { display.clearDisplay(); display.display(); display.ssd1306_command(SSD1306_DISPLAYOFF); }
  if (ready) {
    for (uint8_t pin : {RED_PIN, GREEN_PIN, BLUE_PIN, BUZZER_PIN}) { ledcDetach(pin); pinMode(pin, INPUT); }
  }
  if (wifiEnabled) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
  rtc_gpio_pullup_en(GPIO_NUM_32);
  rtc_gpio_pulldown_dis(GPIO_NUM_32);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, 0);          // GPIO 32 a GND = encender
  esp_deep_sleep_start();
}

// ----------------------------------------------------------------- Pulsadores
void serviceButtons(uint32_t now) {
  // Pulsador 1: corta = cambia de pantalla (principal / informacion); mantener = apagar.
  // Con la placa apagada, pulsarlo la enciende de nuevo.
  uint8_t e = btnReset.poll(now);
  if (powerGuard) {
    // Tras encender se ignora el boton hasta soltarlo, para que no vuelva a apagarse.
    if (digitalRead(BTN_RESET_PIN) == HIGH && !btnReset.stable) powerGuard = false;
    e = 0;
  }
  if (e == 1) { infoScreen = !infoScreen; wake(); }
  else if (e == 2) powerOff();

  // Pulsador 2: corta = apaga o reactiva el buzzer; mantener = cambia el volumen
  // (un nivel cada 0,6 s mientras siga pulsado: sube hasta 8 y luego baja).
  e = btnBuzzer.poll(now);
  if (e == 1) setMuted(!muted);
  else if (e == 2 || (btnBuzzer.stable && btnBuzzer.longFired && !before(now, volumeRepeatAt))) {
    volumeRepeatAt = now + 600;
    muted = false;
    if (volume >= VOLUME_MAX) volumeStep = -1;
    else if (volume <= 1) volumeStep = 1;
    setVolume(volume + volumeStep, true);
  }

  // Pulsador 3: corta = apaga o enciende el LED; mantener = cambia el color de reposo.
  e = btnLed.poll(now);
  if (e == 1) { ledEnabled = !ledEnabled; markDirty(); showOverlay(OV_MESSAGE, ledEnabled ? "LED encendido" : "LED apagado"); }
  else if (e == 2) {
    ledEnabled = true;
    calmIndex = (calmIndex + 1) % CALM_COLORS;
    memcpy(palette[PAL_CALM], CALM_RGB[calmIndex], 3);
    markDirty(); showOverlay(OV_MESSAGE, CALM_NAME[calmIndex]);
  }
}

// ----------------------------------------------------------------- Pantalla
void centered(const char* text, int16_t y, uint8_t size) {
  display.setTextSize(size);
  display.setCursor((128 - (int16_t)strlen(text) * 6 * size) / 2, y);
  display.print(text);
}
void drawSpeaker(int16_t x, int16_t y) {
  display.fillRect(x, y + 3, 3, 4, SSD1306_WHITE);
  for (uint8_t i = 0; i < 4; i++) display.drawFastVLine(x + 3 + i, y + 3 - i, 4 + 2 * i, SSD1306_WHITE);
  if (muted || volume == 0) {
    display.drawLine(x + 9, y + 2, x + 13, y + 7, SSD1306_WHITE);
    display.drawLine(x + 13, y + 2, x + 9, y + 7, SSD1306_WHITE);
  } else {
    display.drawFastVLine(x + 9, y + 3, 4, SSD1306_WHITE);
    if (volume >= 4) display.drawFastVLine(x + 11, y + 1, 8, SSD1306_WHITE);
    if (volume >= 8) display.drawFastVLine(x + 13, y, 10, SSD1306_WHITE);
  }
}
void drawWifi(int16_t x, int16_t y) {
  uint8_t bars = 0;
  if (wifiUp) { int rssi = WiFi.RSSI(); bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1; }
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t h = 2 + 2 * i;
    if (i < bars) display.fillRect(x + i * 3, y + 8 - h, 2, h, SSD1306_WHITE);
    else display.drawFastHLine(x + i * 3, y + 7, 2, SSD1306_WHITE);
  }
}
void drawVolumeBlocks(int16_t x, int16_t y, uint8_t w, uint8_t h, uint8_t gap) {
  for (uint8_t i = 0; i < VOLUME_MAX; i++) {
    int16_t bx = x + i * (w + gap);
    if (i < volume && !muted) display.fillRect(bx, y, w, h, SSD1306_WHITE);
    else display.drawRect(bx, y, w, h, SSD1306_WHITE);
  }
}
const char* stateWord(bool panelLive) {
  if (!ready) return "BLOQUEADO";
  if (testActive) return "PRUEBA";
  if (panelLive) {
    if (currentState == "normal") return "NORMAL";
    if (currentState == "evaluating") return "EVALUANDO";
    if (currentState == "event") return "EPISODIO";
    if (currentState == "insufficient") return "SIN DATOS";
    if (currentState == "camera_fault") return "FALLO CAM";
    return "APAGADO";
  }
  if (!LOCAL_MODE) return "SIN ENLACE";
  return localState == 2 ? "ALERTA" : localState == 1 ? "RUIDO" : "CALMA";
}
void drawHeader(bool panelLive) {
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 3); display.print("ClassSense");
  display.setCursor(66, 3); display.print(panelLive ? "PANEL" : "LOCAL");
  drawWifi(99, 2); drawSpeaker(113, 1);
  display.drawFastHLine(0, 14, 128, SSD1306_WHITE);
}
void drawMain(uint32_t now, bool panelLive, bool alerting) {
  const char* word = stateWord(panelLive);
  if (alerting && (now / 400) % 2 == 0) {
    display.fillRoundRect(0, 17, 128, 20, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }
  centered(word, 19, 2);
  display.setTextColor(SSD1306_WHITE); display.setTextSize(1);

  if (panelLive && (currentState == "event" || currentState == "evaluating")) {
    centered(ReactionLogic::patternText(currentPatterns), 41, 1);
  } else {
  display.setCursor(0, 41); display.print("Ruido");
  display.drawRoundRect(34, 40, 66, 9, 2, SSD1306_WHITE);
  display.fillRect(36, 42, (uint16_t)noiseNow * 62 / 100, 5, SSD1306_WHITE);
  display.drawFastVLine(36 + SENS_ALERT[noiseSens] * 62 / 100, 40, 9, SSD1306_INVERSE);
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%3u%%", noiseNow);
  display.setCursor(104, 41); display.print(buffer);
  }

  display.setCursor(0, 55); display.print("Vol");
  if (muted) { display.setCursor(22, 55); display.print("MUDO"); }
  else drawVolumeBlocks(22, 55, 3, 7, 1);
  char countBuffer[22];
  snprintf(countBuffer, sizeof(countBuffer), panelLive ? "Posturas %lu" : "Ruido %lu", (unsigned long)(panelLive ? reaction.postureCount() : alertCount));
  display.setCursor(128 - (int16_t)strlen(countBuffer) * 6, 55); display.print(countBuffer);
}
void drawInfo(bool panelLive) {
  char line[24];
  display.setTextSize(1);
  display.setCursor(0, 17); display.print("Alerts 2.4 Plus");
  display.setCursor(0, 25);
  if (!wifiEnabled) display.print("WiFi: sin configurar");
  else if (!wifiUp) display.print("WiFi: buscando red...");
  else { display.print("IP: "); display.print(WiFi.localIP()); }
  display.setCursor(0, 33);
  if (wifiUp) { snprintf(line, sizeof(line), "Senal: %d dBm", (int)WiFi.RSSI()); display.print(line); }
  else display.print("Senal: --");
  snprintf(line, sizeof(line), "Panel:%s Sens:%u/10", panelLive ? "si" : "no", noiseSens);
  display.setCursor(0, 41); display.print(line);
  display.setCursor(0, 49); display.print(ready ? "Salidas: activas" : "Salidas: bloqueadas");
  uint32_t s = millis() / 1000;
  snprintf(line, sizeof(line), "Activo: %02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  display.setCursor(0, 57); display.print(line);
}
void drawOverlay() {
  display.fillRect(6, 12, 116, 44, SSD1306_BLACK);
  display.drawRoundRect(8, 14, 112, 40, 4, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  char buffer[20];
  if (overlay == OV_VOLUME) {
    if (muted) { centered("SONIDO", 19, 1); centered("SILENCIADO", 34, 1); return; }
    snprintf(buffer, sizeof(buffer), "VOLUMEN %u/%u", volume, VOLUME_MAX);
    centered(buffer, 19, 1);
    drawVolumeBlocks(19, 33, 7, 13, 2);
  } else if (overlay == OV_BRIGHTNESS) {
    snprintf(buffer, sizeof(buffer), "BRILLO %u%%", brightness);
    centered(buffer, 19, 1);
    display.drawRoundRect(16, 34, 96, 11, 3, SSD1306_WHITE);
    display.fillRect(18, 36, (uint16_t)brightness * 92 / 100, 7, SSD1306_WHITE);
  } else if (overlay == OV_REACTION) {
    centered(reaction.heading(), 19, 1);
    centered(reaction.message(), 34, 1);
  } else if (overlay == OV_IP) {
    centered("IP DE LA PLACA", 19, 1);
    centered(overlayText, 34, 1);
  } else {
    centered("ClassSense", 19, 1);
    centered(overlayText, 34, 1);
  }
}
void serviceDisplay(uint32_t now, bool panelLive, bool alerting) {
  if (!oledOk || now - lastDrawAt < 100) return;
  lastDrawAt = now;
  if (alerting) wake();
  bool shouldDim = now - lastActivityAt > DIM_AFTER_MS;
  if (shouldDim != dimmed) { dimmed = shouldDim; display.dim(dimmed); }
  if (overlay != OV_NONE && !before(now, overlayUntil)) overlay = OV_NONE;

  display.clearDisplay();
  drawHeader(panelLive);
  if (infoScreen) drawInfo(panelLive); else drawMain(now, panelLive, alerting);
  if (overlay != OV_NONE) drawOverlay();
  display.display();
}
void splash() {
  if (!oledOk) return;
  for (uint8_t step = 0; step <= 10; step++) {
    display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
    centered("ClassSense", 12, 2);
    centered("Alerts 2.4 Plus", 34, 1);
    display.drawRoundRect(24, 50, 80, 7, 3, SSD1306_WHITE);
    display.fillRect(26, 52, step * 76 / 10, 3, SSD1306_WHITE);
    display.display();
    delay(70);
  }
}

// ----------------------------------------------------------------- API del panel
cJSON* body() {
  String raw = server.arg("plain");
  if (raw.length() > 2048) { server.send(413, "application/json", "{\"error\":\"Too large\"}"); return nullptr; }
  cJSON* value = cJSON_Parse(raw.c_str());
  if (!cJSON_IsObject(value)) {
    cJSON_Delete(value);
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return nullptr;
  }
  return value;
}
bool number(cJSON* root, const char* key, int low, int high, int& target) {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!item) return true;
  if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint || item->valueint < low || item->valueint > high) return false;
  target = item->valueint;
  return true;
}
// Lee "palette": {"normal":[r,g,b], ...}. Puede traer solo algunos estados. No cambia nada
// hasta que todo lo recibido es valido.
bool readPalette(cJSON* root, uint8_t (*out)[3], bool& present) {
  present = false;
  cJSON* pal = cJSON_GetObjectItemCaseSensitive(root, "palette");
  if (!pal) return true;
  if (!cJSON_IsObject(pal)) return false;
  memcpy(out, palette, sizeof(palette));
  for (uint8_t i = 0; i < PAL_COUNT; i++) {
    cJSON* color = cJSON_GetObjectItemCaseSensitive(pal, PAL_KEY[i]);
    if (!color) continue;
    if (!cJSON_IsArray(color) || cJSON_GetArraySize(color) != 3) return false;
    for (uint8_t k = 0; k < 3; k++) {
      cJSON* n = cJSON_GetArrayItem(color, k);
      if (!cJSON_IsNumber(n) || n->valuedouble != n->valueint || n->valueint < 0 || n->valueint > 255) return false;
      out[i][k] = (uint8_t)n->valueint;
    }
  }
  present = true;
  return true;
}
// Aplica lo que manda el panel. Solo cuenta lo que cambio respecto a su envio anterior.
void applyPanel(int level, int panelVolume, int sound, int sens, uint8_t (*colors)[3], int toneIn) {
  if (toneIn >= 100 && toneIn != lastPanelTone) { lastPanelTone = toneIn; toneMs = toneIn; markDirty(); }
  if (level >= 0 && level != lastPanelBrightness) { lastPanelBrightness = level; brightness = level; markDirty(); }
  if (panelVolume >= 0 && panelVolume != lastPanelVolume) {
    lastPanelVolume = panelVolume; volume = levelFromPercent(panelVolume);
    if (volume == 0) stopMelody();
    markDirty(); showOverlay(OV_VOLUME, "");
  }
  if (sound >= 0 && sound != lastPanelSound) {
    lastPanelSound = sound; muted = !sound;
    if (muted) stopMelody();
    markDirty();
  }
  if (sens >= 1 && sens != lastPanelSens) { lastPanelSens = sens; noiseSens = sens; resetNoise(); markDirty(); }
  if (colors && (!havePanelPalette || memcmp(colors, lastPanelPalette, sizeof(palette)) != 0)) {
    memcpy(lastPanelPalette, colors, sizeof(palette)); havePanelPalette = true;
    memcpy(palette, colors, sizeof(palette)); markDirty();
  }
}
// Identificacion publica, sin clave y sin datos privados: el panel la usa para encontrar la placa
// en la red sin que haya que escribir su IP. Quien la lea solo sabe que es una placa ClassSense.
void hello() {
  server.send(200, "application/json", "{\"device_type\":\"classsense-alerts\",\"protocol\":1,\"firmware\":\"" FIRMWARE_NAME "\"}");
}
void health() {
  if (!authorized()) return;
  uint32_t now = millis();
  bool panelLive = wifiUp && hasHeartbeat && now - heartbeatAt <= WATCHDOG_MS;
  cJSON* value = cJSON_CreateObject();
  cJSON_AddStringToObject(value, "device_type", "classsense-alerts");
  cJSON_AddNumberToObject(value, "protocol", 1);
  cJSON_AddStringToObject(value, "firmware", FIRMWARE_NAME);
  cJSON_AddBoolToObject(value, "outputs_enabled", ready);
  cJSON_AddStringToObject(value, "buzzer_type", "passive");
  cJSON_AddBoolToObject(value, "common_anode", RGB_COMMON_ANODE);
  cJSON_AddNumberToObject(value, "rssi", WiFi.RSSI());
  cJSON_AddNumberToObject(value, "uptime_s", millis() / 1000);
  cJSON_AddNumberToObject(value, "volume", percentFromLevel(volume));
  cJSON_AddNumberToObject(value, "volume_levels", VOLUME_MAX);
  cJSON_AddBoolToObject(value, "muted", muted);
  cJSON_AddBoolToObject(value, "display", oledOk);
  cJSON_AddNumberToObject(value, "reaction_protocol", 2);
  cJSON_AddStringToObject(value, "panel_state", panelLive ? currentState.c_str() : "local");
  cJSON_AddStringToObject(value, "display_message", overlay == OV_REACTION && before(now, overlayUntil) ? reaction.message() :
    panelLive && (currentState == "event" || currentState == "evaluating") ? ReactionLogic::patternText(currentPatterns) : "");
  cJSON_AddNumberToObject(value, "posture_alerts", reaction.postureCount());
  cJSON_AddNumberToObject(value, "noise", noiseNow);
  cJSON_AddNumberToObject(value, "noise_alerts", alertCount);
  cJSON_AddNumberToObject(value, "noise_sensitivity", noiseSens);
  cJSON_AddNumberToObject(value, "noise_alert_at", SENS_ALERT[noiseSens]);
  cJSON_AddNumberToObject(value, "tone_max_ms", TONE_MAX_MS);
  cJSON_AddNumberToObject(value, "tone_ms", toneMs);
  cJSON_AddNumberToObject(value, "last_wifi_reason", lastWifiReason);
  cJSON* pal = cJSON_AddObjectToObject(value, "palette");
  for (uint8_t i = 0; i < PAL_COUNT; i++) {
    cJSON* color = cJSON_AddArrayToObject(pal, PAL_KEY[i]);
    for (uint8_t k = 0; k < 3; k++) cJSON_AddItemToArray(color, cJSON_CreateNumber(palette[i][k]));
  }
  char* text = cJSON_PrintUnformatted(value);
  server.send(200, "application/json", text);
  cJSON_free(text); cJSON_Delete(value);
}
void state() {
  if (!authorized()) return;
  cJSON* value = body(); if (!value) return;
  cJSON* s = cJSON_GetObjectItemCaseSensitive(value, "state");
  String next = cJSON_IsString(s) ? s->valuestring : "";
  int level = -1, panelVolume = -1, sens = -1, duration = 0, toneIn = -1;
  cJSON* alarm = cJSON_GetObjectItemCaseSensitive(value, "alarm_id");
  String id = cJSON_IsString(alarm) ? alarm->valuestring : "";
  cJSON* kindValue = cJSON_GetObjectItemCaseSensitive(value, "signal_kind");
  String kind = cJSON_IsString(kindValue) ? kindValue->valuestring : "";
  cJSON* list = cJSON_GetObjectItemCaseSensitive(value, "patterns");
  uint8_t patternMask = 0;
  bool patternValid = !list || (cJSON_IsArray(list) && cJSON_GetArraySize(list) <= 4);
  if (list && cJSON_IsArray(list)) {
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
      uint8_t bit = cJSON_IsString(item) ? ReactionLogic::pattern(item->valuestring) : 0;
      if (!bit) patternValid = false;
      patternMask |= bit;
    }
  }
  cJSON* sound = cJSON_GetObjectItemCaseSensitive(value, "buzzer_enabled");
  uint8_t colors[PAL_COUNT][3]; bool hasColors = false;
  bool valid = next == "off" || next == "normal" || next == "evaluating" || next == "event" || next == "insufficient" || next == "camera_fault";
  valid = valid && number(value, "brightness", 0, 100, level) && number(value, "volume", 0, 100, panelVolume);
  valid = valid && number(value, "noise_sensitivity", 1, 10, sens) && readPalette(value, colors, hasColors);
  valid = valid && number(value, "buzz_ms", 0, TONE_MAX_MS, duration) && number(value, "tone_ms", 100, TONE_MAX_MS, toneIn) && id.length() <= 119;
  valid = valid && (!sound || cJSON_IsBool(sound)) && patternValid;
  valid = valid && (!kindValue || kind == "state" || kind == "posture" || kind == "recurrence" || kind == "camera_fault");
  if (!valid) { cJSON_Delete(value); server.send(400, "application/json", "{\"error\":\"Invalid state\"}"); return; }
  applyPanel(level, panelVolume, sound ? (cJSON_IsTrue(sound) ? 1 : 0) : -1, sens, hasColors ? colors : nullptr, toneIn);
  if (next != currentState) wake();
  currentState = next; heartbeatAt = millis(); hasHeartbeat = true;
  currentPatterns = patternMask;
  bool toneStarted = false, duplicate = false;
  if (next == "off") { testActive = false; stopMelody(); if (overlay == OV_REACTION) overlay = OV_NONE; }
  else if (ready && id.length()) {
    ReactionLogic::Kind signal = kind == "posture" ? ReactionLogic::Posture :
      kind == "camera_fault" || next == "camera_fault" ? ReactionLogic::CameraFault : ReactionLogic::Recurrence;
    auto result = reaction.accept(id.c_str(), signal, patternMask, duration, ready, muted, volume);
    duplicate = !result.fresh;
    if (result.fresh) {
      infoScreen = false;
      showOverlayFor(OV_REACTION, reaction.message(), duration > 3000 ? duration + 1000 : 4000);
      if (result.sound) { playTone(2000, duration); toneStarted = true; }
    }
  }
  cJSON_Delete(value);
  char reply[200];
  snprintf(reply, sizeof(reply), "{\"applied\":%s,\"tone_started\":%s,\"display_ready\":%s,\"silenced\":%s,\"duplicate\":%s}",
    ready?"true":"false", toneStarted?"true":"false", oledOk?"true":"false", (muted||volume==0)?"true":"false", duplicate?"true":"false");
  server.send(200, "application/json", reply);
}
// El panel guarda aqui volumen, brillo, sonido, sensibilidad y colores al instante, sin cambiar
// el color de la alarma y sin necesitar una sesion de camara. Si el volumen cambia suena un
// pitido de muestra.
void settings() {
  if (!authorized()) return;
  cJSON* value = body(); if (!value) return;
  int level = -1, panelVolume = -1, sens = -1, toneIn = -1;
  cJSON* sound = cJSON_GetObjectItemCaseSensitive(value, "buzzer_enabled");
  cJSON* localValue = cJSON_GetObjectItemCaseSensitive(value, "local");
  bool localRequest = cJSON_IsTrue(localValue);
  uint8_t colors[PAL_COUNT][3]; bool hasColors = false;
  bool valid = number(value, "brightness", 0, 100, level) && number(value, "volume", 0, 100, panelVolume)
    && number(value, "noise_sensitivity", 1, 10, sens) && number(value, "tone_ms", 100, TONE_MAX_MS, toneIn)
    && readPalette(value, colors, hasColors) && (!sound || cJSON_IsBool(sound)) && (!localValue || cJSON_IsBool(localValue));
  if (!valid) { cJSON_Delete(value); server.send(400, "application/json", "{\"error\":\"Invalid settings\"}"); return; }
  uint8_t before = volume;
  int priorBrightness = lastPanelBrightness, priorVolume = lastPanelVolume, priorSound = lastPanelSound;
  int priorSens = lastPanelSens, priorTone = lastPanelTone;
  bool priorPalette = havePanelPalette;
  uint8_t priorColors[PAL_COUNT][3]; memcpy(priorColors, lastPanelPalette, sizeof(priorColors));
  // An explicit Save applies values even when the last heartbeat contained the same numbers.
  lastPanelBrightness = lastPanelVolume = lastPanelSound = lastPanelSens = lastPanelTone = -1;
  havePanelPalette = false;
  applyPanel(level, panelVolume, sound ? (cJSON_IsTrue(sound) ? 1 : 0) : -1, sens, hasColors ? colors : nullptr, toneIn);
  if (localRequest) {
    // Local phone controls behave like physical buttons: an unchanged host heartbeat does not undo them.
    lastPanelBrightness = priorBrightness; lastPanelVolume = priorVolume; lastPanelSound = priorSound;
    lastPanelSens = priorSens; lastPanelTone = priorTone; havePanelPalette = priorPalette;
    memcpy(lastPanelPalette, priorColors, sizeof(priorColors));
  }
  if (volume != before && !muted && volume > 0) playTone(2000, 90);
  cJSON_Delete(value);
  server.send(200, "application/json", "{\"applied\":true}");
}
void test() {
  if (!authorized()) return;
  if (!ready) { server.send(409, "application/json", "{\"applied\":false}"); return; }
  uint32_t now = millis();
  cJSON* value = body(); if (!value) return;
  cJSON* mode = cJSON_GetObjectItemCaseSensitive(value, "mode");
  String selected = cJSON_IsString(mode) ? mode->valuestring : "";
  // El color libre se puede reenviar enseguida para verlo mientras se mueven los controles.
  uint32_t cooldown = selected == "rgb" ? 250 : 3000;
  if (hasTest && now - lastTestAt < cooldown) { cJSON_Delete(value); server.send(429, "application/json", "{\"applied\":false}"); return; }
  int level = -1, panelVolume = -1, duration = 500, r = 0, g = 0, b = 0, ms = 4000;
  bool valid = (selected == "red" || selected == "green" || selected == "blue" || selected == "buzzer" || selected == "rgb")
    && number(value, "brightness", 0, 100, level) && number(value, "volume", 0, 100, panelVolume)
    && number(value, "buzz_ms", 100, TONE_MAX_MS, duration)
    && number(value, "r", 0, 255, r) && number(value, "g", 0, 255, g) && number(value, "b", 0, 255, b)
    && number(value, "ms", 300, 10000, ms);
  cJSON_Delete(value);
  if (!valid) { server.send(400, "application/json", "{\"applied\":false}"); return; }
  applyPanel(level, panelVolume, -1, -1, nullptr, -1);
  testMode = selected;
  if (selected == "rgb") { testRgb[0] = r; testRgb[1] = g; testRgb[2] = b; }
  testUntil = now + (selected == "buzzer" ? duration : selected == "rgb" ? ms : 2000); testActive = true;
  if (selected == "buzzer") playTone(2000, duration);
  lastTestAt = now; hasTest = true; heartbeatAt = now; hasHeartbeat = true; wake();
  server.send(200, "application/json", "{\"applied\":true}");
}

// ----------------------------------------------------------------- Arranque
void controllerPage() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
  server.send_P(200, "text/html; charset=utf-8", CONTROLLER_PAGE);
}

void setup() {
  Serial.begin(115200);
  rtc_gpio_deinit(GPIO_NUM_32);                          // devuelve GPIO 32 a uso normal tras "encender"
  pinMode(BTN_RESET_PIN, INPUT_PULLUP);
  delay(5);
  powerGuard = digitalRead(BTN_RESET_PIN) == LOW;
  pinMode(BTN_BUZZER_PIN, INPUT_PULLUP);
  pinMode(BTN_LED_PIN, INPUT_PULLUP);
  pinMode(MIC_PIN, INPUT);                               // GPIO34 solo es entrada, sin pull-up interno
  loadSettings();

  if (OUTPUTS_CONFIRMED) {
    // Canales RGB 0/1/2 usan los temporizadores 0/1. El buzzer (canal 4) tiene el suyo:
    // compartirlo cambiaria el brillo del azul al sonar.
    bool a = ledcAttachChannel(RED_PIN, 5000, 8, 0);
    bool b = ledcAttachChannel(GREEN_PIN, 5000, 8, 1);
    bool c = ledcAttachChannel(BLUE_PIN, 5000, 8, 2);
    bool d = ledcAttachChannel(BUZZER_PIN, 2000, 10, 4);
    ready = a && b && c && d;
    if (!ready) {
      for (uint8_t pin : {RED_PIN, GREEN_PIN, BLUE_PIN}) { ledcDetach(pin); pinMode(pin, OUTPUT); digitalWrite(pin, RGB_COMMON_ANODE ? HIGH : LOW); }
      ledcDetach(BUZZER_PIN); pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
    }
  }
  rgb8(0, 0, 0); buzzerWrite(0);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(400000);
  // begin() solo confirma memoria disponible; primero comprobamos el ACK del dispositivo.
  uint8_t oledAddress = 0;
  for (uint8_t address : {OLED_ADDRESS, uint8_t(0x3D)}) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) { oledAddress = address; break; }
  }
  oledOk = oledAddress && display.begin(SSD1306_SWITCHCAPVCC, oledAddress, true, false);
  if (!oledOk) Serial.println("Pantalla OLED no detectada: revisa alimentacion, SDA=21 y SCL=22 (0x3C/0x3D).");
  else Serial.printf("OLED SSD1306 detectada en 0x%02X.\n", oledAddress);
  splash();

  attachInterrupt(digitalPinToInterrupt(MIC_PIN), onMicEdge, CHANGE);

  wifiEnabled = strlen(WIFI_SSID) > 0;
  if (wifiEnabled) {
    WiFi.onEvent(wifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    wifiRetry.started(millis()); beginWifi();
    const char* headers[] = {"X-ClassSense-Key"};
    server.collectHeaders(headers, 1);
    server.on("/", HTTP_GET, controllerPage);
    server.on("/api/hello", HTTP_GET, hello);
    server.on("/api/health", HTTP_GET, health);
    server.on("/api/state", HTTP_POST, state);
    server.on("/api/test", HTTP_POST, test);
    server.on("/api/settings", HTTP_POST, settings);
    server.onNotFound([]() { server.send(404, "application/json", "{\"error\":\"Not found\"}"); });
    Serial.println(FIRMWARE_NAME ": esperando Wi-Fi de 2,4 GHz.");
  } else Serial.println(FIRMWARE_NAME ": Wi-Fi sin configurar, modo local.");
  Serial.println(ready ? "Salidas habilitadas." : "Salidas bloqueadas: confirma el cableado en secrets.h.");
  wake();
  // Pitido corto al arrancar: confirma al oido que el buzzer funciona y a que volumen esta.
  if (ready && !muted) playTone(2000, 90);
}

void loop() {
  uint32_t now = millis();

  if (wifiEnabled) {
    serviceWifi(now);
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wifiUp) {
      server.close();
      server.begin();
      Serial.print("IP del controlador: http://"); Serial.println(WiFi.localIP());
      snprintf(ipText, sizeof(ipText), "%s", WiFi.localIP().toString().c_str());
      showOverlayFor(OV_IP, ipText, 15000);
    }
    if (!connected && wifiUp) { hasHeartbeat = false; Serial.println("Wi-Fi desconectado."); }
    wifiUp = connected;
    if (connected) server.handleClient();
  }

  now = millis();
  bool panelLive = wifiUp && hasHeartbeat && now - heartbeatAt <= WATCHDOG_MS;
  if (testActive && (!panelLive || !before(now, testUntil))) testActive = false;

  serviceButtons(now);
  serviceMic(now, panelLive);
  serviceBuzzer(now);

  bool alerting = false;
  if (testActive) {
    if (testMode == "rgb") rgbOf(testRgb);
    else rgb8(testMode == "red" ? 255 : 0, testMode == "green" ? 255 : 0, testMode == "blue" ? 255 : 0);
  } else if (panelLive) {
    bool fault = currentState == "camera_fault";
    alerting = fault || currentState == "event";
    if (currentState == "normal") rgbOf(palette[PAL_NORMAL]);
    else if (currentState == "evaluating") rgbOf(palette[PAL_EVAL]);
    else if (currentState == "event") rgbOf(palette[PAL_EVENT]);
    else if (currentState == "insufficient") rgbOf(palette[PAL_INSUF]);
    else if (fault && (now / 400) % 2 == 0) rgbOf(palette[PAL_EVENT]);
    else rgb8(0, 0, 0);
  } else if (LOCAL_MODE) {
    alerting = localState == 2;
    if (localState == 0) rgbOf(palette[PAL_CALM]);
    else if (localState == 1) rgbOf(palette[PAL_EVAL]);
    else if ((now / 250) % 2 == 0) rgbOf(palette[PAL_EVENT]);
    else rgb8(0, 0, 0);
  } else {
    if (playing) stopMelody();
    rgb8(0, 0, 0);
  }

  serviceDisplay(now, panelLive, alerting);
  saveSettings(now);
  delay(1);
}
