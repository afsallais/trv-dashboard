/*
  TRV UI for Elecrow CrowPanel ESP32-S3 5.79" e-paper (792x272, dual SSD1683)
  Libraries (Library Manager): "GxEPD2" (ZinggJM, latest) and "Adafruit GFX Library"
  Board: ESP32S3 Dev Module | Flash 8MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled

  Buttons (pins from the Elecrow docs, active-LOW, interrupt driven):
            MENU (IO2) = switch screen: TRV -> Timetable -> Calendar -> Settings -> History
            TRV screen:      ROTARY UP (IO6) / DOWN (IO4) = target +/- 0.5 C (hold = repeat)
                             ROTARY PRESS (IO5) = next mode      EXIT (IO1) = toggle "window open" (demo)
            Calendar screen: ROTARY UP / DOWN = previous / next day (hold = repeat)
                             ROTARY PRESS = bookmark / un-bookmark the day (saved in flash)
                             EXIT = jump to today.
            Timetable screen: ROTARY UP / DOWN = previous / next day (today..+6), OK = re-download the .ics, EXIT = today.
            Settings screen: ROTARY UP / DOWN = select item, OK = edit / save the value, EXIT = cancel edit or back to TRV.
                             Values are saved in flash. "Reset defaults" needs OK twice.
            History screen:  ROTARY UP / DOWN = range 24h / 30d / 6m, OK = show all / room / target / heating only, EXIT = reset view.
                             Logs room temp, target and valve opening itself (needs Wi-Fi time): 24 h in RAM, daily records in flash.
            Presses are also logged on Serial (115200).
  Timetable: paste your .ics / webcal link into ICAL_URL (DTSTART/DTEND, SUMMARY, LOCATION, weekly/daily RRULE + EXDATE supported;
  floating or Z times; zone-specific TZID times are read as local time).
  Calendar needs Wi-Fi for the date/time (NTP) and weather (Open-Meteo, set WX_CITY/WX_LAT/WX_LON; now Delft, NL).
  Sensor + valve motor are simulated (USE_SIM 1). Replace readSensors() / driveValve()
  with your real hardware (e.g. SHT4x / AHT20 on the header pins, motor driver).
*/
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <SPI.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

// ---------- user settings ----------
#define USE_SIM 1
const char* WIFI_SSID = "";            // leave empty = no Wi-Fi/NTP (clock shows --:--)
const char* WIFI_PASS = "";
const char* TZ_INFO   = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Amsterdam
const char* ROOM_NAME = "TRV - Living Room";
const char* ICAL_URL  = "";    // class timetable: paste your .ics / webcal:// subscription link (Google, Outlook, university portal...)
const char* WX_CITY   = "Delft";                           // calendar screen weather (Open-Meteo, no API key)
const char* WX_LAT    = "52.01";
const char* WX_LON    = "4.36";

// ---------- CrowPanel 5.79 pins ----------
#define EPD_BUSY 48
#define EPD_RST  47
#define EPD_DC   46
#define EPD_CS   45
#define EPD_SCK  12
#define EPD_MOSI 11
#define EPD_PWR   7      // must be HIGH or the panel stays dead
#define BTN_UP    6
#define BTN_DOWN  4
#define BTN_PRESS 5
#define BTN_MENU  2
#define BTN_EXIT  1

GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT>
  display(GxEPD2_579_GDEY0579T93(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

#define BLK GxEPD_BLACK
#define WHT GxEPD_WHITE

// ---------- state ----------
struct Slot { float from, to, temp; };
Slot SCH[] = {{0,6,17},{6,8,21},{8,17,18},{17,22,22.5},{22,24,17}};
const float DEF_SCH[5] = {17, 21, 18, 22.5, 17};
const char* MODE_NAME[4] = {"Auto", "Eco", "Comfort", "Frost (5C)"};
float MODE_TEMP[4] = {0, 18, 22, 5};
const float DEF_MODE[4] = {0, 18, 22, 5};
float tOff = 0;                  // sensor calibration offset (C), set in Settings

float cur = 21.8, hum = 49;
float ovr = 0; bool hasOvr = false; int ovrSlot = -1;
int  mode = 0;
bool winOpen = false;
int  battPct = -1; // unknown until a real battery measurement is connected
float wxTemp = NAN, wxHi = NAN, wxLo = NAN, wxWind = NAN, wxHum = NAN; int wxCode = -1;
bool dirty = true, headerDirty = false, calendarClockDirty = false, forceFull = true;
int  partialCount = 0, lastMin = -1;
int  lastSetSelDrawn = -1;
uint32_t lastDraw = 0, lastSim = 0;
float curC() { return cur + tOff; }   // room temperature incl. calibration offset

// ---------- logic ----------
float hourNow() {
  struct tm t; if (!getLocalTime(&t, 0)) return 12.0;
  return t.tm_hour + t.tm_min / 60.0;
}
int slotNow() {
  float h = hourNow();
  for (int i = 0; i < 5; i++) if (h >= SCH[i].from && h < SCH[i].to) return i;
  return 0;
}
float eff() {
  if (mode != 0) return MODE_TEMP[mode];
  if (hasOvr && slotNow() != ovrSlot) hasOvr = false;   // override ends at next schedule slot
  return hasOvr ? ovr : SCH[slotNow()].temp;
}
int valvePct() {
  if (winOpen) return 0;
  return constrain((int)((eff() - curC()) * 80), 0, 100);
}
void adjust(float d) {
  ovr = constrain(round((eff() + d) * 2) / 2.0, 5.0, 30.0);
  hasOvr = true; ovrSlot = slotNow(); mode = 0; dirty = true;
}
void readSensors() {
#if USE_SIM
  if (millis() - lastSim < 2000) return;
  lastSim = millis();
  int v = valvePct();
  cur += winOpen ? -0.08 : (v > 0 ? 0.08 * v / 100.0 : -0.02);
  cur = max(10.0f, cur);
#else
  // TODO: cur = temperature from your sensor; hum = humidity; battPct = battery level
#endif
}
void driveValve(int pct) {
  // TODO: drive your valve motor to 'pct' % open
}

// ---------- drawing helpers ----------
void txt(int x, int y, const GFXfont* f, const char* s, int al = 0, uint16_t c = BLK, uint8_t sz = 1) {
  display.setFont(f); display.setTextSize(sz); display.setTextColor(c);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  int px = al == 0 ? x : (al == 1 ? x - (int)w / 2 - x1 : x - (int)w - x1);
  display.setCursor(px, y); display.print(s);
}
// number + degree ring + "C"  (GFX fonts have no degree sign)
void tempStr(int x, int y, float v, int dec, const GFXfont* nf, uint8_t ns, const GFXfont* uf, int r, int al, uint16_t col = BLK) {
  char b[12]; dtostrf(v, 0, dec, b);
  int16_t x1, y1; uint16_t w, h, cw, ch;
  display.setFont(nf); display.setTextSize(ns); display.getTextBounds(b, 0, 0, &x1, &y1, &w, &h);
  display.setFont(uf); display.setTextSize(1);  display.getTextBounds("C", 0, 0, &x1, &y1, &cw, &ch);
  int total = w + 4 + 2 * r + 2 + cw;
  int sx = al == 0 ? x : (al == 1 ? x - total / 2 : x - total);
  txt(sx, y, nf, b, 0, col, ns);
  int topY = y - h;                              // top of the digits
  display.drawCircle(sx + w + 4 + r, topY + r + 1, r, col);
  if (r > 3) display.drawCircle(sx + w + 4 + r, topY + r + 1, r - 1, col);
  txt(sx + w + 4 + 2 * r + 2, topY + ch, uf, "C", 0, col, 1);
}
void arc(int cx, int cy, int r, int a0, int a1) {
  for (int a = a0; a <= a1; a++) { float t = a * DEG_TO_RAD; display.fillCircle(cx + r * cos(t), cy + r * sin(t), 1, BLK); }
}
void vline2(int x, int y0, int y1) { display.drawFastVLine(x, y0, y1 - y0, BLK); display.drawFastVLine(x + 1, y0, y1 - y0, BLK); }
void thickRR(int x, int y, int w, int h, int r) { display.drawRoundRect(x, y, w, h, r, BLK); display.drawRoundRect(x + 1, y + 1, w - 2, h - 2, r, BLK); }

void iconFlame(int cx, int cy, uint16_t c, uint16_t i) { display.fillCircle(cx, cy + 3, 7, c); display.fillTriangle(cx - 6, cy + 1, cx + 6, cy + 1, cx, cy - 11, c); display.fillCircle(cx, cy + 4, 3, i); }
void iconLeaf(int cx, int cy, uint16_t c, uint16_t i) { display.fillRoundRect(cx - 9, cy - 7, 18, 14, 7, c); display.drawLine(cx - 8, cy + 8, cx + 4, cy - 4, i); display.drawLine(cx - 8, cy + 7, cx + 3, cy - 4, i); }
void iconSofa(int cx, int cy, uint16_t c) { display.fillRoundRect(cx - 12, cy - 2, 24, 9, 2, c); display.fillRoundRect(cx - 8, cy - 8, 16, 7, 2, c); display.fillRect(cx - 9, cy + 7, 3, 3, c); display.fillRect(cx + 6, cy + 7, 3, 3, c); }
void iconSnow(int cx, int cy, uint16_t c) {
  for (int a = 0; a < 180; a += 60) { float t = a * DEG_TO_RAD; int dx = 10 * cos(t), dy = 10 * sin(t);
    display.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, c); display.drawLine(cx - dx + 1, cy - dy, cx + dx + 1, cy + dy, c); }
}
void iconGear(int cx, int cy) {
  for (int a = 0; a < 360; a += 45) { float t = a * DEG_TO_RAD; display.fillCircle(cx + 9 * cos(t), cy + 9 * sin(t), 2, BLK); }
  display.fillCircle(cx, cy, 8, BLK); display.fillCircle(cx, cy, 3, WHT);
}

// ---------- shared header (icon 0 = house, 1 = calendar) ----------
void drawHeader(const char* title, int icon) {
  struct tm t; bool haveTime = getLocalTime(&t, 0); char b[40];
  if (icon == 0) { display.drawLine(26, 17, 36, 8, BLK); display.drawLine(36, 8, 46, 17, BLK); display.drawRect(29, 17, 14, 9, BLK); }
  else if (icon == 4) { display.drawLine(27, 7, 27, 26, BLK); display.drawLine(27, 26, 46, 26, BLK); display.drawLine(27, 22, 33, 15, BLK);
                        display.drawLine(33, 15, 38, 19, BLK); display.drawLine(38, 19, 46, 9, BLK); display.drawLine(27, 23, 33, 16, BLK); display.drawLine(33, 16, 38, 20, BLK); display.drawLine(38, 20, 46, 10, BLK); }
  else if (icon == 3) { iconGear(36, 17); }
  else if (icon == 2) { display.drawCircle(36, 16, 10, BLK); display.drawCircle(36, 16, 9, BLK); display.drawLine(36, 16, 36, 9, BLK); display.drawLine(37, 16, 37, 9, BLK); display.drawLine(36, 16, 41, 19, BLK); }
  else { display.drawRoundRect(27, 9, 18, 17, 2, BLK); display.drawFastHLine(27, 14, 18, BLK); display.drawFastHLine(27, 15, 18, BLK);
         display.fillRect(32, 6, 2, 6, BLK); display.fillRect(39, 6, 2, 6, BLK); display.fillRect(31, 18, 3, 3, BLK); display.fillRect(37, 18, 3, 3, BLK); }
  txt(64, 23, &FreeSansBold9pt7b, title);
  if (haveTime) strftime(b, sizeof b, "%a, %d %b   %H:%M", &t); else strcpy(b, "--:--");
  txt(396, 23, &FreeSansBold9pt7b, b, 1);
  txt(510, 23, &FreeSansBold9pt7b, "Delft");
  if (!isnan(wxTemp)) tempStr(586, 23, wxTemp, 0, &FreeSansBold9pt7b, 1, &FreeSansBold9pt7b, 2, 2);
  else txt(586, 23, &FreeSansBold9pt7b, "--", 2);
  arc(595, 24, 12, 228, 312); arc(595, 24, 7, 228, 312); display.fillCircle(595, 23, 2, BLK);
  display.fillCircle(634, 22, 3, BLK); display.fillCircle(640, 19, 5, BLK); display.fillCircle(646, 22, 3, BLK); display.fillRect(634, 22, 13, 4, BLK);
  txt(653, 23, &FreeSans9pt7b, WiFi.status() == WL_CONNECTED ? "Connected" : "Offline");
  display.drawRoundRect(742, 10, 26, 14, 3, BLK); display.fillRect(769, 14, 3, 6, BLK);
  if (battPct >= 0) {
    int pct = constrain(battPct, 0, 100);
    display.fillRect(745, 13, (20 * pct) / 100, 8, BLK);
    snprintf(b, sizeof b, "%d%%", pct);
  } else strcpy(b, "--%");
  txt(736, 22, &FreeSans9pt7b, b, 2);
  display.drawFastHLine(0, 32, 792, BLK); display.drawFastHLine(0, 33, 792, BLK);
}

// ---------- screen ----------
void drawUI() {
  struct tm t; bool haveTime = getLocalTime(&t, 0);
  float e = eff(); int v = valvePct(); bool heat = v > 0 && !winOpen;
  char b[40];

  display.fillScreen(WHT);
  display.drawRect(0, 0, 792, 272, BLK); display.drawRect(1, 1, 790, 270, BLK);

  drawHeader(ROOM_NAME, 0);

  // left: room temperature
  display.drawRoundRect(39, 62, 12, 56, 6, BLK); display.drawRoundRect(40, 63, 10, 54, 5, BLK);
  display.fillCircle(45, 121, 13, BLK); display.fillRoundRect(42, 86, 6, 36, 3, BLK);
  for (int i = 0; i < 5; i++) display.fillRect(57, 70 + i * 8, 5, 2, BLK);
  tempStr(78, 103, curC(), 1, &FreeSansBold24pt7b, 1, &FreeSansBold12pt7b, 4, 0);
  display.fillTriangle(92, 118, 84, 131, 100, 131, BLK); display.fillCircle(92, 131, 8, BLK);
  snprintf(b, sizeof b, "%d %% RH", (int)hum); txt(110, 137, &FreeSansBold9pt7b, b);
  display.drawFastHLine(27, 155, 191, BLK);
  for (int i = 0; i < 4; i++) display.fillRect(33 + i * 9, 191 - (7 + i * 5), 6, 7 + i * 5, BLK);
  txt(88, 187, &FreeSansBold9pt7b, "Heating:");
  if (heat) { display.fillRoundRect(164, 169, 44, 24, 6, BLK); txt(186, 187, &FreeSansBold9pt7b, "ON", 1, WHT); }
  else      { thickRR(164, 169, 44, 24, 6); txt(186, 187, &FreeSansBold9pt7b, "OFF", 1); }
  vline2(236, 47, 200);

  // middle: target
  txt(394, 61, &FreeSansBold12pt7b, "Target Temperature", 1);
  thickRR(257, 86, 44, 44, 9); display.fillRect(269, 107, 20, 3, BLK);
  thickRR(496, 86, 44, 44, 9); display.fillRect(508, 107, 20, 3, BLK); display.fillRect(517, 98, 3, 20, BLK);
  tempStr(382, 126, e, 1, &FreeSansBold18pt7b, 2, &FreeSansBold12pt7b, 4, 1);
  float f = (constrain(e, 16.0f, 28.0f) - 16) / 12.0; int kx = 269 + (int)(262 * f);
  display.drawRoundRect(269, 155, 262, 7, 3, BLK); display.fillRoundRect(269, 155, max(8, kx - 269), 7, 3, BLK);
  display.fillCircle(kx, 158, 11, BLK); display.fillCircle(kx, 158, 7, WHT);
  display.drawFastVLine(270, 172, 6, BLK); display.drawFastVLine(531, 172, 6, BLK);
  tempStr(276, 192, 16, 0, &FreeSans9pt7b, 1, &FreeSans9pt7b, 2, 1);
  tempStr(kx, 194, e, 1, &FreeSans9pt7b, 1, &FreeSans9pt7b, 2, 1);
  tempStr(525, 192, 28, 0, &FreeSans9pt7b, 1, &FreeSans9pt7b, 2, 1);
  vline2(561, 47, 200);

  // right: valve / schedule / window
  thickRR(585, 80, 9, 16, 2); thickRR(618, 80, 9, 16, 2); thickRR(594, 77, 24, 22, 7);
  display.fillRect(605, 62, 3, 15, BLK); thickRR(599, 54, 14, 8, 2);
  txt(645, 60, &FreeSans9pt7b, "Valve Opening");
  snprintf(b, sizeof b, "%d %%", v); txt(644, 93, &FreeSansBold18pt7b, b);
  display.drawRoundRect(632, 102, 135, 9, 4, BLK); if (v > 0) display.fillRoundRect(632, 102, max(8, 135 * v / 100), 9, 4, BLK);
  display.drawRoundRect(585, 131, 26, 23, 3, BLK); display.drawFastHLine(585, 138, 26, BLK); display.fillRect(591, 128, 2, 6, BLK); display.fillRect(605, 128, 2, 6, BLK);
  for (int r = 0; r < 2; r++) for (int c = 0; c < 3; c++) display.fillRect(589 + c * 6, 142 + r * 6, 3, 3, BLK);
  snprintf(b, sizeof b, "Schedule: %s", mode == 0 ? "Home" : MODE_NAME[mode]); if (mode == 3) strcpy(b, "Schedule: Frost");
  txt(625, 147, &FreeSans9pt7b, b);
  display.drawLine(762, 140, 767, 145, BLK); display.drawLine(767, 145, 762, 150, BLK); display.drawLine(763, 140, 768, 145, BLK); display.drawLine(768, 145, 763, 150, BLK);
  display.drawRect(585, 170, 26, 24, BLK); display.drawRect(586, 171, 24, 22, BLK); display.drawFastVLine(598, 170, 24, BLK); display.drawFastHLine(585, 182, 26, BLK);
  txt(625, 188, &FreeSans9pt7b, winOpen ? "Window: Open" : "Window: Closed");
  display.fillCircle(764, 182, 10, BLK);
  if (winOpen) { display.fillRect(763, 175, 3, 8, WHT); display.fillRect(763, 185, 3, 3, WHT); }
  else { display.drawLine(759, 182, 763, 186, WHT); display.drawLine(763, 186, 770, 178, WHT); display.drawLine(759, 183, 763, 187, WHT); display.drawLine(763, 187, 770, 179, WHT); }

  // bottom mode bar
  display.drawFastHLine(16, 212, 760, BLK);
  const int xs[5] = {17, 164, 304, 459, 621}, io[4] = {-23, -19, -38, -42}, to[4] = {-1, 3, -14, -18};
  thickRR(17, 222, 604, 38, 8);
  for (int i = 0; i < 4; i++) {
    int mid = (xs[i] + xs[i + 1]) / 2; bool on = (mode == i); uint16_t c = on ? WHT : BLK, bg = on ? BLK : WHT;
    if (on) display.fillRoundRect(xs[i], 222, xs[i + 1] - xs[i], 38, 8, BLK); else if (i > 0) display.drawFastVLine(xs[i], 224, 34, BLK);
    int ix = mid + io[i], iy = 241;
    if (i == 0) iconFlame(ix, iy, c, bg); else if (i == 1) iconLeaf(ix, iy, c, bg); else if (i == 2) iconSofa(ix, iy, c); else iconSnow(ix, iy, c);
    char lb[16]; if (i == 3) snprintf(lb, sizeof lb, "Frost (%dC)", (int)MODE_TEMP[3]); else strcpy(lb, MODE_NAME[i]);
    txt(mid + to[i], 247, &FreeSansBold9pt7b, lb, 0, c);
  }
  vline2(638, 224, 258); iconGear(680, 241); txt(703, 247, &FreeSansBold9pt7b, "Settings");
}

// ---------- calendar dashboard ----------
int screen = 0;                                   // 0 = TRV, 1 = calendar
Preferences prefs;
uint32_t bm[60]; int bmN = 0;                     // bookmarked dates as yyyymmdd (kept in flash)
int curY = 0, curM = 0, curD = 0; bool curInit = false;
const char* MONTHS[12] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
const char* MON3[12]   = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

void loadBookmarks() { prefs.begin("trv", false); bmN = prefs.getBytesLength("bm") / 4; if (bmN > 60) bmN = 60; if (bmN) prefs.getBytes("bm", bm, bmN * 4); }
void saveBookmarks() { prefs.putBytes("bm", bm, bmN * 4); }
uint32_t curKey() { return curY * 10000UL + curM * 100 + curD; }
bool isBm(uint32_t k) { for (int i = 0; i < bmN; i++) if (bm[i] == k) return true; return false; }
void toggleBm() {
  uint32_t k = curKey();
  for (int i = 0; i < bmN; i++) if (bm[i] == k) { bm[i] = bm[--bmN]; saveBookmarks(); dirty = true; return; }
  if (bmN < 60) { bm[bmN++] = k; saveBookmarks(); }
  dirty = true;
}
void setCursorToday() { struct tm t; if (!getLocalTime(&t, 0)) return; curY = t.tm_year + 1900; curM = t.tm_mon + 1; curD = t.tm_mday; curInit = true; dirty = true; }
void moveCursor(int n) {
  if (!curInit) { setCursorToday(); return; }
  struct tm t = {}; t.tm_year = curY - 1900; t.tm_mon = curM - 1; t.tm_mday = curD + n; t.tm_hour = 12; t.tm_isdst = -1; mktime(&t);
  curY = t.tm_year + 1900; curM = t.tm_mon + 1; curD = t.tm_mday; dirty = true;
}
int daysIn(int y, int m) { static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31}; return (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0)) ? 29 : d[m - 1]; }
int dayDiff(uint32_t key, const struct tm& now) {
  struct tm a = {}; a.tm_year = key / 10000 - 1900; a.tm_mon = (key / 100) % 100 - 1; a.tm_mday = key % 100; a.tm_hour = 12; a.tm_isdst = -1;
  struct tm n = now; n.tm_hour = 12; n.tm_min = 0; n.tm_sec = 0; n.tm_isdst = -1;
  return (int)lround(difftime(mktime(&a), mktime(&n)) / 86400.0);
}

float jnum(const String& s, const char* key, int from) { int i = s.indexOf(key, from); return i < 0 ? NAN : s.substring(i + strlen(key)).toFloat(); }
bool fetchWeather() {
  WiFiClientSecure c; c.setInsecure(); HTTPClient h; h.setTimeout(8000);
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + WX_LAT + "&longitude=" + WX_LON +
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1";
  if (!h.begin(c, url)) return false;
  if (h.GET() != 200) { h.end(); return false; }
  String s = h.getString(); h.end();
  int p = s.indexOf("\"current\":{"), q = s.indexOf("\"daily\":{"); if (p < 0 || q < 0) return false;
  wxTemp = jnum(s, "\"temperature_2m\":", p); wxHum = jnum(s, "\"relative_humidity_2m\":", p);
  wxCode = (int)jnum(s, "\"weather_code\":", p); wxWind = jnum(s, "\"wind_speed_10m\":", p);
  wxHi = jnum(s, "\"temperature_2m_max\":[", q); wxLo = jnum(s, "\"temperature_2m_min\":[", q);
  return !isnan(wxTemp);
}
int wxCat(int c) { if (c == 0) return 0; if (c <= 2) return 1; if (c == 3) return 2; if (c == 45 || c == 48) return 3; if (c >= 95) return 6; if (c >= 71 && c <= 77 || c == 85 || c == 86) return 5; return 4; }
const char* wxName(int c) { static const char* n[7] = {"Clear sky","Partly cloudy","Overcast","Fog","Rain","Snow","Thunderstorm"}; return n[wxCat(c)]; }
void cloud(int x, int y) { display.fillCircle(x - 9, y + 4, 8, BLK); display.fillCircle(x + 3, y - 2, 11, BLK); display.fillCircle(x + 14, y + 5, 7, BLK); display.fillRect(x - 9, y + 5, 24, 7, BLK); }
void wxIcon(int x, int y, int code) {
  int k = wxCat(code);
  if (k == 0 || k == 1) { int sx = k ? x - 10 : x, sy = k ? y - 8 : y, r = k ? 7 : 10;
    display.fillCircle(sx, sy, r, BLK); for (int a = 0; a < 360; a += 45) { float t = a * DEG_TO_RAD; display.drawLine(sx + (r + 4) * cos(t), sy + (r + 4) * sin(t), sx + (r + 9) * cos(t), sy + (r + 9) * sin(t), BLK); } }
  if (k >= 1) cloud(x + (k == 1 ? 4 : 0), y + (k == 1 ? 6 : 0) - 4);
  if (k == 3) for (int i = 0; i < 3; i++) display.fillRect(x - 18, y + 12 + i * 6, 36, 3, BLK);
  if (k == 4 || k == 6) for (int i = 0; i < 3; i++) display.drawLine(x - 10 + i * 10, y + 14, x - 14 + i * 10, y + 22, BLK);
  if (k == 5) for (int i = 0; i < 3; i++) display.fillCircle(x - 10 + i * 10, y + 19, 2, BLK);
  if (k == 6) display.fillTriangle(x + 2, y + 10, x - 6, y + 24, x + 4, y + 20, WHT);
}
void hand(int cx, int cy, float deg, int len, int w) {
  float a = deg * DEG_TO_RAD, s = sin(a), c = cos(a);
  display.fillTriangle(cx + len * s, cy - len * c, cx + w * c, cy + w * s, cx - w * c, cy - w * s, BLK);
}

void drawCalendar() {
  struct tm now; bool haveTime = getLocalTime(&now, 0); char b[48];
  display.fillScreen(WHT);
  display.drawRect(0, 0, 792, 272, BLK); display.drawRect(1, 1, 790, 270, BLK);
  drawHeader("Calendar", 1);
  if (!haveTime) { txt(396, 140, &FreeSansBold12pt7b, "No time yet", 1); txt(396, 170, &FreeSans9pt7b, "Set WIFI_SSID / WIFI_PASS in the sketch", 1); return; }
  if (!curInit) setCursorToday();

  // analog + digital clock
  int cx = 118, cy = 116, R = 64;
  display.drawCircle(cx, cy, R, BLK); display.drawCircle(cx, cy, R - 1, BLK);
  for (int i = 0; i < 12; i++) { float t = i * 30 * DEG_TO_RAD; int in = (i % 3 == 0) ? R - 13 : R - 8;
    display.drawLine(cx + in * sin(t), cy - in * cos(t), cx + (R - 3) * sin(t), cy - (R - 3) * cos(t), BLK);
    if (i % 3 == 0) display.drawLine(cx + in * sin(t) + 1, cy - in * cos(t), cx + (R - 3) * sin(t) + 1, cy - (R - 3) * cos(t), BLK); }
  hand(cx, cy, ((now.tm_hour % 12) + now.tm_min / 60.0) * 30, 36, 3); hand(cx, cy, now.tm_min * 6, 54, 2);
  display.fillCircle(cx, cy, 5, BLK); display.fillCircle(cx, cy, 2, WHT);
  snprintf(b, sizeof b, "%02d:%02d", now.tm_hour, now.tm_min); txt(cx, 226, &FreeSansBold18pt7b, b, 1);
  strftime(b, sizeof b, "%A %d %b", &now); txt(cx, 250, &FreeSans9pt7b, b, 1);
  vline2(236, 44, 264);

  // month grid (Monday first)
  snprintf(b, sizeof b, "%s %d", MONTHS[curM - 1], curY); txt(406, 60, &FreeSansBold12pt7b, b, 1);
  const char* wd[7] = {"Mo","Tu","We","Th","Fr","Sa","Su"};
  for (int c = 0; c < 7; c++) txt(252 + c * 44 + 22, 80, &FreeSansBold9pt7b, wd[c], 1);
  display.drawFastHLine(252, 85, 308, BLK);
  struct tm f = {}; f.tm_year = curY - 1900; f.tm_mon = curM - 1; f.tm_mday = 1; f.tm_hour = 12; f.tm_isdst = -1; mktime(&f);
  int off = (f.tm_wday + 6) % 7, dim = daysIn(curY, curM);
  for (int d = 1; d <= dim; d++) {
    int col = (off + d - 1) % 7, row = (off + d - 1) / 7, x = 252 + col * 44 + 22, ry = 88 + row * 27;
    bool today = (curY == now.tm_year + 1900 && curM == now.tm_mon + 1 && d == now.tm_mday);
    uint32_t key = curY * 10000UL + curM * 100 + d;
    if (today) display.fillRoundRect(x - 17, ry + 1, 34, 25, 6, BLK);
    snprintf(b, sizeof b, "%d", d); txt(x, ry + 18, &FreeSansBold9pt7b, b, 1, today ? WHT : BLK);
    if (isBm(key)) display.fillCircle(x, ry + 23, 2, today ? WHT : BLK);
    if (d == curD) { display.drawRoundRect(x - 20, ry, 40, 27, 6, BLK); display.drawRoundRect(x - 19, ry + 1, 38, 25, 5, BLK); }
  }
  vline2(560, 44, 264);

  // weather
  txt(574, 58, &FreeSansBold9pt7b, WX_CITY);
  if (wxCode >= 0) {
    tempStr(574, 102, wxTemp, 0, &FreeSansBold24pt7b, 1, &FreeSansBold12pt7b, 4, 0);
    wxIcon(742, 84, wxCode);
    txt(574, 124, &FreeSans9pt7b, wxName(wxCode));
    snprintf(b, sizeof b, "Wind %d km/h", (int)round(wxWind)); txt(574, 146, &FreeSans9pt7b, b);
    snprintf(b, sizeof b, "Humidity %d %%", (int)round(wxHum)); txt(574, 164, &FreeSans9pt7b, b);
    snprintf(b, sizeof b, "High %d / Low %d", (int)round(wxHi), (int)round(wxLo)); txt(574, 182, &FreeSans9pt7b, b);
  } else txt(574, 110, &FreeSans9pt7b, WiFi.status() == WL_CONNECTED ? "Loading weather..." : "Weather needs Wi-Fi");
  display.drawFastHLine(574, 194, 204, BLK);

  // upcoming bookmarks
  txt(574, 212, &FreeSansBold9pt7b, "Bookmarks");
  uint32_t todayKey = (now.tm_year + 1900) * 10000UL + (now.tm_mon + 1) * 100 + now.tm_mday, last = 0; int shown = 0;
  for (int n = 0; n < 2; n++) {
    uint32_t best = 0xFFFFFFFF; for (int i = 0; i < bmN; i++) if (bm[i] >= todayKey && bm[i] > last && bm[i] < best) best = bm[i];
    if (best == 0xFFFFFFFF) break; last = best; shown++;
    int dd = dayDiff(best, now);
    if (dd == 0) snprintf(b, sizeof b, "%d %s - today", (int)(best % 100), MON3[(best / 100) % 100 - 1]);
    else snprintf(b, sizeof b, "%d %s - in %d d", (int)(best % 100), MON3[(best / 100) % 100 - 1], dd);
    txt(574, 232 + n * 19, &FreeSans9pt7b, b);
  }
  if (!shown) txt(574, 232, &FreeSans9pt7b, "OK on a day = save");
}

// ---------- timetable (iCalendar .ics feed) ----------
struct Ev { time_t s, e; char title[44]; char loc[28]; };
Ev ev[80]; int evN = 0; bool icOK = false; int ttDay = 0; uint32_t nextIc = 0;

long dnum(int y, int m, int d) {                    // days since 1970-01-01 (civil date)
  y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}
bool parseDT(const String& v, time_t& out, bool& allDay) {
  allDay = false; if (v.length() < 8) return false;
  int Y = v.substring(0, 4).toInt(), Mo = v.substring(4, 6).toInt(), D = v.substring(6, 8).toInt();
  if (v.length() < 15 || v[8] != 'T') { allDay = true; struct tm t = {}; t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D; t.tm_isdst = -1; out = mktime(&t); return true; }
  int H = v.substring(9, 11).toInt(), Mi = v.substring(11, 13).toInt(), S = v.substring(13, 15).toInt();
  if (v.endsWith("Z")) out = (time_t)(dnum(Y, Mo, D) * 86400L + H * 3600L + Mi * 60L + S);        // UTC -> epoch
  else { struct tm t = {}; t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D; t.tm_hour = H; t.tm_min = Mi; t.tm_sec = S; t.tm_isdst = -1; out = mktime(&t); }  // local / TZID
  return true;
}
String unesc(String s) { s.replace("\\,", ","); s.replace("\\;", ";"); s.replace("\\n", " "); s.replace("\\N", " "); s.replace("\\\\", "\\"); return s; }
String rrGet(const String& r, const char* k) { String key = String(k) + "="; int i = r.indexOf(key); if (i < 0) return ""; i += key.length(); int j = r.indexOf(';', i); return j < 0 ? r.substring(i) : r.substring(i, j); }
void addEv(time_t s, time_t e, const String& t, const String& l) {
  if (evN >= 80) return; Ev& x = ev[evN++]; x.s = s; x.e = e;
  strncpy(x.title, t.c_str(), 43); x.title[43] = 0; strncpy(x.loc, l.c_str(), 27); x.loc[27] = 0;
}
void expandEvent(time_t s, time_t e, const String& title, const String& loc, const String& rr, const uint32_t* ex, int exN, time_t winS, time_t winE) {
  if (rr.length() == 0) { if (e > winS && s < winE) addEv(s, e > s ? e : s + 3600, title, loc); return; }
  String freq = rrGet(rr, "FREQ"); int interval = max(1, rrGet(rr, "INTERVAL").toInt());
  time_t until = 0; bool ad; String u = rrGet(rr, "UNTIL"); if (u.length()) parseDT(u, until, ad);
  String bd = rrGet(rr, "BYDAY"); unsigned mask = 0; const char* nm[7] = {"SU","MO","TU","WE","TH","FR","SA"};
  for (int i = 0; i < 7; i++) if (bd.indexOf(nm[i]) >= 0) mask |= 1u << i;
  struct tm sT, wT; localtime_r(&s, &sT); localtime_r(&winS, &wT);
  time_t dur = e > s ? e - s : 3600; long sDn = dnum(sT.tm_year + 1900, sT.tm_mon + 1, sT.tm_mday);
  for (int k = 0; k < 8; k++) {
    struct tm c = wT; c.tm_mday += k; c.tm_hour = sT.tm_hour; c.tm_min = sT.tm_min; c.tm_sec = 0; c.tm_isdst = -1;
    time_t occ = mktime(&c); if (occ < s || (until && occ > until)) continue;
    long cDn = dnum(c.tm_year + 1900, c.tm_mon + 1, c.tm_mday); int wd = c.tm_wday;
    if (freq == "WEEKLY") {
      unsigned m = mask ? mask : (1u << sT.tm_wday); if (!(m & (1u << wd))) continue;
      long w = (cDn - ((wd + 6) % 7)) - (sDn - ((sT.tm_wday + 6) % 7)); if ((w / 7) % interval) continue;
    } else if (freq == "DAILY") { if ((cDn - sDn) % interval) continue; }
    else continue;
    uint32_t key = (c.tm_year + 1900) * 10000UL + (c.tm_mon + 1) * 100 + c.tm_mday; bool skip = false;
    for (int i = 0; i < exN; i++) if (ex[i] == key) skip = true;
    if (!skip) addEv(occ, occ + dur, title, loc);
  }
}
bool readLine(WiFiClient* st, String& out) {
  out = ""; uint32_t t0 = millis();
  while (true) {
    if (!st->available()) { if (!st->connected() || millis() - t0 > 8000) return out.length() > 0; delay(2); continue; }
    char c = st->read(); t0 = millis(); if (c == '\n') break; if (c != '\r') out += c;
  }
  return true;
}
bool readLogical(WiFiClient* st, String& out) {      // joins folded lines (continuations start with space/tab)
  if (!readLine(st, out)) return false;
  while (true) {
    uint32_t t0 = millis(); while (!st->available() && st->connected() && millis() - t0 < 30) delay(1);
    int p = st->peek(); if (p != ' ' && p != '\t') break;
    String nx; if (!readLine(st, nx)) break; out += nx.substring(1);
  }
  return true;
}
bool fetchIcal() {
  String url = ICAL_URL; if (url.startsWith("webcal://")) url = "https://" + url.substring(9);
  struct tm now; if (!getLocalTime(&now, 0)) return false;
  WiFiClientSecure c; c.setInsecure(); HTTPClient h; h.setTimeout(15000); h.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!h.begin(c, url)) return false; if (h.GET() != 200) { h.end(); return false; }
  struct tm d = now; d.tm_hour = d.tm_min = d.tm_sec = 0; d.tm_isdst = -1; time_t winS = mktime(&d), winE = winS + 8 * 86400L;
  evN = 0; WiFiClient* st = h.getStreamPtr(); String ln, sum, loc, rr;
  bool inEv = false, allDay = false, cancelled = false; time_t ts = 0, te = 0; uint32_t ex[10]; int exN = 0;
  while (readLogical(st, ln)) {
    if (ln == "BEGIN:VEVENT") { inEv = true; ts = te = 0; allDay = false; cancelled = false; sum = loc = rr = ""; exN = 0; continue; }
    if (ln == "END:VEVENT") { if (inEv && ts && !allDay && !cancelled) expandEvent(ts, te, sum, loc, rr, ex, exN, winS, winE); inEv = false; continue; }
    if (!inEv) continue;
    int col = ln.indexOf(':'); if (col < 0) continue; String head = ln.substring(0, col), val = ln.substring(col + 1), name = head;
    int sc = head.indexOf(';'); if (sc >= 0) name = head.substring(0, sc);
    if (name == "DTSTART") parseDT(val, ts, allDay);
    else if (name == "DTEND") { bool a; parseDT(val, te, a); }
    else if (name == "SUMMARY") sum = unesc(val);
    else if (name == "LOCATION") loc = unesc(val);
    else if (name == "RRULE") rr = val;
    else if (name == "STATUS" && val == "CANCELLED") cancelled = true;
    else if (name == "EXDATE") { for (int i = 0; i + 8 <= (int)val.length() && exN < 10;) { ex[exN++] = val.substring(i, i + 8).toInt(); int cm = val.indexOf(',', i); if (cm < 0) break; i = cm + 1; } }
  }
  h.end();
  for (int i = 1; i < evN; i++) { Ev t = ev[i]; int j = i - 1; while (j >= 0 && ev[j].s > t.s) { ev[j + 1] = ev[j]; j--; } ev[j + 1] = t; }
  icOK = true; return true;
}
void fitTxt(int x, int y, const GFXfont* f, const char* s, int maxW, uint16_t c = BLK, int al = 0) {
  char b[48]; strncpy(b, s, 47); b[47] = 0; int16_t x1, y1; uint16_t w, h; bool cut = false;
  display.setFont(f); display.setTextSize(1); display.getTextBounds(b, 0, 0, &x1, &y1, &w, &h);
  while (w > maxW && strlen(b) > 3) { b[strlen(b) - 1] = 0; cut = true; display.getTextBounds(b, 0, 0, &x1, &y1, &w, &h); }
  if (cut) { size_t l = strlen(b); b[l - 1] = '.'; b[l - 2] = '.'; }
  txt(x, y, f, b, al, c);
}
void hhmm(time_t t, char* o) { struct tm x; localtime_r(&t, &x); snprintf(o, 8, "%02d:%02d", x.tm_hour, x.tm_min); }

void drawTimetable() {
  struct tm now; bool haveTime = getLocalTime(&now, 0); char b[64], b2[8];
  display.fillScreen(WHT); display.drawRect(0, 0, 792, 272, BLK); display.drawRect(1, 1, 790, 270, BLK);
  drawHeader("Timetable", 2);
  if (!haveTime) { txt(396, 140, &FreeSansBold12pt7b, "No time yet", 1); txt(396, 170, &FreeSans9pt7b, "Set WIFI_SSID / WIFI_PASS in the sketch", 1); return; }
  if (!strlen(ICAL_URL)) { txt(396, 135, &FreeSansBold12pt7b, "No timetable link set", 1); txt(396, 165, &FreeSans9pt7b, "Paste your .ics / webcal link into ICAL_URL", 1); return; }
  if (!icOK) { txt(396, 150, &FreeSansBold12pt7b, WiFi.status() == WL_CONNECTED ? "Loading timetable..." : "Timetable needs Wi-Fi", 1); return; }
  time_t nowT = time(nullptr);
  struct tm d = now; d.tm_hour = d.tm_min = d.tm_sec = 0; d.tm_mday += ttDay; d.tm_isdst = -1; time_t ds = mktime(&d);
  struct tm dn = d; dn.tm_mday += 1; dn.tm_isdst = -1; time_t de = mktime(&dn);
  struct tm d2 = dn; d2.tm_mday += 1; d2.tm_isdst = -1; time_t de2 = mktime(&d2);

  // left: selected day
  char ds_[24]; strftime(ds_, sizeof ds_, "%a %d %b", &d);
  snprintf(b, sizeof b, "%s%s", ttDay == 0 ? "Today  " : "", ds_); txt(16, 60, &FreeSansBold12pt7b, b);
  int idx[20], n = 0; for (int i = 0; i < evN && n < 20; i++) if (ev[i].s >= ds && ev[i].s < de) idx[n++] = i;
  bool nextDone = false;
  for (int r = 0; r < n && r < 5; r++) {
    Ev& e = ev[idx[r]]; int ry = 70 + r * 39; bool cur = ttDay == 0 && nowT >= e.s && nowT < e.e; uint16_t fg = cur ? WHT : BLK;
    if (cur) display.fillRoundRect(14, ry, 516, 36, 8, BLK); else display.drawRoundRect(14, ry, 516, 36, 8, BLK);
    if (ttDay == 0 && !cur && e.s > nowT && !nextDone) { nextDone = true; display.drawRoundRect(15, ry + 1, 514, 34, 7, BLK); display.drawRoundRect(16, ry + 2, 512, 32, 6, BLK); }
    hhmm(e.s, b2); txt(24, ry + 17, &FreeSansBold12pt7b, b2, 0, fg); hhmm(e.e, b2); txt(24, ry + 33, &FreeSans9pt7b, b2, 0, fg);
    display.drawFastVLine(104, ry + 5, 26, fg);
    fitTxt(116, ry + 17, &FreeSansBold9pt7b, e.title[0] ? e.title : "(no title)", 290, fg);
    if (e.loc[0]) fitTxt(116, ry + 33, &FreeSans9pt7b, e.loc, 290, fg);
    if (ttDay == 0) {
      if (cur) strcpy(b, "now");
      else if (e.s > nowT) { int mn = (e.s - nowT) / 60; if (mn < 60) snprintf(b, sizeof b, "in %d min", mn); else snprintf(b, sizeof b, "in %d h %02d", mn / 60, mn % 60); }
      else strcpy(b, "done");
      txt(518, ry + 23, &FreeSansBold9pt7b, b, 2, fg);
    }
  }
  if (n > 5) { snprintf(b, sizeof b, "+%d more", n - 5); txt(520, 60, &FreeSans9pt7b, b, 2); }
  if (!n) txt(270, 150, &FreeSansBold12pt7b, "No classes", 1);

  // right: following day
  vline2(540, 44, 264);
  strftime(ds_, sizeof ds_, "%a %d %b", &dn); txt(556, 60, &FreeSansBold12pt7b, ds_);
  int m2 = 0, shown = 0; for (int i = 0; i < evN; i++) if (ev[i].s >= de && ev[i].s < de2) {
    m2++; if (shown < 4) { int y = 86 + shown * 43; char a[8], c2[8]; hhmm(ev[i].s, a); hhmm(ev[i].e, c2);
      snprintf(b, sizeof b, "%s-%s", a, c2); txt(556, y, &FreeSansBold9pt7b, b); fitTxt(556, y + 17, &FreeSans9pt7b, ev[i].title, 215); shown++; } }
  if (!m2) txt(556, 100, &FreeSans9pt7b, "No classes");
  else if (m2 > 4) { snprintf(b, sizeof b, "+%d more", m2 - 4); txt(556, 262, &FreeSans9pt7b, b); }
}

// ---------- settings screen (MENU cycles TRV -> Timetable -> Calendar -> Settings) ----------
const uint8_t NEXT_SCREEN[5] = {2, 3, 1, 4, 0};  // screen ids: 0 TRV, 1 calendar, 2 timetable, 3 settings, 4 history
#define SET_N 10
const char* SET_LABEL[SET_N] = {"Night 00-06", "Morning 06-08", "Day 08-17", "Evening 17-22", "Late 22-24",
                                "Eco", "Comfort", "Frost", "Sensor offset", "Reset defaults"};
int setSel = 0; bool setEdit = false, resetArm = false;

void loadSettings() {
  char k[4];
  for (int i = 0; i < 5; i++) { snprintf(k, sizeof k, "s%d", i); SCH[i].temp = prefs.getFloat(k, DEF_SCH[i]); }
  for (int i = 1; i < 4; i++) { snprintf(k, sizeof k, "m%d", i); MODE_TEMP[i] = prefs.getFloat(k, DEF_MODE[i]); }
  tOff = prefs.getFloat("off", 0);
}
void saveSettings() {
  char k[4];
  for (int i = 0; i < 5; i++) { snprintf(k, sizeof k, "s%d", i); prefs.putFloat(k, SCH[i].temp); }
  for (int i = 1; i < 4; i++) { snprintf(k, sizeof k, "m%d", i); prefs.putFloat(k, MODE_TEMP[i]); }
  prefs.putFloat("off", tOff);
}
float* setPtr(int i) { if (i < 5) return &SCH[i].temp; if (i < 8) return &MODE_TEMP[i - 4]; return &tOff; }
void setRange(int i, float& lo, float& hi, float& st) {
  if (i < 5)       { lo = 5;   hi = 30; st = 0.5f; }   // schedule slots
  else if (i == 5) { lo = 12;  hi = 21; st = 0.5f; }   // eco
  else if (i == 6) { lo = 18;  hi = 26; st = 0.5f; }   // comfort
  else if (i == 7) { lo = 3;   hi = 10; st = 1.0f; }   // frost
  else             { lo = -3;  hi = 3;  st = 0.1f; }   // sensor offset
}
void settingsRotary(int up, int n) {                    // up = +1 (rotary UP) or -1 (rotary DOWN)
  resetArm = false; dirty = true;
  if (!setEdit) { setSel = (setSel + SET_N * 4 - up * n) % SET_N; return; }
  float lo, hi, st; setRange(setSel, lo, hi, st); float* p = setPtr(setSel);
  *p = constrain(roundf((*p + up * n * st) / st) * st, lo, hi);
}
void settingsOK() {
  dirty = true;
  if (setSel == SET_N - 1) {                            // reset row: press OK twice
    if (!resetArm) { resetArm = true; return; }
    for (int i = 0; i < 5; i++) SCH[i].temp = DEF_SCH[i];
    for (int i = 1; i < 4; i++) MODE_TEMP[i] = DEF_MODE[i];
    tOff = 0; hasOvr = false; resetArm = false; setEdit = false; saveSettings(); return;
  }
  if (setEdit) saveSettings();                          // leaving edit mode = commit
  setEdit = !setEdit;
}
void exitSettings() {
  if (setEdit || resetArm) { setEdit = false; resetArm = false; loadSettings(); }   // cancel
  else { screen = 0; forceFull = true; }                                            // back to TRV
  dirty = true;
}
void settingRow(int i, int x, int ry, int w) {
  bool sel = (setSel == i), ed = sel && setEdit; uint16_t fg = sel ? WHT : BLK;
  if (sel) display.fillRoundRect(x, ry, w, 32, 8, BLK); else display.drawRoundRect(x, ry, w, 32, 8, BLK);
  if (ed) display.drawRoundRect(x + 3, ry + 3, w - 6, 26, 6, WHT);
  txt(x + 16, ry + 22, &FreeSansBold9pt7b, SET_LABEL[i], 0, fg);
  if (i == SET_N - 1) { txt(x + w - 16, ry + 22, &FreeSans9pt7b, resetArm ? "OK again" : "OK", 2, fg); return; }
  float v = *setPtr(i); if (fabsf(v) < 0.05f) v = 0;
  tempStr(x + w - 16, ry + 22, v, i == 7 ? 0 : 1, &FreeSansBold9pt7b, 1, &FreeSansBold9pt7b, 2, 2, fg);
}
void drawSettings() {
  display.fillScreen(WHT); display.drawRect(0, 0, 792, 272, BLK); display.drawRect(1, 1, 790, 270, BLK);
  drawHeader("Settings", 3);
  txt(16, 56, &FreeSansBold12pt7b, "Schedule temperatures");
  txt(412, 56, &FreeSansBold12pt7b, "Modes and sensor");
  vline2(395, 44, 244);
  for (int r = 0; r < 5; r++) { settingRow(r, 14, 64 + r * 36, 368); settingRow(5 + r, 410, 64 + r * 36, 368); }
  display.drawFastHLine(16, 246, 760, BLK);
  const char* h = resetArm ? "OK again: reset all settings     Exit: cancel"
                : setEdit  ? "Up / Down: change value     OK: save     Exit: cancel"
                           : "Up / Down: select     OK: edit     Exit: back to TRV";
  txt(396, 263, &FreeSans9pt7b, h, 1);
}

// ---------- history screen: "Thermal dynamics" (room / target / heating; 24 h, 30 d, 6 m) ----------
// The sketch logs its own data: every 30 min a sample (room temp incl. offset, target, valve %) goes into a 24 h RAM ring;
// each finished day is condensed into one record (avg room, peak target, avg valve %) and kept in flash (~6 months).
// Needs the clock (Wi-Fi/NTP). A restart clears the 24 h ring; daily records survive.
#define H_N 48
#define D_N 186
struct HSamp { uint32_t slot; int16_t room10, tgt10; uint8_t heat; };
struct DayRec { uint32_t date; int16_t room10, tgt10; uint8_t heat; };      // date = yyyymmdd
HSamp hs[H_N]; DayRec dr[D_N]; int drN = 0;
uint32_t hLastSlot = 0, accDate = 0; float accRoom = 0, accHeat = 0, accTgt = -99; int accN = 0;
int hvRange = 0, hvPreset = 0;                                              // range 0/1/2 = 24h/30d/6m; preset 0 all,1 room,2 target,3 heating
float gR[H_N], gT[H_N], gH[H_N]; bool gOK[H_N]; int gN = 0;
int tkI[6], tkN = 0; char tkS[6][8];

void loadHistory() {
  size_t l = prefs.getBytesLength("hd"); drN = l / sizeof(DayRec);
  if (drN > D_N || (size_t)drN * sizeof(DayRec) != l) drN = 0; else if (drN) prefs.getBytes("hd", dr, l);
}
void pushDay(uint32_t date) {
  if (!accN) return;
  DayRec r; r.date = date; r.room10 = (int16_t)lroundf(accRoom / accN * 10); r.tgt10 = (int16_t)lroundf(accTgt * 10); r.heat = (uint8_t)lroundf(accHeat / accN);
  if (drN == D_N) { memmove(&dr[0], &dr[1], (D_N - 1) * sizeof(DayRec)); drN--; }
  dr[drN++] = r; prefs.putBytes("hd", dr, drN * sizeof(DayRec));
  accRoom = accHeat = 0; accTgt = -99; accN = 0;
}
void historyTick() {
  static uint32_t chk = 0; if (millis() - chk < 15000) return; chk = millis();
  struct tm t; if (!getLocalTime(&t, 0)) return;
  uint32_t slot = (uint32_t)(time(nullptr) / 1800), date = (t.tm_year + 1900) * 10000UL + (t.tm_mon + 1) * 100 + t.tm_mday;
  if (accDate && date != accDate) pushDay(accDate);
  accDate = date;
  if (slot == hLastSlot) return;
  hLastSlot = slot;
  HSamp& s = hs[slot % H_N]; float rc = curC(), tg = eff(); int vp = valvePct();
  s.slot = slot; s.room10 = (int16_t)lroundf(rc * 10); s.tgt10 = (int16_t)lroundf(tg * 10); s.heat = (uint8_t)vp;
  accRoom += rc; accHeat += vp; accTgt = max(accTgt, tg); accN++;
  if (screen == 4) dirty = true;
}
bool dayVals(uint32_t key, float& r, float& t, float& h) {
  for (int i = 0; i < drN; i++) if (dr[i].date == key) { r = dr[i].room10 / 10.0f; t = dr[i].tgt10 / 10.0f; h = dr[i].heat; return true; }
  if (accN && key == accDate) { r = accRoom / accN; t = accTgt; h = accHeat / accN; return true; }
  return false;
}
void buildSeries(const struct tm& now) {
  tkN = 0;
  if (hvRange == 0) {                                                       // last 24 h, 30-minute samples
    gN = H_N; uint32_t cs = (uint32_t)(time(nullptr) / 1800);
    for (int i = 0; i < H_N; i++) {
      uint32_t sl = cs - (H_N - 1) + i; HSamp& s = hs[sl % H_N]; gOK[i] = (s.slot == sl);
      if (gOK[i]) { gR[i] = s.room10 / 10.0f; gT[i] = s.tgt10 / 10.0f; gH[i] = s.heat; }
    }
    const int ti[5] = {0, 12, 24, 36, 47}; const char* ts[5] = {"-24h", "-18h", "-12h", "-6h", "now"};
    for (int k = 0; k < 5; k++) { tkI[k] = ti[k]; strcpy(tkS[k], ts[k]); } tkN = 5;
  } else if (hvRange == 1) {                                                // last 30 days, one value per day
    gN = 30;
    for (int i = 0; i < 30; i++) {
      struct tm c = now; c.tm_hour = 12; c.tm_min = c.tm_sec = 0; c.tm_mday -= 29 - i; c.tm_isdst = -1; mktime(&c);
      uint32_t key = (c.tm_year + 1900) * 10000UL + (c.tm_mon + 1) * 100 + c.tm_mday;
      gOK[i] = dayVals(key, gR[i], gT[i], gH[i]);
      if (i == 0 || i == 7 || i == 14 || i == 21 || i == 29) { tkI[tkN] = i; snprintf(tkS[tkN], 8, "%d", c.tm_mday); tkN++; }
    }
  } else {                                                                  // last 6 calendar months, monthly values
    gN = 6;
    for (int i = 0; i < 6; i++) {
      int y = now.tm_year + 1900, m = now.tm_mon + 1 - (5 - i); while (m < 1) { m += 12; y--; }
      uint32_t ym = y * 100UL + m; float sr = 0, sh = 0, mt = -99; int cnt = 0;
      for (int j = 0; j < drN; j++) if (dr[j].date / 100 == ym) { sr += dr[j].room10 / 10.0f; sh += dr[j].heat; mt = max(mt, dr[j].tgt10 / 10.0f); cnt++; }
      if (accN && accDate / 100 == ym) { sr += accRoom / accN; sh += accHeat / accN; mt = max(mt, accTgt); cnt++; }
      gOK[i] = cnt > 0; if (cnt) { gR[i] = sr / cnt; gT[i] = mt; gH[i] = sh / cnt; }
      tkI[tkN] = i; strcpy(tkS[tkN], MON3[m - 1]); tkN++;
    }
  }
}
void historyRotary(int up, int n) { hvRange = (hvRange + 33 - up * n) % 3; dirty = true; }   // DOWN = next range
void historyOK() { hvPreset = (hvPreset + 1) % 4; dirty = true; }
void historyExit() { hvRange = 0; hvPreset = 0; dirty = true; }

void pill(int x, int y, int w, int h, const char* s, bool on) {
  if (on) display.fillRoundRect(x, y, w, h, h / 2, BLK); else display.drawRoundRect(x, y, w, h, h / 2, BLK);
  txt(x + w / 2, y + h - 7, &FreeSansBold9pt7b, s, 1, on ? WHT : BLK);
}
void dashSeg(int x0, int y0, int x1, int y1) {                              // consistent 6 px on / 4 px off dash pattern
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, step = 0;
  while (true) {
    if (step % 10 < 6) display.drawPixel(x0, y0, BLK);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
    step++;
  }
}
void kpiCard(int ry, const char* label, float v, int kind) {                // kind 0 = temperature, 1 = percent
  display.drawRoundRect(580, ry, 196, 44, 8, BLK);
  txt(592, ry + 15, &FreeSans9pt7b, label);
  if (isnan(v)) { txt(592, ry + 39, &FreeSansBold12pt7b, "--"); return; }
  if (kind == 0) tempStr(592, ry + 39, v, 1, &FreeSansBold12pt7b, 1, &FreeSansBold9pt7b, 3, 0);
  else { char b[12]; snprintf(b, sizeof b, "%d %%", (int)lroundf(v)); txt(592, ry + 39, &FreeSansBold12pt7b, b); }
}
void drawHistory() {
  struct tm now; bool haveTime = getLocalTime(&now, 0);
  display.fillScreen(WHT); display.drawRect(0, 0, 792, 272, BLK); display.drawRect(1, 1, 790, 270, BLK);
  drawHeader("History", 4);
  if (!haveTime) { txt(396, 140, &FreeSansBold12pt7b, "No time yet", 1); txt(396, 170, &FreeSans9pt7b, "Set WIFI_SSID / WIFI_PASS in the sketch", 1); return; }
  buildSeries(now);
  bool vR = hvPreset == 0 || hvPreset == 1, vT = hvPreset == 0 || hvPreset == 2, vH = hvPreset == 0 || hvPreset == 3;

  txt(16, 58, &FreeSansBold12pt7b, "Thermal dynamics");
  const char* nt[3] = {"24 h, 30 min samples", "30 days, daily values", "6 months, monthly values"};
  txt(252, 58, &FreeSans9pt7b, nt[hvRange]);
  const char* rn[3] = {"24h", "30d", "6m"};
  for (int i = 0; i < 3; i++) pill(590 + i * 58, 40, 54, 24, rn[i], hvRange == i);

  float mn = 1e9f, mx = -1e9f, sr = 0, sh = 0, pk = -99; int cnt = 0;
  for (int i = 0; i < gN; i++) if (gOK[i]) {
    mn = min(mn, min(gR[i], gT[i])); mx = max(mx, max(gR[i], gT[i]));
    sr += gR[i]; sh += gH[i]; pk = max(pk, gT[i]); cnt++;
  }
  kpiCard(74, "Room avg", cnt ? sr / cnt : NAN, 0);
  kpiCard(122, "Peak target", cnt ? pk : NAN, 0);
  kpiCard(170, "Avg heating", cnt ? sh / cnt : NAN, 1);

  const int x0 = 64, x1 = 556, ty0 = 74, ty1 = 176, hy0 = 184, hy1 = 206, pw = x1 - x0, pt = ty1 - ty0, pl = hy1 - hy0;
  int lo = 15, hi = 25, stp = 5;                                            // temperature axis fits the data (default 15-25 C)
  if (mx >= mn) {
    lo = 2 * (int)floorf((mn - 0.5f) / 2); hi = 2 * (int)ceilf((mx + 0.5f) / 2); stp = 2;
    if (hi - lo < 6) hi = lo + 6;
    if (hi - lo > 8) { stp = 5; lo = 5 * (int)floorf(lo / 5.0f); hi = 5 * (int)ceilf(hi / 5.0f); }
  }
  for (int v = lo; v <= hi; v += stp) {
    int y = ty1 - (v - lo) * pt / (hi - lo); char b[8]; snprintf(b, sizeof b, "%d", v);
    if (v > lo) for (int x = x0; x <= x1; x += 4) display.drawPixel(x, y, BLK);
    txt(x0 - 8, y + 6, &FreeSans9pt7b, b, 2);
  }
  display.drawFastVLine(x0, ty0, hy1 - ty0 + 1, BLK);
  display.drawFastHLine(x0, ty1, pw + 1, BLK); display.drawFastHLine(x0, hy1, pw + 1, BLK);
  txt(x0 - 8, hy1 - 5, &FreeSans9pt7b, "Heat", 2);
  float sw = (float)pw / gN;
  auto xc = [&](int i) { return (int)(x0 + (i + 0.5f) * sw); };
  auto yT = [&](float v) { return (int)(ty1 - (constrain(v, (float)lo, (float)hi) - lo) / (hi - lo) * pt); };
  for (int k = 0; k < tkN; k++) { int x = xc(tkI[k]); display.drawFastVLine(x, hy1, 4, BLK); txt(x, hy1 + 22, &FreeSans9pt7b, tkS[k], 1); }

  if (!cnt) {
    txt(310, 128, &FreeSansBold12pt7b, "Collecting data...", 1);
    txt(310, 154, &FreeSans9pt7b, hvRange == 0 ? "A sample is saved every 30 minutes" : "Daily values appear after the first full day", 1);
  } else {
    if (vH) { int bw = min(30, max(2, (int)(sw * 0.6f)));                    // heating: solid bars in their own lane
      for (int i = 0; i < gN; i++) if (gOK[i] && gH[i] > 0) { int bh = max(2, (int)lroundf(gH[i] / 100.0f * pl)); display.fillRect(xc(i) - bw / 2, hy1 - bh, bw, bh, BLK); } }
    const int markerEvery = max(1, (gN + 9) / 10); // keep markers readable at every history range
    if (vT) for (int i = 1; i < gN; i++) if (gOK[i] && gOK[i - 1]) {
      // Join target samples directly; staircase transitions created distracting full-height spikes.
      dashSeg(xc(i - 1), yT(gT[i - 1]), xc(i), yT(gT[i]));
    }
    if (vR) for (int i = 1; i < gN; i++) if (gOK[i] && gOK[i - 1])
      display.drawLine(xc(i - 1), yT(gR[i - 1]), xc(i), yT(gR[i]), BLK);
    for (int i = 0; i < gN; i++) if (gOK[i] && (i % markerEvery == 0 || i == gN - 1)) {
      if (vR) display.fillCircle(xc(i), yT(gR[i]), 2, BLK);
      if (vT) { display.drawCircle(xc(i), yT(gT[i]), 2, BLK); display.drawPixel(xc(i), yT(gT[i]), WHT); }
    }
  }
  pill(64, 238, 92, 24, "Room", vR); pill(162, 238, 100, 24, "Target", vT); pill(268, 238, 128, 24, "Heating %", vH);
  txt(776, 255, &FreeSans9pt7b, "Up/Down: range   OK: series", 2);
}

void drawScreen() { if (screen == 0) drawUI(); else if (screen == 1) drawCalendar(); else if (screen == 2) drawTimetable(); else if (screen == 3) drawSettings(); else drawHistory(); }

void drawCurrentHeader() {
  if (screen == 0) drawHeader(ROOM_NAME, 0);
  else if (screen == 1) drawHeader("Calendar", 1);
  else if (screen == 2) drawHeader("Timetable", 2);
  else if (screen == 3) drawHeader("Settings", 3);
  else drawHeader("History", 4);
}

void partialRefresh(int x, int y, int w, int h, bool headerOnly) {
  display.setPartialWindow(x, y, w, h);
  display.firstPage();
  do {
    if (headerOnly) drawCurrentHeader();
    else drawScreen(); // GxEPD2 clips these drawing calls to the partial window.
  } while (display.nextPage());
}

void partialRefreshBody() {
  if (screen == 0) {
    // Temperature dial/target controls, status cards, and mode selector are separate regions.
    partialRefresh(8, 34, 548, 176, false);
    partialRefresh(576, 34, 208, 176, false);
    partialRefresh(8, 212, 776, 50, false);
  } else if (screen == 1) {
    partialRefresh(244, 38, 320, 190, false);  // month label and day grid
    partialRefresh(568, 38, 216, 224, false);  // weather and bookmarks
  } else if (screen == 2) {
    partialRefresh(8, 38, 532, 226, false);   // selected day's events
    partialRefresh(540, 38, 244, 226, false);  // following day's events
  } else if (screen == 3) {
    int rows[2] = {setSel, lastSetSelDrawn};
    for (int n = 0; n < 2; n++) {
      int row = rows[n];
      if (row < 0 || (n == 1 && row == rows[0])) continue;
      int x = row < 5 ? 8 : 404, y = 60 + (row % 5) * 36;
      partialRefresh(x, y, 380, 40, false);
    }
    partialRefresh(8, 244, 776, 22, false);    // instruction / reset prompt
    lastSetSelDrawn = setSel;
  } else {
    partialRefresh(8, 34, 776, 34, false);      // title, subtitle and range buttons
    partialRefresh(56, 38, 508, 190, false);   // graph and range controls
    partialRefresh(576, 68, 208, 154, false);  // summary cards
    partialRefresh(56, 230, 344, 34, false);   // series selectors
    partialRefresh(400, 238, 384, 26, false);  // button hints
  }
}

void refresh() {
  bool full = forceFull || partialCount >= 25;
  if (full) {
    display.setFullWindow();
    display.firstPage();
    do { drawScreen(); } while (display.nextPage());
    partialCount = 0;
    forceFull = false;
    headerDirty = false;
    calendarClockDirty = false;
    lastSetSelDrawn = setSel;
  } else {
    bool didPartial = dirty || calendarClockDirty || headerDirty;
    if (dirty) partialRefreshBody();
    if (calendarClockDirty && screen == 1) partialRefresh(44, 38, 152, 224, false);
    if (headerDirty) partialRefresh(0, 0, 792, 34, true);
    if (didPartial) partialCount++; // count logical updates, not each disjoint window
    headerDirty = false;
    calendarClockDirty = false;
  }
  lastDraw = millis(); dirty = false;
}

// ---------- buttons ----------
// Per Elecrow docs/examples: MENU(HOME)=IO2, EXIT=IO1, rotary UP(PRV)=IO6, DOWN(NEXT)=IO4, PRESS(OK)=IO5.
// All are active-LOW (pressed = GND). Presses are captured by interrupts, so nothing is lost
// while the e-paper is busy refreshing (a refresh blocks the loop for ~1 s).
enum { B_UP, B_DOWN, B_OK, B_MENU, B_EXIT, B_N };
const uint8_t  BTN_PIN[B_N]  = {BTN_UP, BTN_DOWN, BTN_PRESS, BTN_MENU, BTN_EXIT};
const char*    BTN_NAME[B_N] = {"UP", "DOWN", "OK", "MENU", "EXIT"};
volatile uint8_t  btnCount[B_N];
volatile uint32_t btnLast[B_N];
uint32_t holdStart[B_N], holdRep[B_N];

void IRAM_ATTR btnISR(void* arg) {
  uint8_t i = (uint8_t)(uintptr_t)arg; uint32_t now = millis();
  if (now - btnLast[i] > 35) { btnLast[i] = now; if (btnCount[i] < 30) btnCount[i]++; }   // 35 ms debounce
}
void initButtons() {
  for (uint8_t i = 0; i < B_N; i++) {
    pinMode(BTN_PIN[i], INPUT_PULLUP);
    attachInterruptArg(digitalPinToInterrupt(BTN_PIN[i]), btnISR, (void*)(uintptr_t)i, FALLING);
  }
}
void rotary(int up, int n) {          // up = +1 (rotary UP) or -1 (rotary DOWN)
  if (screen == 0) adjust(0.5f * up * n);                               // TRV: warmer / cooler
  else if (screen == 1) moveCursor(-up * n);                            // calendar: UP = previous day, DOWN = next day
  else if (screen == 3) settingsRotary(up, n);
  else if (screen == 4) historyRotary(up, n);                          // history: range                         // settings: select / change
  else { ttDay = constrain(ttDay - up * n, 0, 6); dirty = true; }       // timetable: previous / next day
}
void pollButtons() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < B_N; i++) {
    uint8_t n = btnCount[i]; btnCount[i] -= n;                 // take all queued presses
    if (n) Serial.printf("BTN %s x%u\n", BTN_NAME[i], n);
    if (n) switch (i) {
      case B_UP:   rotary(+1, n); break;
      case B_DOWN: rotary(-1, n); break;
      case B_OK:   if (screen == 0) { mode = (mode + n) % 4; if (mode == 0) hasOvr = false; dirty = true; }
                   else if (screen == 1) { if (n & 1) toggleBm(); }       // calendar: bookmark the day
                   else if (screen == 2) { nextIc = 0; }
                   else if (screen == 4) historyOK();
                   else settingsOK();                                    // timetable: re-download the .ics now
                   break;
      case B_MENU: if (screen == 3) { setEdit = false; resetArm = false; loadSettings(); }   // leaving settings drops unsaved edits
                   for (uint8_t press = 0; press < n; press++) screen = NEXT_SCREEN[screen];
                   forceFull = true; dirty = true; break;                         // TRV -> timetable -> calendar -> settings
      case B_EXIT: if (screen == 0) { if (n & 1) { winOpen = !winOpen; dirty = true; } }              // demo: window open
                   else if (screen == 1) setCursorToday();                                             // calendar: jump to today
                   else if (screen == 3) exitSettings();
                   else if (screen == 4) historyExit();
                   else { ttDay = 0; dirty = true; }                                                   // timetable: back to today
                   break;
    }
    if (i == B_UP || i == B_DOWN) {                            // hold rotary up/down = auto-repeat
      if (digitalRead(BTN_PIN[i]) == LOW) {
        if (!holdStart[i]) { holdStart[i] = now; holdRep[i] = now; }
        else if (now - holdStart[i] > 600 && now - holdRep[i] > 250) { holdRep[i] = now; rotary(i == B_UP ? +1 : -1, 1); }
      } else holdStart[i] = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(EPD_PWR, OUTPUT); digitalWrite(EPD_PWR, HIGH); delay(50);   // panel power rail
  initButtons(); loadBookmarks(); loadSettings(); loadHistory();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
  display.init(115200, true, 2, false);
  display.setRotation(0);                                   // 792 x 272 landscape
  if (strlen(WIFI_SSID)) { WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS); configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com"); }
  refresh();
}

void loop() {
  pollButtons();
  readSensors();
  driveValve(valvePct());
  historyTick();
  static uint32_t nextWx = 0;
  if (WiFi.status() == WL_CONNECTED && millis() >= nextWx) {
    bool ok = fetchWeather(); nextWx = millis() + (ok ? 1800000UL : 300000UL);
    if (ok) { headerDirty = true; if (screen == 1) dirty = true; }
  }
  if (strlen(ICAL_URL) && WiFi.status() == WL_CONNECTED && millis() >= nextIc) {
    struct tm tt; if (getLocalTime(&tt, 0)) { bool ok = fetchIcal(); nextIc = millis() + (ok ? 1800000UL : 300000UL); if (screen == 2) dirty = true; }
  }
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_min != lastMin) {
    static float lastScheduledTarget = NAN;
    lastMin = t.tm_min;
    headerDirty = true;
    if (screen == 1) calendarClockDirty = true;   // only its hands and digital clock change each minute
    if (screen == 2) dirty = true;                // live timetable status also changes
    float scheduledTarget = eff();
    if (screen == 0 && (isnan(lastScheduledTarget) || fabsf(scheduledTarget - lastScheduledTarget) >= 0.05f)) dirty = true;
    lastScheduledTarget = scheduledTarget;
  }
  static int lv = -1; static float lc = -99;
  if (valvePct() != lv || fabs(cur - lc) >= 0.1) { lv = valvePct(); lc = cur; if (millis() - lastDraw > 30000) dirty = true; }
  if ((dirty || headerDirty) && millis() - lastDraw > 300) refresh(); // presses made during refresh are queued by ISRs
  delay(20);
}
