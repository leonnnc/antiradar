#include <Arduino.h>
#include <FS.h>
#include <RF24.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLEScan.h>
#include <BLESecurity.h>
#include <esp_bt.h>
#include "USB.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDKeyboard.h"
#include <math.h>

#include "AppInput.h"
#include "CyberdeckPins.h"

namespace {

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr uint8_t FRAME_COLOR_DEPTH = 8;
constexpr uint16_t COL_BG = TFT_BLACK;
constexpr uint16_t COL_PANEL = 0x0208;
constexpr uint16_t COL_PANEL_2 = 0x09A6;
constexpr uint16_t COL_GRID = 0x19E7;
constexpr uint16_t COL_GREEN = 0x07E0;
constexpr uint16_t COL_CYAN = 0x07FF;
constexpr uint16_t COL_AMBER = 0xFD20;
constexpr uint16_t COL_RED = 0xF800;
constexpr uint16_t COL_MUTED = 0x8410;
constexpr uint16_t COL_TEXT = TFT_WHITE;

TFT_eSPI tft;
TFT_eSprite frame(&tft);
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;
SPIClass sdSPI(HSPI);
RF24 radio1(CD_NRF1_CE, CD_NRF1_CSN);
RF24 radio2(CD_NRF2_CE, CD_NRF2_CSN);
USBHIDKeyboard HidKeyboard;
USBHIDConsumerControl HidConsumer;
BLEServer* bleRemoteServer = nullptr;
BLEHIDDevice* bleRemoteHid = nullptr;
BLECharacteristic* bleRemoteKeyboardInput = nullptr;
BLECharacteristic* bleRemoteMediaInput = nullptr;
BLESecurity* bleRemoteSecurity = nullptr;

bool frameReady = false;
bool sdReady = false;
bool sdTried = false;
bool radio1Ready = false;
bool radio2Ready = false;
bool radiosTried = false;
bool gpsPortReady = false;
bool radioScanArmed = false;
bool hidReady = false;
bool bleRemoteReady = false;
bool bleRemoteConnected = false;
uint32_t gpsLastChars = 0;
uint32_t gpsLastCharMs = 0;
uint32_t gpsLastUiMs = 0;
uint32_t gpsStatsLastMs = 0;
uint32_t gpsStatsLastChars = 0;
uint16_t gpsCharsPerSec = 0;
uint32_t gpsPlaceLastScanMs = 0;
double gpsPlaceLastScanLat = 999.0;
double gpsPlaceLastScanLng = 999.0;
uint32_t sdMountHz = 0;
uint32_t lastRenderMs = 0;
uint32_t lastSensorMs = 0;
uint32_t lastRadioMs = 0;
uint32_t radioScanTicks = 0;
uint32_t bootMs = 0;
float batteryVolts = 0.0f;
int batteryPct = 0;
uint16_t radioBars[80] = {};
uint8_t radioScanChannel = 0;
char statusLine[48] = "Ready";
constexpr char GPS_PLACE_DB_PATH[] = "/APPS/GPS/places.csv";
constexpr uint32_t GPS_PLACE_RESCAN_MS = 15000;
constexpr float GPS_PLACE_RESCAN_KM = 1.0f;
constexpr uint32_t GPS_PLACE_MAX_ROWS = 65000;
constexpr uint8_t WIFI_MAX_APS = 24;
constexpr uint8_t WIFI_HISTORY_LEN = 44;
constexpr uint8_t BLE_MAX_DEVICES = 24;
constexpr uint8_t BLE_HISTORY_LEN = 36;

enum class Screen : uint8_t {
    Home,
    SystemPulse,
    GpsRadar,
    WifiLocator,
    BleRadar,
    GpsSos,
    DemoLauncher,
    SdVault,
    RadioScope,
    PasscodeSim,
    HidDemo,
    IphoneRemote,
    Battery,
    About
};

struct MenuEntry {
    const char* title;
    const char* subtitle;
    Screen screen;
};

const MenuEntry MENU[] = {
    {"SYSTEM PULSE", "Estado del hardware", Screen::SystemPulse},
    {"GPS RADAR", "Ubicacion real, altitud y lugar", Screen::GpsRadar},
    {"WIFI LOCATOR", "Lista redes y radar RSSI", Screen::WifiLocator},
    {"BLE DEVICE RADAR", "Radar de dispositivos Bluetooth", Screen::BleRadar},
    {"GPS SOS MODE", "Coordenadas grandes para emergencia", Screen::GpsSos},
    {"CYBER DEMO", "Launcher rapido para reels", Screen::DemoLauncher},
    {"SD VAULT", "microSD y prueba de escritura", Screen::SdVault},
    {"RADIO SCOPE", "Escaneo pasivo 2.4 GHz", Screen::RadioScope},
    {"PASSCODE SIM", "Demo visual de PIN", Screen::PasscodeSim},
    {"HID PAD", "Apps, terminal y multimedia", Screen::HidDemo},
    {"IPHONE REMOTE", "BLE app launcher y multimedia", Screen::IphoneRemote},
    {"BATTERY METER", "Voltaje y porcentaje Li-ion", Screen::Battery},
    {"ABOUT TEMPLATE", "Pines, controles y version", Screen::About},
};

struct GpsPlace {
    const char* city;
    const char* municipality;
    const char* state;
    float lat;
    float lng;
    uint16_t closeKm;
};

struct GpsPlaceMatch {
    const GpsPlace* place;
    float km;
    bool close;
};

struct GpsResolvedPlace {
    bool valid;
    bool close;
    bool fromSd;
    uint32_t rowsScanned;
    float km;
    float radiusKm;
    char city[34];
    char municipality[34];
    char state[34];
    char country[18];
};

struct WifiApInfo {
    char ssid[33];
    char bssid[18];
    int32_t rssi;
    uint8_t channel;
    uint8_t encryption;
    bool hidden;
};

struct BleDeviceInfo {
    char name[28];
    char label[28];
    char kind[18];
    char address[18];
    int32_t rssi;
    int32_t bestRssi;
    int8_t txPower;
    uint8_t serviceCount;
    uint16_t companyId;
    uint16_t appearance;
    bool hasName;
    bool hasTxPower;
    bool hasManufacturer;
    bool hasAppearance;
};

const GpsPlace GPS_PLACES[] = {
    {"Chihuahua", "Chihuahua", "Chihuahua MX", 28.6353f, -106.0889f, 65},
    {"Ciudad Juarez", "Juarez", "Chihuahua MX", 31.6904f, -106.4245f, 70},
    {"Delicias", "Delicias", "Chihuahua MX", 28.1901f, -105.4701f, 45},
    {"Cuauhtemoc", "Cuauhtemoc", "Chihuahua MX", 28.4050f, -106.8667f, 55},
    {"Hidalgo del Parral", "Hidalgo del Parral", "Chihuahua MX", 26.9333f, -105.6667f, 55},
    {"Camargo", "Camargo", "Chihuahua MX", 27.6903f, -105.1714f, 45},
    {"Jimenez", "Jimenez", "Chihuahua MX", 27.1303f, -104.9073f, 50},
    {"Nuevo Casas Grandes", "Nuevo Casas Grandes", "Chihuahua MX", 30.4155f, -107.9110f, 60},
    {"Ojinaga", "Ojinaga", "Chihuahua MX", 29.5669f, -104.5449f, 60},
    {"Aldama", "Aldama", "Chihuahua MX", 28.8392f, -105.9148f, 35},
    {"Meoqui", "Meoqui", "Chihuahua MX", 28.2728f, -105.4818f, 35},
    {"Santa Isabel", "Santa Isabel", "Chihuahua MX", 28.3428f, -106.3732f, 35},
    {"Saucillo", "Saucillo", "Chihuahua MX", 28.0313f, -105.2935f, 40},
    {"Creel", "Bocoyna", "Chihuahua MX", 27.7506f, -107.6354f, 45},
    {"Guachochi", "Guachochi", "Chihuahua MX", 26.8200f, -107.0740f, 55},
    {"Madera", "Madera", "Chihuahua MX", 29.1900f, -108.1460f, 55},
    {"Ahumada", "Ahumada", "Chihuahua MX", 30.6180f, -106.5120f, 60},
    {"Buenaventura", "Buenaventura", "Chihuahua MX", 29.8380f, -107.4710f, 55},
    {"Ascension", "Ascension", "Chihuahua MX", 31.0920f, -107.9960f, 60},
    {"Janos", "Janos", "Chihuahua MX", 30.8900f, -108.1930f, 55},
};

GpsResolvedPlace gpsResolvedPlace = {};
WifiApInfo wifiAps[WIFI_MAX_APS] = {};
uint8_t wifiApCount = 0;
uint8_t wifiListSelected = 0;
uint8_t wifiListScroll = 0;
bool wifiTargetValid = false;
bool wifiTargetSeen = false;
char wifiTargetSsid[33] = "";
char wifiTargetBssid[18] = "";
uint8_t wifiTargetChannel = 0;
int32_t wifiTargetRssi = -127;
int32_t wifiLastTargetRssi = -127;
int32_t wifiBestRssi = -127;
int16_t wifiTrendDb = 0;
int16_t wifiHistory[WIFI_HISTORY_LEN] = {};
uint8_t wifiHistoryHead = 0;
uint32_t wifiScanPass = 0;
uint32_t wifiLastTargetScanMs = 0;
uint32_t wifiAsyncScanStartedMs = 0;
bool wifiAsyncScanActive = false;
bool btClassicReleased = false;
bool bleStackReady = false;
BLEScan* bleScan = nullptr;
BleDeviceInfo bleDevices[BLE_MAX_DEVICES] = {};
uint8_t bleDeviceCount = 0;
uint8_t bleListSelected = 0;
uint8_t bleListScroll = 0;
bool bleTargetValid = false;
bool bleTargetSeen = false;
char bleTargetName[28] = "";
char bleTargetAddress[18] = "";
int32_t bleTargetRssi = -127;
int32_t bleLastTargetRssi = -127;
int32_t bleBestRssi = -127;
int16_t bleTrendDb = 0;
int16_t bleHistory[BLE_HISTORY_LEN] = {};
uint8_t bleHistoryHead = 0;
uint32_t bleScanPass = 0;

class BleRemoteServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer* server) override {
        (void)server;
        bleRemoteConnected = true;
    }

    void onDisconnect(BLEServer* server) override {
        bleRemoteConnected = false;
        if (server) server->startAdvertising();
    }
};

BleRemoteServerCallbacks bleRemoteCallbacks;

constexpr uint8_t MENU_COUNT = sizeof(MENU) / sizeof(MENU[0]);
Screen currentScreen = Screen::Home;
uint8_t menuIndex = 0;
uint8_t menuScroll = 0;
bool radioPaused = false;

void drawPixelSafe(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    frame.drawPixel(x, y, color);
}

void toneClick(uint16_t freq = 2500, uint16_t ms = 12) {
    ledcWriteTone(0, freq);
    delay(ms);
    ledcWriteTone(0, 0);
}

void setStatus(const char* text) {
    strncpy(statusLine, text, sizeof(statusLine) - 1);
    statusLine[sizeof(statusLine) - 1] = '\0';
}

String uptimeText() {
    uint32_t seconds = (millis() - bootMs) / 1000;
    char out[16];
    snprintf(out, sizeof(out), "%02lu:%02lu", seconds / 60, seconds % 60);
    return String(out);
}

float readBatteryVolts() {
    uint32_t mv = 0;
    for (uint8_t i = 0; i < 20; i++) {
        mv += analogReadMilliVolts(CD_VBAT_ADC);
        delay(2);
    }
    return (mv / 20.0f / 1000.0f) * CD_VBAT_DIVIDER;
}

int batteryPercent(float volts) {
    const float minV = 3.25f;
    const float maxV = 4.20f;
    int pct = roundf(((volts - minV) / (maxV - minV)) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void drainGps(uint16_t ms = 8) {
    const uint32_t start = millis();
    while ((millis() - start) < ms) {
        while (gpsSerial.available()) {
            const char c = gpsSerial.read();
            gps.encode(c);
            gpsLastCharMs = millis();
            gpsLastChars = gps.charsProcessed();
        }
        delay(1);
    }
}

void updateGpsStats() {
    const uint32_t now = millis();
    const uint32_t chars = gps.charsProcessed();
    if (gpsStatsLastMs == 0) {
        gpsStatsLastMs = now;
        gpsStatsLastChars = chars;
        return;
    }
    const uint32_t elapsed = now - gpsStatsLastMs;
    if (elapsed >= 1000) {
        gpsCharsPerSec = ((chars - gpsStatsLastChars) * 1000UL) / elapsed;
        gpsStatsLastMs = now;
        gpsStatsLastChars = chars;
    }
}

void serviceGps(uint16_t ms = 12) {
    drainGps(ms);
    updateGpsStats();
}

void drawGrid() {
    for (int x = 0; x < SCREEN_W; x += 16) {
        frame.drawFastVLine(x, 0, SCREEN_H, (x % 64 == 0) ? 0x0320 : 0x0104);
    }
    for (int y = 0; y < SCREEN_H; y += 16) {
        frame.drawFastHLine(0, y, SCREEN_W, (y % 64 == 0) ? 0x0320 : 0x0104);
    }
}

void pushFrame() {
    if (frameReady) {
        digitalWrite(CD_NRF1_CSN, HIGH);
        digitalWrite(CD_NRF2_CSN, HIGH);
        digitalWrite(CD_SD_CS, HIGH);
        frame.pushSprite(0, 0);
    }
}

void restoreTftBus() {
    if (radio1Ready) radio1.stopListening();
    if (radio2Ready) radio2.stopListening();
    digitalWrite(CD_NRF1_CE, LOW);
    digitalWrite(CD_NRF2_CE, LOW);
    digitalWrite(CD_NRF1_CSN, HIGH);
    digitalWrite(CD_NRF2_CSN, HIGH);
    digitalWrite(CD_SD_CS, HIGH);
    digitalWrite(CD_TFT_CS, HIGH);
    SPI.begin(CD_SPI_SCK, CD_SPI_MISO, CD_SPI_MOSI);
    delayMicroseconds(60);
}

void drawDirectStatus(const char* title, const char* line, uint16_t color = COL_CYAN) {
    restoreTftBus();
    tft.fillScreen(COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(color, COL_BG);
    tft.drawString(title, 18, 46, 2);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(line, 18, 78, 2);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("CYBERDECK iPHONE REMOTE", 18, 112, 2);
    tft.drawRoundRect(16, 38, 288, 112, 5, color);
}

void releaseFrameForBleStartup() {
    if (frameReady) {
        frame.deleteSprite();
        frameReady = false;
    }
    drawDirectStatus("INICIANDO BLE", "Liberando memoria de pantalla...", COL_AMBER);
    delay(80);
}

void releaseClassicBtMemoryOnce() {
    if (btClassicReleased) return;
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    btClassicReleased = true;
}

bool recreateFrameAfterBleStartup() {
    restoreTftBus();
    frame.setColorDepth(FRAME_COLOR_DEPTH);
    frameReady = frame.createSprite(SCREEN_W, SCREEN_H) != nullptr;
    if (!frameReady) {
        drawDirectStatus("ERROR TFT", "No se pudo recrear sprite.", COL_RED);
        return false;
    }
    return true;
}

void drawText(int x, int y, const String& text, uint16_t color = COL_TEXT, uint8_t size = 1) {
    frame.setTextColor(color, COL_BG);
    frame.setTextSize(size);
    frame.setTextDatum(TL_DATUM);
    frame.drawString(text, x, y, 2);
}

void drawTextOn(int x, int y, const String& text, uint16_t color, uint16_t bg, uint8_t size = 1) {
    frame.setTextColor(color, bg);
    frame.setTextSize(size);
    frame.setTextDatum(TL_DATUM);
    frame.drawString(text, x, y, 2);
}

void drawHeader(const char* title, const char* tag) {
    frame.fillRect(0, 0, SCREEN_W, 28, 0x0184);
    frame.drawFastHLine(0, 28, SCREEN_W, COL_GREEN);
    frame.fillCircle(10, 14, 4, COL_GREEN);
    drawTextOn(20, 6, title, COL_GREEN, 0x0184, 1);
    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(COL_CYAN, 0x0184);
    frame.setTextSize(1);
    frame.drawString(tag, 314, 6, 2);
    frame.setTextDatum(TL_DATUM);
}

void drawFooter(const char* hint = "ENC/UP/DOWN MOVE  OK SELECT  BACK EXIT") {
    frame.fillRect(0, 218, SCREEN_W, 22, 0x0184);
    frame.drawFastHLine(0, 217, SCREEN_W, COL_GRID);
    frame.setTextColor(COL_MUTED, 0x0184);
    frame.setTextSize(1);
    frame.setTextDatum(TL_DATUM);
    frame.drawString(hint, 8, 221, 2);
}

void drawBadge(int x, int y, int w, const char* label, const String& value, uint16_t color) {
    frame.drawRoundRect(x, y, w, 38, 4, color);
    frame.fillRect(x + 1, y + 1, w - 2, 12, color);
    frame.setTextSize(1);
    frame.setTextColor(COL_BG, color);
    frame.drawString(label, x + 5, y + 1, 1);
    frame.setTextColor(COL_TEXT, COL_BG);
    frame.drawString(value, x + 6, y + 18, 2);
}

void drawMiniBadge(int x, int y, int w, const char* label, const String& value, uint16_t color) {
    frame.drawRoundRect(x, y, w, 25, 4, color);
    frame.setTextSize(1);
    frame.setTextColor(color, COL_BG);
    frame.drawString(label, x + 4, y + 2, 1);
    frame.setTextColor(COL_TEXT, COL_BG);
    frame.drawString(value, x + 4, y + 11, 1);
}

void drawBar(int x, int y, int w, int h, int pct, uint16_t color) {
    pct = constrain(pct, 0, 100);
    frame.drawRect(x, y, w, h, COL_GRID);
    frame.fillRect(x + 2, y + 2, w - 4, h - 4, 0x0841);
    frame.fillRect(x + 2, y + 2, ((w - 4) * pct) / 100, h - 4, color);
}

String fitText(const String& text, uint8_t maxChars) {
    if (text.length() <= maxChars) return text;
    if (maxChars <= 1) return "~";
    return text.substring(0, maxChars - 1) + "~";
}

void drawBoot() {
    frame.fillSprite(COL_BG);
    drawGrid();
    frame.drawRoundRect(18, 28, 284, 178, 5, COL_GREEN);
    frame.setTextColor(COL_GREEN, COL_BG);
    frame.setTextSize(2);
    frame.setTextDatum(MC_DATUM);
    frame.drawString("CYBERDECK", SCREEN_W / 2, 74, 2);
    frame.setTextSize(1);
    frame.drawString("S3 APPS TEMPLATE", SCREEN_W / 2, 104, 2);

    const int bx = 50;
    const int by = 136;
    const int bw = 220;
    frame.drawRect(bx, by, bw, 12, COL_GREEN);
    for (int i = 0; i <= bw - 4; i += 18) {
        frame.fillRect(bx + 2, by + 2, i, 8, COL_GREEN);
        pushFrame();
        delay(22);
    }

    frame.setTextColor(COL_MUTED, COL_BG);
    frame.drawString("TFT / INPUT / GPS / SD / NRF READY", SCREEN_W / 2, 170, 2);
    frame.setTextDatum(TL_DATUM);
    pushFrame();
}

bool beginSdCard() {
    if (sdReady) return true;
    if (sdTried && !sdReady) return false;
    sdTried = true;

    pinMode(CD_TFT_CS, OUTPUT);
    digitalWrite(CD_TFT_CS, HIGH);
    pinMode(CD_NRF1_CSN, OUTPUT);
    digitalWrite(CD_NRF1_CSN, HIGH);
    pinMode(CD_NRF2_CSN, OUTPUT);
    digitalWrite(CD_NRF2_CSN, HIGH);
    pinMode(CD_SD_CS, OUTPUT);
    digitalWrite(CD_SD_CS, HIGH);
    pinMode(CD_SD_MISO, INPUT_PULLUP);

    sdSPI.begin(CD_SD_SCK, CD_SD_MISO, CD_SD_MOSI, CD_SD_CS);
    const uint32_t speeds[] = {4000000, 10000000, 20000000};
    for (uint8_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        SD.end();
        delay(50);
        if (SD.begin(CD_SD_CS, sdSPI, speeds[i])) {
            sdReady = true;
            sdMountHz = speeds[i];
            return true;
        }
    }
    return false;
}

bool writeTextFile(const char* path, const String& content) {
    if (!beginSdCard()) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.print(content);
    f.close();
    return true;
}

bool appendTextFile(const char* path, const String& content) {
    if (!beginSdCard()) return false;
    File f = SD.open(path, FILE_APPEND);
    if (!f) return false;
    f.print(content);
    f.close();
    return true;
}

void countRootFiles(uint16_t& dirs, uint16_t& files) {
    dirs = 0;
    files = 0;
    if (!beginSdCard()) return;

    File root = SD.open("/");
    if (!root) return;

    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) dirs++;
        else files++;
        entry.close();
    }
    root.close();
}

void ensureSdFolders() {
    if (!beginSdCard()) {
        setStatus("SD mount failed");
        return;
    }
    SD.mkdir("/APPS");
    SD.mkdir("/APPS/GPS");
    SD.mkdir("/APPS/LOGS");
    SD.mkdir("/APPS/EXPORTS");

    String out;
    out += "CYBERDECK S3 APPS\r\n";
    out += "Uptime: " + uptimeText() + "\r\n";
    out += "Battery: " + String(batteryVolts, 2) + "V\r\n";
    out += "GPS chars: " + String(gps.charsProcessed()) + "\r\n";
    writeTextFile("/APPS/APP_TEST.txt", out);
    setStatus("SD wrote /APPS/APP_TEST.txt");
}

void beginGps() {
    gpsSerial.setRxBufferSize(1024);
    gpsSerial.begin(CD_GPS_BAUD, SERIAL_8N1, CD_GPS_RX, CD_GPS_TX);
    gpsLastCharMs = millis();
    gpsStatsLastMs = 0;
    gpsStatsLastChars = gps.charsProcessed();
    gpsPortReady = true;
}

void beginHid() {
    if (hidReady) return;
    HidKeyboard.begin();
    HidConsumer.begin();
    USB.begin();
    hidReady = true;
}

bool beginRadios() {
    if (radiosTried) return radio1Ready || radio2Ready;
    radiosTried = true;

    pinMode(CD_NRF1_CE, OUTPUT);
    pinMode(CD_NRF2_CE, OUTPUT);
    pinMode(CD_NRF1_CSN, OUTPUT);
    pinMode(CD_NRF2_CSN, OUTPUT);
    digitalWrite(CD_NRF1_CE, LOW);
    digitalWrite(CD_NRF2_CE, LOW);
    digitalWrite(CD_NRF1_CSN, HIGH);
    digitalWrite(CD_NRF2_CSN, HIGH);

    radio1Ready = radio1.begin();
    radio2Ready = radio2.begin();

    RF24* radios[] = {&radio1, &radio2};
    const bool ready[] = {radio1Ready, radio2Ready};
    for (uint8_t i = 0; i < 2; i++) {
        if (!ready[i]) continue;
        radios[i]->setAutoAck(false);
        radios[i]->disableCRC();
        radios[i]->setPALevel(RF24_PA_MIN);
        radios[i]->setDataRate(RF24_2MBPS);
        radios[i]->setChannel(0);
        radios[i]->startListening();
    }

    setStatus((radio1Ready || radio2Ready) ? "NRF scan ready" : "NRF not detected");
    return radio1Ready || radio2Ready;
}

void quietRadiosForGps() {
    if (radio1Ready) radio1.stopListening();
    if (radio2Ready) radio2.stopListening();
    digitalWrite(CD_NRF1_CE, LOW);
    digitalWrite(CD_NRF2_CE, LOW);
    digitalWrite(CD_NRF1_CSN, HIGH);
    digitalWrite(CD_NRF2_CSN, HIGH);
    radioScanArmed = false;
}

void prepareGpsMode() {
    quietRadiosForGps();
    gpsSerial.end();
    delay(30);
    beginGps();
    serviceGps(120);
    setStatus("GPS UART live");
}

void updateSensors() {
    serviceGps(5);
    const uint32_t now = millis();
    if (now - lastSensorMs < 900) return;
    lastSensorMs = now;
    batteryVolts = readBatteryVolts();
    batteryPct = batteryPercent(batteryVolts);
}

void scanRadioBurst(uint8_t channelsToScan = 10) {
    if (!radioScanArmed) return;
    if (radioPaused) return;
    if (!beginRadios()) return;
    if (millis() - lastRadioMs < 6) return;
    lastRadioMs = millis();

    digitalWrite(CD_TFT_CS, HIGH);
    digitalWrite(CD_SD_CS, HIGH);
    digitalWrite(CD_NRF1_CSN, HIGH);
    digitalWrite(CD_NRF2_CSN, HIGH);

    RF24* activeRadios[2];
    uint8_t activeCount = 0;
    if (radio1Ready) activeRadios[activeCount++] = &radio1;
    if (radio2Ready) activeRadios[activeCount++] = &radio2;
    if (activeCount == 0) return;

    uint8_t scanned = 0;
    while (scanned < channelsToScan) {
        uint8_t channelForSlot[2] = {0, 0};
        uint8_t slotCount = 0;

        for (; slotCount < activeCount && scanned < channelsToScan; slotCount++, scanned++) {
            channelForSlot[slotCount] = radioScanChannel;
            activeRadios[slotCount]->setChannel(radioScanChannel);
            activeRadios[slotCount]->startListening();
            radioScanChannel = (radioScanChannel + 1) % 80;
        }

        delayMicroseconds(140);

        uint8_t hits[2] = {0, 0};
        for (uint8_t sample = 0; sample < 5; sample++) {
            for (uint8_t slot = 0; slot < slotCount; slot++) {
                if (activeRadios[slot]->testCarrier()) hits[slot]++;
            }
            delayMicroseconds(18);
        }

        for (uint8_t slot = 0; slot < slotCount; slot++) {
            activeRadios[slot]->stopListening();
            uint16_t& bar = radioBars[channelForSlot[slot]];
            const uint16_t target = min<uint16_t>(100, hits[slot] * 20);
            if (target > bar) {
                bar = (bar * 2 + target * 3) / 5;
            } else if (bar > 0) {
                bar -= max<uint16_t>(1, bar / 8);
            }
            radioScanTicks++;
        }
    }
}

void drawHome() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("CYBERDECK S3 APPS", "launcher");

    frame.setTextSize(1);
    frame.setTextColor(COL_CYAN, COL_BG);
    frame.drawString("Selecciona una herramienta para probar o grabar", 10, 34, 2);

    const int listY = 48;
    const int rowH = 33;
    const uint8_t visible = 4;
    if (menuIndex < menuScroll) menuScroll = menuIndex;
    if (menuIndex >= menuScroll + visible) menuScroll = menuIndex - visible + 1;

    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t idx = menuScroll + row;
        const int y = listY + row * rowH;
        if (idx >= MENU_COUNT) {
            frame.drawRect(10, y, 300, 30, 0x0821);
            continue;
        }

        const bool selected = idx == menuIndex;
        const uint16_t bg = selected ? COL_GREEN : COL_PANEL;
        const uint16_t fg = selected ? COL_BG : COL_TEXT;
        const uint16_t subColor = selected ? COL_BG : COL_MUTED;
        frame.fillRoundRect(10, y, 300, 29, 5, bg);
        frame.drawRoundRect(10, y, 300, 29, 5, selected ? COL_TEXT : COL_GRID);
        frame.fillRoundRect(16, y + 8, 16, 14, 4, selected ? COL_BG : 0x0184);
        drawTextOn(20, y + 9, String(idx + 1), selected ? COL_GREEN : COL_CYAN, selected ? COL_BG : 0x0184, 1);
        drawTextOn(40, y + 3, fitText(MENU[idx].title, 21), fg, bg, 1);
        drawTextOn(40, y + 17, fitText(MENU[idx].subtitle, 33), subColor, bg, 1);
        if (selected) {
            frame.fillTriangle(297, y + 14, 304, y + 9, 304, y + 19, COL_BG);
        }
    }

    drawMiniBadge(12, 187, 70, "BAT", String(batteryPct) + "%", batteryPct < 20 ? COL_RED : COL_GREEN);
    drawMiniBadge(90, 187, 70, "SD", sdReady ? "OK" : "WAIT", sdReady ? COL_CYAN : COL_AMBER);
    drawMiniBadge(168, 187, 70, "GPS", String(gps.satellites.value()), gps.location.isValid() ? COL_GREEN : COL_AMBER);
    drawMiniBadge(246, 187, 62, "APP", String(menuIndex + 1) + "/" + MENU_COUNT, COL_CYAN);
    drawFooter("ENC/UP/DOWN MOVE  OK OPEN  OK HOLD BACK");
}

void drawSystemPulse() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("SYSTEM PULSE", "hardware");

    drawBadge(10, 38, 72, "VBAT", String(batteryVolts, 2) + "V", batteryPct < 20 ? COL_RED : COL_GREEN);
    drawBadge(90, 38, 72, "SD", sdReady ? "MOUNT" : "NO CARD", sdReady ? COL_CYAN : COL_RED);
    drawBadge(170, 38, 65, "NRF1", radio1Ready ? "OK" : "MISS", radio1Ready ? COL_GREEN : COL_AMBER);
    drawBadge(243, 38, 65, "NRF2", radio2Ready ? "OK" : "MISS", radio2Ready ? COL_GREEN : COL_AMBER);

    frame.drawRoundRect(10, 88, 300, 92, 5, COL_GRID);
    drawText(20, 98, "BATTERY", COL_MUTED);
    drawBar(86, 101, 208, 12, batteryPct, batteryPct < 20 ? COL_RED : COL_GREEN);

    drawText(20, 124, "GPS CHARS", COL_MUTED);
    drawBar(102, 127, 192, 12, min<uint32_t>(gps.charsProcessed() / 8, 100), COL_CYAN);
    drawText(20, 150, "SATELLITES", COL_MUTED);
    drawBar(102, 153, 192, 12, min<uint32_t>(gps.satellites.value() * 8, 100), COL_AMBER);

    drawText(14, 188, String("Status: ") + statusLine, COL_CYAN);
    drawFooter("BACK EXIT  OK REFRESH  OK HOLD MENU");
}

void drawCompass(int cx, int cy, int r, float course, bool valid) {
    frame.drawCircle(cx, cy, r, COL_GRID);
    frame.drawCircle(cx, cy, r - 16, 0x03E0);
    drawText(cx - 6, cy - r + 4, "N", COL_GREEN);
    drawText(cx - 6, cy + r - 18, "S", COL_MUTED);
    drawText(cx - r + 6, cy - 8, "W", COL_MUTED);
    drawText(cx + r - 16, cy - 8, "E", COL_MUTED);

    if (!valid) {
        frame.drawLine(cx - 18, cy, cx + 18, cy, COL_RED);
        frame.drawLine(cx, cy - 18, cx, cy + 18, COL_RED);
        return;
    }

    const float rad = (course - 90.0f) * DEG_TO_RAD;
    const int nx = cx + cosf(rad) * (r - 24);
    const int ny = cy + sinf(rad) * (r - 24);
    frame.drawLine(cx, cy, nx, ny, COL_GREEN);
    frame.fillCircle(nx, ny, 4, COL_GREEN);
}

String fitGpsText(const String& text, uint8_t maxChars) {
    return fitText(text, maxChars);
}

String gpsCoordText(double value, const char* positive, const char* negative) {
    const char* hemi = value >= 0 ? positive : negative;
    return String(fabs(value), 6) + " " + hemi;
}

String gpsTimeText() {
    if (!gps.time.isValid()) return "--:--:-- UTC";
    char out[16];
    snprintf(out, sizeof(out), "%02d:%02d:%02d UTC", gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(out);
}

String gpsAltText() {
    if (!gps.altitude.isValid()) return "--";
    return String(gps.altitude.meters(), 1) + " m";
}

String gpsSpeedText() {
    if (!gps.speed.isValid()) return "--";
    return String(gps.speed.kmph(), 1) + " km/h";
}

String gpsCourseText() {
    if (!gps.course.isValid()) return "--";
    return String((int)roundf(gps.course.deg())) + " deg " + TinyGPSPlus::cardinal(gps.course.deg());
}

String gpsHdopText() {
    if (!gps.hdop.isValid()) return "--";
    return String(gps.hdop.hdop(), 2);
}

String gpsKmText(float km) {
    if (km < 10.0f) return String(km, 1) + " km";
    return String((int)roundf(km)) + " km";
}

float gpsDistanceKm(float lat1, float lng1, float lat2, float lng2) {
    const float dLat = (lat2 - lat1) * DEG_TO_RAD;
    const float dLng = (lng2 - lng1) * DEG_TO_RAD;
    const float rLat1 = lat1 * DEG_TO_RAD;
    const float rLat2 = lat2 * DEG_TO_RAD;
    const float sLat = sinf(dLat * 0.5f);
    const float sLng = sinf(dLng * 0.5f);
    const float a = (sLat * sLat) + cosf(rLat1) * cosf(rLat2) * (sLng * sLng);
    const float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    return 6371.0f * c;
}

String gpsSignedCoordText(double value) {
    return String(value, 6);
}

String gpsDmsText(double value, const char* positive, const char* negative) {
    const char* hemi = value >= 0 ? positive : negative;
    double v = fabs(value);
    const int deg = (int)v;
    v = (v - deg) * 60.0;
    const int min = (int)v;
    const double sec = (v - min) * 60.0;
    char out[28];
    snprintf(out, sizeof(out), "%d %02d %.2f %s", deg, min, sec, hemi);
    return String(out);
}

void gpsCopyField(char* dst, size_t dstSize, const String& src) {
    if (dstSize == 0) return;
    String clean = src;
    clean.trim();
    clean.replace("\"", "");
    clean.toCharArray(dst, dstSize);
    dst[dstSize - 1] = '\0';
}

void gpsSetResolvedPlace(GpsResolvedPlace& out, const char* city, const char* municipality,
                         const char* state, const char* country, float km, float radiusKm,
                         bool fromSd, uint32_t rowsScanned) {
    out.valid = true;
    out.close = km <= radiusKm;
    out.fromSd = fromSd;
    out.rowsScanned = rowsScanned;
    out.km = km;
    out.radiusKm = radiusKm;
    strlcpy(out.city, city ? city : "", sizeof(out.city));
    strlcpy(out.municipality, municipality ? municipality : "", sizeof(out.municipality));
    strlcpy(out.state, state ? state : "", sizeof(out.state));
    strlcpy(out.country, country ? country : "", sizeof(out.country));
}

bool gpsNextCsvField(const String& line, int& start, String& out) {
    if (start > (int)line.length()) return false;
    int comma = line.indexOf(',', start);
    if (comma < 0) {
        out = line.substring(start);
        start = line.length() + 1;
    } else {
        out = line.substring(start, comma);
        start = comma + 1;
    }
    out.trim();
    return true;
}

bool gpsParsePlaceCsvLine(const String& line, float& lat, float& lng,
                          String& city, String& municipality, String& state,
                          String& country, float& radiusKm) {
    if (line.length() < 8 || line[0] == '#') return false;
    String fields[7];
    int start = 0;
    for (uint8_t i = 0; i < 7; i++) {
        if (!gpsNextCsvField(line, start, fields[i])) return false;
    }
    if (fields[0].equalsIgnoreCase("lat")) return false;
    lat = fields[0].toFloat();
    lng = fields[1].toFloat();
    city = fields[2];
    municipality = fields[3].length() ? fields[3] : fields[2];
    state = fields[4];
    country = fields[5];
    radiusKm = fields[6].length() ? fields[6].toFloat() : 25.0f;
    if (radiusKm < 1.0f) radiusKm = 1.0f;
    return city.length() > 0 && (lat != 0.0f || lng != 0.0f);
}

void gpsWriteSamplePlaceDb() {
    if (!beginSdCard()) return;
    SD.mkdir("/APPS");
    SD.mkdir("/APPS/GPS");
    if (SD.exists(GPS_PLACE_DB_PATH)) return;

    String sample;
    sample += "lat,lng,city,municipality,state,country,radius_km\r\n";
    sample += "28.6353,-106.0889,Chihuahua,Chihuahua,Chihuahua,Mexico,65\r\n";
    sample += "31.6904,-106.4245,Ciudad Juarez,Juarez,Chihuahua,Mexico,70\r\n";
    sample += "40.4168,-3.7038,Madrid,Madrid,Comunidad de Madrid,Espana,45\r\n";
    sample += "41.3874,2.1686,Barcelona,Barcelona,Catalunya,Espana,35\r\n";
    writeTextFile(GPS_PLACE_DB_PATH, sample);
}

bool gpsScanSdPlaceDb(float lat, float lng, GpsResolvedPlace& best) {
    if (!beginSdCard()) return false;
    gpsWriteSamplePlaceDb();

    File f = SD.open(GPS_PLACE_DB_PATH, FILE_READ);
    if (!f) return false;

    uint32_t rows = 0;
    bool found = false;
    while (f.available() && rows < GPS_PLACE_MAX_ROWS) {
        String line = f.readStringUntil('\n');
        line.trim();
        float pLat = 0.0f;
        float pLng = 0.0f;
        float radiusKm = 25.0f;
        String city;
        String municipality;
        String state;
        String country;
        if (!gpsParsePlaceCsvLine(line, pLat, pLng, city, municipality, state, country, radiusKm)) continue;
        rows++;
        const float km = gpsDistanceKm(lat, lng, pLat, pLng);
        if (!best.valid || km < best.km) {
            char cityBuf[34];
            char municipalityBuf[34];
            char stateBuf[34];
            char countryBuf[18];
            gpsCopyField(cityBuf, sizeof(cityBuf), city);
            gpsCopyField(municipalityBuf, sizeof(municipalityBuf), municipality);
            gpsCopyField(stateBuf, sizeof(stateBuf), state);
            gpsCopyField(countryBuf, sizeof(countryBuf), country);
            gpsSetResolvedPlace(best, cityBuf, municipalityBuf, stateBuf, countryBuf, km, radiusKm, true, rows);
            found = true;
        }
    }
    f.close();
    if (found) best.rowsScanned = rows;
    return found;
}

GpsPlaceMatch resolveGpsPlace() {
    GpsPlaceMatch best = {nullptr, 99999.0f, false};
    if (!gps.location.isValid()) return best;

    const float lat = gps.location.lat();
    const float lng = gps.location.lng();
    for (const GpsPlace& place : GPS_PLACES) {
        const float km = gpsDistanceKm(lat, lng, place.lat, place.lng);
        if (km < best.km) {
            best.place = &place;
            best.km = km;
        }
    }
    best.close = best.place != nullptr && best.km <= best.place->closeKm;
    return best;
}

void updateGpsResolvedPlace(bool force = false) {
    if (!gps.location.isValid()) {
        gpsResolvedPlace = {};
        return;
    }

    const float lat = gps.location.lat();
    const float lng = gps.location.lng();
    const uint32_t now = millis();
    const bool hasLast = gpsPlaceLastScanLat < 900.0 && gpsPlaceLastScanLng < 900.0;
    const float movedKm = hasLast ? gpsDistanceKm(lat, lng, gpsPlaceLastScanLat, gpsPlaceLastScanLng) : 99999.0f;
    if (!force && gpsResolvedPlace.valid && (now - gpsPlaceLastScanMs) < GPS_PLACE_RESCAN_MS && movedKm < GPS_PLACE_RESCAN_KM) {
        return;
    }

    GpsResolvedPlace best = {};
    const GpsPlaceMatch builtIn = resolveGpsPlace();
    if (builtIn.place) {
        gpsSetResolvedPlace(best, builtIn.place->city, builtIn.place->municipality,
                            builtIn.place->state, "MX", builtIn.km, builtIn.place->closeKm,
                            false, 0);
    }
    gpsScanSdPlaceDb(lat, lng, best);

    gpsResolvedPlace = best;
    gpsPlaceLastScanMs = now;
    gpsPlaceLastScanLat = lat;
    gpsPlaceLastScanLng = lng;
}

String gpsPlaceSourceText() {
    if (!gpsResolvedPlace.valid) return "SIN BASE";
    return gpsResolvedPlace.fromSd ? "SD DB" : "BASE INT";
}

void drawGpsPanel(int x, int y, int w, int h, const char* title) {
    frame.drawRoundRect(x, y, w, h, 5, COL_GRID);
    frame.setTextSize(1);
    frame.setTextDatum(TL_DATUM);
    frame.setTextColor(COL_GREEN, COL_BG);
    frame.drawString(title, x + 8, y + 5, 1);
}

void drawGpsLine(int x, int y, const char* label, const String& value, uint16_t color = COL_TEXT, uint8_t maxChars = 18) {
    frame.setTextDatum(TL_DATUM);
    frame.setTextSize(1);
    frame.setTextColor(COL_MUTED, COL_BG);
    frame.drawString(label, x, y + 4, 1);
    frame.setTextColor(color, COL_BG);
    frame.drawString(fitGpsText(value, maxChars), x + 42, y, 2);
}

void saveGpsSnapshot() {
    if (beginSdCard()) {
        SD.mkdir("/APPS");
        SD.mkdir("/APPS/GPS");
    }

    String out;
    updateGpsResolvedPlace(true);
    out += "CYBERDECK S3 APPS GPS SNAPSHOT\r\n";
    out += "Fix: " + String(gps.location.isValid() ? "YES" : "NO") + "\r\n";
    out += "Satellites: " + String(gps.satellites.value()) + "\r\n";
    out += "HDOP: " + String(gps.hdop.isValid() ? gps.hdop.hdop() : 0.0, 2) + "\r\n";
    if (gps.location.isValid()) {
        out += "Lat: " + String(gps.location.lat(), 6) + "\r\n";
        out += "Lng: " + String(gps.location.lng(), 6) + "\r\n";
        out += "Lat DMS: " + gpsDmsText(gps.location.lat(), "N", "S") + "\r\n";
        out += "Lng DMS: " + gpsDmsText(gps.location.lng(), "E", "W") + "\r\n";
        out += "Altitude: " + gpsAltText() + "\r\n";
        out += "Speed: " + gpsSpeedText() + "\r\n";
        out += "Course: " + gpsCourseText() + "\r\n";
        out += "UTC: " + gpsTimeText() + "\r\n";
        out += "Maps: https://maps.google.com/?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) + "\r\n";
    }
    if (gpsResolvedPlace.valid) {
        out += "Nearest city: " + String(gpsResolvedPlace.city) + "\r\n";
        out += "Municipality: " + String(gpsResolvedPlace.municipality) + "\r\n";
        out += "State: " + String(gpsResolvedPlace.state) + "\r\n";
        out += "Country: " + String(gpsResolvedPlace.country) + "\r\n";
        out += "Distance: " + gpsKmText(gpsResolvedPlace.km) + "\r\n";
        out += "Close match: " + String(gpsResolvedPlace.close ? "YES" : "NO") + "\r\n";
        out += "Place source: " + gpsPlaceSourceText() + "\r\n";
        out += "Rows scanned: " + String(gpsResolvedPlace.rowsScanned) + "\r\n";
    }
    out += "Battery: " + String(batteryVolts, 2) + "V\r\n";

    if (writeTextFile("/APPS/GPS_SNAPSHOT.txt", out) && writeTextFile("/APPS/GPS/GPS_SNAPSHOT.txt", out)) {
        setStatus("GPS snapshot saved");
    } else {
        setStatus("GPS save failed");
    }
}

void drawGpsRadar() {
    frame.fillSprite(COL_BG);
    drawGrid();
    const bool nmeaLive = gps.charsProcessed() > 0 && (millis() - gpsLastCharMs) < 2500;
    const bool hasFix = gps.location.isValid();
    updateGpsResolvedPlace(false);
    drawHeader("GPS RESCUE", hasFix ? "coord real" : (nmeaLive ? "nmea activo" : "sin rx"));

    drawGpsPanel(8, 36, 84, 124, "SENAL");
    drawCompass(50, 88, 28, gps.course.deg(), gps.course.isValid());
    drawText(18, 122, String("SAT ") + gps.satellites.value(), hasFix ? COL_GREEN : COL_AMBER);
    drawText(18, 138, String("HDOP ") + gpsHdopText(), gps.hdop.isValid() ? COL_CYAN : COL_MUTED);

    drawGpsPanel(100, 36, 212, 124, "COORDENADAS EMERGENCIA");

    if (hasFix) {
        drawGpsLine(110, 54, "LAT", gpsSignedCoordText(gps.location.lat()), COL_GREEN, 20);
        drawGpsLine(110, 74, "LON", gpsSignedCoordText(gps.location.lng()), COL_GREEN, 20);
        drawGpsLine(110, 94, "ALT", gpsAltText(), gps.altitude.isValid() ? COL_CYAN : COL_MUTED, 20);
        drawGpsLine(110, 114, "DMS", gpsDmsText(gps.location.lat(), "N", "S"), COL_AMBER, 20);
        drawGpsLine(110, 134, "UTC", gpsTimeText(), gps.time.isValid() ? COL_TEXT : COL_MUTED, 20);
    } else if (!nmeaLive) {
        drawGpsLine(110, 58, "GPS", "NO NMEA on RX18", COL_RED, 20);
        drawGpsLine(110, 82, "PIN", "TX GPS -> GPIO18", COL_AMBER, 20);
        drawGpsLine(110, 106, "UART", "RX18 TX17 9600", COL_CYAN, 20);
        drawGpsLine(110, 130, "RX", String(gpsCharsPerSec) + "/s", COL_MUTED, 20);
    } else {
        drawGpsLine(110, 58, "GPS", "NMEA OK", COL_GREEN, 20);
        drawGpsLine(110, 82, "FIX", "Busca cielo abierto", COL_AMBER, 20);
        drawGpsLine(110, 106, "RX", String(gpsCharsPerSec) + "/s", COL_CYAN, 20);
        drawGpsLine(110, 130, "CHK", String(gps.passedChecksum()) + "/" + gps.failedChecksum(), COL_MUTED, 20);
    }

    drawGpsPanel(8, 166, 304, 46, "LUGAR ESTIMADO");
    if (hasFix && gpsResolvedPlace.valid) {
        const String cityLine = String("Ciudad: ") + gpsResolvedPlace.city + "  Mun: " + gpsResolvedPlace.municipality;
        const String stateLine = String(gpsResolvedPlace.close ? "Zona: " : "Cerca: ") + gpsResolvedPlace.state + " " + gpsResolvedPlace.country + " " + gpsKmText(gpsResolvedPlace.km);
        drawText(18, 182, fitGpsText(cityLine, 38), gpsResolvedPlace.close ? COL_GREEN : COL_AMBER);
        drawText(18, 198, fitGpsText(stateLine + " " + gpsPlaceSourceText(), 38), COL_CYAN);
    } else if (nmeaLive) {
        drawText(18, 184, "GPS conectado. Esperando fix real...", COL_AMBER);
        drawText(18, 200, String("RX ") + gpsCharsPerSec + "/s  OK/Bad " + gps.passedChecksum() + "/" + gps.failedChecksum(), COL_CYAN);
    } else {
        drawText(18, 184, "Sin datos NMEA. Revisa TX GPS -> RX18.", COL_RED);
        drawText(18, 200, String("Chars ") + gps.charsProcessed() + "  Age " + ((millis() - gpsLastCharMs) / 1000) + "s", COL_MUTED);
    }
    drawFooter("OK SNAPSHOT  UP/DOWN ACTUALIZAR LUGAR  BACK SALIR");
}

void runGpsRadarApp() {
    currentScreen = Screen::GpsRadar;
    prepareGpsMode();
    gpsLastUiMs = 0;
    drawGpsRadar();
    pushFrame();

    while (true) {
        serviceGps(28);

        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            toneClick(1600, 10);
            drawHome();
            pushFrame();
            return;
        }
        if (action == AppAction::Select) {
            saveGpsSnapshot();
            toneClick(3000, 18);
            drawGpsRadar();
            pushFrame();
        } else if (action == AppAction::Up || action == AppAction::Down) {
            updateGpsResolvedPlace(true);
            setStatus("GPS place refreshed");
            toneClick(2200, 10);
            drawGpsRadar();
            pushFrame();
        }

        if (millis() - gpsLastUiMs >= 220) {
            gpsLastUiMs = millis();
            drawGpsRadar();
            pushFrame();
        }
        delay(2);
    }
}

bool saveGpsSosSnapshot() {
    if (beginSdCard()) {
        SD.mkdir("/APPS");
        SD.mkdir("/APPS/GPS");
    }
    updateGpsResolvedPlace(true);

    String out;
    out += "CYBERDECK GPS SOS SNAPSHOT\r\n";
    out += "MENSAJE RAPIDO:\r\n";
    if (gps.location.isValid()) {
        out += "Necesito ayuda. Mi ubicacion GPS es " + String(gps.location.lat(), 6) + ", " + String(gps.location.lng(), 6) + ".\r\n";
        out += "Mapa: https://maps.google.com/?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) + "\r\n";
    } else {
        out += "Sin fix GPS todavia. Usar datos de senal abajo para diagnostico.\r\n";
    }
    out += "\r\nDATOS TECNICOS:\r\n";
    out += "Fix: " + String(gps.location.isValid() ? "YES" : "NO") + "\r\n";
    out += "UTC: " + gpsTimeText() + "\r\n";
    out += "Battery: " + String(batteryVolts, 2) + "V (" + String(batteryPct) + "%)\r\n";
    out += "Satellites: " + String(gps.satellites.value()) + "\r\n";
    out += "HDOP: " + gpsHdopText() + "\r\n";
    out += "RX chars/s: " + String(gpsCharsPerSec) + "\r\n";
    out += "NMEA chars: " + String(gps.charsProcessed()) + "\r\n";
    if (gps.location.isValid()) {
        out += "LAT: " + String(gps.location.lat(), 6) + "\r\n";
        out += "LON: " + String(gps.location.lng(), 6) + "\r\n";
        out += "LAT DMS: " + gpsDmsText(gps.location.lat(), "N", "S") + "\r\n";
        out += "LON DMS: " + gpsDmsText(gps.location.lng(), "E", "W") + "\r\n";
        out += "ALT: " + gpsAltText() + "\r\n";
        out += "Speed: " + gpsSpeedText() + "\r\n";
        out += "Course: " + gpsCourseText() + "\r\n";
        out += "MAPS: https://maps.google.com/?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) + "\r\n";
    }
    if (gpsResolvedPlace.valid) {
        out += "Nearest: " + String(gpsResolvedPlace.city) + ", " + gpsResolvedPlace.state + ", " + gpsResolvedPlace.country + "\r\n";
        out += "Municipality: " + String(gpsResolvedPlace.municipality) + "\r\n";
        out += "Distance: " + gpsKmText(gpsResolvedPlace.km) + "\r\n";
        out += "Place source: " + gpsPlaceSourceText() + "\r\n";
        out += "Rows scanned: " + String(gpsResolvedPlace.rowsScanned) + "\r\n";
    }
    out += "\r\n";

    const bool ok1 = writeTextFile("/APPS/SOS_LAST.txt", out);
    const bool ok2 = writeTextFile("/APPS/GPS/SOS_LAST.txt", out);
    const bool ok3 = appendTextFile("/APPS/GPS/SOS_LOG.txt", String("---- SOS SNAPSHOT ----\r\n") + out);
    setStatus((ok1 || ok2 || ok3) ? "SOS snapshot saved" : "SOS save failed");
    return ok1 || ok2 || ok3;
}

String gpsSosStatusText(bool hasFix, bool nmeaLive) {
    if (hasFix) return "FIX OK";
    if (nmeaLive) return "BUSCANDO";
    return "SIN RX";
}

uint16_t gpsSosStatusColor(bool hasFix, bool nmeaLive) {
    if (hasFix) return COL_GREEN;
    if (nmeaLive) return COL_AMBER;
    return COL_RED;
}

void drawGpsSos() {
    frame.fillSprite(COL_BG);
    drawGrid();
    const bool nmeaLive = gps.charsProcessed() > 0 && (millis() - gpsLastCharMs) < 2500;
    const bool hasFix = gps.location.isValid();
    updateGpsResolvedPlace(false);
    const uint16_t statusColor = gpsSosStatusColor(hasFix, nmeaLive);
    drawHeader("GPS SOS MODE", gpsSosStatusText(hasFix, nmeaLive).c_str());

    frame.drawRoundRect(8, 34, 304, 26, 5, statusColor);
    frame.fillRect(12, 38, 296, 18, 0x0004);
    drawTextOn(18, 40, "SOS 112 / 911", statusColor, 0x0004);
    frame.setTextDatum(TR_DATUM);
    frame.setTextColor(statusColor, 0x0004);
    frame.setTextSize(1);
    frame.drawString(gpsSosStatusText(hasFix, nmeaLive), 302, 40, 2);
    frame.setTextDatum(TL_DATUM);

    frame.drawRoundRect(8, 66, 304, 82, 5, statusColor);
    frame.fillRect(14, 72, 292, 70, 0x0004);
    frame.setTextDatum(TL_DATUM);
    frame.setTextSize(1);
    if (hasFix) {
        frame.setTextColor(COL_GREEN, 0x0004);
        frame.drawString("LAT", 22, 76, 2);
        frame.setTextColor(COL_TEXT, 0x0004);
        frame.drawString(String(gps.location.lat(), 6), 72, 72, 4);
        frame.setTextColor(COL_GREEN, 0x0004);
        frame.drawString("LON", 22, 108, 2);
        frame.setTextColor(COL_TEXT, 0x0004);
        frame.drawString(String(gps.location.lng(), 6), 72, 104, 4);
        drawTextOn(22, 132, fitText(String("ALT ") + gpsAltText() + "  UTC " + gpsTimeText(), 37), COL_CYAN, 0x0004);
    } else {
        drawTextOn(24, 82, nmeaLive ? "GPS conectado, esperando fix real." : "Sin datos GPS en RX18.", nmeaLive ? COL_AMBER : COL_RED, 0x0004);
        drawTextOn(24, 106, "Sal a cielo abierto y deja antena visible.", COL_CYAN, 0x0004);
        drawTextOn(24, 128, String("Chars ") + gps.charsProcessed() + "  RX " + gpsCharsPerSec + "/s", COL_MUTED, 0x0004);
    }

    frame.drawRoundRect(8, 156, 148, 56, 5, COL_GRID);
    drawText(18, 166, "SENAL GPS", COL_MUTED);
    drawText(18, 184, String("SAT ") + gps.satellites.value() + "  HDOP " + gpsHdopText(), hasFix ? COL_GREEN : COL_AMBER);
    drawText(18, 198, String("BAT ") + batteryPct + "% " + String(batteryVolts, 2) + "V", batteryPct < 20 ? COL_RED : COL_CYAN);

    frame.drawRoundRect(164, 156, 148, 56, 5, COL_GRID);
    drawText(174, 166, "LUGAR ESTIMADO", COL_MUTED);
    if (gpsResolvedPlace.valid) {
        drawText(174, 184, fitText(gpsResolvedPlace.city, 16), gpsResolvedPlace.close ? COL_GREEN : COL_AMBER);
        drawText(174, 198, fitText(String(gpsResolvedPlace.state) + " " + gpsKmText(gpsResolvedPlace.km), 18), COL_CYAN);
    } else {
        drawText(174, 184, "Lugar offline", COL_MUTED);
        drawText(174, 198, hasFix ? "No resuelto" : "Sin fix aun", COL_AMBER);
    }

    drawFooter("OK GUARDA SOS SD  BACK SALIR");
}

void runGpsSosApp() {
    currentScreen = Screen::GpsSos;
    prepareGpsMode();
    uint32_t lastDraw = 0;
    drawGpsSos();
    pushFrame();

    while (true) {
        serviceGps(30);
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }
        if (action == AppAction::Select) {
            saveGpsSosSnapshot();
            toneClick(3800, 35);
            drawGpsSos();
            pushFrame();
        }
        if (millis() - lastDraw > 240) {
            lastDraw = millis();
            drawGpsSos();
            pushFrame();
        }
        delay(2);
    }
}

String sdTypeText() {
    if (!beginSdCard()) return "NO CARD";
    switch (SD.cardType()) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC";
        default: return "UNKNOWN";
    }
}

void drawSdVault() {
    uint16_t dirs = 0;
    uint16_t files = 0;
    countRootFiles(dirs, files);

    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("SD VAULT", sdReady ? "mounted" : "offline");

    frame.drawRoundRect(10, 42, 300, 120, 5, sdReady ? COL_CYAN : COL_RED);
    drawText(22, 54, String("Card: ") + sdTypeText(), sdReady ? COL_GREEN : COL_RED);
    drawText(22, 76, String("SPI: ") + String((int)CD_SD_SCK) + "/" + String((int)CD_SD_MOSI) + "/" + String((int)CD_SD_MISO) + " CS" + String((int)CD_SD_CS), COL_MUTED);
    drawText(22, 98, String("Speed: ") + String(sdMountHz / 1000000) + " MHz", COL_MUTED);
    drawText(22, 120, String("Root dirs/files: ") + String(dirs) + "/" + String(files), COL_CYAN);
    drawText(22, 142, "OK creates /APPS and writes APP_TEST.txt", COL_AMBER);

    drawText(12, 188, String("Status: ") + statusLine, COL_CYAN);
    drawFooter("OK TEST WRITE  BACK EXIT");
}

void drawRadioScope() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("RADIO SCOPE", !radioScanArmed ? "ready" : (radioPaused ? "paused" : "passive"));

    frame.drawRoundRect(10, 42, 300, 132, 5, COL_GRID);
    const int plotX = 18;
    const int plotY = 58;
    const int plotW = 284;
    const int plotH = 96;
    frame.drawRect(plotX, plotY, plotW, plotH, 0x02A0);

    uint16_t peak = 0;
    uint8_t peakCh = 0;
    for (uint8_t ch = 0; ch < 80; ch++) {
        if (radioBars[ch] > peak) {
            peak = radioBars[ch];
            peakCh = ch;
        }
        const int x = plotX + 2 + (ch * (plotW - 4)) / 80;
        const int h = map(radioBars[ch], 0, 100, 1, plotH - 5);
        const uint16_t color = radioBars[ch] > 65 ? COL_RED : (radioBars[ch] > 32 ? COL_AMBER : COL_GREEN);
        frame.drawFastVLine(x, plotY + plotH - 2 - h, h, color);
    }

    drawText(18, 164, String("NRF1 ") + (radio1Ready ? "OK" : "MISS") + "  NRF2 " + (radio2Ready ? "OK" : "MISS"), COL_CYAN);
    drawText(18, 184, String("Peak CH ") + peakCh + "  Energy " + peak + "%", peak > 65 ? COL_RED : COL_GREEN);
    drawText(180, 184, radioPaused ? "OK resume" : "OK pause", COL_AMBER);
    drawText(18, 202, String("Samples ") + radioScanTicks + "  CH " + radioScanChannel, COL_MUTED);
    drawFooter("OK PAUSE/RESUME  BACK EXIT");
}

void drawRadioDirectText(int x, int y, const String& text, uint16_t color, uint16_t bg = COL_BG) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(color, bg);
    tft.drawString(text, x, y, 2);
}

void drawRadioScopeDirectFrame() {
    restoreTftBus();
    tft.fillScreen(COL_BG);

    for (int x = 0; x < SCREEN_W; x += 16) {
        tft.drawFastVLine(x, 0, SCREEN_H, (x % 64 == 0) ? 0x0320 : 0x0104);
    }
    for (int y = 0; y < SCREEN_H; y += 16) {
        tft.drawFastHLine(0, y, SCREEN_W, (y % 64 == 0) ? 0x0320 : 0x0104);
    }

    tft.fillRect(0, 0, SCREEN_W, 28, 0x0184);
    tft.drawFastHLine(0, 28, SCREEN_W, COL_GREEN);
    tft.fillCircle(10, 14, 4, COL_GREEN);
    drawRadioDirectText(20, 6, "RADIO SCOPE", COL_GREEN, 0x0184);

    tft.drawRoundRect(10, 42, 300, 132, 5, COL_GRID);
    tft.drawRect(18, 58, 284, 96, 0x02A0);

    tft.fillRect(0, 218, SCREEN_W, 22, 0x0184);
    tft.drawFastHLine(0, 217, SCREEN_W, COL_GRID);
}

void drawRadioScopeDirectDynamic() {
    restoreTftBus();

    const int plotX = 18;
    const int plotY = 58;
    const int plotW = 284;
    const int plotH = 96;

    tft.fillRect(218, 4, 96, 20, 0x0184);
    tft.setTextDatum(TR_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(COL_CYAN, 0x0184);
    tft.drawString(radioPaused ? "paused" : "live", 314, 6, 2);
    tft.setTextDatum(TL_DATUM);

    tft.fillRect(plotX + 1, plotY + 1, plotW - 2, plotH - 2, COL_BG);
    for (int i = 1; i < 4; i++) {
        int y = plotY + (plotH * i) / 4;
        for (int x = plotX + 2; x < plotX + plotW - 2; x += 6) {
            tft.drawPixel(x, y, COL_GRID);
        }
    }

    uint16_t peak = 0;
    uint8_t peakCh = 0;
    for (uint8_t ch = 0; ch < 80; ch++) {
        if (radioBars[ch] > peak) {
            peak = radioBars[ch];
            peakCh = ch;
        }
        const int x = plotX + 2 + (ch * (plotW - 4)) / 80;
        const int h = map(radioBars[ch], 0, 100, 1, plotH - 5);
        const uint16_t color = radioBars[ch] > 65 ? COL_RED : (radioBars[ch] > 32 ? COL_AMBER : COL_GREEN);
        tft.drawFastVLine(x, plotY + plotH - 2 - h, h, color);
    }

    tft.fillRect(14, 160, 294, 54, COL_BG);
    drawRadioDirectText(18, 164, String("NRF1 ") + (radio1Ready ? "OK" : "MISS") + "  NRF2 " + (radio2Ready ? "OK" : "MISS"), COL_CYAN);
    drawRadioDirectText(18, 184, String("Peak CH ") + peakCh + "  Energy " + peak + "%", peak > 65 ? COL_RED : COL_GREEN);
    drawRadioDirectText(180, 184, radioPaused ? "OK resume" : "OK pause", COL_AMBER);
    drawRadioDirectText(18, 202, String("Samples ") + radioScanTicks + "  CH " + radioScanChannel, COL_MUTED);

    tft.fillRect(0, 218, SCREEN_W, 22, 0x0184);
    tft.drawFastHLine(0, 217, SCREEN_W, COL_GRID);
    drawRadioDirectText(8, 221, "OK PAUSE/RESUME  BACK EXIT", COL_MUTED, 0x0184);
}

void runRadioScopeApp() {
    currentScreen = Screen::RadioScope;
    radioScanArmed = true;
    radioPaused = false;
    memset(radioBars, 0, sizeof(radioBars));
    radioScanChannel = 0;
    radioScanTicks = 0;
    lastRadioMs = 0;
    setStatus(beginRadios() ? "Radio scope running" : "NRF not detected");

    drawRadioScopeDirectFrame();
    drawRadioScopeDirectDynamic();

    uint32_t lastDraw = 0;
    bool running = true;
    while (running) {
        AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) {
            running = false;
            break;
        }

        if (action == AppAction::Select) {
            radioPaused = !radioPaused;
            setStatus(radioPaused ? "Radio scope paused" : "Radio scope running");
            toneClick(2200, 12);
            drawRadioScopeDirectDynamic();
        }

        if (!radioPaused) {
            scanRadioBurst(18);
        }

        uint32_t now = millis();
        if (now - lastDraw >= 55) {
            lastDraw = now;
            drawRadioScopeDirectDynamic();
        }

        delay(2);
    }

    if (radio1Ready) radio1.stopListening();
    if (radio2Ready) radio2.stopListening();
    digitalWrite(CD_NRF1_CE, LOW);
    digitalWrite(CD_NRF2_CE, LOW);
    digitalWrite(CD_NRF1_CSN, HIGH);
    digitalWrite(CD_NRF2_CSN, HIGH);

    radioScanArmed = false;
    radioPaused = false;
    currentScreen = Screen::Home;
    setStatus("Ready");
    toneClick(1600, 10);
    restoreTftBus();
    drawHome();
    pushFrame();
}

String wifiSsidText(const char* ssid, bool hidden) {
    if (hidden || ssid[0] == '\0') return "<oculta>";
    return String(ssid);
}

String wifiEncryptionText(uint8_t enc) {
    switch (enc) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
        default: return "LOCK";
    }
}

uint8_t wifiRssiPct(int32_t rssi) {
    if (rssi <= -95) return 0;
    if (rssi >= -35) return 100;
    return (uint8_t)map(rssi, -95, -35, 0, 100);
}

uint16_t wifiEstimateMeters(int32_t rssi) {
    if (rssi < -120) return 0;
    const float rssiAtOneMeter = -45.0f;
    const float pathLoss = 2.2f;
    float meters = powf(10.0f, (rssiAtOneMeter - (float)rssi) / (10.0f * pathLoss));
    if (meters < 1.0f) meters = 1.0f;
    if (meters > 99.0f) meters = 99.0f;
    return (uint16_t)roundf(meters);
}

String wifiMetersText() {
    if (!wifiTargetSeen) return "-- m";
    return String(wifiEstimateMeters(wifiTargetRssi)) + " m";
}

String wifiTrendText() {
    if (!wifiTargetSeen) return "BUSCANDO";
    if (wifiTrendDb >= 4) return "ACERCANDOTE";
    if (wifiTrendDb <= -4) return "ALEJANDOTE";
    return "ESTABLE";
}

uint16_t wifiTrendColor() {
    if (!wifiTargetSeen) return COL_RED;
    if (wifiTrendDb >= 4) return COL_GREEN;
    if (wifiTrendDb <= -4) return COL_AMBER;
    return COL_CYAN;
}

void wifiPrepareMode() {
    quietRadiosForGps();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    delay(80);
}

void wifiResetTargetHistory() {
    wifiTargetSeen = false;
    wifiTargetRssi = -127;
    wifiLastTargetRssi = -127;
    wifiBestRssi = -127;
    wifiTrendDb = 0;
    wifiHistoryHead = 0;
    memset(wifiHistory, 0, sizeof(wifiHistory));
    wifiScanPass = 0;
    wifiLastTargetScanMs = 0;
    wifiAsyncScanActive = false;
    wifiAsyncScanStartedMs = 0;
}

void wifiRememberSample(int32_t rssi) {
    wifiHistory[wifiHistoryHead] = (int16_t)rssi;
    wifiHistoryHead = (wifiHistoryHead + 1) % WIFI_HISTORY_LEN;
}

void drawWifiScanning(const char* line) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("WIFI LOCATOR", "scan");
    frame.drawRoundRect(18, 50, 284, 116, 5, COL_CYAN);
    drawText(34, 72, line, COL_CYAN);
    drawText(34, 98, "Modo pasivo: no conecta, no ataca.", COL_MUTED);
    drawText(34, 124, "Usa redes propias o autorizadas.", COL_AMBER);
    drawFooter("BACK SALIR");
    pushFrame();
}

void drawWifiDirectionPrompt(const char* sector) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("WIFI DIR SCAN", "sector");
    frame.drawRoundRect(18, 42, 284, 142, 5, COL_CYAN);
    drawText(34, 60, String("Apunta hacia: ") + sector, COL_GREEN);
    drawText(34, 88, "Manten el equipo quieto.", COL_CYAN);
    drawText(34, 112, "OK mide este sector.", COL_TEXT);
    drawText(34, 136, "BACK cancela al radar RSSI.", COL_AMBER);
    drawText(34, 160, fitGpsText(wifiSsidText(wifiTargetSsid, wifiTargetSsid[0] == '\0'), 28), COL_MUTED);
    drawFooter("OK MEDIR  BACK RADAR");
    pushFrame();
}

bool ensureBleStackReady(const char* line = "Preparando escaner Bluetooth...") {
    if (bleStackReady) return true;
    releaseFrameForBleStartup();
    drawDirectStatus("INICIANDO BLE", line, COL_AMBER);
    releaseClassicBtMemoryOnce();
    BLEDevice::init("CYBERDECK-REMOTE");
    bleStackReady = true;
    return recreateFrameAfterBleStartup();
}

void drawWifiDirectionMeasure(const char* sector, uint8_t sample, uint8_t total) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("WIFI DIR SCAN", "midiendo");
    frame.drawRoundRect(18, 52, 284, 112, 5, COL_GREEN);
    drawText(34, 74, String("Midiendo ") + sector, COL_CYAN);
    drawText(34, 100, String("Muestra ") + sample + "/" + total, COL_TEXT);
    drawBar(34, 128, 252, 12, (sample * 100) / total, COL_GREEN);
    drawFooter("NO MOVER EL EQUIPO");
    pushFrame();
}

void wifiScanList() {
    wifiPrepareMode();
    drawWifiScanning("Escaneando redes WiFi...");
    int n = WiFi.scanNetworks(false, true, false, 160);
    if (n < 0) n = 0;
    wifiApCount = min<uint8_t>((uint8_t)n, WIFI_MAX_APS);
    for (uint8_t i = 0; i < wifiApCount; i++) {
        String ssid = WiFi.SSID(i);
        String bssid = WiFi.BSSIDstr(i);
        ssid.toCharArray(wifiAps[i].ssid, sizeof(wifiAps[i].ssid));
        bssid.toCharArray(wifiAps[i].bssid, sizeof(wifiAps[i].bssid));
        wifiAps[i].rssi = WiFi.RSSI(i);
        wifiAps[i].channel = WiFi.channel(i);
        wifiAps[i].encryption = WiFi.encryptionType(i);
        wifiAps[i].hidden = ssid.length() == 0;
    }
    WiFi.scanDelete();
    wifiListSelected = 0;
    wifiListScroll = 0;
    setStatus(wifiApCount ? "WiFi scan ready" : "No WiFi networks");
}

void drawWifiList() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("WIFI LOCATOR", wifiApCount ? "elige red" : "sin redes");
    drawText(12, 35, String("Redes: ") + wifiApCount + "  OK radar  UP/DOWN mover", COL_CYAN);

    const int listY = 56;
    const int rowH = 25;
    const uint8_t visible = 6;
    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t idx = wifiListScroll + row;
        if (idx >= wifiApCount) break;
        const int y = listY + row * rowH;
        const bool selected = idx == wifiListSelected;
        const uint16_t bg = selected ? COL_GREEN : COL_PANEL;
        const uint16_t fg = selected ? COL_BG : COL_TEXT;
        frame.fillRoundRect(10, y, 300, 21, 4, bg);
        frame.drawRoundRect(10, y, 300, 21, 4, selected ? COL_TEXT : COL_GRID);
        drawTextOn(16, y + 2, fitGpsText(wifiSsidText(wifiAps[idx].ssid, wifiAps[idx].hidden), 18), fg, bg, 1);
        frame.setTextDatum(TR_DATUM);
        frame.setTextColor(selected ? COL_BG : (wifiAps[idx].rssi > -60 ? COL_GREEN : COL_AMBER), bg);
        frame.drawString(String(wifiAps[idx].rssi) + "dBm CH" + wifiAps[idx].channel, 303, y + 2, 2);
        frame.setTextDatum(TL_DATUM);
    }

    if (!wifiApCount) {
        drawText(28, 92, "No se detectaron redes.", COL_AMBER);
        drawText(28, 116, "OK vuelve a escanear.", COL_CYAN);
    } else {
        const WifiApInfo& ap = wifiAps[wifiListSelected];
        drawText(14, 210, fitGpsText(String(ap.bssid) + "  " + wifiEncryptionText(ap.encryption), 38), COL_MUTED);
    }
    drawFooter("OK RADAR  UP/DOWN MOVER  BACK SALIR");
    pushFrame();
}

bool wifiScanTargetOnce(int32_t& rssiOut) {
    if (!wifiTargetValid) return false;
    wifiPrepareMode();
    int n = WiFi.scanNetworks(false, true, false, 95, wifiTargetChannel);
    if (n < 0) n = 0;
    bool found = false;
    int32_t best = -127;
    for (int i = 0; i < n; i++) {
        if (WiFi.BSSIDstr(i).equalsIgnoreCase(wifiTargetBssid)) {
            best = WiFi.RSSI(i);
            found = true;
            break;
        }
    }
    WiFi.scanDelete();
    rssiOut = best;
    return found;
}

void wifiApplyTargetSample(bool found, int32_t best) {
    wifiScanPass++;
    wifiLastTargetScanMs = millis();
    wifiLastTargetRssi = wifiTargetRssi;
    wifiTargetSeen = found;
    if (found) {
        wifiTargetRssi = best;
        if (wifiBestRssi < -120 || best > wifiBestRssi) wifiBestRssi = best;
        wifiTrendDb = (wifiLastTargetRssi < -120) ? 0 : (int16_t)(best - wifiLastTargetRssi);
        wifiRememberSample(best);
    } else {
        wifiTargetRssi = -127;
        wifiTrendDb = -8;
        wifiRememberSample(-100);
    }
}

bool wifiUpdateTargetRssi(bool drawWait = false) {
    if (!wifiTargetValid) return false;
    if (drawWait) drawWifiScanning("Rastreando BSSID seleccionado...");

    int32_t best = -127;
    const bool found = wifiScanTargetOnce(best);
    wifiApplyTargetSample(found, best);
    return found;
}

void wifiCancelAsyncTargetScan() {
    if (!wifiAsyncScanActive) return;
    WiFi.scanDelete();
    wifiAsyncScanActive = false;
}

void wifiStartAsyncTargetScan() {
    if (!wifiTargetValid || wifiAsyncScanActive) return;
    wifiPrepareMode();
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true, false, 85, wifiTargetChannel);
    wifiAsyncScanActive = true;
    wifiAsyncScanStartedMs = millis();
}

bool wifiPollAsyncTargetScan() {
    if (!wifiAsyncScanActive) return false;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return false;

    bool found = false;
    int32_t best = -127;
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (WiFi.BSSIDstr(i).equalsIgnoreCase(wifiTargetBssid)) {
                best = WiFi.RSSI(i);
                found = true;
                break;
            }
        }
    }
    WiFi.scanDelete();
    wifiAsyncScanActive = false;
    wifiApplyTargetSample(found, best);
    return true;
}

bool wifiWaitDirectionOk(const char* sector) {
    drawWifiDirectionPrompt(sector);
    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) return false;
        if (action == AppAction::Select) {
            toneClick(3000, 12);
            return true;
        }
        delay(4);
    }
}

bool wifiMeasureDirectionSector(const char* sector, int32_t& averageOut) {
    const uint8_t samples = 3;
    int32_t sum = 0;
    uint8_t hits = 0;
    for (uint8_t i = 0; i < samples; i++) {
        drawWifiDirectionMeasure(sector, i + 1, samples);
        int32_t rssi = -127;
        if (wifiScanTargetOnce(rssi)) {
            sum += rssi;
            hits++;
        }
        delay(90);
    }
    if (!hits) {
        averageOut = -127;
        return false;
    }
    averageOut = sum / hits;
    return true;
}

uint8_t wifiBestDirectionIndex(const int32_t* values, const bool* valid) {
    uint8_t best = 255;
    for (uint8_t i = 0; i < 4; i++) {
        if (!valid[i]) continue;
        if (best == 255 || values[i] > values[best]) best = i;
    }
    return best;
}

int32_t wifiSecondBestDirection(const int32_t* values, const bool* valid, uint8_t best) {
    int32_t second = -127;
    for (uint8_t i = 0; i < 4; i++) {
        if (!valid[i] || i == best) continue;
        if (values[i] > second) second = values[i];
    }
    return second;
}

String wifiDirectionConfidence(int32_t diffDb) {
    if (diffDb >= 9) return "ALTA";
    if (diffDb >= 5) return "MEDIA";
    if (diffDb >= 3) return "BAJA";
    return "INCIERTA";
}

uint16_t wifiDirectionConfidenceColor(int32_t diffDb) {
    if (diffDb >= 9) return COL_GREEN;
    if (diffDb >= 5) return COL_CYAN;
    if (diffDb >= 3) return COL_AMBER;
    return COL_RED;
}

void drawWifiDirectionResult(const int32_t* values, const bool* valid) {
    static const char* labels[] = {"FRENTE", "DERECHA", "ATRAS", "IZQUIERDA"};
    const uint8_t best = wifiBestDirectionIndex(values, valid);
    const int32_t second = best == 255 ? -127 : wifiSecondBestDirection(values, valid, best);
    const int32_t diff = best == 255 ? 0 : values[best] - second;

    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("WIFI DIR SCAN", best == 255 ? "sin senal" : "resultado");

    frame.drawRoundRect(12, 38, 296, 74, 5, best == 255 ? COL_RED : wifiDirectionConfidenceColor(diff));
    if (best == 255) {
        drawText(24, 58, "No se encontro el BSSID objetivo.", COL_RED);
        drawText(24, 82, "Acercate o vuelve a escanear.", COL_AMBER);
    } else {
        drawText(24, 56, String("MAYOR SENAL: ") + labels[best], wifiDirectionConfidenceColor(diff));
        drawText(24, 78, String("Confianza: ") + wifiDirectionConfidence(diff) + "  +" + diff + " dB", COL_CYAN);
        drawText(198, 78, String(values[best]) + " dBm", COL_GREEN);
    }

    const int y0 = 126;
    for (uint8_t i = 0; i < 4; i++) {
        const int y = y0 + i * 21;
        const uint16_t color = (i == best) ? COL_GREEN : (valid[i] ? COL_CYAN : COL_RED);
        drawText(18, y, labels[i], color);
        const uint8_t pct = valid[i] ? wifiRssiPct(values[i]) : 0;
        drawBar(86, y + 3, 150, 9, pct, color);
        drawText(246, y, valid[i] ? String(values[i]) + "dBm" : "--", color);
    }

    drawText(18, 212, "Tip: si es incierta, repite y gira mas lento.", COL_MUTED);
    drawFooter("OK REPETIR  BACK RADAR  HOLD HOME");
    pushFrame();
}

bool runWifiDirectionScan() {
    static const char* sectors[] = {"FRENTE", "DERECHA", "ATRAS", "IZQUIERDA"};
    int32_t values[4] = {-127, -127, -127, -127};
    bool valid[4] = {false, false, false, false};

    while (true) {
        for (uint8_t i = 0; i < 4; i++) {
            if (!wifiWaitDirectionOk(sectors[i])) return false;
            valid[i] = wifiMeasureDirectionSector(sectors[i], values[i]);
        }
        drawWifiDirectionResult(values, valid);

        while (true) {
            const AppAction action = inputRead();
            if (action == AppAction::LongSelect) {
                currentScreen = Screen::Home;
                setStatus("Ready");
                drawHome();
                pushFrame();
                return true;
            }
            if (action == AppAction::Back) return false;
            if (action == AppAction::Select) {
                toneClick(3000, 12);
                break;
            }
            delay(4);
        }
    }
}

void drawWifiRadarHistory(int x, int y, int w, int h) {
    frame.drawRect(x, y, w, h, COL_GRID);
    for (uint8_t i = 1; i < 4; i++) {
        const int gy = y + (h * i) / 4;
        for (int gx = x + 2; gx < x + w - 2; gx += 5) frame.drawPixel(gx, gy, COL_GRID);
    }
    for (uint8_t i = 0; i < WIFI_HISTORY_LEN; i++) {
        const uint8_t idx = (wifiHistoryHead + i) % WIFI_HISTORY_LEN;
        if (wifiHistory[idx] == 0) continue;
        const uint8_t pct = wifiRssiPct(wifiHistory[idx]);
        const int px = x + 2 + (i * (w - 4)) / WIFI_HISTORY_LEN;
        const int barH = max(1, (pct * (h - 4)) / 100);
        const uint16_t color = pct > 70 ? COL_GREEN : (pct > 38 ? COL_AMBER : COL_RED);
        frame.drawFastVLine(px, y + h - 2 - barH, barH, color);
    }
}

void drawWifiRadar() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("WIFI RADAR", wifiAsyncScanActive ? "scan..." : (wifiTargetSeen ? "rssi vivo" : "buscando"));

    const uint8_t pct = wifiTargetSeen ? wifiRssiPct(wifiTargetRssi) : 0;
    const int cx = 76;
    const int cy = 116;
    const int maxR = 64;
    frame.fillCircle(cx, cy, maxR + 5, 0x0004);
    frame.drawCircle(cx, cy, maxR, COL_CYAN);
    frame.drawCircle(cx, cy, 48, COL_GRID);
    frame.drawCircle(cx, cy, 32, COL_GRID);
    frame.drawCircle(cx, cy, 16, COL_GRID);
    for (uint8_t a = 0; a < 8; a++) {
        const float rad = (a * 45.0f) * DEG_TO_RAD;
        const int x = cx + cosf(rad) * maxR;
        const int y = cy + sinf(rad) * maxR;
        frame.drawLine(cx, cy, x, y, (a % 2 == 0) ? 0x03E0 : COL_GRID);
    }

    const float sweep = ((millis() % 2100UL) / 2100.0f) * TWO_PI;
    const uint16_t sweepColors[] = {COL_GREEN, 0x05E0, 0x03E0, 0x02A0, 0x0180};
    for (uint8_t t = 0; t < 5; t++) {
        const float rad = sweep - (t * 0.16f);
        const int x = cx + cosf(rad) * (maxR - 2);
        const int y = cy + sinf(rad) * (maxR - 2);
        frame.drawLine(cx, cy, x, y, sweepColors[t]);
    }
    const int sweepTipX = cx + cosf(sweep) * (maxR - 2);
    const int sweepTipY = cy + sinf(sweep) * (maxR - 2);
    frame.fillCircle(sweepTipX, sweepTipY, 2, COL_GREEN);

    const int dotR = map(pct, 0, 100, maxR - 5, 8);
    const float angle = (sweep * 0.65f) + ((wifiScanPass * 23) * DEG_TO_RAD);
    const int dx = cx + cosf(angle) * dotR;
    const int dy = cy + sinf(angle) * dotR;
    const uint16_t dotColor = wifiTargetSeen ? (pct > 70 ? COL_GREEN : (pct > 38 ? COL_AMBER : COL_RED)) : COL_RED;
    const int pulse = 11 + ((millis() / 120) % 8);
    frame.drawCircle(dx, dy, pulse, dotColor);
    frame.drawCircle(dx, dy, pulse + 6, pct > 55 ? 0x03E0 : COL_GRID);
    frame.fillCircle(dx, dy, 6, dotColor);
    frame.fillCircle(cx, cy, 4, COL_CYAN);
    frame.drawCircle(cx, cy, 7, COL_GREEN);
    frame.setTextDatum(MC_DATUM);
    frame.setTextSize(1);
    frame.setTextColor(COL_MUTED, COL_BG);
    frame.drawString("1m", cx, cy - 20, 1);
    frame.drawString("5m", cx, cy - 36, 1);
    frame.drawString("15m", cx, cy - 52, 1);
    frame.setTextDatum(TL_DATUM);

    frame.drawRoundRect(154, 38, 154, 80, 5, COL_GRID);
    drawText(164, 50, fitGpsText(wifiSsidText(wifiTargetSsid, wifiTargetSsid[0] == '\0'), 18), COL_GREEN);
    drawText(164, 68, String(wifiTargetRssi) + " dBm  " + String(pct) + "%", wifiTargetSeen ? COL_CYAN : COL_RED);
    drawText(164, 86, String("Peak ") + wifiBestRssi + " dBm", wifiBestRssi > -127 ? COL_GREEN : COL_MUTED);
    drawText(164, 102, String("Trend ") + wifiTrendDb + " dB", wifiTrendColor());

    frame.drawRoundRect(154, 126, 154, 72, 5, COL_GRID);
    drawText(164, 138, wifiTrendText(), wifiTrendColor());
    drawText(164, 156, String("CH ") + wifiTargetChannel + "  Scan " + wifiScanPass, COL_MUTED);
    drawText(164, 174, String("Metros ~ ") + wifiMetersText(), COL_AMBER);
    drawBar(164, 190, 132, 6, pct, dotColor);

    drawWifiRadarHistory(18, 188, 124, 22);
    drawFooter("OK RESCAN  DOWN DIR  BACK LISTA  HOLD HOME");
    pushFrame();
}

void runWifiRadar() {
    wifiResetTargetHistory();
    wifiStartAsyncTargetScan();
    drawWifiRadar();
    uint32_t lastDraw = 0;

    while (true) {
        serviceGps(4);
        const AppAction action = inputRead();
        if (action == AppAction::LongSelect) {
            wifiCancelAsyncTargetScan();
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }
        if (action == AppAction::Back) {
            wifiCancelAsyncTargetScan();
            return;
        }
        if (action == AppAction::Down) {
            wifiCancelAsyncTargetScan();
            if (runWifiDirectionScan()) return;
            drawWifiRadar();
            lastDraw = millis();
            wifiStartAsyncTargetScan();
        } else if (action == AppAction::Select || action == AppAction::Up) {
            wifiCancelAsyncTargetScan();
            wifiStartAsyncTargetScan();
        }

        if (wifiPollAsyncTargetScan()) {
            drawWifiRadar();
            lastDraw = millis();
        }
        if (!wifiAsyncScanActive && millis() - wifiLastTargetScanMs > 420) {
            wifiStartAsyncTargetScan();
        }

        if (millis() - lastDraw >= 65) {
            drawWifiRadar();
            lastDraw = millis();
        }
        delay(4);
    }
}

void runWifiLocatorApp() {
    currentScreen = Screen::WifiLocator;
    wifiScanList();
    drawWifiList();

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }
        if (action == AppAction::Up && wifiApCount) {
            wifiListSelected = (wifiListSelected == 0) ? wifiApCount - 1 : wifiListSelected - 1;
            toneClick();
        } else if (action == AppAction::Down && wifiApCount) {
            wifiListSelected = (wifiListSelected + 1) % wifiApCount;
            toneClick();
        } else if (action == AppAction::Select) {
            if (!wifiApCount) {
                wifiScanList();
                drawWifiList();
                continue;
            }
            const WifiApInfo& ap = wifiAps[wifiListSelected];
            strlcpy(wifiTargetSsid, ap.ssid, sizeof(wifiTargetSsid));
            strlcpy(wifiTargetBssid, ap.bssid, sizeof(wifiTargetBssid));
            wifiTargetChannel = ap.channel;
            wifiTargetValid = true;
            toneClick(3200, 15);
            runWifiRadar();
            currentScreen = Screen::WifiLocator;
        } else {
            delay(4);
            continue;
        }

        if (wifiListSelected < wifiListScroll) wifiListScroll = wifiListSelected;
        if (wifiListSelected >= wifiListScroll + 6) wifiListScroll = wifiListSelected - 5;
        drawWifiList();
        delay(4);
    }
}

uint8_t bleRssiPct(int32_t rssi) {
    if (rssi <= -100) return 0;
    if (rssi >= -35) return 100;
    return (uint8_t)map(rssi, -100, -35, 0, 100);
}

uint16_t bleEstimateMeters(int32_t rssi) {
    if (rssi < -120) return 0;
    const float rssiAtOneMeter = -59.0f;
    const float pathLoss = 2.3f;
    float meters = powf(10.0f, (rssiAtOneMeter - (float)rssi) / (10.0f * pathLoss));
    if (meters < 1.0f) meters = 1.0f;
    if (meters > 99.0f) meters = 99.0f;
    return (uint16_t)roundf(meters);
}

String bleMetersText() {
    if (!bleTargetSeen) return "-- m";
    return String(bleEstimateMeters(bleTargetRssi)) + " m";
}

String bleProximityText(uint8_t pct) {
    if (!bleTargetSeen) return "BUSCANDO";
    if (pct >= 70) return "CERCA";
    if (pct >= 38) return "MEDIA";
    return "LEJOS";
}

uint16_t bleProximityColor(uint8_t pct) {
    if (!bleTargetSeen) return COL_RED;
    if (pct >= 70) return COL_GREEN;
    if (pct >= 38) return COL_AMBER;
    return COL_RED;
}

String bleTrendText() {
    if (!bleTargetSeen) return "BUSCANDO";
    if (bleTrendDb >= 4) return "ACERCANDOTE";
    if (bleTrendDb <= -4) return "ALEJANDOTE";
    return "ESTABLE";
}

uint16_t bleTrendColor() {
    if (!bleTargetSeen) return COL_RED;
    if (bleTrendDb >= 4) return COL_GREEN;
    if (bleTrendDb <= -4) return COL_AMBER;
    return COL_CYAN;
}

String bleAddressShort(const String& address) {
    if (address.length() <= 8) return address;
    return address.substring(address.length() - 8);
}

uint16_t bleManufacturerId(BLEAdvertisedDevice& device) {
    if (!device.haveManufacturerData()) return 0xFFFF;
    const std::string data = device.getManufacturerData();
    if (data.length() < 2) return 0xFFFF;
    return (uint8_t)data[0] | ((uint16_t)(uint8_t)data[1] << 8);
}

String bleCompanyLabel(uint16_t companyId) {
    switch (companyId) {
        case 0x004C: return "Apple BLE";
        case 0x0006: return "Microsoft BLE";
        case 0x0075: return "Samsung BLE";
        case 0x00E0: return "Google BLE";
        case 0x0059: return "Nordic BLE";
        case 0x000F: return "Broadcom BLE";
        case 0x00D2: return "Sony BLE";
        case 0x0131: return "Xiaomi BLE";
        case 0x038F: return "Meta BLE";
        case 0x0499: return "Espressif BLE";
        default: break;
    }
    char label[12];
    snprintf(label, sizeof(label), "MFG %04X", companyId);
    return String(label);
}

String bleServiceLabel(BLEAdvertisedDevice& device) {
    for (int i = 0; i < device.getServiceUUIDCount(); i++) {
        String uuid = String(device.getServiceUUID(i).toString().c_str());
        uuid.toLowerCase();
        if (uuid.indexOf("1812") >= 0) return "HID Device";
        if (uuid.indexOf("180f") >= 0) return "Battery BLE";
        if (uuid.indexOf("180d") >= 0) return "Heart Rate";
        if (uuid.indexOf("180a") >= 0) return "Device Info";
        if (uuid.indexOf("1809") >= 0) return "Thermo BLE";
        if (uuid.indexOf("181a") >= 0) return "Env Sensor";
        if (uuid.indexOf("1816") >= 0) return "Cycling BLE";
        if (uuid.indexOf("1814") >= 0) return "Phone Alert";
        if (uuid.indexOf("feaa") >= 0) return "Beacon BLE";
    }
    return "";
}

String bleAppearanceLabel(uint16_t appearance) {
    const uint16_t category = appearance >> 6;
    switch (category) {
        case 1: return "Phone BLE";
        case 2: return "Computer BLE";
        case 3: return "Watch BLE";
        case 5: return "Display BLE";
        case 10: return "Tag BLE";
        case 15: return "HID BLE";
        case 49: return "Sensor BLE";
        default: break;
    }
    return "";
}

String bleBuildLabel(BLEAdvertisedDevice& device, const String& address) {
    if (device.haveName()) {
        String name = String(device.getName().c_str());
        name.trim();
        if (name.length()) return name;
    }

    const uint16_t companyId = bleManufacturerId(device);
    if (companyId != 0xFFFF) return bleCompanyLabel(companyId);

    const String service = bleServiceLabel(device);
    if (service.length()) return service;

    if (device.haveAppearance()) {
        const String appearance = bleAppearanceLabel(device.getAppearance());
        if (appearance.length()) return appearance;
    }

    return String("BLE ") + bleAddressShort(address);
}

String bleKindText(const BleDeviceInfo& device) {
    if (device.hasName) return "nombre adv";
    if (device.hasManufacturer) return device.kind;
    if (device.serviceCount) return "servicio BLE";
    if (device.hasAppearance) return "apariencia";
    return "direccion";
}

String bleDisplayText(const char* label) {
    if (label[0] == '\0') return "BLE desconocido";
    return String(label);
}

void bleRememberSample(int32_t rssi) {
    bleHistory[bleHistoryHead] = (int16_t)rssi;
    bleHistoryHead = (bleHistoryHead + 1) % BLE_HISTORY_LEN;
}

void bleResetTargetHistory() {
    bleTargetSeen = false;
    bleTargetRssi = -127;
    bleLastTargetRssi = -127;
    bleBestRssi = -127;
    bleTrendDb = 0;
    bleHistoryHead = 0;
    bleScanPass = 0;
    memset(bleHistory, 0, sizeof(bleHistory));
}

bool blePrepareScan() {
    quietRadiosForGps();
    wifiCancelAsyncTargetScan();
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    if (!ensureBleStackReady("Preparando radar Bluetooth...")) return false;
    BLEDevice::stopAdvertising();
    bleScan = BLEDevice::getScan();
    if (!bleScan) return false;
    bleScan->setActiveScan(true);
    bleScan->setInterval(96);
    bleScan->setWindow(64);
    return true;
}

void drawBleScanning(const char* line) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("BLE DEVICE RADAR", "scan");
    frame.drawRoundRect(18, 50, 284, 116, 5, COL_CYAN);
    drawText(34, 72, line, COL_CYAN);
    drawText(34, 98, "Escucha anuncios BLE cercanos.", COL_MUTED);
    drawText(34, 124, "No conecta, no empareja, no ataca.", COL_AMBER);
    drawFooter("BACK SALIR");
    pushFrame();
}

void bleSortDevices() {
    for (uint8_t i = 0; i < bleDeviceCount; i++) {
        for (uint8_t j = i + 1; j < bleDeviceCount; j++) {
            if (bleDevices[j].rssi > bleDevices[i].rssi) {
                BleDeviceInfo tmp = bleDevices[i];
                bleDevices[i] = bleDevices[j];
                bleDevices[j] = tmp;
            }
        }
    }
}

void bleScanDevices() {
    bleDeviceCount = 0;
    if (!blePrepareScan()) {
        setStatus("BLE init failed");
        return;
    }

    drawBleScanning("Escaneando dispositivos BLE...");
    BLEScanResults results = bleScan->start(4, false);
    const int count = results.getCount();
    bleDeviceCount = (uint8_t)min(count, (int)BLE_MAX_DEVICES);
    for (uint8_t i = 0; i < bleDeviceCount; i++) {
        BLEAdvertisedDevice d = results.getDevice(i);
        String name = d.haveName() ? String(d.getName().c_str()) : "";
        String address = String(d.getAddress().toString().c_str());
        const uint16_t companyId = bleManufacturerId(d);
        const String label = bleBuildLabel(d, address);
        const String kind = companyId != 0xFFFF ? bleCompanyLabel(companyId) : bleServiceLabel(d);
        name.toCharArray(bleDevices[i].name, sizeof(bleDevices[i].name));
        label.toCharArray(bleDevices[i].label, sizeof(bleDevices[i].label));
        kind.toCharArray(bleDevices[i].kind, sizeof(bleDevices[i].kind));
        address.toCharArray(bleDevices[i].address, sizeof(bleDevices[i].address));
        bleDevices[i].rssi = d.getRSSI();
        bleDevices[i].bestRssi = d.getRSSI();
        bleDevices[i].hasName = d.haveName();
        bleDevices[i].hasTxPower = d.haveTXPower();
        bleDevices[i].txPower = d.haveTXPower() ? d.getTXPower() : 0;
        bleDevices[i].serviceCount = d.getServiceUUIDCount();
        bleDevices[i].companyId = companyId;
        bleDevices[i].appearance = d.haveAppearance() ? d.getAppearance() : 0;
        bleDevices[i].hasManufacturer = companyId != 0xFFFF;
        bleDevices[i].hasAppearance = d.haveAppearance();
    }
    bleScan->clearResults();
    bleSortDevices();
    bleListSelected = 0;
    bleListScroll = 0;
    setStatus(bleDeviceCount ? "BLE scan ready" : "No BLE devices");
}

void drawBleList() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("BLE DEVICE RADAR", bleDeviceCount ? "elige device" : "sin devices");
    drawText(12, 35, String("Dispositivos: ") + bleDeviceCount + "  OK radar  UP/DOWN mover", COL_CYAN);

    const int listY = 56;
    const int rowH = 25;
    const uint8_t visible = 6;
    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t idx = bleListScroll + row;
        if (idx >= bleDeviceCount) break;
        const int y = listY + row * rowH;
        const bool selected = idx == bleListSelected;
        const uint16_t bg = selected ? COL_CYAN : COL_PANEL;
        const uint16_t fg = selected ? COL_BG : COL_TEXT;
        frame.fillRoundRect(10, y, 300, 21, 4, bg);
        frame.drawRoundRect(10, y, 300, 21, 4, selected ? COL_TEXT : COL_GRID);
        drawTextOn(16, y + 2, fitGpsText(bleDisplayText(bleDevices[idx].label), 18), fg, bg, 1);
        frame.setTextDatum(TR_DATUM);
        frame.setTextColor(selected ? COL_BG : (bleDevices[idx].rssi > -65 ? COL_GREEN : COL_AMBER), bg);
        frame.drawString(String(bleDevices[idx].rssi) + "dBm SVC" + bleDevices[idx].serviceCount, 303, y + 2, 2);
        frame.setTextDatum(TL_DATUM);
    }

    if (!bleDeviceCount) {
        drawText(28, 92, "No se detectaron anuncios BLE.", COL_AMBER);
        drawText(28, 116, "OK vuelve a escanear.", COL_CYAN);
    } else {
        const BleDeviceInfo& device = bleDevices[bleListSelected];
        drawText(14, 198, fitGpsText(bleKindText(device), 24), COL_CYAN);
        drawText(14, 214, fitGpsText(String(device.address) + (device.hasTxPower ? String("  TX ") + device.txPower : ""), 38), COL_MUTED);
    }
    drawFooter("OK RADAR  UP/DOWN MOVER  BACK SALIR");
    pushFrame();
}

bool bleScanTargetOnce(int32_t& rssiOut) {
    if (!bleTargetValid || !blePrepareScan()) return false;
    BLEScanResults results = bleScan->start(1, false);
    bool found = false;
    int32_t best = -127;
    for (int i = 0; i < results.getCount(); i++) {
        BLEAdvertisedDevice d = results.getDevice(i);
        String address = String(d.getAddress().toString().c_str());
        if (address.equalsIgnoreCase(bleTargetAddress)) {
            best = d.getRSSI();
            found = true;
            break;
        }
    }
    bleScan->clearResults();
    rssiOut = best;
    return found;
}

void bleUpdateTargetRssi(bool drawWait = false) {
    if (drawWait) drawBleScanning("Rastreando dispositivo BLE...");
    int32_t rssi = -127;
    const bool found = bleScanTargetOnce(rssi);
    bleScanPass++;
    bleLastTargetRssi = bleTargetRssi;
    bleTargetSeen = found;
    if (found) {
        bleTargetRssi = rssi;
        if (bleBestRssi < -120 || rssi > bleBestRssi) bleBestRssi = rssi;
        bleTrendDb = (bleLastTargetRssi < -120) ? 0 : (int16_t)(rssi - bleLastTargetRssi);
        bleRememberSample(rssi);
    } else {
        bleTargetRssi = -127;
        bleTrendDb = -8;
        bleRememberSample(-100);
    }
}

void drawBleHistory(int x, int y, int w, int h) {
    frame.drawRect(x, y, w, h, COL_GRID);
    for (uint8_t i = 0; i < BLE_HISTORY_LEN; i++) {
        const uint8_t idx = (bleHistoryHead + i) % BLE_HISTORY_LEN;
        if (bleHistory[idx] == 0) continue;
        const uint8_t pct = bleRssiPct(bleHistory[idx]);
        const int px = x + 2 + (i * (w - 4)) / BLE_HISTORY_LEN;
        const int barH = max(1, (pct * (h - 4)) / 100);
        const uint16_t color = pct > 70 ? COL_GREEN : (pct > 38 ? COL_AMBER : COL_RED);
        frame.drawFastVLine(px, y + h - 2 - barH, barH, color);
    }
}

void drawBleRadar() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("BLE PULSE", bleTargetSeen ? "rssi vivo" : "buscando");

    const uint8_t pct = bleTargetSeen ? bleRssiPct(bleTargetRssi) : 0;
    const uint16_t dotColor = bleTargetSeen ? (pct > 70 ? COL_GREEN : (pct > 38 ? COL_AMBER : COL_RED)) : COL_RED;
    const uint32_t now = millis();
    const int panelX = 14;
    const int panelY = 42;
    const int panelW = 132;
    const int panelH = 138;
    const int iconX = panelX + 43;
    const int iconY = panelY + 64;
    const int trackX = panelX + 93;
    const int trackTop = panelY + 25;
    const int trackBottom = panelY + 114;
    const int trackH = trackBottom - trackTop;

    frame.fillRoundRect(panelX, panelY, panelW, panelH, 6, 0x0004);
    frame.drawRoundRect(panelX, panelY, panelW, panelH, 6, COL_CYAN);

    frame.fillRoundRect(panelX + 9, panelY + 7, 68, 18, 4, COL_PANEL);
    drawText(panelX + 14, panelY + 9, bleProximityText(pct), bleProximityColor(pct));
    frame.fillTriangle(trackX - 5, trackTop - 8, trackX + 5, trackTop - 8, trackX, trackTop - 2, COL_GREEN);
    frame.fillTriangle(trackX - 5, trackBottom + 8, trackX + 5, trackBottom + 8, trackX, trackBottom + 2, COL_RED);
    frame.drawFastVLine(trackX, trackTop, trackH, COL_GRID);
    for (uint8_t i = 0; i < 5; i++) {
        const int y = trackTop + (i * trackH) / 4;
        frame.drawFastHLine(trackX - 14, y, 28, COL_GRID);
    }

    const int fillH = (pct * trackH) / 100;
    if (fillH > 0) {
        frame.fillRoundRect(trackX - 7, trackBottom - fillH, 14, fillH, 5, dotColor);
    }
    const int markerY = bleTargetSeen ? trackBottom - fillH : trackBottom;
    const int markerPulse = 7 + ((now / 140) % 5);
    frame.drawCircle(trackX, markerY, markerPulse, dotColor);
    frame.drawCircle(trackX, markerY, markerPulse + 6, COL_GRID);
    frame.fillCircle(trackX, markerY, 6, dotColor);

    for (uint8_t wave = 0; wave < 3; wave++) {
        const int radius = 18 + ((now / 120 + wave * 9) % 32);
        const uint16_t color = wave == 0 ? dotColor : (wave == 1 ? COL_CYAN : COL_GRID);
        frame.drawCircle(iconX, iconY, radius, bleTargetSeen ? color : COL_MUTED);
    }
    frame.fillRoundRect(iconX - 15, iconY - 25, 30, 50, 6, COL_PANEL);
    frame.drawRoundRect(iconX - 15, iconY - 25, 30, 50, 6, dotColor);
    frame.drawLine(iconX, iconY - 17, iconX, iconY + 17, COL_CYAN);
    frame.drawLine(iconX, iconY - 1, iconX + 11, iconY - 10, COL_CYAN);
    frame.drawLine(iconX, iconY - 1, iconX + 11, iconY + 8, COL_CYAN);
    frame.drawLine(iconX, iconY - 17, iconX + 10, iconY - 9, COL_CYAN);
    frame.drawLine(iconX, iconY + 17, iconX + 10, iconY + 8, COL_CYAN);
    frame.fillCircle(iconX, iconY, 3, COL_GREEN);

    frame.drawRoundRect(154, 38, 154, 84, 5, COL_GRID);
    drawText(164, 50, fitGpsText(bleDisplayText(bleTargetName), 18), COL_GREEN);
    drawText(164, 68, String(bleTargetRssi) + " dBm  " + String(pct) + "%", bleTargetSeen ? COL_CYAN : COL_RED);
    drawText(164, 86, String("Peak ") + bleBestRssi + " dBm", bleBestRssi > -127 ? COL_GREEN : COL_MUTED);
    drawText(164, 104, String("Metros ~ ") + bleMetersText(), COL_AMBER);

    frame.drawRoundRect(154, 130, 154, 68, 5, COL_GRID);
    drawText(164, 142, bleTrendText(), bleTrendColor());
    drawText(164, 160, String("Scan ") + bleScanPass + "  BLE adv", COL_MUTED);
    drawBar(164, 180, 132, 6, pct, dotColor);

    drawBleHistory(18, 188, 124, 22);
    drawFooter("OK RESCAN  BACK LISTA  HOLD HOME");
    pushFrame();
}

void runBleTargetRadar() {
    bleResetTargetHistory();
    bleUpdateTargetRssi(true);
    drawBleRadar();
    uint32_t lastScan = millis();
    uint32_t lastDraw = 0;

    while (true) {
        serviceGps(3);
        const AppAction action = inputRead();
        if (action == AppAction::LongSelect) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }
        if (action == AppAction::Back) return;
        if (action == AppAction::Select || action == AppAction::Up || action == AppAction::Down) {
            bleUpdateTargetRssi(true);
            drawBleRadar();
            lastScan = millis();
        }
        if (millis() - lastScan > 1400) {
            bleUpdateTargetRssi(false);
            drawBleRadar();
            lastScan = millis();
        }
        if (millis() - lastDraw > 80) {
            drawBleRadar();
            lastDraw = millis();
        }
        delay(4);
    }
}

void runBleDeviceRadarApp() {
    currentScreen = Screen::BleRadar;
    bleScanDevices();
    drawBleList();

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }
        if (action == AppAction::Up && bleDeviceCount) {
            bleListSelected = (bleListSelected == 0) ? bleDeviceCount - 1 : bleListSelected - 1;
            toneClick();
        } else if (action == AppAction::Down && bleDeviceCount) {
            bleListSelected = (bleListSelected + 1) % bleDeviceCount;
            toneClick();
        } else if (action == AppAction::Select) {
            if (!bleDeviceCount) {
                bleScanDevices();
                drawBleList();
                continue;
            }
            const BleDeviceInfo& device = bleDevices[bleListSelected];
            strlcpy(bleTargetName, device.label, sizeof(bleTargetName));
            strlcpy(bleTargetAddress, device.address, sizeof(bleTargetAddress));
            bleTargetValid = true;
            toneClick(3200, 15);
            runBleTargetRadar();
            currentScreen = Screen::BleRadar;
        } else {
            delay(4);
            continue;
        }

        if (bleListSelected < bleListScroll) bleListScroll = bleListSelected;
        if (bleListSelected >= bleListScroll + 6) bleListScroll = bleListSelected - 5;
        drawBleList();
        delay(4);
    }
}

String pinToString(const uint8_t* digits) {
    String out;
    for (uint8_t i = 0; i < 4; i++) out += String(digits[i]);
    return out;
}

void drawPasscodeEditor(const uint8_t* digits, uint8_t cursor) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("PASSCODE SIM", "setup");

    drawText(12, 38, "Set a 4 digit DEMO PIN for the video.", COL_CYAN);
    drawText(12, 58, "This only animates the TFT screen.", COL_MUTED);

    const int startX = 34;
    for (uint8_t i = 0; i < 4; i++) {
        const int x = startX + i * 66;
        const bool selected = i == cursor;
        const uint16_t border = selected ? COL_GREEN : COL_GRID;
        const uint16_t bg = selected ? 0x0340 : COL_BG;
        frame.fillRoundRect(x, 92, 52, 66, 5, bg);
        frame.drawRoundRect(x, 92, 52, 66, 5, border);
        frame.setTextDatum(MC_DATUM);
        frame.setTextSize(3);
        frame.setTextColor(COL_TEXT, bg);
        frame.drawString(String(digits[i]), x + 26, 125, 2);
        frame.setTextDatum(TL_DATUM);
    }

    drawText(20, 174, "UP/DOWN or encoder changes digit", COL_MUTED);
    drawText(20, 192, "OK next/start  BACK exit", COL_AMBER);
    drawFooter("SIMULATION ONLY  NO HID  NO REAL UNLOCK");
    pushFrame();
}

void drawPasscodeRunFrame(const uint8_t* digits, uint32_t startMs, uint32_t attempts) {
    const uint32_t elapsed = millis() - startMs;
    const int progress = constrain((int)((elapsed * 100UL) / 15000UL), 0, 100);

    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("PASSCODE SIM", "running");

    frame.drawRoundRect(10, 38, 300, 134, 5, COL_GRID);
    frame.fillRect(16, 44, 288, 122, 0x0004);

    drawText(22, 50, "ATTEMPT STREAM", COL_GREEN);
    drawText(210, 50, String(progress) + "%", COL_CYAN);

    for (uint8_t row = 0; row < 7; row++) {
        char line[42];
        const uint16_t value = random(0, 10000);
        const uint32_t id = attempts + row;
        snprintf(line, sizeof(line), "#%05lu  TRY %04u  HASH %04X",
                 (unsigned long)id, value, (unsigned int)((value * 73 + id) & 0xFFFF));
        drawText(24, 72 + row * 13, line, row == 6 ? COL_AMBER : COL_MUTED);
    }

    drawBar(18, 178, 284, 12, progress, COL_GREEN);
    drawText(18, 196, String("Target searching: "));
    drawFooter("BACK CANCEL  CINEMATIC LOCAL SIM");
    pushFrame();
}

void drawPasscodeFound(const uint8_t* digits) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("PASSCODE SIM", "complete");

    frame.drawRoundRect(22, 42, 116, 154, 10, COL_GRID);
    frame.fillRoundRect(30, 54, 100, 126, 8, 0x0208);
    frame.drawFastHLine(58, 64, 44, COL_MUTED);
    frame.fillCircle(80, 156, 7, COL_GREEN);

    frame.setTextDatum(MC_DATUM);
    frame.setTextSize(2);
    frame.setTextColor(COL_GREEN, 0x0208);
    frame.drawString("UNLOCK", 80, 96, 2);
    frame.setTextSize(1);
    frame.drawString("SIM", 80, 126, 2);
    frame.setTextDatum(TL_DATUM);

    drawText(156, 54, "CODE FOUND", COL_GREEN, 2);
    drawText(156, 88, "PIN DEMO", COL_MUTED);

    frame.setTextDatum(TL_DATUM);
    frame.setTextSize(3);
    frame.setTextColor(COL_TEXT, COL_BG);
    frame.drawString(pinToString(digits), 156, 108, 2);
    frame.setTextSize(1);

    drawText(156, 162, "The device did not type anything.", COL_CYAN);
    drawText(156, 180, "Screen-only effect for ethical demos.", COL_MUTED);
    drawFooter("OK/BACK RETURN");
    pushFrame();
}

void runPasscodeSimApp() {
    currentScreen = Screen::PasscodeSim;
    uint8_t digits[4] = {9, 7, 6, 4};

    toneClick(3200, 18);
    setStatus("Passcode sim running");

    randomSeed((uint32_t)micros() ^ analogRead(CD_VBAT_ADC));
    const uint32_t startMs = millis();
    uint32_t attempts = 0;
    while (millis() - startMs < 15000UL) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }

        drawPasscodeRunFrame(digits, startMs, attempts);
        attempts += 7;
        delay(145);
    }

    toneClick(3600, 35);
    delay(60);
    toneClick(4200, 45);
    drawPasscodeFound(digits);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect || action == AppAction::Select) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }
        delay(5);
    }
}

void drawHidDemoScreen(const char* tag, const String& line1, const String& line2, int progress) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("HID DEMO", tag);

    frame.drawRoundRect(10, 38, 300, 138, 5, COL_GRID);
    frame.fillRect(16, 44, 288, 126, 0x0004);

    drawText(24, 54, "TECLADO HID SEGURO", COL_GREEN, 1);
    drawText(24, 78, line1, COL_CYAN, 1);
    drawText(24, 100, line2, COL_MUTED, 1);

    frame.drawRoundRect(30, 130, 260, 18, 4, COL_GREEN);
    frame.fillRect(34, 134, ((252 * constrain(progress, 0, 100)) / 100), 10, COL_GREEN);

    drawText(18, 186, "Solo corre con confirmacion fisica.", COL_AMBER, 1);
    drawFooter("OK INICIAR  BACK CANCELAR");
    pushFrame();
}

void hidComboWinR() {
    HidKeyboard.press(KEY_LEFT_GUI);
    delay(40);
    HidKeyboard.press('r');
    delay(80);
    HidKeyboard.releaseAll();
    delay(450);
}

void hidTypeSlow(const char* text, uint16_t charDelay = 24) {
    for (const char* p = text; *p; p++) {
        HidKeyboard.write(*p);
        delay(charDelay);
    }
}

void hidTypeLine(const char* text, uint16_t charDelay = 24) {
    hidTypeSlow(text, charDelay);
    HidKeyboard.write(KEY_RETURN);
    delay(260);
}

void hidOpenRun(const char* command, uint16_t waitMs = 900) {
    hidComboWinR();
    hidTypeLine(command, 24);
    delay(waitMs);
}

void hidOpenStartSearch(const char* query, uint16_t waitMs = 1200) {
    HidKeyboard.press(KEY_LEFT_GUI);
    delay(80);
    HidKeyboard.releaseAll();
    delay(420);
    hidTypeLine(query, 24);
    delay(waitMs);
}

void hidConsumerTap(uint16_t key) {
    HidConsumer.press(key);
    delay(80);
    HidConsumer.release();
    delay(180);
}

void hidSendCtrlC() {
    HidKeyboard.press(KEY_LEFT_CTRL);
    delay(35);
    HidKeyboard.press('c');
    delay(90);
    HidKeyboard.releaseAll();
    delay(260);
}

struct HidPadEntry {
    const char* title;
    const char* subtitle;
    uint8_t action;
};

enum HidPadAction : uint8_t {
    HID_ACT_BACK = 0,
    HID_ACT_APPS,
    HID_ACT_CMD,
    HID_ACT_PS,
    HID_ACT_MEDIA,
    HID_ACT_GUIDED_DEMO,
    HID_ACT_OPEN_CMD,
    HID_ACT_OPEN_PS,
    HID_ACT_OPEN_OPERA,
    HID_ACT_OPEN_PAINT,
    HID_ACT_OPEN_NOTEPAD,
    HID_ACT_OPEN_CALC,
    HID_ACT_TYPE_CMD,
    HID_ACT_TYPE_PS,
    HID_ACT_MEDIA_PLAY,
    HID_ACT_MEDIA_NEXT,
    HID_ACT_MEDIA_PREV,
    HID_ACT_MEDIA_STOP,
    HID_ACT_MEDIA_VOL_UP,
    HID_ACT_MEDIA_VOL_DOWN,
    HID_ACT_MEDIA_MUTE,
    HID_ACT_MEDIA_VOLUME,
    HID_ACT_CTRL_C
};

const HidPadEntry HID_MAIN_MENU[] = {
    {"ABRIR APPS", "CMD, PowerShell, Opera GX, Paint", HID_ACT_APPS},
    {"CMD TOOLS", "Abre CMD y muestra comandos", HID_ACT_CMD},
    {"POWERSHELL", "Abre PS y muestra comandos", HID_ACT_PS},
    {"MULTIMEDIA", "YouTube, musica y volumen", HID_ACT_MEDIA},
    {"DEMO GUIADO", "Secuencia automatica segura", HID_ACT_GUIDED_DEMO},
    {"VOLVER", "Regresar al launcher", HID_ACT_BACK},
};

const HidPadEntry HID_APPS_MENU[] = {
    {"CMD", "Abrir simbolo del sistema", HID_ACT_OPEN_CMD},
    {"POWERSHELL", "Abrir terminal PowerShell", HID_ACT_OPEN_PS},
    {"OPERA GX", "Buscar y abrir Opera GX", HID_ACT_OPEN_OPERA},
    {"PAINT", "Abrir Microsoft Paint", HID_ACT_OPEN_PAINT},
    {"BLOC NOTAS", "Abrir Notepad", HID_ACT_OPEN_NOTEPAD},
    {"CALCULADORA", "Abrir calc", HID_ACT_OPEN_CALC},
    {"VOLVER", "Menu HID PAD", HID_ACT_BACK},
};

const HidPadEntry HID_CMD_MENU[] = {
    {"DETENER", "Enviar Ctrl+C", HID_ACT_CTRL_C},
    {"cls", "Limpiar terminal", HID_ACT_TYPE_CMD},
    {"echo CYBERDECK S3", "Banner para video", HID_ACT_TYPE_CMD},
    {"whoami", "Mostrar usuario actual", HID_ACT_TYPE_CMD},
    {"hostname", "Nombre del equipo", HID_ACT_TYPE_CMD},
    {"ver", "Version de Windows", HID_ACT_TYPE_CMD},
    {"echo %COMPUTERNAME%", "Nombre Windows", HID_ACT_TYPE_CMD},
    {"echo %PROCESSOR_ARCHITECTURE%", "Arquitectura CPU", HID_ACT_TYPE_CMD},
    {"echo %NUMBER_OF_PROCESSORS%", "Nucleos logicos", HID_ACT_TYPE_CMD},
    {"ipconfig", "Configuracion de red", HID_ACT_TYPE_CMD},
    {"netstat", "Conexiones locales", HID_ACT_TYPE_CMD},
    {"ping 8.8.8.8", "Prueba de conexion", HID_ACT_TYPE_CMD},
    {"tasklist", "Procesos locales", HID_ACT_TYPE_CMD},
    {"driverquery", "Drivers instalados", HID_ACT_TYPE_CMD},
    {"route print", "Tabla de rutas", HID_ACT_TYPE_CMD},
    {"systeminfo", "Informacion del sistema", HID_ACT_TYPE_CMD},
    {"VOLVER", "Menu HID PAD", HID_ACT_BACK},
};

const HidPadEntry HID_PS_MENU[] = {
    {"DETENER", "Enviar Ctrl+C", HID_ACT_CTRL_C},
    {"cls", "Limpiar terminal", HID_ACT_TYPE_PS},
    {"echo CYBERDECK S3", "Banner para video", HID_ACT_TYPE_PS},
    {"whoami", "Mostrar usuario actual", HID_ACT_TYPE_PS},
    {"hostname", "Nombre del equipo", HID_ACT_TYPE_PS},
    {"ver", "Version de Windows", HID_ACT_TYPE_PS},
    {"ipconfig", "Configuracion de red", HID_ACT_TYPE_PS},
    {"netstat", "Conexiones locales", HID_ACT_TYPE_PS},
    {"tasklist", "Procesos locales", HID_ACT_TYPE_PS},
    {"ping 8.8.8.8", "Prueba de conexion", HID_ACT_TYPE_PS},
    {"driverquery", "Drivers instalados", HID_ACT_TYPE_PS},
    {"systeminfo", "Informacion del sistema", HID_ACT_TYPE_PS},
    {"date", "Fecha local", HID_ACT_TYPE_PS},
    {"VOLVER", "Menu HID PAD", HID_ACT_BACK},
};

const HidPadEntry HID_MEDIA_MENU[] = {
    {"PLAY / PAUSA", "YouTube o reproductor activo", HID_ACT_MEDIA_PLAY},
    {"SIGUIENTE", "Siguiente pista/video", HID_ACT_MEDIA_NEXT},
    {"ANTERIOR", "Pista/video anterior", HID_ACT_MEDIA_PREV},
    {"STOP", "Detener reproduccion", HID_ACT_MEDIA_STOP},
    {"VOLUMEN LIVE", "UP/DOWN repetido, OK mute", HID_ACT_MEDIA_VOLUME},
    {"VOLUMEN +", "Subir volumen", HID_ACT_MEDIA_VOL_UP},
    {"VOLUMEN -", "Bajar volumen", HID_ACT_MEDIA_VOL_DOWN},
    {"MUTE", "Silenciar audio", HID_ACT_MEDIA_MUTE},
    {"VOLVER", "Menu HID PAD", HID_ACT_BACK},
};

enum BleRemoteAction : uint8_t {
    BLE_ACT_BACK = 0,
    BLE_ACT_PAIRING,
    BLE_ACT_OPEN_SAFARI,
    BLE_ACT_OPEN_NOTES,
    BLE_ACT_OPEN_YOUTUBE,
    BLE_ACT_OPEN_SPOTIFY,
    BLE_ACT_OPEN_WHATSAPP,
    BLE_ACT_OPEN_INSTAGRAM,
    BLE_ACT_OPEN_FACEBOOK,
    BLE_ACT_OPEN_SETTINGS,
    BLE_ACT_OPEN_PHOTOS,
    BLE_ACT_HOME,
    BLE_ACT_APP_SWITCH,
    BLE_ACT_WRITE_NOTES,
    BLE_ACT_SAFARI_SEARCH,
    BLE_ACT_MEDIA_MENU,
    BLE_ACT_MEDIA_PLAY,
    BLE_ACT_MEDIA_NEXT,
    BLE_ACT_MEDIA_PREV,
    BLE_ACT_MEDIA_STOP,
    BLE_ACT_MEDIA_VOL_UP,
    BLE_ACT_MEDIA_VOL_DOWN,
    BLE_ACT_MEDIA_MUTE,
    BLE_ACT_VOLUME_LIVE,
    BLE_ACT_CAMERA_MENU,
    BLE_ACT_CAMERA_OPEN,
    BLE_ACT_CAMERA_PHOTO,
    BLE_ACT_CAMERA_VIDEO_TOGGLE,
    BLE_ACT_CAMERA_PAUSE_TOGGLE,
    BLE_ACT_CAMERA_BURST,
    BLE_ACT_CAMERA_TIMER,
    BLE_ACT_CAMERA_SHORTCUT_PHOTO,
    BLE_ACT_CAMERA_SHORTCUT_VIDEO
};

const HidPadEntry BLE_REMOTE_MENU[] = {
    {"EMPAREJAR", "Ajustes > Bluetooth", BLE_ACT_PAIRING},
    {"SAFARI", "Abrir por Spotlight", BLE_ACT_OPEN_SAFARI},
    {"NOTAS", "Abrir por Spotlight", BLE_ACT_OPEN_NOTES},
    {"YOUTUBE", "Abrir por Spotlight", BLE_ACT_OPEN_YOUTUBE},
    {"SPOTIFY", "Abrir por Spotlight", BLE_ACT_OPEN_SPOTIFY},
    {"WHATSAPP", "Abrir por Spotlight", BLE_ACT_OPEN_WHATSAPP},
    {"INSTAGRAM", "Abrir por Spotlight", BLE_ACT_OPEN_INSTAGRAM},
    {"FACEBOOK", "Abrir por Spotlight", BLE_ACT_OPEN_FACEBOOK},
    {"CONFIGURACION", "Abrir por Spotlight", BLE_ACT_OPEN_SETTINGS},
    {"GALERIA", "Abrir Fotos", BLE_ACT_OPEN_PHOTOS},
    {"HOME", "Atajo Command+H", BLE_ACT_HOME},
    {"APP SWITCH", "Atajo Command+Tab", BLE_ACT_APP_SWITCH},
    {"TEXTO DEMO", "Escribir en Notas", BLE_ACT_WRITE_NOTES},
    {"BUSCAR WEB", "Safari + busqueda", BLE_ACT_SAFARI_SEARCH},
    {"MULTIMEDIA", "Play, pistas y volumen", BLE_ACT_MEDIA_MENU},
    {"CAMARA", "Foto, video y disparo remoto", BLE_ACT_CAMERA_MENU},
    {"VOLVER", "Regresar al launcher", BLE_ACT_BACK},
};

const HidPadEntry BLE_REMOTE_MEDIA_MENU[] = {
    {"PLAY / PAUSA", "iPhone multimedia", BLE_ACT_MEDIA_PLAY},
    {"SIGUIENTE", "Siguiente pista/video", BLE_ACT_MEDIA_NEXT},
    {"ANTERIOR", "Pista/video anterior", BLE_ACT_MEDIA_PREV},
    {"STOP", "Detener reproduccion", BLE_ACT_MEDIA_STOP},
    {"VOLUMEN LIVE", "UP/DOWN repetido, OK mute", BLE_ACT_VOLUME_LIVE},
    {"VOLUMEN +", "Subir volumen", BLE_ACT_MEDIA_VOL_UP},
    {"VOLUMEN -", "Bajar volumen", BLE_ACT_MEDIA_VOL_DOWN},
    {"MUTE", "Silenciar audio", BLE_ACT_MEDIA_MUTE},
    {"VOLVER", "Menu iPhone Remote", BLE_ACT_BACK},
};

const HidPadEntry BLE_REMOTE_CAMERA_MENU[] = {
    {"ABRIR CAMARA", "Spotlight: Camara", BLE_ACT_CAMERA_OPEN},
    {"ATAJO FOTO", "Spotlight: Cyber Foto", BLE_ACT_CAMERA_SHORTCUT_PHOTO},
    {"ATAJO VIDEO", "Spotlight: Cyber Video", BLE_ACT_CAMERA_SHORTCUT_VIDEO},
    {"FOTO", "Disparo remoto", BLE_ACT_CAMERA_PHOTO},
    {"VIDEO REC", "Start/stop en modo video", BLE_ACT_CAMERA_VIDEO_TOGGLE},
    {"PAUSA VIDEO", "Espacio, si iOS soporta", BLE_ACT_CAMERA_PAUSE_TOGGLE},
    {"BURST 3", "Tres disparos seguidos", BLE_ACT_CAMERA_BURST},
    {"TIMER 3S", "Cuenta y dispara foto", BLE_ACT_CAMERA_TIMER},
    {"VOLVER", "Menu iPhone Remote", BLE_ACT_BACK},
};

const char* hidCommandForEntry(const HidPadEntry& entry) {
    if (strcmp(entry.title, "echo CYBERDECK S3") == 0) {
        return "echo CYBERDECK S3 CONTROL LOCAL";
    }
    return entry.title;
}

void drawHidPadMenu(const char* title, const char* subtitle, const HidPadEntry* items,
                    uint8_t count, uint8_t selected, uint8_t scroll) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader(title, "hid");
    drawText(10, 36, subtitle, COL_CYAN);

    const int listY = 58;
    const int rowH = 25;
    const uint8_t visible = 6;
    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t idx = scroll + row;
        const int y = listY + row * rowH;
        if (idx >= count) break;

        const bool isSelected = idx == selected;
        const uint16_t bg = isSelected ? COL_GREEN : COL_PANEL;
        const uint16_t fg = isSelected ? COL_BG : COL_TEXT;
        frame.fillRoundRect(10, y, 300, 21, 4, bg);
        frame.drawRoundRect(10, y, 300, 21, 4, isSelected ? COL_TEXT : COL_GRID);
        drawTextOn(18, y + 2, items[idx].title, fg, bg, 1);
        frame.setTextDatum(TR_DATUM);
        frame.setTextColor(isSelected ? COL_BG : COL_MUTED, bg);
        frame.drawString(items[idx].subtitle, 303, y + 2, 2);
        frame.setTextDatum(TL_DATUM);
    }

    drawText(12, 210, "Solo en tu PC. Acciones locales seguras.", COL_AMBER);
    drawFooter("ENC/UP/DOWN  OK EJECUTAR  BACK VOLVER");
    pushFrame();
}

uint8_t runHidPadMenu(const char* title, const char* subtitle, const HidPadEntry* items,
                      uint8_t count, uint8_t startSelected = 0) {
    const uint8_t visible = 6;
    uint8_t selected = min<uint8_t>(startSelected, count - 1);
    uint8_t scroll = selected >= visible ? selected - visible + 1 : 0;
    drawHidPadMenu(title, subtitle, items, count, selected, scroll);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) return 255;

        if (action == AppAction::Up) {
            selected = (selected == 0) ? count - 1 : selected - 1;
            toneClick();
        } else if (action == AppAction::Down) {
            selected = (selected + 1) % count;
            toneClick();
        } else if (action == AppAction::Select) {
            toneClick(3000, 12);
            return selected;
        } else {
            delay(4);
            continue;
        }

        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;
        drawHidPadMenu(title, subtitle, items, count, selected, scroll);
        delay(4);
    }
}

void drawHidStatus(const char* title, const String& message, int progress = 100) {
    drawHidDemoScreen("accion", title, message, progress);
}

void hidRunMediaAction(uint8_t action) {
    switch (action) {
        case HID_ACT_MEDIA_PLAY: hidConsumerTap(CONSUMER_CONTROL_PLAY_PAUSE); break;
        case HID_ACT_MEDIA_NEXT: hidConsumerTap(CONSUMER_CONTROL_SCAN_NEXT); break;
        case HID_ACT_MEDIA_PREV: hidConsumerTap(CONSUMER_CONTROL_SCAN_PREVIOUS); break;
        case HID_ACT_MEDIA_STOP: hidConsumerTap(CONSUMER_CONTROL_STOP); break;
        case HID_ACT_MEDIA_VOL_UP: hidConsumerTap(CONSUMER_CONTROL_VOLUME_INCREMENT); break;
        case HID_ACT_MEDIA_VOL_DOWN: hidConsumerTap(CONSUMER_CONTROL_VOLUME_DECREMENT); break;
        case HID_ACT_MEDIA_MUTE: hidConsumerTap(CONSUMER_CONTROL_MUTE); break;
        default: break;
    }
}

void drawHidVolumeScreen(uint8_t level, const char* lastAction) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("VOLUMEN LIVE", "media");

    frame.drawRoundRect(18, 44, 284, 132, 5, COL_GRID);
    frame.fillRect(24, 50, 272, 120, 0x0004);
    drawText(34, 62, "Control fluido para YouTube / PC", COL_CYAN);
    drawText(34, 88, "UP: subir    DOWN: bajar", COL_TEXT);
    drawText(34, 108, "OK: mute     BACK: volver", COL_TEXT);
    drawText(34, 132, lastAction, COL_AMBER);
    drawBar(34, 150, 252, 12, level, COL_GREEN);

    drawFooter("UP/DOWN VOLUMEN  OK MUTE  BACK VOLVER");
    pushFrame();
}

void runHidVolumeControl() {
    uint8_t level = 50;
    const char* lastAction = "Listo para ajustar volumen";
    drawHidVolumeScreen(level, lastAction);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) return;

        if (action == AppAction::Up) {
            hidConsumerTap(CONSUMER_CONTROL_VOLUME_INCREMENT);
            level = min<uint8_t>(100, level + 4);
            lastAction = "Volumen +";
            toneClick(2800, 10);
        } else if (action == AppAction::Down) {
            hidConsumerTap(CONSUMER_CONTROL_VOLUME_DECREMENT);
            level = level < 4 ? 0 : level - 4;
            lastAction = "Volumen -";
            toneClick(1800, 10);
        } else if (action == AppAction::Select) {
            hidConsumerTap(CONSUMER_CONTROL_MUTE);
            lastAction = "Mute enviado";
            toneClick(3200, 12);
        } else {
            delay(4);
            continue;
        }

        drawHidVolumeScreen(level, lastAction);
        delay(4);
    }
}

constexpr uint8_t BLE_KEY_A = 0x04;
constexpr uint8_t BLE_KEY_1 = 0x1E;
constexpr uint8_t BLE_KEY_ENTER = 0x28;
constexpr uint8_t BLE_KEY_ESC = 0x29;
constexpr uint8_t BLE_KEY_BACKSPACE = 0x2A;
constexpr uint8_t BLE_KEY_TAB = 0x2B;
constexpr uint8_t BLE_KEY_SPACE = 0x2C;
constexpr uint8_t BLE_KEY_MINUS = 0x2D;
constexpr uint8_t BLE_KEY_PERIOD = 0x37;
constexpr uint8_t BLE_KEY_RIGHT = 0x4F;
constexpr uint8_t BLE_KEY_LEFT = 0x50;
constexpr uint8_t BLE_MOD_SHIFT = 0x02;
constexpr uint8_t BLE_MOD_GUI = 0x08;
constexpr uint16_t BLE_MEDIA_NEXT = 1 << 0;
constexpr uint16_t BLE_MEDIA_PREV = 1 << 1;
constexpr uint16_t BLE_MEDIA_STOP = 1 << 2;
constexpr uint16_t BLE_MEDIA_PLAY = 1 << 3;
constexpr uint16_t BLE_MEDIA_MUTE = 1 << 4;
constexpr uint16_t BLE_MEDIA_VOL_UP = 1 << 5;
constexpr uint16_t BLE_MEDIA_VOL_DOWN = 1 << 6;

struct BleKeyReport {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
};

const uint8_t BLE_REMOTE_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
    0x05, 0x0C, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x10, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7,
    0x09, 0xCD, 0x09, 0xE2, 0x09, 0xE9, 0x09, 0xEA,
    0x0A, 0x23, 0x02, 0x0A, 0x24, 0x02, 0x0A, 0x25, 0x02,
    0x0A, 0x26, 0x02, 0x0A, 0x27, 0x02, 0x0A, 0x2A, 0x02,
    0x0A, 0xB1, 0x01, 0x0A, 0xB8, 0x01, 0x0A, 0xB7, 0x01,
    0x81, 0x02, 0xC0
};

bool bleAsciiToKey(char c, uint8_t& usage, uint8_t& modifier) {
    modifier = 0;
    if (c >= 'a' && c <= 'z') {
        usage = BLE_KEY_A + (c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        usage = BLE_KEY_A + (c - 'A');
        modifier = BLE_MOD_SHIFT;
        return true;
    }
    if (c >= '1' && c <= '9') {
        usage = BLE_KEY_1 + (c - '1');
        return true;
    }
    if (c == '0') {
        usage = 0x27;
        return true;
    }
    if (c == ' ') {
        usage = BLE_KEY_SPACE;
        return true;
    }
    if (c == '\n' || c == '\r') {
        usage = BLE_KEY_ENTER;
        return true;
    }
    if (c == '-') {
        usage = BLE_KEY_MINUS;
        return true;
    }
    if (c == '.') {
        usage = BLE_KEY_PERIOD;
        return true;
    }
    usage = 0;
    return false;
}

void beginBleRemote() {
    if (bleRemoteReady) {
        if (!bleRemoteConnected) BLEDevice::startAdvertising();
        return;
    }

    releaseFrameForBleStartup();
    drawDirectStatus("INICIANDO BLE", "Preparando memoria Bluetooth...", COL_AMBER);
    releaseClassicBtMemoryOnce();
    delay(80);

    drawDirectStatus("INICIANDO BLE", "Creando dispositivo BLE...", COL_AMBER);
    if (!bleStackReady) {
        BLEDevice::init("CYBERDECK-REMOTE");
        bleStackReady = true;
    }
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
    bleRemoteServer = BLEDevice::createServer();
    bleRemoteServer->setCallbacks(&bleRemoteCallbacks);

    drawDirectStatus("INICIANDO BLE", "Creando servicio HID...", COL_AMBER);
    bleRemoteHid = new BLEHIDDevice(bleRemoteServer);
    drawDirectStatus("INICIANDO BLE", "Reporte teclado...", COL_AMBER);
    bleRemoteKeyboardInput = bleRemoteHid->inputReport(1);
    drawDirectStatus("INICIANDO BLE", "Reporte multimedia...", COL_AMBER);
    bleRemoteMediaInput = bleRemoteHid->inputReport(2);
    drawDirectStatus("INICIANDO BLE", "Datos del fabricante...", COL_AMBER);
    BLECharacteristic* manufacturer = bleRemoteHid->manufacturer();
    if (manufacturer) manufacturer->setValue("PepeAngell");
    bleRemoteHid->pnp(0x02, 0x303A, 0x1002, 0x0110);
    bleRemoteHid->hidInfo(0x00, 0x02);
    drawDirectStatus("INICIANDO BLE", "Mapa HID...", COL_AMBER);
    bleRemoteHid->reportMap((uint8_t*)BLE_REMOTE_REPORT_MAP, sizeof(BLE_REMOTE_REPORT_MAP));
    drawDirectStatus("INICIANDO BLE", "Activando servicios...", COL_AMBER);
    bleRemoteHid->startServices();

    drawDirectStatus("INICIANDO BLE", "Seguridad BLE...", COL_AMBER);
    bleRemoteSecurity = new BLESecurity();
    bleRemoteSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
    bleRemoteSecurity->setCapability(ESP_IO_CAP_NONE);
    bleRemoteSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    bleRemoteSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    drawDirectStatus("INICIANDO BLE", "Anunciando para emparejar...", COL_AMBER);
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    BLEAdvertisementData advData;
    advData.setFlags(0x06);
    advData.setAppearance(HID_KEYBOARD);
    advData.setCompleteServices(BLEUUID((uint16_t)0x1812));
    BLEAdvertisementData scanData;
    scanData.setName("CYBERDECK-REMOTE");
    advertising->setAdvertisementData(advData);
    advertising->setScanResponseData(scanData);
    advertising->setAdvertisementType(ADV_TYPE_IND);
    advertising->setScanResponse(true);
    advertising->setMinInterval(0x20);
    advertising->setMaxInterval(0x40);
    advertising->setMinPreferred(0x06);
    advertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    bleRemoteReady = true;
    recreateFrameAfterBleStartup();
}

bool bleRemoteCanSend() {
    return bleRemoteReady && bleRemoteConnected && bleRemoteKeyboardInput && bleRemoteMediaInput;
}

void bleSendKey(uint8_t usage, uint8_t modifier = 0, uint16_t holdMs = 70) {
    if (!bleRemoteCanSend()) return;
    BleKeyReport report = {};
    report.modifiers = modifier;
    report.keys[0] = usage;
    bleRemoteKeyboardInput->setValue((uint8_t*)&report, sizeof(report));
    bleRemoteKeyboardInput->notify();
    delay(holdMs);
    memset(&report, 0, sizeof(report));
    bleRemoteKeyboardInput->setValue((uint8_t*)&report, sizeof(report));
    bleRemoteKeyboardInput->notify();
    delay(110);
}

void bleTypeText(const char* text, uint16_t charDelay = 28) {
    for (const char* p = text; *p; p++) {
        uint8_t usage = 0;
        uint8_t modifier = 0;
        if (bleAsciiToKey(*p, usage, modifier)) {
            bleSendKey(usage, modifier, 45);
            delay(charDelay);
        }
    }
}

void bleSendShortcut(uint8_t modifier, uint8_t usage) {
    bleSendKey(usage, modifier, 90);
}

void bleSendMedia(uint16_t mask) {
    if (!bleRemoteCanSend()) return;
    bleRemoteMediaInput->setValue((uint8_t*)&mask, sizeof(mask));
    bleRemoteMediaInput->notify();
    delay(80);
    mask = 0;
    bleRemoteMediaInput->setValue((uint8_t*)&mask, sizeof(mask));
    bleRemoteMediaInput->notify();
    delay(120);
}

void drawBleRemoteMenu(const char* title, const char* subtitle, const HidPadEntry* items,
                       uint8_t count, uint8_t selected, uint8_t scroll) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader(title, bleRemoteConnected ? "ble ok" : "pair");
    drawText(10, 36, subtitle, COL_CYAN);

    const int listY = 58;
    const int rowH = 25;
    const uint8_t visible = 6;
    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t idx = scroll + row;
        const int y = listY + row * rowH;
        if (idx >= count) break;

        const bool isSelected = idx == selected;
        const uint16_t bg = isSelected ? COL_CYAN : COL_PANEL;
        const uint16_t fg = isSelected ? COL_BG : COL_TEXT;
        frame.fillRoundRect(10, y, 300, 21, 4, bg);
        frame.drawRoundRect(10, y, 300, 21, 4, isSelected ? COL_TEXT : COL_GRID);
        drawTextOn(18, y + 2, items[idx].title, fg, bg, 1);
        frame.setTextDatum(TR_DATUM);
        frame.setTextColor(isSelected ? COL_BG : COL_MUTED, bg);
        frame.drawString(items[idx].subtitle, 303, y + 2, 2);
        frame.setTextDatum(TL_DATUM);
    }

    drawText(12, 210, bleRemoteConnected ? "iPhone conectado. Control local por BLE." : "Empareja: Bluetooth > CYBERDECK-REMOTE", COL_AMBER);
    drawFooter("ENC/UP/DOWN  OK EJECUTAR  BACK VOLVER");
    pushFrame();
}

uint8_t runBleRemoteMenu(const char* title, const char* subtitle, const HidPadEntry* items,
                         uint8_t count, uint8_t startSelected = 0) {
    const uint8_t visible = 6;
    uint8_t selected = min<uint8_t>(startSelected, count - 1);
    uint8_t scroll = selected >= visible ? selected - visible + 1 : 0;
    drawBleRemoteMenu(title, subtitle, items, count, selected, scroll);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) return 255;

        if (action == AppAction::Up) {
            selected = (selected == 0) ? count - 1 : selected - 1;
            toneClick();
        } else if (action == AppAction::Down) {
            selected = (selected + 1) % count;
            toneClick();
        } else if (action == AppAction::Select) {
            toneClick(3000, 12);
            return selected;
        } else {
            delay(4);
            continue;
        }

        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;
        drawBleRemoteMenu(title, subtitle, items, count, selected, scroll);
        delay(4);
    }
}

void drawBleStatus(const char* title, const String& line, int progress = 100) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("IPHONE REMOTE", bleRemoteConnected ? "conectado" : "espera");
    frame.drawRoundRect(14, 42, 292, 130, 5, bleRemoteConnected ? COL_CYAN : COL_AMBER);
    frame.fillRect(20, 48, 280, 118, 0x0004);
    drawText(30, 62, title, bleRemoteConnected ? COL_CYAN : COL_AMBER, 1);
    drawText(30, 88, line, COL_TEXT, 1);
    drawText(30, 114, "CYBERDECK-REMOTE", COL_GREEN, 1);
    drawBar(30, 142, 260, 12, progress, bleRemoteConnected ? COL_CYAN : COL_AMBER);
    drawFooter("BACK VOLVER");
    pushFrame();
}

bool ensureBleRemoteConnected() {
    beginBleRemote();
    if (bleRemoteConnected) return true;
    drawBleStatus("Esperando iPhone", "Ajustes > Bluetooth > CYBERDECK-REMOTE", 20);
    delay(900);
    return false;
}

void runBlePairingScreen() {
    beginBleRemote();
    uint32_t lastDraw = 0;
    while (true) {
        if (millis() - lastDraw > 450) {
            frame.fillSprite(COL_BG);
            drawGrid();
            drawHeader("EMPAREJAR iPHONE", bleRemoteConnected ? "conectado" : "anunciando");
            frame.drawRoundRect(16, 42, 288, 140, 5, bleRemoteConnected ? COL_CYAN : COL_GREEN);
            frame.fillRect(22, 48, 276, 128, 0x0004);
            drawText(32, 60, "1. iPhone: Ajustes > Bluetooth", COL_CYAN);
            drawText(32, 84, "2. Toca CYBERDECK-REMOTE", COL_TEXT);
            drawText(32, 108, "3. Acepta el emparejamiento", COL_TEXT);
            drawText(32, 136, bleRemoteConnected ? "Estado: conectado" : "Estado: esperando iPhone", bleRemoteConnected ? COL_GREEN : COL_AMBER);
            drawFooter("BACK VOLVER");
            pushFrame();
            lastDraw = millis();
        }

        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect || action == AppAction::Select) return;
        delay(5);
    }
}

void bleOpenIphoneApp(const char* appName) {
    if (!ensureBleRemoteConnected()) return;
    drawBleStatus("Abriendo app", String("Spotlight: ") + appName, 45);
    bleSendShortcut(BLE_MOD_GUI, BLE_KEY_SPACE);
    delay(650);
    bleTypeText(appName, 24);
    delay(250);
    bleSendKey(BLE_KEY_ENTER);
    delay(650);
}

void bleWriteNotesDemo() {
    if (!ensureBleRemoteConnected()) return;
    drawBleStatus("Notas demo", "Abriendo Notas y escribiendo", 30);
    bleOpenIphoneApp("Notas");
    delay(1700);
    bleSendShortcut(BLE_MOD_GUI, BLE_KEY_A + ('n' - 'a'));
    delay(600);
    bleTypeText("CYBERDECK S3 BLE REMOTE\n", 24);
    bleTypeText("Control local por Bluetooth desde mi dispositivo.\n", 24);
    bleTypeText("Demo segura: Ejemplo visual conceptual.\n", 24);
    drawBleStatus("Notas demo", "Texto enviado al iPhone", 100);
    delay(700);
}

void bleSafariSearchDemo() {
    if (!ensureBleRemoteConnected()) return;
    drawBleStatus("Safari demo", "Abriendo busqueda segura", 30);
    bleOpenIphoneApp("Safari");
    delay(1500);
    bleSendShortcut(BLE_MOD_GUI, BLE_KEY_A + ('l' - 'a'));
    delay(500);
    bleTypeText("instagram.com/pepeangelll", 24);
    bleSendKey(BLE_KEY_ENTER);
    drawBleStatus("Safari demo", "Busqueda enviada", 100);
    delay(700);
}

void bleOpenCameraApp() {
    if (!ensureBleRemoteConnected()) return;
    drawBleStatus("Camara", "Abriendo app Camara", 35);
    bleSendShortcut(BLE_MOD_GUI, BLE_KEY_SPACE);
    delay(650);
    bleTypeText("Camara", 24);
    delay(250);
    bleSendKey(BLE_KEY_ENTER);
    delay(900);
}

void bleRunSpotlightItem(const char* title, const char* query) {
    if (!ensureBleRemoteConnected()) return;
    drawBleStatus(title, String("Spotlight: ") + query, 45);
    bleSendShortcut(BLE_MOD_GUI, BLE_KEY_SPACE);
    delay(650);
    bleTypeText(query, 24);
    delay(250);
    bleSendKey(BLE_KEY_ENTER);
    delay(900);
}

void drawBleCameraStatus(const char* action, const String& detail, int progress = 100) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("iPHONE CAMARA", bleRemoteConnected ? "ble ok" : "pair");
    frame.drawRoundRect(14, 38, 292, 138, 5, COL_CYAN);
    frame.fillRect(20, 44, 280, 126, 0x0004);
    drawText(30, 58, action, COL_CYAN, 1);
    drawText(30, 84, detail, COL_TEXT, 1);
    drawText(30, 112, "Volumen + actua como shutter.", COL_AMBER, 1);
    drawBar(30, 144, 260, 12, progress, COL_CYAN);
    drawFooter("BACK VOLVER");
    pushFrame();
}

void bleCameraShutter(const char* label, const char* detail) {
    if (!ensureBleRemoteConnected()) return;
    drawBleCameraStatus(label, detail, 70);
    bleSendMedia(BLE_MEDIA_VOL_UP);
    toneClick(3400, 16);
    delay(420);
}

void bleCameraShortcutPhoto() {
    bleRunSpotlightItem("ATAJO FOTO", "Cyber Foto");
}

void bleCameraShortcutVideo() {
    bleRunSpotlightItem("ATAJO VIDEO", "Cyber Video");
}

void bleCameraBurst() {
    if (!ensureBleRemoteConnected()) return;
    for (uint8_t i = 0; i < 3; i++) {
        drawBleCameraStatus("BURST 3", String("Disparo ") + (i + 1) + "/3", 35 + i * 25);
        bleSendMedia(BLE_MEDIA_VOL_UP);
        toneClick(3000 + i * 220, 14);
        delay(520);
    }
    drawBleCameraStatus("BURST 3", "Secuencia terminada", 100);
    delay(450);
}

void bleCameraTimerShot() {
    if (!ensureBleRemoteConnected()) return;
    for (int i = 3; i > 0; i--) {
        drawBleCameraStatus("TIMER 3S", String("Disparo en ") + i, (3 - i) * 28);
        toneClick(1800 + i * 260, 18);
        delay(850);
    }
    drawBleCameraStatus("TIMER 3S", "Disparando foto", 90);
    bleSendMedia(BLE_MEDIA_VOL_UP);
    toneClick(3800, 22);
    delay(520);
}

void bleCameraPauseToggle() {
    if (!ensureBleRemoteConnected()) return;
    drawBleCameraStatus("PAUSA VIDEO", "Enviando Space", 75);
    bleSendKey(BLE_KEY_SPACE);
    delay(420);
}

void bleRunMediaAction(uint8_t action) {
    switch (action) {
        case BLE_ACT_MEDIA_PLAY: bleSendMedia(BLE_MEDIA_PLAY); break;
        case BLE_ACT_MEDIA_NEXT: bleSendMedia(BLE_MEDIA_NEXT); break;
        case BLE_ACT_MEDIA_PREV: bleSendMedia(BLE_MEDIA_PREV); break;
        case BLE_ACT_MEDIA_STOP: bleSendMedia(BLE_MEDIA_STOP); break;
        case BLE_ACT_MEDIA_VOL_UP: bleSendMedia(BLE_MEDIA_VOL_UP); break;
        case BLE_ACT_MEDIA_VOL_DOWN: bleSendMedia(BLE_MEDIA_VOL_DOWN); break;
        case BLE_ACT_MEDIA_MUTE: bleSendMedia(BLE_MEDIA_MUTE); break;
        default: break;
    }
}

void drawBleVolumeScreen(uint8_t level, const char* lastAction) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("iPHONE VOLUME", bleRemoteConnected ? "ble ok" : "pair");
    frame.drawRoundRect(18, 44, 284, 132, 5, COL_GRID);
    frame.fillRect(24, 50, 272, 120, 0x0004);
    drawText(34, 62, "Control de volumen por Bluetooth", COL_CYAN);
    drawText(34, 88, "UP: subir    DOWN: bajar", COL_TEXT);
    drawText(34, 108, "OK: mute     BACK: volver", COL_TEXT);
    drawText(34, 132, lastAction, COL_AMBER);
    drawBar(34, 150, 252, 12, level, COL_CYAN);
    drawFooter("UP/DOWN VOLUMEN  OK MUTE  BACK VOLVER");
    pushFrame();
}

void runBleVolumeControl() {
    if (!ensureBleRemoteConnected()) return;
    uint8_t level = 50;
    const char* lastAction = "Listo para ajustar volumen";
    drawBleVolumeScreen(level, lastAction);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) return;

        if (action == AppAction::Up) {
            bleSendMedia(BLE_MEDIA_VOL_UP);
            level = min<uint8_t>(100, level + 4);
            lastAction = "Volumen +";
            toneClick(2800, 10);
        } else if (action == AppAction::Down) {
            bleSendMedia(BLE_MEDIA_VOL_DOWN);
            level = level < 4 ? 0 : level - 4;
            lastAction = "Volumen -";
            toneClick(1800, 10);
        } else if (action == AppAction::Select) {
            bleSendMedia(BLE_MEDIA_MUTE);
            lastAction = "Mute enviado";
            toneClick(3200, 12);
        } else {
            delay(4);
            continue;
        }

        drawBleVolumeScreen(level, lastAction);
        delay(4);
    }
}

void runIphoneRemoteApp() {
    currentScreen = Screen::IphoneRemote;
    uint8_t selected = 0;
    uint8_t mediaSelected = 0;
    uint8_t cameraSelected = 0;

    while (true) {
        const uint8_t idx = runBleRemoteMenu("IPHONE REMOTE", "Control remoto BLE seguro",
                                             BLE_REMOTE_MENU, sizeof(BLE_REMOTE_MENU) / sizeof(BLE_REMOTE_MENU[0]), selected);
        if (idx == 255) break;
        selected = idx;

        const HidPadEntry& entry = BLE_REMOTE_MENU[idx];
        if (entry.action == BLE_ACT_BACK) break;

        switch (entry.action) {
            case BLE_ACT_PAIRING:
                runBlePairingScreen();
                break;
            case BLE_ACT_OPEN_SAFARI:
                bleOpenIphoneApp("Safari");
                break;
            case BLE_ACT_OPEN_NOTES:
                bleOpenIphoneApp("Notas");
                break;
            case BLE_ACT_OPEN_YOUTUBE:
                bleOpenIphoneApp("YouTube");
                break;
            case BLE_ACT_OPEN_SPOTIFY:
                bleOpenIphoneApp("Spotify");
                break;
            case BLE_ACT_OPEN_WHATSAPP:
                bleOpenIphoneApp("WhatsApp");
                break;
            case BLE_ACT_OPEN_INSTAGRAM:
                bleOpenIphoneApp("Instagram");
                break;
            case BLE_ACT_OPEN_FACEBOOK:
                bleOpenIphoneApp("Facebook");
                break;
            case BLE_ACT_OPEN_SETTINGS:
                bleOpenIphoneApp("Configuracion");
                break;
            case BLE_ACT_OPEN_PHOTOS:
                bleOpenIphoneApp("Fotos");
                break;
            case BLE_ACT_HOME:
                if (ensureBleRemoteConnected()) {
                    drawBleStatus("HOME", "Enviando Command+H", 80);
                    bleSendShortcut(BLE_MOD_GUI, BLE_KEY_A + ('h' - 'a'));
                    delay(400);
                }
                break;
            case BLE_ACT_APP_SWITCH:
                if (ensureBleRemoteConnected()) {
                    drawBleStatus("APP SWITCH", "Enviando Command+Tab", 80);
                    bleSendShortcut(BLE_MOD_GUI, BLE_KEY_TAB);
                    delay(400);
                }
                break;
            case BLE_ACT_WRITE_NOTES:
                bleWriteNotesDemo();
                break;
            case BLE_ACT_SAFARI_SEARCH:
                bleSafariSearchDemo();
                break;
            case BLE_ACT_MEDIA_MENU:
                while (true) {
                    const uint8_t mediaIdx = runBleRemoteMenu("iPHONE MEDIA", "Control multimedia BLE",
                                                              BLE_REMOTE_MEDIA_MENU,
                                                              sizeof(BLE_REMOTE_MEDIA_MENU) / sizeof(BLE_REMOTE_MEDIA_MENU[0]),
                                                              mediaSelected);
                    if (mediaIdx == 255) break;
                    mediaSelected = mediaIdx;
                    const HidPadEntry& mediaEntry = BLE_REMOTE_MEDIA_MENU[mediaIdx];
                    if (mediaEntry.action == BLE_ACT_BACK) break;
                    if (mediaEntry.action == BLE_ACT_VOLUME_LIVE) {
                        runBleVolumeControl();
                        continue;
                    }
                    if (ensureBleRemoteConnected()) {
                        drawBleStatus("Multimedia", mediaEntry.title, 80);
                        bleRunMediaAction(mediaEntry.action);
                        delay(260);
                    }
                }
                break;
            case BLE_ACT_CAMERA_MENU:
                while (true) {
                    const uint8_t cameraIdx = runBleRemoteMenu("iPHONE CAMARA", "Disparador remoto BLE",
                                                               BLE_REMOTE_CAMERA_MENU,
                                                               sizeof(BLE_REMOTE_CAMERA_MENU) / sizeof(BLE_REMOTE_CAMERA_MENU[0]),
                                                               cameraSelected);
                    if (cameraIdx == 255) break;
                    cameraSelected = cameraIdx;
                    const HidPadEntry& cameraEntry = BLE_REMOTE_CAMERA_MENU[cameraIdx];
                    if (cameraEntry.action == BLE_ACT_BACK) break;

                    switch (cameraEntry.action) {
                        case BLE_ACT_CAMERA_OPEN:
                            bleOpenCameraApp();
                            break;
                        case BLE_ACT_CAMERA_PHOTO:
                            bleCameraShutter("FOTO", "Disparo remoto enviado");
                            break;
                        case BLE_ACT_CAMERA_VIDEO_TOGGLE:
                            bleCameraShutter("VIDEO REC", "Start/stop si estas en modo video");
                            break;
                        case BLE_ACT_CAMERA_SHORTCUT_PHOTO:
                            bleCameraShortcutPhoto();
                            break;
                        case BLE_ACT_CAMERA_SHORTCUT_VIDEO:
                            bleCameraShortcutVideo();
                            break;
                        case BLE_ACT_CAMERA_PAUSE_TOGGLE:
                            bleCameraPauseToggle();
                            break;
                        case BLE_ACT_CAMERA_BURST:
                            bleCameraBurst();
                            break;
                        case BLE_ACT_CAMERA_TIMER:
                            bleCameraTimerShot();
                            break;
                        default:
                            break;
                    }
                }
                break;
            default:
                break;
        }
    }

    currentScreen = Screen::Home;
    setStatus("Ready");
    drawHome();
    pushFrame();
}

void runHidDemoApp() {
    currentScreen = Screen::HidDemo;
    beginHid();
    HidKeyboard.releaseAll();

    drawHidDemoScreen("listo", "Objetivo: tu propia PC Windows", "Abrira Bloc de notas y PowerShell.", 0);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back) {
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }

        if (action == AppAction::Select || action == AppAction::LongSelect) break;
        delay(5);
    }

    for (int i = 3; i > 0; i--) {
        drawHidDemoScreen("cuenta", String("Iniciando en ") + i, "Deja el foco en tu PC.", (3 - i) * 33);
        toneClick(1800 + i * 250, 20);
        delay(750);
    }

    drawHidDemoScreen("escribiendo", "Abriendo Bloc de notas...", "Escribiendo banner seguro.", 20);
    hidComboWinR();
    hidTypeLine("notepad", 24);
    delay(1200);
    hidTypeLine("CYBERDECK S3 HID DEMO", 26);
    hidTypeLine("Demostracion local y segura de teclado USB.", 26);
    hidTypeLine("No descarga nada. No crea persistencia. No toca credenciales.", 26);
    hidTypeLine("Ahora abrira PowerShell con comandos inofensivos.", 26);

    drawHidDemoScreen("escribiendo", "Abriendo PowerShell...", "Ejecutando comandos locales seguros.", 45);
    hidComboWinR();
    hidTypeLine("powershell", 24);
    delay(1600);

    const char* commands[] = {
        "cls",
        "echo CYBERDECK S3 HID DEMO",
        "echo COMANDOS LOCALES E INOFENSIVOS",
        "whoami",
        "hostname",
        "ipconfig",
        "ping 8.8.8.8",
        "echo DEMO FINALIZADA SIN CAMBIOS EN EL SISTEMA"
    };

    const uint8_t commandCount = sizeof(commands) / sizeof(commands[0]);
    for (uint8_t i = 0; i < commandCount; i++) {
        drawHidDemoScreen("escribiendo", String("Comando ") + (i + 1) + "/" + commandCount, commands[i], 50 + ((i * 45) / commandCount));
        hidTypeLine(commands[i], 24);
        delay(i == 6 ? 1200 : 360);
    }

    drawHidDemoScreen("listo", "Demo HID finalizada.", "OK/BACK regresa al menu.", 100);
    toneClick(3600, 35);
    delay(80);
    toneClick(4200, 45);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect || action == AppAction::Select) {
            HidKeyboard.releaseAll();
            currentScreen = Screen::Home;
            setStatus("Ready");
            drawHome();
            pushFrame();
            return;
        }
        delay(5);
    }
}

void runHidPadApp() {
    currentScreen = Screen::HidDemo;
    beginHid();
    HidKeyboard.releaseAll();

    while (true) {
        static uint8_t mainSelected = 0;
        uint8_t idx = runHidPadMenu("HID PAD", "Control seguro para tu PC",
                                    HID_MAIN_MENU, sizeof(HID_MAIN_MENU) / sizeof(HID_MAIN_MENU[0]), mainSelected);
        if (idx == 255) break;
        mainSelected = idx;

        const uint8_t action = HID_MAIN_MENU[idx].action;
        if (action == HID_ACT_BACK) break;

        if (action == HID_ACT_APPS) {
            uint8_t appSelected = 0;
            while (true) {
                uint8_t appIdx = runHidPadMenu("ABRIR APPS", "Selecciona una app local",
                                               HID_APPS_MENU, sizeof(HID_APPS_MENU) / sizeof(HID_APPS_MENU[0]), appSelected);
                if (appIdx == 255) break;
                appSelected = appIdx;

                const HidPadEntry& entry = HID_APPS_MENU[appIdx];
                if (entry.action == HID_ACT_BACK) break;

                drawHidStatus("Abriendo app", entry.title, 45);
                switch (entry.action) {
                    case HID_ACT_OPEN_CMD: hidOpenRun("cmd", 900); break;
                    case HID_ACT_OPEN_PS: hidOpenRun("powershell", 1200); break;
                    case HID_ACT_OPEN_OPERA: hidOpenStartSearch("Opera GX", 1800); break;
                    case HID_ACT_OPEN_PAINT: hidOpenRun("mspaint", 1100); break;
                    case HID_ACT_OPEN_NOTEPAD: hidOpenRun("notepad", 900); break;
                    case HID_ACT_OPEN_CALC: hidOpenRun("calc", 900); break;
                    default: break;
                }
                drawHidStatus("Listo", String(entry.title) + " enviado", 100);
                delay(650);
            }
        } else if (action == HID_ACT_CMD) {
            drawHidStatus("CMD", "Abriendo simbolo del sistema", 30);
            hidOpenRun("cmd", 1000);
            uint8_t cmdSelected = 0;
            while (true) {
                uint8_t cmdIdx = runHidPadMenu("CMD TOOLS", "Comandos seguros para CMD",
                                               HID_CMD_MENU, sizeof(HID_CMD_MENU) / sizeof(HID_CMD_MENU[0]), cmdSelected);
                if (cmdIdx == 255) break;
                cmdSelected = cmdIdx;

                const HidPadEntry& entry = HID_CMD_MENU[cmdIdx];
                if (entry.action == HID_ACT_BACK) break;

                if (entry.action == HID_ACT_CTRL_C) {
                    drawHidStatus("CMD", "Enviando Ctrl+C para detener", 75);
                    hidSendCtrlC();
                    delay(220);
                    continue;
                }

                const char* command = hidCommandForEntry(entry);
                drawHidStatus("CMD", command, 75);
                hidTypeLine(command, 24);
                delay(380);
            }
        } else if (action == HID_ACT_PS) {
            drawHidStatus("PowerShell", "Abriendo terminal", 30);
            hidOpenRun("powershell", 1300);
            uint8_t psSelected = 0;
            while (true) {
                uint8_t psIdx = runHidPadMenu("POWERSHELL", "Comandos seguros para PS",
                                              HID_PS_MENU, sizeof(HID_PS_MENU) / sizeof(HID_PS_MENU[0]), psSelected);
                if (psIdx == 255) break;
                psSelected = psIdx;

                const HidPadEntry& entry = HID_PS_MENU[psIdx];
                if (entry.action == HID_ACT_BACK) break;

                if (entry.action == HID_ACT_CTRL_C) {
                    drawHidStatus("PowerShell", "Enviando Ctrl+C para detener", 75);
                    hidSendCtrlC();
                    delay(220);
                    continue;
                }

                const char* command = hidCommandForEntry(entry);
                drawHidStatus("PowerShell", command, 75);
                hidTypeLine(command, 24);
                delay(380);
            }
        } else if (action == HID_ACT_MEDIA) {
            uint8_t mediaSelected = 0;
            while (true) {
                uint8_t mediaIdx = runHidPadMenu("MULTIMEDIA", "Control para YouTube y musica",
                                                 HID_MEDIA_MENU, sizeof(HID_MEDIA_MENU) / sizeof(HID_MEDIA_MENU[0]), mediaSelected);
                if (mediaIdx == 255) break;
                mediaSelected = mediaIdx;

                const HidPadEntry& entry = HID_MEDIA_MENU[mediaIdx];
                if (entry.action == HID_ACT_BACK) break;

                if (entry.action == HID_ACT_MEDIA_VOLUME) {
                    runHidVolumeControl();
                    continue;
                }

                drawHidStatus("Multimedia", entry.title, 70);
                hidRunMediaAction(entry.action);
                delay(300);
            }
        } else if (action == HID_ACT_GUIDED_DEMO) {
            runHidDemoApp();
            return;
        }
    }

    HidKeyboard.releaseAll();
    HidConsumer.release();
    currentScreen = Screen::Home;
    setStatus("Ready");
    drawHome();
    pushFrame();
}

enum DemoLauncherAction : uint8_t {
    DEMO_ACT_BACK = 0,
    DEMO_ACT_WIFI,
    DEMO_ACT_BLE,
    DEMO_ACT_GPS_SOS,
    DEMO_ACT_PASSCODE,
    DEMO_ACT_HID,
    DEMO_ACT_IPHONE,
    DEMO_ACT_RADIO
};

const HidPadEntry DEMO_LAUNCHER_MENU[] = {
    {"WIFI LOCATOR", "Radar RSSI, metros y direccion", DEMO_ACT_WIFI},
    {"BLE RADAR", "Proximidad de dispositivos BLE", DEMO_ACT_BLE},
    {"GPS SOS", "Coordenadas grandes de emergencia", DEMO_ACT_GPS_SOS},
    {"PASSCODE SIM", "Animacion visual 9764", DEMO_ACT_PASSCODE},
    {"HID PAD", "PC control seguro", DEMO_ACT_HID},
    {"IPHONE REMOTE", "Control BLE para iPhone", DEMO_ACT_IPHONE},
    {"RADIO SCOPE", "nRF24 2.4 GHz visual", DEMO_ACT_RADIO},
    {"VOLVER", "Regresar al launcher", DEMO_ACT_BACK},
};

const char* demoActionTag(uint8_t action) {
    switch (action) {
        case DEMO_ACT_WIFI: return "WIFI";
        case DEMO_ACT_BLE: return "BLE";
        case DEMO_ACT_GPS_SOS: return "SOS";
        case DEMO_ACT_PASSCODE: return "SIM";
        case DEMO_ACT_HID: return "USB";
        case DEMO_ACT_IPHONE: return "iOS";
        case DEMO_ACT_RADIO: return "2.4G";
        default: return "EXIT";
    }
}

uint16_t demoActionColor(uint8_t action) {
    switch (action) {
        case DEMO_ACT_WIFI: return COL_GREEN;
        case DEMO_ACT_BLE: return COL_CYAN;
        case DEMO_ACT_GPS_SOS: return COL_RED;
        case DEMO_ACT_PASSCODE: return COL_AMBER;
        case DEMO_ACT_HID: return COL_TEXT;
        case DEMO_ACT_IPHONE: return COL_CYAN;
        case DEMO_ACT_RADIO: return COL_GREEN;
        default: return COL_MUTED;
    }
}

void drawDemoLauncherMenu(uint8_t selected, uint8_t scroll) {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("CYBER DEMO", "reels");
    const uint8_t count = sizeof(DEMO_LAUNCHER_MENU) / sizeof(DEMO_LAUNCHER_MENU[0]);
    const HidPadEntry& active = DEMO_LAUNCHER_MENU[selected];
    const uint16_t accent = demoActionColor(active.action);

    frame.drawRoundRect(10, 36, 300, 70, 6, accent);
    frame.fillRect(16, 42, 288, 58, 0x0004);
    frame.fillRoundRect(22, 50, 42, 28, 5, accent);
    frame.setTextColor(COL_BG, accent);
    frame.setTextDatum(MC_DATUM);
    frame.setTextSize(1);
    frame.drawString(demoActionTag(active.action), 43, 64, 2);
    frame.setTextDatum(TL_DATUM);
    drawTextOn(76, 48, fitText(active.title, 22), COL_TEXT, 0x0004);
    drawTextOn(76, 66, fitText(active.subtitle, 31), COL_CYAN, 0x0004);
    drawTextOn(76, 84, "OK lanza cuenta regresiva", COL_MUTED, 0x0004);

    const int listY = 114;
    const int rowH = 22;
    const uint8_t visible = 4;
    for (uint8_t row = 0; row < visible; row++) {
        const uint8_t idx = scroll + row;
        if (idx >= count) break;
        const int y = listY + row * rowH;
        const bool isSelected = idx == selected;
        const uint16_t rowAccent = demoActionColor(DEMO_LAUNCHER_MENU[idx].action);
        const uint16_t bg = isSelected ? rowAccent : COL_PANEL;
        const uint16_t fg = isSelected ? COL_BG : COL_TEXT;
        frame.fillRoundRect(10, y, 300, 19, 4, bg);
        frame.drawRoundRect(10, y, 300, 19, 4, isSelected ? COL_TEXT : COL_GRID);
        drawTextOn(18, y + 1, String(idx + 1), isSelected ? COL_BG : rowAccent, bg, 1);
        drawTextOn(40, y + 1, fitText(DEMO_LAUNCHER_MENU[idx].title, 22), fg, bg, 1);
        frame.setTextDatum(TR_DATUM);
        frame.setTextColor(isSelected ? COL_BG : rowAccent, bg);
        frame.drawString(demoActionTag(DEMO_LAUNCHER_MENU[idx].action), 303, y + 1, 2);
        frame.setTextDatum(TL_DATUM);
    }

    frame.fillRoundRect(12, 202, 296, 13, 4, 0x0004);
    drawTextOn(18, 201, String("REEL ") + String(selected + 1) + "/" + count + "  MODO ETICO  @pepeangelll", accent, 0x0004, 1);
    drawFooter("UP/DOWN  OK LANZAR  BACK VOLVER");
    pushFrame();
}

bool drawDemoCountdown(const HidPadEntry& entry) {
    const uint16_t accent = demoActionColor(entry.action);
    for (int i = 3; i > 0; i--) {
        frame.fillSprite(COL_BG);
        drawGrid();
        drawHeader("CYBER DEMO", "rec ready");
        frame.drawRoundRect(18, 40, 284, 156, 6, accent);
        frame.fillRect(24, 46, 272, 144, 0x0004);
        frame.setTextDatum(MC_DATUM);
        frame.setTextSize(1);
        frame.setTextColor(COL_CYAN, 0x0004);
        frame.drawString(demoActionTag(entry.action), SCREEN_W / 2, 60, 2);
        frame.setTextColor(COL_TEXT, 0x0004);
        frame.drawString(entry.title, SCREEN_W / 2, 82, 2);
        frame.setTextSize(5);
        frame.setTextColor(accent, 0x0004);
        frame.drawString(String(i), SCREEN_W / 2, 130, 2);
        frame.setTextSize(1);
        frame.setTextColor(COL_MUTED, 0x0004);
        frame.drawString("prepara camara / encuadre", SCREEN_W / 2, 176, 2);
        frame.setTextDatum(TL_DATUM);
        drawBar(50, 200, 220, 8, ((4 - i) * 33), accent);
        drawFooter("BACK CANCELA  OK ESPERA");
        pushFrame();
        toneClick(1700 + i * 350, 28);
        for (uint8_t tick = 0; tick < 8; tick++) {
            const AppAction action = inputRead();
            if (action == AppAction::Back || action == AppAction::LongSelect) {
                setStatus("Demo cancelada");
                return false;
            }
            delay(95);
        }
    }

    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("CYBER DEMO", "live");
    frame.drawRoundRect(24, 54, 272, 118, 6, accent);
    frame.setTextDatum(MC_DATUM);
    frame.setTextColor(accent, COL_BG);
    frame.setTextSize(2);
    frame.drawString("EN VIVO", SCREEN_W / 2, 96, 2);
    frame.setTextSize(1);
    frame.setTextColor(COL_TEXT, COL_BG);
    frame.drawString(entry.title, SCREEN_W / 2, 136, 2);
    frame.setTextDatum(TL_DATUM);
    drawFooter("INICIANDO DEMO...");
    pushFrame();
    toneClick(3600, 60);
    delay(260);
    return true;
}

void runCyberDemoLauncherApp() {
    currentScreen = Screen::DemoLauncher;
    static uint8_t selected = 0;
    uint8_t scroll = selected >= 4 ? selected - 3 : 0;
    const uint8_t count = sizeof(DEMO_LAUNCHER_MENU) / sizeof(DEMO_LAUNCHER_MENU[0]);
    drawDemoLauncherMenu(selected, scroll);

    while (true) {
        const AppAction action = inputRead();
        if (action == AppAction::Back || action == AppAction::LongSelect) break;

        if (action == AppAction::Up) {
            selected = (selected == 0) ? count - 1 : selected - 1;
            toneClick();
        } else if (action == AppAction::Down) {
            selected = (selected + 1) % count;
            toneClick();
        } else if (action == AppAction::Select) {
            const HidPadEntry& entry = DEMO_LAUNCHER_MENU[selected];
            if (entry.action == DEMO_ACT_BACK) break;
            toneClick(3200, 16);
            if (!drawDemoCountdown(entry)) {
                drawDemoLauncherMenu(selected, scroll);
                continue;
            }
            switch (entry.action) {
                case DEMO_ACT_WIFI: runWifiLocatorApp(); break;
                case DEMO_ACT_BLE: runBleDeviceRadarApp(); break;
                case DEMO_ACT_GPS_SOS: runGpsSosApp(); break;
                case DEMO_ACT_PASSCODE: runPasscodeSimApp(); break;
                case DEMO_ACT_HID: runHidPadApp(); break;
                case DEMO_ACT_IPHONE: runIphoneRemoteApp(); break;
                case DEMO_ACT_RADIO: runRadioScopeApp(); break;
                default: break;
            }
            currentScreen = Screen::DemoLauncher;
        } else {
            delay(4);
            continue;
        }

        if (selected < scroll) scroll = selected;
        if (selected >= scroll + 4) scroll = selected - 3;
        drawDemoLauncherMenu(selected, scroll);
        delay(4);
    }

    currentScreen = Screen::Home;
    setStatus("Ready");
    drawHome();
    pushFrame();
}

void drawBattery() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("BATTERY METER", "1S Li-ion");

    frame.drawRoundRect(26, 55, 268, 102, 6, batteryPct < 20 ? COL_RED : COL_GREEN);
    frame.drawRect(294, 88, 12, 34, batteryPct < 20 ? COL_RED : COL_GREEN);
    drawBar(46, 82, 228, 46, batteryPct, batteryPct < 20 ? COL_RED : COL_GREEN);

    frame.setTextColor(COL_TEXT, COL_BG);
    frame.setTextSize(2);
    frame.setTextDatum(MC_DATUM);
    frame.drawString(String(batteryVolts, 2) + "V", SCREEN_W / 2, 170, 2);
    frame.setTextSize(1);
    frame.drawString(String(batteryPct) + "% estimated from 2.2k / 1k divider", SCREEN_W / 2, 196, 2);
    frame.setTextDatum(TL_DATUM);
    drawFooter("BACK EXIT");
}

void drawAbout() {
    frame.fillSprite(COL_BG);
    drawGrid();
    drawHeader("ABOUT TEMPLATE", "v0.1");

    drawText(12, 42, "Base for individual CYBERDECK-S3 apps.", COL_GREEN);
    drawText(12, 66, "Pins match the finished MINI firmware.", COL_CYAN);
    drawText(12, 92, "Controls:", COL_AMBER);
    drawText(28, 112, "UP/DOWN + encoder: navigate", COL_TEXT);
    drawText(28, 132, "OK or encoder SW: select", COL_TEXT);
    drawText(28, 152, "BACK or OK hold: exit", COL_TEXT);
    drawText(12, 184, "Next app can replace one screen or this whole launcher.", COL_MUTED);
    drawFooter("BACK EXIT");
}

void renderCurrent() {
    switch (currentScreen) {
        case Screen::SystemPulse: drawSystemPulse(); break;
        case Screen::GpsRadar: drawGpsRadar(); break;
        case Screen::WifiLocator: drawHome(); break;
        case Screen::BleRadar: drawHome(); break;
        case Screen::GpsSos: drawHome(); break;
        case Screen::DemoLauncher: drawHome(); break;
        case Screen::SdVault: drawSdVault(); break;
        case Screen::RadioScope: drawRadioScope(); break;
        case Screen::PasscodeSim: drawHome(); break;
        case Screen::HidDemo: drawHome(); break;
        case Screen::IphoneRemote: drawHome(); break;
        case Screen::Battery: drawBattery(); break;
        case Screen::About: drawAbout(); break;
        case Screen::Home:
        default: drawHome(); break;
    }
    pushFrame();
}

void goHome() {
    currentScreen = Screen::Home;
    setStatus("Ready");
    toneClick(1600, 10);
    renderCurrent();
}

void handleAction(AppAction action) {
    if (action == AppAction::None) return;

    if (currentScreen == Screen::Home) {
        if (action == AppAction::Up) {
            menuIndex = (menuIndex == 0) ? MENU_COUNT - 1 : menuIndex - 1;
            toneClick();
        } else if (action == AppAction::Down) {
            menuIndex = (menuIndex + 1) % MENU_COUNT;
            toneClick();
        } else if (action == AppAction::Select) {
            if (MENU[menuIndex].screen == Screen::GpsRadar) {
                toneClick(3200, 18);
                runGpsRadarApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::WifiLocator) {
                toneClick(3200, 18);
                runWifiLocatorApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::BleRadar) {
                toneClick(3200, 18);
                runBleDeviceRadarApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::GpsSos) {
                toneClick(3200, 18);
                runGpsSosApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::DemoLauncher) {
                toneClick(3200, 18);
                runCyberDemoLauncherApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::RadioScope) {
                toneClick(3200, 18);
                runRadioScopeApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::PasscodeSim) {
                runPasscodeSimApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::HidDemo) {
                runHidPadApp();
                return;
            } else if (MENU[menuIndex].screen == Screen::IphoneRemote) {
                runIphoneRemoteApp();
                return;
            } else {
                currentScreen = MENU[menuIndex].screen;
                setStatus("Opened app");
            }
            toneClick(3200, 18);
        }
        renderCurrent();
        return;
    }

    if (action == AppAction::Back || action == AppAction::LongSelect) {
        goHome();
        return;
    }

    if (currentScreen == Screen::GpsRadar && action == AppAction::Select) {
        saveGpsSnapshot();
        toneClick(3000, 18);
        renderCurrent();
        return;
    }

    if (currentScreen == Screen::SdVault && action == AppAction::Select) {
        ensureSdFolders();
        toneClick(3000, 18);
        renderCurrent();
        return;
    }

    if (currentScreen == Screen::RadioScope && action == AppAction::Select) {
        radioPaused = !radioPaused;
        setStatus(radioPaused ? "Radio scope paused" : "Radio scope running");
        toneClick(2200, 18);
        renderCurrent();
    }
}

void setupPins() {
    pinMode(CD_TFT_CS, OUTPUT);
    digitalWrite(CD_TFT_CS, HIGH);
    pinMode(CD_NRF1_CSN, OUTPUT);
    digitalWrite(CD_NRF1_CSN, HIGH);
    pinMode(CD_NRF2_CSN, OUTPUT);
    digitalWrite(CD_NRF2_CSN, HIGH);
    pinMode(CD_NRF1_CE, OUTPUT);
    digitalWrite(CD_NRF1_CE, LOW);
    pinMode(CD_NRF2_CE, OUTPUT);
    digitalWrite(CD_NRF2_CE, LOW);
    pinMode(CD_VBAT_ADC, INPUT);
    analogSetPinAttenuation(CD_VBAT_ADC, ADC_11db);

    if (CD_TFT_BL >= 0) {
        pinMode(CD_TFT_BL, OUTPUT);
        digitalWrite(CD_TFT_BL, HIGH);
    }

    pinMode(CD_BUZZER, OUTPUT);
    ledcSetup(0, 2400, 8);
    ledcAttachPin(CD_BUZZER, 0);
    ledcWriteTone(0, 0);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    bootMs = millis();

    setupPins();
    inputBegin();
    beginGps();

    pinMode(CD_TFT_RST, OUTPUT);
    digitalWrite(CD_TFT_RST, LOW);
    delay(80);
    digitalWrite(CD_TFT_RST, HIGH);
    delay(120);

    SPI.begin(CD_SPI_SCK, CD_SPI_MISO, CD_SPI_MOSI);
    tft.begin();
    tft.invertDisplay(false);
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);

    frame.setColorDepth(FRAME_COLOR_DEPTH);
    frameReady = frame.createSprite(SCREEN_W, SCREEN_H) != nullptr;
    if (!frameReady) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Sprite alloc failed", 12, 12, 2);
        return;
    }

    drawBoot();
    batteryVolts = readBatteryVolts();
    batteryPct = batteryPercent(batteryVolts);
    beginSdCard();
    beginRadios();
    setStatus("Template ready");
    renderCurrent();
}

void loop() {
    updateSensors();
    if (currentScreen == Screen::RadioScope) scanRadioBurst(14);

    const AppAction action = inputRead();
    handleAction(action);

    const uint16_t interval = (currentScreen == Screen::RadioScope) ? 90 : 350;
    if (millis() - lastRenderMs >= interval) {
        lastRenderMs = millis();
        renderCurrent();
    }

    delay(4);
}
