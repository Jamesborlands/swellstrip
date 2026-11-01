/* ============================================================================
   SwellStrip — surf forecast on the Elecrow CrowPanel 5.79" e-paper (272x792)
   Board: ESP32-S3-WROOM-1-N8R8

   Reads the same sources, with the same math, as Swellbar on the Mac
   (github.com/Jamesborlands/swellbar), so the two never disagree:
     waves  NDBC 46266 Del Mar Nearshore buoy  (Open-Meteo Marine if it's down)
     swell  NDBC 46266 spectral summary
     wind   NDBC LJPC1 Scripps Pier            (Open-Meteo Forecast if it's down)
     tides  NOAA CO-OPS 9410230 La Jolla
     week   Open-Meteo Marine, scaled to match today's buoy reading
   Draws one full-screen frame, then deep sleeps until the top of the next
   hour. Any button (MENU, EXIT or the rotary switch) wakes it to refresh now.
   All of the APIs are free and keyless.

   ---------------------------------------------------------------------------
   ARDUINO IDE SETUP
     Board:           ESP32S3 Dev Module
     USB CDC On Boot: Disabled         <-- this board has a USB-serial bridge
                                            chip (it enumerates as usbserial,
                                            not usbmodem), so Serial goes out
                                            over UART. Enabling CDC sends it to
                                            a native USB port that isn't wired
                                            to the connector = blank monitor.
     Flash Size:      8MB
     PSRAM:           OPI PSRAM
     Partition:       8M with spiffs (3MB APP/1.5MB SPIFFS)

   LIBRARIES (Library Manager)
     GxEPD2          by Jean-Marc Zingg
     Adafruit GFX    by Adafruit
     ArduinoJson     by Benoit Blanchon  (v7.x)
   ============================================================================ */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

/* ===========================  CONFIGURE ME  =============================== */

// WiFi name and password live in secrets.h (not in git). Copy
// secrets.example.h to secrets.h and fill it in.
#include "secrets.h"

// Del Mar Beach, CA. Same spot, stations and coordinates as Swellbar's
// "Del Mar" entry (Sources/Spot.swift). Change one, change both.
const char* SPOT_NAME = "DEL MAR";
const char* SPOT_REGION = "CA";
const float SPOT_LAT  = 32.959;
const float SPOT_LON  = -117.269;

const char* WAVE_BUOY    = "46266";    // Del Mar Nearshore
const char* WIND_STATION = "LJPC1";    // Scripps Pier
const char* TIDE_STATION = "9410230";  // La Jolla (Scripps Pier)

// Del Mar faces 265 deg (W), so offshore wind blows FROM 85 deg (E). This is
// what separates a clean day from a blown-out one.
const int BEACH_FACING_DEG  = 265;
const int OFFSHORE_FROM_DEG = (BEACH_FACING_DEG + 180) % 360;

// Scales the Komar-Gaughan breaking-height estimate to match Surfline.
// Must equal Surf.calibration in Swellbar's SurfMath.swift. Fitted Oct 25:
// with 2-hour averaging, all 41 buoy readings that day landed in the range
// Surfline showed for that hour.
const float CALIBRATION = 0.57f;

// Used instead when the buoy's .spec file splits the reading into swell and
// wind sea (Surf.swellCalibration / windSeaWeight in SurfMath.swift). Nov 1:
// 0.8 m of almost pure 15 s SW swell read 2-3 ft on the total height while
// Surfline observed 4-5 ft; sizing the swell part alone fits both days.
const float SWELL_CALIBRATION = 0.9f;
const float WIND_SEA_WEIGHT   = 0.6f;

// Pacific time, for waking on the hour and for the tide table.
const char* TZ_INFO = "PST8PDT,M3.2.0,M11.1.0";

// Outlook chart style. Bars are crisper on a 1-bit panel; the line shows the
// shape of the week better. Flip it, reflash, keep whichever you prefer.
const bool OUTLOOK_AS_LINE = false;

/* ========================================================================== */

// CrowPanel 5.79" e-paper pins (ESP32-S3). Verified against Elecrow's
// GxEPD2 demo and the MicroPython port — do not change.
#define EINK_CS    45
#define EINK_DC    46
#define EINK_RST   47
#define EINK_BUSY  48
#define EINK_SCK   12
#define EINK_MOSI  11
#define EPD_POWER   7   // must be driven HIGH before the panel responds
#define LED_PIN    41

// Front buttons, all active low: EXIT, MENU, and the rotary switch's
// down / press / up. Any of them wakes the board for a refresh.
const int BUTTONS[] = {1, 2, 4, 5, 6};

GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT>
  display(GxEPD2_579_GDEY0579T93(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY));

// Panel is natively portrait; rotation 1 gives us 792 wide x 272 tall.
const int W = 792;
const int H = 272;

// Column geometry, mirroring the mockup.
const int COL1 = 252;            // headline block
const int COL2 = 249;            // detail rows + tide
const int X1   = 0;
const int X2   = COL1 + 1;
const int X3   = COL1 + 1 + COL2 + 1;
const int COL3 = W - X3;

/* ---------------------------- data model ---------------------------------- */

struct Forecast {
  bool  ok = false;
  bool  fromModel   = false; // buoy down: waves came from Open-Meteo

  float surfFt      = 0;     // breaking height, Swellbar's formula
  float periodS     = 0;     // dominant period
  int   waveDir     = -1;    // deg, -1 if unknown

  bool  haveSwell   = false;
  float swellHt     = 0;     // ft
  float swellPer    = 0;     // s
  String swellDir;
  float swell2Ht    = 0;     // wind swell
  float swell2Per   = 0;

  bool  haveWind    = false;
  float windKt      = 0;
  int   windDir     = 0;
  bool  haveAir     = false;
  float airTempF    = 0;
  bool  haveSeaTemp = false;
  float seaTempF    = 0;

  String tideText   = "--";

  // Day 0 is today. Morning wind (6-10 am) per day, for the day's rating.
  String dayLabel[6];
  float  daySurf[6]  = {0};
  float  dayPeriod[6] = {0};
  int    dayLevel[6] = {0};
  bool   dayHaveWind[6] = {false};
  float  dayWindKt[6] = {0};
  int    dayWindDir[6] = {0};
  int    dayCount     = 0;
};

Forecast fc;

// Surf size as Swellbar shows it: "3-4ft", "+" when it's in the top half.
// (Declared up here: the Arduino IDE puts its function prototypes above the
// first function, so types they use must come before it.)
struct Range { int lo, hi; bool plus; };

enum WindKind { OFFSHORE, CROSS_SHORE, ONSHORE };

// Survives deep sleep; zeroed only on a cold boot or the reset button.
RTC_DATA_ATTR int wakeCount = 0;

/* ------------------------------ surf math --------------------------------- */
// Straight ports of Swellbar's SurfMath.swift. Keep them in step.

const char* compass(int deg) {
  static const char* pts[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                              "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  return pts[int((deg % 360) / 22.5f + 0.5f) % 16];
}

float breakingHeightFt(float buoyFt, float periodS, float calibration = CALIBRATION) {
  float h  = buoyFt * 0.3048f;
  float hb = 0.39f * powf(9.81f, 0.2f) * powf(periodS * h * h, 0.4f) * calibration;
  return hb / 0.3048f;
}

Range surfRange(float ft) {
  if (ft < 1) return {0, 1, ft >= 0.6f};
  int lo = int(ft);
  bool plus = ft - lo >= 0.5f;
  if (lo < 5)  return {lo, lo + 1, plus};
  if (lo < 10) return {lo, lo + 2, plus};
  return {lo, lo + 3, plus};
}

String rangeText(Range r) { return String(r.lo) + "-" + String(r.hi); }

const char* bodyWords(Range r) {
  switch (r.lo) {
    case 0:  return "FLAT TO ANKLE";
    case 1:  return r.plus ? "SHIN TO THIGH" : "ANKLE TO KNEE";
    case 2:  return r.plus ? "THIGH TO STOMACH" : "KNEE TO WAIST";
    case 3:  return r.plus ? "WAIST TO SHOULDER" : "WAIST TO CHEST";
    case 4:  return r.plus ? "SHOULDER TO HEAD HIGH" : "SHOULDER TO HEAD";
    case 5: case 6:          return "HEAD HIGH TO OVERHEAD";
    case 7: case 8: case 9:  return "OVERHEAD TO WELL OH";
    default: return "DOUBLE OVERHEAD+";
  }
}

// Smallest angle between two compass bearings, 0-180.
int arc(int a, int b) {
  int d = abs(a - b) % 360;
  return d > 180 ? 360 - d : d;
}

WindKind windKind(int fromDeg) {
  int d = arc(fromDeg, OFFSHORE_FROM_DEG);
  if (d < 50)  return OFFSHORE;
  if (d < 130) return CROSS_SHORE;
  return ONSHORE;
}

// -1 = FLAT, 0 = VERY POOR .. 6 = EPIC. Size sets the base, long-period size
// bumps it, and onshore or cross-shore wind knocks it down. Tuned Oct 25 to
// Surfline's hourly Del Mar ratings (133 of 144 hours match).
const int FLAT = -1;

int rating(Range r, float periodS, bool haveWind, float windKt, int windDir) {
  if (!(r.hi > 1 || r.plus)) return FLAT;
  int level;
  if (r.lo < 2)       level = 1;   // POOR
  else if (r.lo <= 5) level = 3;   // FAIR
  else                level = 4;   // FAIR TO GOOD
  if (periodS >= 14 && r.lo >= 4) level++;
  if (haveWind) {
    // Small surf is spoiled by much lighter wind than surf of 3 ft and up.
    bool small = r.lo <= 2;
    WindKind k = windKind(windDir);
    if      (k == ONSHORE && windKt >= 15)                    level -= 2;
    else if (k == ONSHORE && windKt >= (small ? 4 : 8))       level -= 1;
    else if (k == CROSS_SHORE && windKt >= (small ? 3 : 10))  level -= 1;
  }
  return constrain(level, 0, 6);
}

int todayLevel() {
  return rating(surfRange(fc.surfFt), fc.periodS, fc.haveWind, fc.windKt, fc.windDir);
}

String verdict() {
  static const char* NAMES[] = { "FLAT", "VERY POOR", "POOR", "POOR TO FAIR",
                                 "FAIR", "FAIR TO GOOD", "GOOD", "EPIC" };
  return NAMES[todayLevel() + 1];
}

// Pips come off the same level, so the boxes and the words can never
// contradict each other.
int rateConditions() {
  int level = todayLevel();
  return level <= 0 ? 0 : (level * 5 + 3) / 6;   // 1-6 level -> 1-5 pips
}

// One line naming what decided it, so the verdict is never a black box.
String reason() {
  char buf[40];
  int w = int(fc.windKt + 0.5f);
  int p = int(fc.periodS + 0.5f);

  if (todayLevel() == FLAT) {
    snprintf(buf, sizeof(buf), "ONLY %.1fFT", fc.surfFt);
  } else if (!fc.haveWind) {
    snprintf(buf, sizeof(buf), "%dS SWELL - NO WIND DATA", p);
  } else if (fc.windKt < 4) {
    snprintf(buf, sizeof(buf), "GLASSY - %dS SWELL", p);
  } else {
    static const char* KIND[] = {"OFFSHORE", "CROSS", "ONSHORE"};
    snprintf(buf, sizeof(buf), "%s %dKT - %dS", KIND[windKind(fc.windDir)], w, p);
  }
  return String(buf);
}

/* ----------------------------- networking --------------------------------- */

const char* USER_AGENT = "SwellStrip/2.0 (github.com/Jamesborlands/swellstrip)";

String httpGet(const char* url) {
  WiFiClientSecure client;
  client.setInsecure();               // no cert pinning; fine for public reads
  client.setTimeout(15000);

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return "";
  http.setUserAgent(USER_AGENT);

  int code = http.GET();
  String body;
  if (code == HTTP_CODE_OK) body = http.getString();
  else Serial.printf("HTTP %d for %s\n", code, url);
  http.end();
  return body;
}

int splitWords(const String& line, String* out, int max) {
  int n = 0, i = 0, len = line.length();
  while (n < max) {
    while (i < len && isspace((unsigned char)line[i])) i++;
    if (i >= len) break;
    int start = i;
    while (i < len && !isspace((unsigned char)line[i])) i++;
    out[n++] = line.substring(start, i);
  }
  return n;
}

bool num(const String& s, float& v) {
  if (s.isEmpty() || s == "MM") return false;
  v = s.toFloat();
  return true;
}

// NDBC realtime files hold 45 days of rows, newest first (hundreds of KB).
// Read the header and the newest few rows only, then hang up. Fills `out`
// with up to `maxRows` of the newest rows where the first `required` columns
// are present (n values per row; the rest may be "MM"). Returns the count.
int ndbcRows(const char* file, const char* const* cols, int n, int required, String* out, int maxRows) {
  char url[96];
  snprintf(url, sizeof(url), "https://www.ndbc.noaa.gov/data/realtime2/%s", file);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  HTTPClient http;
  http.useHTTP10(true);               // plain stream, no chunked encoding
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return 0;
  http.setUserAgent(USER_AGENT);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d for %s\n", code, url);
    http.end();
    return 0;
  }
  Stream* s = http.getStreamPtr();

  String words[24];
  String header = s->readStringUntil('\n');
  header.replace("#", "");
  int nh = splitWords(header, words, 24);
  int idx[8];
  for (int c = 0; c < n; c++) {
    idx[c] = -1;
    for (int j = 0; j < nh; j++) if (words[j] == cols[c]) idx[c] = j;
    if (idx[c] < 0) { http.end(); return 0; }
  }
  s->readStringUntil('\n');           // units row

  int found = 0;
  for (int row = 0; row < 12 && found < maxRows; row++) {
    String line = s->readStringUntil('\n');
    if (line.isEmpty()) break;
    int nw = splitWords(line, words, 24);
    String* o = out + found * n;
    bool ok = true;
    for (int c = 0; c < n; c++) {
      o[c] = idx[c] < nw ? words[idx[c]] : "MM";
      if (c < required && o[c] == "MM") ok = false;
    }
    if (ok) found++;
  }
  http.end();
  return found;
}

/* ------------------------------- fetches ---------------------------------- */

// Breaking height is averaged over the last 2 hours (4 readings): single
// readings jump a whole 0.1 m step from one half hour to the next.
bool fetchBuoy() {
  static const char* const cols[] = {"WVHT", "DPD", "MWD", "WTMP"};
  String v[4 * 4];
  float wvht, dpd, mwd, wtmp;
  int rows = ndbcRows((String(WAVE_BUOY) + ".txt").c_str(), cols, 4, 2, v, 4);
  if (!rows || !num(v[0], wvht) || !num(v[1], dpd)) return false;

  fc.periodS = dpd;
  float sum = 0;
  for (int r = 0; r < rows; r++) {
    num(v[r * 4], wvht);
    num(v[r * 4 + 1], dpd);
    sum += breakingHeightFt(wvht * 3.28084f, dpd);
  }
  fc.surfFt = sum / rows;
  if (num(v[2], mwd)) fc.waveDir = int(mwd);
  if (num(v[3], wtmp)) { fc.seaTempF = wtmp * 9 / 5 + 32; fc.haveSeaTemp = true; }
  return true;
}

// Also replaces the buoy's breaking height with one from the swell part alone
// (2-hour average, like fetchBuoy): clean groundswell breaks bigger than the
// same total height of mixed chop.
void fetchSwell() {
  static const char* const cols[] = {"SwH", "SwP", "SwD", "WWH", "WWP"};
  String v[5 * 4];
  float h, p;
  int rows = ndbcRows((String(WAVE_BUOY) + ".spec").c_str(), cols, 5, 2, v, 4);
  if (!rows) return;
  float sum = 0;
  int used = 0;
  for (int r = 0; r < rows; r++) {
    float sh, sp, wh, wp;
    if (!num(v[r * 5], sh) || !num(v[r * 5 + 1], sp) || sp <= 0) continue;
    float ft = breakingHeightFt(sh * 3.28084f, sp, SWELL_CALIBRATION);
    if (num(v[r * 5 + 3], wh) && num(v[r * 5 + 4], wp) && wp > 0)
      ft = max(ft, WIND_SEA_WEIGHT * breakingHeightFt(wh * 3.28084f, wp, SWELL_CALIBRATION));
    sum += ft;
    used++;
  }
  if (used) fc.surfFt = sum / used;
  if (!num(v[0], h) || !num(v[1], p)) return;
  fc.haveSwell = true;
  fc.swellHt   = h * 3.28084f;
  fc.swellPer  = p;
  fc.swellDir  = v[2] == "MM" ? "" : v[2];
  if (num(v[3], h)) fc.swell2Ht = h * 3.28084f;
  if (num(v[4], p)) fc.swell2Per = p;
}

void fetchStationWind() {
  static const char* const cols[] = {"WSPD", "WDIR"};
  String v[2];
  float spd, dir;
  if (!ndbcRows((String(WIND_STATION) + ".txt").c_str(), cols, 2, 2, v, 1)) return;
  if (!num(v[0], spd) || !num(v[1], dir)) return;
  fc.windKt   = spd * 1.94384f;       // m/s -> kt
  fc.windDir  = int(dir);
  fc.haveWind = true;
}

// Air temp, wind if the pier station is down, and each day's morning wind.
void fetchWeather() {
  char url[400];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=%.3f&longitude=%.3f"
    "&current=wind_speed_10m,wind_direction_10m,temperature_2m"
    "&hourly=wind_speed_10m,wind_direction_10m&forecast_days=6"
    "&wind_speed_unit=kn&temperature_unit=fahrenheit&timezone=auto",
    SPOT_LAT, SPOT_LON);

  String body = httpGet(url);
  if (body.isEmpty()) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;

  JsonObject cur = doc["current"];
  if (!cur["temperature_2m"].isNull()) {
    fc.airTempF = cur["temperature_2m"];
    fc.haveAir = true;
  }
  if (!fc.haveWind && !cur["wind_speed_10m"].isNull()) {
    fc.windKt   = cur["wind_speed_10m"];
    fc.windDir  = int(cur["wind_direction_10m"] | 0.0f);
    fc.haveWind = true;
  }

  // Hourly starts at local midnight today: index = day * 24 + hour.
  // Average 6-10 am as a vector so 350 and 10 deg don't average to 180.
  JsonArray spd = doc["hourly"]["wind_speed_10m"];
  JsonArray dir = doc["hourly"]["wind_direction_10m"];
  for (int d = 0; d < 6; d++) {
    float x = 0, y = 0, sum = 0;
    int n = 0;
    for (int h = 6; h <= 10; h++) {
      int i = d * 24 + h;
      if (i >= (int)spd.size() || spd[i].isNull() || dir[i].isNull()) continue;
      float s = spd[i], a = dir[i].as<float>() * PI / 180;
      x += s * cosf(a); y += s * sinf(a); sum += s; n++;
    }
    if (!n) continue;
    fc.dayHaveWind[d] = true;
    fc.dayWindKt[d]   = sum / n;
    fc.dayWindDir[d]  = (int(atan2f(y, x) * 180 / PI) + 360) % 360;
  }
}

// The week from the wave model, plus today's waves if the buoy is down.
// The model reads open water, so the week is scaled by today's buoy / model
// ratio, exactly as Swellbar does it.
bool fetchMarine(bool buoyOk) {
  char url[512];
  snprintf(url, sizeof(url),
    "https://marine-api.open-meteo.com/v1/marine"
    "?latitude=%.3f&longitude=%.3f"
    "&current=wave_height,wave_period,wave_direction,swell_wave_height"
    ",swell_wave_period,swell_wave_direction,wind_wave_height,wind_wave_period"
    ",sea_surface_temperature"
    "&hourly=wave_height,wave_period,swell_wave_period"
    "&forecast_days=6&timezone=auto",
    SPOT_LAT, SPOT_LON);

  String body = httpGet(url);
  if (body.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  JsonObject cur = doc["current"];
  if (!buoyOk) {
    if (cur["wave_height"].isNull()) return false;
    float per = !cur["swell_wave_period"].isNull() ? cur["swell_wave_period"].as<float>()
                                                   : cur["wave_period"] | 0.0f;
    fc.periodS   = per;
    fc.surfFt    = breakingHeightFt(cur["wave_height"].as<float>() * 3.28084f, per);
    fc.waveDir   = cur["wave_direction"].isNull() ? -1 : int(cur["wave_direction"].as<float>());
    fc.fromModel = true;
    if (!cur["swell_wave_height"].isNull()) {
      fc.haveSwell = true;
      fc.swellHt   = cur["swell_wave_height"].as<float>() * 3.28084f;
      fc.swellPer  = cur["swell_wave_period"] | 0.0f;
      fc.swellDir  = compass(int(cur["swell_wave_direction"] | 0.0f));
      fc.swell2Ht  = (cur["wind_wave_height"] | 0.0f) * 3.28084f;
      fc.swell2Per = cur["wind_wave_period"] | 0.0f;
    }
  }
  if (!fc.haveSeaTemp && !cur["sea_surface_temperature"].isNull()) {
    fc.seaTempF = cur["sea_surface_temperature"].as<float>() * 9 / 5 + 32;
    fc.haveSeaTemp = true;
  }

  // Daytime (6 am - 6 pm) average breaking height and longest period per day.
  JsonArray times = doc["hourly"]["time"];
  JsonArray hts   = doc["hourly"]["wave_height"];
  JsonArray sper  = doc["hourly"]["swell_wave_period"];
  JsonArray wper  = doc["hourly"]["wave_period"];
  float sum[6] = {0}, maxPer[6] = {0}, nowFt = 0;
  int cnt[6] = {0};

  // The hour nearest now, from the model's own clock ("2026-10-25T19:15").
  String ct = cur["time"] | "";
  int nowIdx = ct.length() >= 16 ? ct.substring(11, 13).toInt() + (ct.substring(14, 16).toInt() >= 30) : -1;

  for (int i = 0; i < (int)times.size() && i < 144; i++) {
    if (hts[i].isNull()) continue;
    float per = !sper[i].isNull() ? sper[i].as<float>() : wper[i] | 0.0f;
    float ft  = breakingHeightFt(hts[i].as<float>() * 3.28084f, per);
    if (i == nowIdx) nowFt = ft;
    int d = i / 24, h = i % 24;
    if (h < 6 || h > 18) continue;
    sum[d] += ft; cnt[d]++;
    maxPer[d] = max(maxPer[d], per);
  }

  float ratio = 1;
  if (!fc.fromModel && nowFt > 0.3f) ratio = constrain(fc.surfFt / nowFt, 0.5f, 1.5f);

  static const char* dow[] = {"SU","MO","TU","WE","TH","FR","SA"};
  fc.dayCount = 0;
  for (int d = 0; d < 6 && cnt[d]; d++) {
    String day = times[d * 24].as<String>();       // "YYYY-MM-DDT00:00"
    int y  = day.substring(0, 4).toInt();
    int m  = day.substring(5, 7).toInt();
    int dd = day.substring(8, 10).toInt();
    // Sakamoto's algorithm
    static int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    int wd = (y + y/4 - y/100 + y/400 + t[m-1] + dd) % 7;

    fc.dayLabel[d]  = dow[wd];
    // Today shows the headline number, so the first bar always agrees with it.
    fc.daySurf[d]   = d == 0 ? fc.surfFt : sum[d] / cnt[d] * ratio;
    fc.dayPeriod[d] = d == 0 ? fc.periodS : maxPer[d];
    fc.dayLevel[d]  = d == 0 ? todayLevel()
                             : rating(surfRange(fc.daySurf[d]), fc.dayPeriod[d],
                                      fc.dayHaveWind[d], fc.dayWindKt[d], fc.dayWindDir[d]);
    fc.dayCount++;
  }
  return fc.fromModel || buoyOk;
}

// The next highs and lows from an hour ago, skipping turns of under 0.3 ft
// (Surfline and Swellbar skip a 0.1 ft "low" sitting between two highs).
bool fetchTides(bool haveClock) {
  char when[40] = "date=today";
  if (haveClock) {
    time_t t = time(nullptr) - 3600;
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(when, sizeof(when), "begin_date=%Y%m%d%%20%H:%M&range=48", &lt);
  }
  char url[420];
  snprintf(url, sizeof(url),
    "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter"
    "?product=predictions&application=SwellStrip&datum=MLLW"
    "&station=%s&time_zone=lst_ldt&units=english"
    "&interval=hilo&format=json&%s",
    TIDE_STATION, when);

  String body = httpGet(url);
  if (body.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  JsonArray preds = doc["predictions"];
  if (preds.isNull()) return false;

  String out;
  int shown = 0;
  float last = -99;
  for (JsonObject p : preds) {
    if (shown >= 3) break;
    float v = p["v"].as<String>().toFloat();
    if (fabsf(v - last) < 0.3f) continue;
    last = v;

    String t = p["t"].as<String>();          // "YYYY-MM-DD HH:MM"
    String type = p["type"].as<String>();    // "H" or "L"

    int hh = t.substring(11,13).toInt();
    int mm = t.substring(14,16).toInt();
    const char* ap = hh >= 12 ? "p" : "a";
    int h12 = hh % 12; if (h12 == 0) h12 = 12;

    char buf[24];
    snprintf(buf, sizeof(buf), "%s %d:%02d%s", type.c_str(), h12, mm, ap);
    if (shown) out += "  ";
    out += buf;
    shown++;
  }
  if (shown) fc.tideText = out;
  return shown > 0;
}

/* ------------------------------- drawing ---------------------------------- */

// 50% checkerboard — the only "grey" a 1-bit panel has.
void ditherRect(int x, int y, int w, int h) {
  for (int j = 0; j < h; j++)
    for (int i = (j & 1); i < w; i += 2)
      display.drawPixel(x + i, y + j, GxEPD_BLACK);
}

void drawHeadline() {
  int x = X1 + 16;

  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(x, 32);
  display.print(SPOT_NAME);

  display.setFont(&FreeSans9pt7b);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(SPOT_REGION, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(X1 + COL1 - 16 - bw, 32);
  display.print(SPOT_REGION);

  Range r = surfRange(fc.surfFt);
  String range = rangeText(r);

  // Big number. FreeSansBold24pt at scale 2 is the largest legible option
  // without shipping a custom font — see the note at the bottom of this file.
  display.setFont(&FreeSansBold24pt7b);
  display.setTextSize(2);
  display.setCursor(x - 4, 128);
  display.print(range);
  display.setTextSize(1);

  display.getTextBounds(range, 0, 0, &bx, &by, &bw, &bh);
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(x - 4 + bw * 2 + 10, 128);
  display.print(r.plus ? "FT+" : "FT");

  // "SHOULDER TO HEAD HIGH" is wider than the column in bold; drop to regular.
  const char* words = bodyWords(r);
  display.setFont(&FreeSansBold9pt7b);
  display.getTextBounds(words, 0, 0, &bx, &by, &bw, &bh);
  if ((int)bw > COL1 - 32) display.setFont(&FreeSans9pt7b);
  display.setCursor(x, 158);
  display.print(words);

  // rating pips
  int rating = rateConditions();
  int px = x, py = 176;
  for (int i = 0; i < 5; i++) {
    display.drawRect(px, py, 15, 9, GxEPD_BLACK);
    if (i < rating) display.fillRect(px, py, 15, 9, GxEPD_BLACK);
    px += 18;
  }

  display.setFont(&FreeSans9pt7b);
  display.setCursor(x, 208);
  display.print(fc.fromModel ? "MODEL " : "BUOY ");
  if (fc.waveDir >= 0) {
    display.print(compass(fc.waveDir));
    display.print("  ");
  }
  display.print(int(fc.periodS + 0.5f));
  display.print("S");

  // The verdict. Everything above this is data; this is the answer.
  display.drawLine(x, 222, X1 + COL1 - 16, 222, GxEPD_BLACK);

  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(x, 248);
  display.print(verdict());

  display.setFont(&FreeSans9pt7b);
  display.setCursor(x, 266);
  display.print(reason());
}

// Two labelled temperatures on one line. AIR sits on the left edge; SEA is
// right-aligned as a group so its value keeps the same right margin as every
// other value in the column.
void tempRow(int y, int airF, int seaF) {
  const int x = X2 + 16;
  const int right = X2 + COL2 - 16;
  const int GAP = 8;
  int16_t bx, by;
  uint16_t airLw, airVw, seaLw, seaVw, h;
  char airBuf[8], seaBuf[8];

  snprintf(airBuf, sizeof(airBuf), "%dF", airF);
  snprintf(seaBuf, sizeof(seaBuf), "%dF", seaF);

  display.setFont(&FreeSans9pt7b);
  display.getTextBounds("AIR", 0, 0, &bx, &by, &airLw, &h);
  display.getTextBounds("SEA", 0, 0, &bx, &by, &seaLw, &h);
  display.setFont(&FreeSansBold12pt7b);
  display.getTextBounds(airBuf, 0, 0, &bx, &by, &airVw, &h);
  display.getTextBounds(seaBuf, 0, 0, &bx, &by, &seaVw, &h);

  int seaX = right - (seaLw + GAP + seaVw);

  // Guard: three-digit temps on both sides still clear by ~26px, but drop the
  // F suffixes rather than ever letting the two groups touch.
  if (x + airLw + GAP + (int)airVw >= seaX - 6) {
    snprintf(airBuf, sizeof(airBuf), "%d", airF);
    snprintf(seaBuf, sizeof(seaBuf), "%d", seaF);
    display.getTextBounds(airBuf, 0, 0, &bx, &by, &airVw, &h);
    display.getTextBounds(seaBuf, 0, 0, &bx, &by, &seaVw, &h);
    seaX = right - (seaLw + GAP + seaVw);
  }

  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x, y);                   display.print("AIR");
  display.setCursor(seaX, y);                display.print("SEA");
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(x + airLw + GAP, y);     display.print(airBuf);
  display.setCursor(seaX + seaLw + GAP, y);  display.print(seaBuf);

  display.drawLine(x, y + 9, right, y + 9, GxEPD_BLACK);
}

void detailRow(int y, const char* key, const String& valueIn) {
  int x = X2 + 16;
  int right = X2 + COL2 - 16;
  int16_t bx, by;
  uint16_t kw, kh, bw, bh;
  String value = valueIn;

  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeSans9pt7b);
  display.getTextBounds(key, 0, 0, &bx, &by, &kw, &kh);
  display.setCursor(x, y);
  display.print(key);

  // Measure the label, then fit the value into whatever space is actually
  // left. The original just right-aligned the value and hoped — on real
  // hardware "PRIMARY" and "2.2FT 13S SSW" ran straight through each other.
  // Drop a font size rather than overlap.
  const int GAP = 10;
  int avail = right - (x + kw) - GAP;

  display.setFont(&FreeSansBold12pt7b);
  display.getTextBounds(value, 0, 0, &bx, &by, &bw, &bh);
  if ((int)bw > avail) {
    display.setFont(&FreeSansBold9pt7b);
    display.getTextBounds(value, 0, 0, &bx, &by, &bw, &bh);
  }

  // Last resort. One font step down isn't always enough — "WINDSWELL" beside
  // "0.0FT 0S WSW" overran even at 9pt and drew straight through the label.
  // Drop trailing words until it fits: losing the least important token beats
  // overlapping text every time.
  while ((int)bw > avail && value.lastIndexOf(' ') > 0) {
    value = value.substring(0, value.lastIndexOf(' '));
    display.getTextBounds(value, 0, 0, &bx, &by, &bw, &bh);
  }

  display.setCursor(right - bw, y);
  display.print(value);

  display.drawLine(x, y + 9, right, y + 9, GxEPD_BLACK);
}

void drawDetails() {
  char buf[48];

  if (fc.haveSwell) {
    snprintf(buf, sizeof(buf), "%.1fFT %dS %s",
             fc.swellHt, int(fc.swellPer + 0.5f), fc.swellDir.c_str());
    detailRow(38, "SWELL", buf);
  } else {
    detailRow(38, "SWELL", "--");
  }

  // No direction on this row — the WIND row underneath already gives one, and
  // when the windswell is flat a direction is meaningless anyway.
  if (!fc.haveSwell || fc.swell2Ht < 0.2f) {
    detailRow(78, "WINDSWELL", "NONE");
  } else {
    snprintf(buf, sizeof(buf), "%.1fFT %dS",
             fc.swell2Ht, int(fc.swell2Per + 0.5f));
    detailRow(78, "WINDSWELL", buf);
  }

  if (fc.haveWind) {
    snprintf(buf, sizeof(buf), "%dKT %s", int(fc.windKt + 0.5f), compass(fc.windDir));
    detailRow(118, "WIND", buf);
  } else {
    detailRow(118, "WIND", "--");
  }

  if (fc.haveSeaTemp && fc.haveAir) {
    tempRow(158, int(fc.airTempF + 0.5f), int(fc.seaTempF + 0.5f));
  } else if (fc.haveSeaTemp) {
    snprintf(buf, sizeof(buf), "%dF", int(fc.seaTempF + 0.5f));
    detailRow(158, "SEA", buf);
  } else {
    snprintf(buf, sizeof(buf), "%dF", int(fc.airTempF + 0.5f));
    detailRow(158, "AIR", fc.haveAir ? String(buf) : String("--"));
  }

  display.setFont(&FreeSans9pt7b);
  display.setCursor(X2 + 16, 200);
  display.print("TIDE");

  display.setFont(&FreeSansBold9pt7b);
  display.setCursor(X2 + 16, 224);
  display.print(fc.tideText);
}

void drawOutlook() {
  if (fc.dayCount == 0) return;

  // The acrylic case overlaps the last few millimetres of the panel, so keep
  // the day columns clear of the right edge.
  const int RIGHT_MARGIN = 30;
  int colW = (COL3 - RIGHT_MARGIN) / fc.dayCount;

  float maxH = 0.5f;
  for (int i = 0; i < fc.dayCount; i++) maxH = max(maxH, fc.daySurf[i]);

  int barTop = 96, barBottom = 236, barSpan = barBottom - barTop;

  // Which day is actually BEST, not merely biggest: the highest rating, the
  // same one Swellbar colours each day by (size, long-period bump, morning
  // wind). Ties break on height.
  int peak = 0;
  for (int i = 1; i < fc.dayCount; i++) {
    if (fc.dayLevel[i] > fc.dayLevel[peak] ||
        (fc.dayLevel[i] == fc.dayLevel[peak] && fc.daySurf[i] > fc.daySurf[peak]))
      peak = i;
  }

  for (int i = 0; i < fc.dayCount; i++) {
    int cx = X3 + i * colW;
    if (i) display.drawLine(cx, 0, cx, H, GxEPD_BLACK);

    // Two different marks so they can never be confused:
    //   TODAY     -> day label reversed out in a black chip
    //   BEST DAY  -> solid black bar (below)
    // If today happens to be the best day you get both, which reads correctly.
    display.setFont(&FreeSansBold9pt7b);
    int16_t bx, by; uint16_t bw, bh;
    display.getTextBounds(fc.dayLabel[i], 0, 0, &bx, &by, &bw, &bh);
    int lx = cx + (colW - bw) / 2;

    if (i == 0) {
      display.fillRect(lx - 6, 14, bw + 12, 22, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
    } else {
      display.setTextColor(GxEPD_BLACK);
    }
    display.setCursor(lx, 30);
    display.print(fc.dayLabel[i]);
    display.setTextColor(GxEPD_BLACK);

    // The same "3-4" range Swellbar's outlook shows; "10-13" needs the
    // smaller font to fit a column.
    String ht = rangeText(surfRange(fc.daySurf[i]));
    display.setFont(&FreeSansBold12pt7b);
    display.getTextBounds(ht, 0, 0, &bx, &by, &bw, &bh);
    if ((int)bw > colW - 4) {
      display.setFont(&FreeSansBold9pt7b);
      display.getTextBounds(ht, 0, 0, &bx, &by, &bw, &bh);
    }
    display.setCursor(cx + (colW - bw) / 2, 62);
    display.print(ht);

    if (!OUTLOOK_AS_LINE) {
      int barH = int(barSpan * (fc.daySurf[i] / maxH));
      if (barH < 3) barH = 3;
      int barW = 17;
      int bxx = cx + (colW - barW) / 2;
      int byy = barBottom - barH;

      display.drawRect(bxx, byy, barW, barH, GxEPD_BLACK);
      if (i == peak) display.fillRect(bxx, byy, barW, barH, GxEPD_BLACK);
      else           ditherRect(bxx + 1, byy + 1, barW - 2, barH - 2);
    }

    if (fc.dayPeriod[i] > 0) {
      String pr = String(int(fc.dayPeriod[i] + 0.5f)) + "S";
      display.setFont(&FreeSans9pt7b);
      display.getTextBounds(pr, 0, 0, &bx, &by, &bw, &bh);
      display.setCursor(cx + (colW - bw) / 2, 258);
      display.print(pr);
    }
  }

  // Line style: one pass across all six points, drawn after the columns so
  // it sits on top of the dividers.
  if (OUTLOOK_AS_LINE) {
    display.drawLine(X3 + 6, barBottom, X3 + colW * fc.dayCount - 6,
                     barBottom, GxEPD_BLACK);

    int px = 0, py = 0;
    for (int i = 0; i < fc.dayCount; i++) {
      int x = X3 + i * colW + colW / 2;
      int y = barBottom - int(barSpan * (fc.daySurf[i] / maxH));

      if (i) {
        // 3 px thick — a single-pixel diagonal is nearly invisible on e-paper
        for (int t = -1; t <= 1; t++)
          display.drawLine(px, py + t, x, y + t, GxEPD_BLACK);
      }
      px = x; py = y;
    }

    for (int i = 0; i < fc.dayCount; i++) {
      int x = X3 + i * colW + colW / 2;
      int y = barBottom - int(barSpan * (fc.daySurf[i] / maxH));
      int r = (i == peak) ? 6 : 4;
      display.fillCircle(x, y, r, GxEPD_BLACK);
      if (i != peak) display.fillCircle(x, y, r - 2, GxEPD_WHITE);
    }
  }
}

void drawOffline(const char* why) {
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeSansBold24pt7b);
  display.setCursor(40, 130);
  display.print("NO DATA");
  display.setFont(&FreeSans9pt7b);
  display.setCursor(40, 168);
  display.print(why);
  display.setCursor(40, 192);
  display.print("Retrying at the top of the hour, or press any button.");
}

void render(bool fullClear) {
  display.setFullWindow();

  // A full white flush clears accumulated ghosting, but it doubles the visible
  // black-white flashing, so only do it on cold boot (when the layout may have
  // just changed) and once a day after that.
  if (fullClear) display.clearScreen();

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    if (!fc.ok) {
      drawOffline("Could not reach the buoy or the forecast APIs.");
    } else {
      display.drawLine(X2 - 1, 0, X2 - 1, H, GxEPD_BLACK);
      display.drawLine(X3 - 1, 0, X3 - 1, H, GxEPD_BLACK);
      drawHeadline();
      drawDetails();
      drawOutlook();
    }
  } while (display.nextPage());
}

/* -------------------------------- main ------------------------------------ */

bool haveClock() { return time(nullptr) > 1700000000; }

// Seconds until the top of the next hour, plus a few so the clock has
// definitely rolled over. Without a clock, just wait an hour.
uint32_t secondsToNextHour() {
  if (!haveClock()) return 3600;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  return 3600 - (lt.tm_min * 60 + lt.tm_sec) + 5;
}

void sleepFor(uint32_t seconds) {
  digitalWrite(EPD_POWER, LOW);       // cut panel rail
  digitalWrite(LED_PIN, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // A button still held down would wake us straight back up.
  unsigned long t0 = millis();
  for (int p : BUTTONS)
    while (digitalRead(p) == LOW && millis() - t0 < 3000) delay(10);

  uint64_t mask = 0;
  for (int p : BUTTONS) {
    mask |= 1ULL << p;
    rtc_gpio_pullup_en((gpio_num_t)p);
    rtc_gpio_pulldown_dis((gpio_num_t)p);
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // keeps the pull-ups on
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

  Serial.printf("Sleeping %u s.\n", seconds);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool timerWake  = cause == ESP_SLEEP_WAKEUP_TIMER;
  bool buttonWake = cause == ESP_SLEEP_WAKEUP_EXT1;
  Serial.printf("wake #%d (%s)\n", wakeCount,
                timerWake ? "timer" : buttonWake ? "button" : "power on");

  for (int p : BUTTONS) {
    rtc_gpio_deinit((gpio_num_t)p);   // hand the pins back from the RTC domain
    pinMode(p, INPUT_PULLUP);
  }
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);        // lit while it works, so a press shows

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

  bool online = WiFi.status() == WL_CONNECTED;
  if (online) {
    Serial.printf("WiFi ok, %s\n", WiFi.localIP().toString().c_str());
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    struct tm lt;
    getLocalTime(&lt, 10000);
  } else {
    Serial.println("WiFi failed.");
  }

  // The sleep timer runs on an RC clock that can drift a minute or two per
  // hour. If it woke us just before the hour, nap until the hour so the
  // refresh still lands on it.
  if (timerWake && haveClock()) {
    uint32_t s = secondsToNextHour();
    if (s < 300) sleepFor(s);
  }

  if (online) {
    bool buoy = fetchBuoy();
    if (buoy) fetchSwell();
    fetchStationWind();
    fetchWeather();                   // air temp, wind fallback, morning winds
    bool marine = fetchMarine(buoy);  // week, plus waves if the buoy is down
    fetchTides(haveClock());
    fc.ok = buoy || marine;
    Serial.printf("surf %.2f ft (%s), %.0fs, wind %.1f kt from %d, %s\n", fc.surfFt,
                  fc.fromModel ? "model" : "buoy", fc.periodS, fc.windKt, fc.windDir, verdict().c_str());
  }

  pinMode(EPD_POWER, OUTPUT);
  digitalWrite(EPD_POWER, HIGH);      // panel rail on — required before init
  delay(100);

  // Order matters: GxEPD2's init() calls SPI.begin() with the board defaults,
  // so the remap has to happen AFTER it or it gets clobbered. This is the
  // pattern from GxEPD2's own ESP32 example.
  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(EINK_SCK, -1, EINK_MOSI, EINK_CS);
  display.setRotation(2);   // Set on hardware by James. The clearScreen() call
                            // in render() wipes ghosting from the previous
                            // orientation on the first flash after a change.

  // Full ghost-clearing flush on power-on and on the 4 am refresh.
  bool fullClear = !timerWake && !buttonWake;
  if (timerWake && haveClock()) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    fullClear = lt.tm_hour == 4;
  }

  render(fullClear);
  display.hibernate();
  wakeCount++;
  sleepFor(secondsToNextHour());
}

void loop() { /* never runs — setup() ends in deep sleep */ }

/* ============================================================================
   NOTE ON THE BIG NUMBER

   Adafruit GFX tops out at FreeSansBold24pt7b (~34 px cap height). The layout
   above scales it 2x, which is readable but slightly blocky up close. For the
   crisp 90 px numerals in the mockup, generate a real font once:

     rop.nl/truetype2gfx/   ->  upload any TTF, pick 72 pt, download the .h
     drop it beside this .ino, #include it, and swap the setTextSize(2) block

   That is the single change that takes this from "clearly a DIY project" to
   "looks bought". Everything else here is final.
   ========================================================================== */
