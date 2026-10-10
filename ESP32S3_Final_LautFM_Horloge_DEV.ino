



#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <TFT_eSPI.h>
#include "U8g2_for_TFT_eSPI.h"
#include <TJpg_Decoder.h>
#include <Audio.h>

#define USE_PNG 1        // 1 = logos PNG gérés (bibliothèque PNGdec)
#if USE_PNG
  #include <PNGdec.h>
#endif

#include "types.h"       // Mode, ImgSlot, Scroller

// À ADAPTER
const char* WIFI_SSID = "Bbox-E295749E-2,4GHz";
const char* WIFI_PASS = "Mb&2427242711";

// I2S -> MAX98357A
#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 4

// Tactile FT6336U (I2C 0x38)
#define TP_ADDR 0x38
#define TP_SDA  8
#define TP_SCL  9
#define TP_RST  -1

// Panneau natif (portrait)
#define PANEL_W 320
#define PANEL_H 480

#define POLL_MS 10000 // mise à jour artiste/titre/pochette

// Horloge
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Paris
const bool  SMOOTH_SECONDS = true;        // true = aiguille fluide
const bool  SHOW_DATE_TIME = true;        // date en haut, heure numérique en bas
const uint16_t FRAME_MS = 40;             // ~25 images/s max
#define SCR_W 480                         // paysage (rotation 1)
#define SCR_H 320

TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
Audio audio;
Preferences prefs;

enum Page { PAGE_CLOCK, PAGE_LIST, PAGE_PLAYER };
volatile Page page = PAGE_CLOCK;

Mode mode = M_LETTER;

// Liste complète des stations (mode ABC) : blocs uniques en PSRAM
const int    MAX_STATIONS = 20000;
const size_t NAMEBUF_SIZE = 700 * 1024;
char*     nameBuf    = nullptr;
uint32_t* nameOff    = nullptr;
int       nameCount  = 0;
int*      shownIdx   = nullptr;
int       shownCount = 0;

// Genres et stations d'un genre (PSRAM)
const int    MAX_GENRES   = 800;
const size_t GENBUF_SIZE  = 64 * 1024;
char*     genBuf   = nullptr;
uint32_t* genOff   = nullptr;
int       genreCount = 0;

const int    MAX_GS      = 1500;
const size_t GSBUF_SIZE  = 96 * 1024;
char*     gsBuf   = nullptr;
uint32_t* gsOff   = nullptr;
int       gsCount = 0;

bool genreStationsView = false; // mode GEN : false = liste des genres, true = stations
int  genreCur = -1;

// Favoris (mémoire permanente) 
const int MAX_FAV = 40;
char favs[MAX_FAV][64];
int  favCount = 0;
int  confirmAct = 0;            // 0 = aucune, 1 = supprimer un favori, 2 = vider la liste
int  confirmIdx = -1;           // favori concerné (cas 1)

// Appui long en mode FAV
bool     favPress = false;
uint32_t favT0 = 0;
int      favIdx = -1;           // >= 0 : ligne ; -2 : bouton central

int letterIdx = 0;              // 0..25 = A..Z, 26 = chiffres (#)
int topIdx = 0;
int volume = 10;                // 0..21

String curStation = "";
String curArtist  = "";
String curTitle   = "";

// Mise en page : liste
const int HEADER_H = 36;
const int LIST_Y   = 38;
const int ROW_H    = 34;
const int ROWS     = 6;
const int BTN_Y    = 268;
const int BTN_H    = 48;
const int BTN_W    = 60;         // 8 boutons de 60 px

// Mise en page : lecteur
const int PB_Y = 272, PB_H = 42;
const int PB_VM_X = 10,  PB_VM_W = 110;
const int PB_ST_X = 185, PB_ST_W = 110;
const int PB_VP_X = 360, PB_VP_W = 110;

// Logo de la station (à gauche, devant le nom)
const int LOGO_X = 6, LOGO_Y = 40, LOGO_S = 80;
const int STATION_TEXT_X = 96;

// Pochette artiste (en haut à droite, sur la ligne STATION)
const int BOX_X = 418, BOX_Y = 44, BOX_W = 56, BOX_H = 52;
const int INNER_W = BOX_W - 4, INNER_H = BOX_H - 4;

// Communication entre tâches (le dessin se fait uniquement dans loop()) 
volatile bool newStream   = false;
volatile bool stopStream  = false;
volatile bool volDirty    = false;
volatile bool apiRefresh  = false;
volatile bool songDirty   = false;
volatile bool thumbReset  = false;
volatile bool logoReset   = false;
volatile bool stationDirty = false;
volatile int  netReq      = 0;      // 1 = genres, 2 = stations d'un genre
volatile bool listLoading = false;
volatile bool listReady   = false;
volatile bool listFail    = false;
char reqGenre[64]    = "";
char pendingUrl[160];
char apiStation[64]  = "";          // identifiant (slug) de la station en lecture
char songArtist[96]  = "";
char songTitle[128]  = "";
char stationDisp[64] = "";

bool wifiOk = true;
bool streamActive = false;
uint32_t lastAudioOk = 0;
uint32_t loadingSince = 0;

// Images téléchargées (tâche réseau -> loop)
ImgSlot thumbSlot = { nullptr, 0, false };
ImgSlot logoSlot  = { nullptr, 0, false };
SemaphoreHandle_t imgMux;

// Textes défilants
Scroller scStation, scArtist, scTitle, hdrSc;

// ArduinoJson : documents volumineux en PSRAM
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
SpiRamAllocator spiRamAlloc;

// ==========================
//          HORLOGE
// ==========================
TFT_eSprite spr = TFT_eSprite(&tft);

// Géométrie : le cadran occupe un carré de 320x320, centré à l'horizontale
const int FACE   = 320;
const int CX     = 160;       // centre dans le sprite
const int CY     = 160;
const int FACE_X = 80;        // (480 - 320) / 2
const int FACE_Y = 0;
const int BAND_H = 40;
const int NBANDS = FACE / BAND_H;

const float R_FACE  = 158.0f;
const float R_OUT   = 141.0f;
const float R_IN    = 112.0f;
const float R_INNER = 78.0f;

const int TRAIL_N = 26;
const float TRAIL_STEP = 2.5f;

uint16_t colBlack, colFace, colBorder, colOff, colOn, colOnHi, colHour, colMin, colInner, colHubDark;
uint16_t trailCol[TRAIL_N];

int16_t segX[60][4], segY[60][4];
int16_t segMinY[60], segMaxY[60];

int yOff = 0;
uint32_t lastFrame = 0;

uint16_t mix565(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2, float t) {
  return tft.color565(r1 + (r2 - r1) * t, g1 + (g2 - g1) * t, b1 + (b2 - b1) * t);
}

inline float cpx(float aDeg, float r) { return CX + r * sinf(aDeg * DEG_TO_RAD); }
inline float cpy(float aDeg, float r) { return CY - r * cosf(aDeg * DEG_TO_RAD); }

void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
  spr.fillTriangle((int)(x0 + 0.5f), (int)(y0 + 0.5f) - yOff,
                   (int)(x1 + 0.5f), (int)(y1 + 0.5f) - yOff,
                   (int)(x2 + 0.5f), (int)(y2 + 0.5f) - yOff, c);
}

void thickLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) return;
  float nx = -dy / len * w * 0.5f;
  float ny =  dx / len * w * 0.5f;
  tri(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
  tri(x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, c);
}

void hand(float aDeg, float tail, float length, float width, uint16_t c) {
  thickLine(cpx(aDeg, -tail), cpy(aDeg, -tail), cpx(aDeg, length), cpy(aDeg, length), width, c);
}

void initColors() {
  colBlack   = tft.color565(0, 0, 0);
  colFace    = tft.color565(14, 15, 19);
  colBorder  = tft.color565(45, 46, 50);
  colOff     = tft.color565(32, 33, 36);
  colOn      = tft.color565(0, 190, 210);
  colOnHi    = tft.color565(60, 235, 255);
  colHour    = tft.color565(205, 205, 208);
  colMin     = tft.color565(160, 160, 165);
  colInner   = tft.color565(38, 40, 45);
  colHubDark = tft.color565(6, 16, 20);

  for (int k = 0; k < TRAIL_N; k++) {
    float t = 0.42f * powf(1.0f - (float)k / TRAIL_N, 1.7f);
    trailCol[k] = mix565(14, 15, 19, 0, 190, 210, t);
  }
}

void initSegments() {
  for (int i = 0; i < 60; i++) {
    float a0 = i * 6.0f + 1.2f;
    float a1 = (i + 1) * 6.0f - 1.2f;
    segX[i][0] = cpx(a0, R_OUT); segY[i][0] = cpy(a0, R_OUT);
    segX[i][1] = cpx(a1, R_OUT); segY[i][1] = cpy(a1, R_OUT);
    segX[i][2] = cpx(a1, R_IN);  segY[i][2] = cpy(a1, R_IN);
    segX[i][3] = cpx(a0, R_IN);  segY[i][3] = cpy(a0, R_IN);
    int mn = 9999, mx = -9999;
    for (int k = 0; k < 4; k++) {
      if (segY[i][k] < mn) mn = segY[i][k];
      if (segY[i][k] > mx) mx = segY[i][k];
    }
    segMinY[i] = mn - 1;
    segMaxY[i] = mx + 1;
  }
}

void drawBand(int secInt, float secAngle, float minAngle, float hourAngle) {
  spr.fillSprite(colBlack);

  spr.fillCircle(CX, CY - yOff, (int)R_FACE, colFace);
  spr.drawCircle(CX, CY - yOff, (int)R_FACE + 1, colBorder);

  for (int i = 0; i < 60; i++) {
    if (segMaxY[i] < yOff || segMinY[i] >= yOff + BAND_H) continue;
    uint16_t c = colOff;
    if (i <= secInt) c = (i == secInt) ? colOnHi : colOn;
    tri(segX[i][0], segY[i][0], segX[i][1], segY[i][1], segX[i][2], segY[i][2], c);
    tri(segX[i][0], segY[i][0], segX[i][2], segY[i][2], segX[i][3], segY[i][3], c);
  }

  const float TR = R_IN - 3.0f;
  for (int k = 0; k < TRAIL_N; k++) {
    float a0 = secAngle - k * TRAIL_STEP;
    float a1 = a0 - TRAIL_STEP - 0.3f;
    tri(CX, CY, cpx(a0, TR), cpy(a0, TR), cpx(a1, TR), cpy(a1, TR), trailCol[k]);
  }

  spr.drawCircle(CX, CY - yOff, (int)R_INNER, colInner);

  hand(hourAngle, 18, 88, 7, colHour);
  hand(minAngle,  20, 122, 5, colMin);
  hand(secAngle, 30, 132, 2.2f, colOnHi);

  spr.fillCircle(CX, CY - yOff, 8, colOn);
  spr.fillCircle(CX, CY - yOff, 4, colHubDark);
}

// Heure de secours : date/heure de compilation
void setFallbackTime() {
  static const char* mois[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char mon[4] = "Jan";
  int day = 1, year = 2026, h = 0, m = 0, sec = 0;
  sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &h, &m, &sec);
  int mi = 0;
  for (int i = 0; i < 12; i++) if (strncmp(mon, mois[i], 3) == 0) mi = i;

  setenv("TZ", TZ_INFO, 1);
  tzset();
  struct tm tmv = {};
  tmv.tm_year = year - 1900;
  tmv.tm_mon = mi;
  tmv.tm_mday = day;
  tmv.tm_hour = h;
  tmv.tm_min = m;
  tmv.tm_sec = sec;
  tmv.tm_isdst = -1;
  struct timeval tv = { mktime(&tmv), 0 };
  settimeofday(&tv, nullptr);
}

// Une image de l'horloge (appelée à chaque tour de loop en mode horloge)
void clockLoop() {
  if (!spr.created()) return;
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm t;
  localtime_r(&tv.tv_sec, &t);

  float ms = tv.tv_usec / 1000.0f;
  float secF = SMOOTH_SECONDS ? (t.tm_sec + ms / 1000.0f) : (float)t.tm_sec;

  float secAngle  = secF * 6.0f;
  float minAngle  = (t.tm_min + t.tm_sec / 60.0f) * 6.0f;
  float hourAngle = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 30.0f;

  for (int b = 0; b < NBANDS; b++) {
    yOff = b * BAND_H;
    drawBand(t.tm_sec, secAngle, minAngle, hourAngle);
    spr.pushSprite(FACE_X, FACE_Y + yOff);
  }
}

// ========================
//          RADIO
// ========================
// Polices U8g2 (accents inclus, Latin-1)
const uint8_t* const FONTS[4] = {
  u8g2_font_profont29_tf, u8g2_font_profont22_tf,
  u8g2_font_profont17_tf, u8g2_font_profont15_tf
};

void chopUtf8(String& s) {
  if (!s.length()) return;
  int i = s.length() - 1;
  while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
  s.remove(i);
}

// Texte fixe (réduit puis tronqué) : utilisé pour les listes et la ligne d'état
void drawTextFit(const String& text, int x, int yc, int maxW, uint16_t color, int firstFont) {
  String t = text;
  u8f.setForegroundColor(color);
  int f = firstFont;
  for (; f < 4; f++) {
    u8f.setFont(FONTS[f]);
    if (u8f.getUTF8Width(t.c_str()) <= maxW) break;
  }
  if (f >= 4) {
    u8f.setFont(FONTS[3]);
    while (t.length() > 1 && u8f.getUTF8Width((t + "~").c_str()) > maxW) chopUtf8(t);
    t += "~";
  }
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  u8f.setCursor(x, yc + (asc + desc) / 2);
  u8f.print(t);
}

// Texte défilant
void scrollInit(Scroller& s, int x, int y, int w, int h, uint16_t color, uint16_t bg, int fFirst, int fLast) {
  s.x = x; s.y = y; s.w = w; s.h = h; s.color = color; s.bg = bg;
  s.fFirst = fFirst; s.fLast = fLast;
}

void scrollRender(Scroller& s) {
  tft.setViewport(s.x, s.y, s.w, s.h);
  tft.fillRect(0, 0, s.w, s.h, s.bg);
  u8f.setFont(FONTS[s.font]);
  u8f.setForegroundColor(s.color);
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  u8f.setCursor(s.scrolling ? -s.offset : 0, s.h / 2 + (asc + desc) / 2);
  u8f.print(s.text);
  tft.resetViewport();
}

void scrollSet(Scroller& s, const String& text) {
  s.text = text.length() ? text : String("-");
  int f = s.fFirst;
  for (; f <= s.fLast; f++) {
    u8f.setFont(FONTS[f]);
    if (u8f.getUTF8Width(s.text.c_str()) <= s.w) break;
  }
  if (f > s.fLast) { f = s.fLast; u8f.setFont(FONTS[f]); s.scrolling = true; }
  else s.scrolling = false;
  s.font = f;
  s.textW = u8f.getUTF8Width(s.text.c_str());
  s.offset = 0; s.dir = 1;
  s.pauseUntil = millis() + 1500;
  s.lastStep = millis();
  scrollRender(s);
}

void scrollTick(Scroller& s) {
  if (!s.scrolling) return;
  uint32_t now = millis();
  if (now < s.pauseUntil || now - s.lastStep < 35) return;
  s.lastStep = now;
  int maxOff = s.textW - s.w + 8;
  s.offset += s.dir * 2;
  if (s.offset >= maxOff) { s.offset = maxOff; s.dir = -1; s.pauseUntil = now + 1200; }
  else if (s.offset <= 0) { s.offset = 0;      s.dir = 1;  s.pauseUntil = now + 1200; }
  scrollRender(s);
}

// Étoile (favoris) 
void drawStar(int cx, int cy, int R, uint16_t color, bool filled) {
  int sx[10], sy[10];
  for (int i = 0; i < 10; i++) {
    float a = -PI / 2 + i * PI / 5;
    float r = (i % 2 == 0) ? R : R * 0.42f;
    sx[i] = cx + (int)roundf(cosf(a) * r);
    sy[i] = cy + (int)roundf(sinf(a) * r);
  }
  for (int i = 0; i < 10; i++) {
    int j = (i + 1) % 10;
    if (filled) tft.fillTriangle(cx, cy, sx[i], sy[i], sx[j], sy[j], color);
    else        tft.drawLine(sx[i], sy[i], sx[j], sy[j], color);
  }
}

// Favoris : mémoire permanente 
void favsLoad() {
  favCount = 0;
  prefs.begin("radio", true);
  String s = prefs.getString("favs", "");
  prefs.end();
  int i = 0;
  while (i < (int)s.length() && favCount < MAX_FAV) {
    int e = s.indexOf('\n', i);
    if (e < 0) e = s.length();
    if (e > i) {
      String n = s.substring(i, e);
      strlcpy(favs[favCount++], n.c_str(), sizeof(favs[0]));
    }
    i = e + 1;
  }
}

void favsSave() {
  String s;
  for (int i = 0; i < favCount; i++) { s += favs[i]; s += '\n'; }
  prefs.begin("radio", false);
  prefs.putString("favs", s);
  prefs.end();
}

int favIndex(const char* name) {
  for (int i = 0; i < favCount; i++) if (strcmp(favs[i], name) == 0) return i;
  return -1;
}
bool isFav(const char* name) { return favIndex(name) >= 0; }

void favToggle(const char* name) {
  if (!name[0]) return;
  int i = favIndex(name);
  if (i >= 0) {
    for (int k = i; k < favCount - 1; k++) strlcpy(favs[k], favs[k + 1], sizeof(favs[0]));
    favCount--;
  } else if (favCount < MAX_FAV) {
    strlcpy(favs[favCount++], name, sizeof(favs[0]));
  }
  favsSave();
}

void favRemoveAt(int i) {
  if (i < 0 || i >= favCount) return;
  for (int k = i; k < favCount - 1; k++) strlcpy(favs[k], favs[k + 1], sizeof(favs[0]));
  favCount--;
  favsSave();
}

void favClear() {
  favCount = 0;
  favsSave();
}

// Décodage d'images -> TFT
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

int fitScaleTo(uint16_t w, uint16_t h, int maxW, int maxH) {
  int s = 1;
  while (s < 8 && (w / s > maxW || h / s > maxH)) s *= 2;
  return s;
}

#if USE_PNG
PNG* pngObj = nullptr;
int pngX0 = 0, pngY0 = 0;
uint16_t pngLine[LOGO_S];

int pngDraw(PNGDRAW* pDraw) {
  pngObj->getLineAsRGB565(pDraw, pngLine, PNG_RGB565_BIG_ENDIAN, 0x00000000);
  tft.pushImage(pngX0, pngY0 + pDraw->y, pDraw->iWidth, 1, pngLine);
  return 1;
}
#endif

// Pochette artiste 
void clearThumbBox() {
  tft.fillRect(BOX_X, BOX_Y, BOX_W, BOX_H, TFT_BLACK);
  tft.drawRect(BOX_X, BOX_Y, BOX_W, BOX_H, TFT_DARKGREY);
}

void drawDefaultThumb() {
  clearThumbBox();
  int cx = BOX_X + BOX_W / 2;
  int cy = BOX_Y + BOX_H / 2;
  tft.fillCircle(cx - 8, cy + 10, 6, TFT_DARKGREY);
  tft.fillRect(cx - 4, cy - 14, 3, 24, TFT_DARKGREY);
  tft.fillTriangle(cx - 1, cy - 14, cx + 11, cy - 6, cx - 1, cy - 4, TFT_DARKGREY);
}

void drawThumb(uint8_t* buf, size_t len) {
  if (!buf) { drawDefaultThumb(); return; }
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, buf, len);
  if (w == 0 || h == 0) { drawDefaultThumb(); return; }
  clearThumbBox();
  int s = fitScaleTo(w, h, INNER_W, INNER_H);
  TJpgDec.setJpgScale(s);
  TJpgDec.drawJpg(BOX_X + 2 + (INNER_W - w / s) / 2,
                  BOX_Y + 2 + (INNER_H - h / s) / 2, buf, len);
  TJpgDec.setJpgScale(1);
}

// Logo de la station 
void clearLogoBox() {
  tft.fillRect(LOGO_X, LOGO_Y, LOGO_S, LOGO_S, TFT_BLACK);
}

void drawDefaultLogo() {
  clearLogoBox();
  const uint16_t c = TFT_DARKGREY;
  tft.drawRoundRect(LOGO_X + 8, LOGO_Y + 28, 64, 42, 6, c);
  tft.drawCircle(LOGO_X + 52, LOGO_Y + 49, 11, c);
  tft.drawFastHLine(LOGO_X + 16, LOGO_Y + 40, 22, c);
  tft.drawFastHLine(LOGO_X + 16, LOGO_Y + 49, 22, c);
  tft.drawFastHLine(LOGO_X + 16, LOGO_Y + 58, 22, c);
  tft.drawLine(LOGO_X + 20, LOGO_Y + 28, LOGO_X + 56, LOGO_Y + 8, c);
}

void drawLogo(uint8_t* buf, size_t len) {
  if (!buf || len < 4) { drawDefaultLogo(); return; }

  if (buf[0] == 0xFF && buf[1] == 0xD8) {                    // JPEG
    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, buf, len);
    if (w == 0 || h == 0) { drawDefaultLogo(); return; }
    clearLogoBox();
    int s = fitScaleTo(w, h, LOGO_S, LOGO_S);
    TJpgDec.setJpgScale(s);
    TJpgDec.drawJpg(LOGO_X + (LOGO_S - w / s) / 2,
                    LOGO_Y + (LOGO_S - h / s) / 2, buf, len);
    TJpgDec.setJpgScale(1);
    return;
  }

#if USE_PNG
  if (buf[0] == 0x89 && buf[1] == 'P') {         // PNG
    pngObj = new PNG();
    int rc = pngObj->openRAM(buf, len, pngDraw);
    if (rc == PNG_SUCCESS) {
      int w = pngObj->getWidth(), h = pngObj->getHeight();
      if (w > 0 && w <= LOGO_S && h <= LOGO_S) {
        clearLogoBox();
        pngX0 = LOGO_X + (LOGO_S - w) / 2;
        pngY0 = LOGO_Y + (LOGO_S - h) / 2;
        pngObj->decode(nullptr, 0);
        pngObj->close();
        delete pngObj; pngObj = nullptr;
        return;
      }
      Serial.printf("Logo PNG trop grand : %dx%d\n", w, h);
      pngObj->close();
    }
    delete pngObj; pngObj = nullptr;
  }
#endif

  drawDefaultLogo();
}

// Téléchargement d'image (tâche réseau) 
bool downloadImage(const String& url, uint8_t** outBuf, size_t* outLen, bool allowPng) {
  WiFiClientSecure sclient;
  WiFiClient pclient;
  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool ok;
  if (url.startsWith("https")) { sclient.setInsecure(); ok = http.begin(sclient, url); }
  else                         { ok = http.begin(pclient, url); }
  if (!ok) return false;

  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }

  int len = http.getSize();
  if (len <= 0 || len > 200000) { http.end(); return false; }

  uint8_t* b = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!b) b = (uint8_t*)malloc(len);
  if (!b) { http.end(); return false; }

  WiFiClient* s = http.getStreamPtr();
  int got = 0;
  uint32_t lastByte = millis();
  while (got < len && millis() - lastByte < 8000) {
    int av = s->available();
    if (av > 0) {
      got += s->readBytes(b + got, min(av, len - got));
      lastByte = millis();
    } else if (!s->connected()) break;
    else vTaskDelay(pdMS_TO_TICKS(5));
  }
  http.end();

  bool isJpeg = (len >= 4 && b[0] == 0xFF && b[1] == 0xD8);
  bool isPng  = (len >= 4 && b[0] == 0x89 && b[1] == 'P');
  if (got != len || !(isJpeg || (allowPng && isPng))) {
    if (got == len) Serial.println("Format d'image non gere (ni JPEG ni PNG autorise)");
    free(b);
    return false;
  }

  *outBuf = b; *outLen = len;
  return true;
}

void publishImg(ImgSlot& slot, uint8_t* b, size_t l) {
  xSemaphoreTake(imgMux, portMAX_DELAY);
  if (slot.buf) free(slot.buf);
  slot.buf = b; slot.len = l; slot.ready = true;
  xSemaphoreGive(imgMux);
}

// Audio (tâche dédiée, cœur 1) 
void audioTask(void*) {
  for (;;) {
    if (stopStream) {
      stopStream = false;
      newStream = false;
      audio.stopSong();
    }
    if (newStream) {
      newStream = false;
      audio.stopSong();
      audio.setVolume(volume);
      audio.connecttohost(pendingUrl);
    }
    if (volDirty) { volDirty = false; audio.setVolume(volume); }
    audio.loop();
    vTaskDelay(1);
  }
}

// Genres et stations d'un genre (tâche réseau) 
String urlEncode(const char* s) {
  String o;
  const char* hex = "0123456789ABCDEF";
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

int cmpGenre(const void* a, const void* b) {
  return strcasecmp(genBuf + *(const uint32_t*)a, genBuf + *(const uint32_t*)b);
}

bool fetchGenres() {
  genreCount = 0;
  if (!genBuf || !genOff) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(client, "https://api.laut.fm/genres")) return false;
  int code = http.GET();
  Serial.printf("genres HTTP %d\n", code);
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  filter[0] = true;                                   // accepte textes ou objets
  JsonDocument doc(&spiRamAlloc);
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("genres JSON : %s\n", err.c_str()); return false; }

  size_t pos = 0;
  for (JsonVariant v : doc.as<JsonArray>()) {
    const char* n = v.is<const char*>() ? v.as<const char*>() : v["name"].as<const char*>();
    if (!n || !n[0]) continue;
    size_t l = min(strlen(n), (size_t)63);
    if (genreCount >= MAX_GENRES || pos + l + 1 > GENBUF_SIZE) break;
    memcpy(genBuf + pos, n, l);
    genBuf[pos + l] = 0;
    genOff[genreCount++] = pos;
    pos += l + 1;
  }
  qsort(genOff, genreCount, sizeof(uint32_t), cmpGenre);
  Serial.printf("%d genres\n", genreCount);
  return genreCount > 0;
}

bool fetchGenreStations(const char* genre) {
  gsCount = 0;
  if (!gsBuf || !gsOff) return false;

  String url = String("https://api.laut.fm/stations/genre/") + urlEncode(genre) + "?limit=600";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  Serial.printf("genre '%s' HTTP %d\n", genre, code);
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  filter[0]["name"] = true;
  JsonDocument doc(&spiRamAlloc);
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("stations JSON : %s\n", err.c_str()); return false; }

  size_t pos = 0;
  for (JsonVariant v : doc.as<JsonArray>()) {
    const char* n = v["name"].as<const char*>();
    if (!n || !n[0]) continue;
    size_t l = min(strlen(n), (size_t)63);
    if (gsCount >= MAX_GS || pos + l + 1 > GSBUF_SIZE) break;
    memcpy(gsBuf + pos, n, l);
    gsBuf[pos + l] = 0;
    gsOff[gsCount++] = pos;
    pos += l + 1;
  }
  Serial.printf("%d stations pour ce genre\n", gsCount);
  return true;
}

// Infos station + logo (tâche réseau) 
void fetchStationInfo(const char* name) {
  String body;
  int code = 0;
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (http.begin(client, String("https://api.laut.fm/station/") + name)) {
      code = http.GET();
      if (code == HTTP_CODE_OK) body = http.getString();
      http.end();
    }
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("station info HTTP %d\n", code);
    publishImg(logoSlot, nullptr, 0);
    return;
  }

  JsonDocument filter;
  filter["display_name"] = true;
  filter["images"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    Serial.println("station info : JSON illisible");
    publishImg(logoSlot, nullptr, 0);
    return;
  }

  const char* dn = doc["display_name"] | "";
  if (dn[0] && strcmp(name, apiStation) == 0) {
    strlcpy(stationDisp, dn, sizeof(stationDisp));
    stationDirty = true;
  }

  Serial.print("images = ");
  serializeJson(doc["images"], Serial);
  Serial.println();

  JsonObject im = doc["images"].as<JsonObject>();
  const char* keys[] = { "station_80x80", "station_100x100", "station_120x120", "station" };
  String url;
  for (const char* k : keys) {
    const char* v = im[k] | "";
    if (v[0]) { url = v; break; }
  }
  if (!url.length()) {
    for (JsonPair kv : im) {
      if (kv.value().is<const char*>()) { url = kv.value().as<const char*>(); break; }
    }
  }
  if (url.startsWith("//")) url = "https:" + url;

  if (!url.length()) { publishImg(logoSlot, nullptr, 0); return; }
  Serial.println("logo : " + url);

  uint8_t* b = nullptr; size_t l = 0;
  bool ok = downloadImage(url, &b, &l, USE_PNG);
  if (strcmp(name, apiStation) != 0) { if (ok) free(b); return; }
  if (ok) publishImg(logoSlot, b, l); else publishImg(logoSlot, nullptr, 0);
}

// Tâche réseau unique (cœur 0) 
void apiTask(void*) {
  uint32_t lastPoll = 0;
  String lastThumbUrl = "";
  for (;;) {
    // --- Requêtes de listes (genres / stations d'un genre) ---
    int req = netReq;
    if (req) {
      netReq = 0;
      bool ok = false;
      if (WiFi.status() == WL_CONNECTED) {
        if (req == 1) ok = fetchGenres();
        else if (req == 2) {
          char g[64];
          strlcpy(g, reqGenre, sizeof(g));
          ok = fetchGenreStations(g);
        }
      }
      listFail = !ok;
      listReady = true;
    }

    if (thumbReset) { thumbReset = false; lastThumbUrl = ""; }

    // --- Logo + nom affiché de la station ---
    if (logoReset && WiFi.status() == WL_CONNECTED) {
      logoReset = false;
      char name[64];
      strlcpy(name, apiStation, sizeof(name));
      if (name[0]) fetchStationInfo(name);
    }

    // --- Artiste / titre / pochette ---
    if (apiStation[0] && WiFi.status() == WL_CONNECTED &&
        (apiRefresh || millis() - lastPoll >= POLL_MS)) {
      apiRefresh = false;
      lastPoll = millis();

      char name[64];
      strlcpy(name, apiStation, sizeof(name));
      String url = String("https://api.laut.fm/station/") + name + "/current_song";

      String body;
      int code = 0;
      {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(8000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        if (http.begin(client, url)) {
          code = http.GET();
          if (code == HTTP_CODE_OK) body = http.getString();
          http.end();
        }
      }

      if (code == HTTP_CODE_OK) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
          const char* title  = doc["title"]          | "";
          const char* artist = doc["artist"]["name"] | "";
          String thumbUrl    = String(doc["artist"]["thumb"] | "");

          if (strcmp(name, apiStation) == 0 && (title[0] || artist[0])) {
            strlcpy(songArtist, artist, sizeof(songArtist));
            strlcpy(songTitle,  title,  sizeof(songTitle));
            songDirty = true;

            if (thumbUrl != lastThumbUrl) {
              lastThumbUrl = thumbUrl;
              if (thumbUrl.length()) {
                uint8_t* b = nullptr; size_t l = 0;
                bool ok = downloadImage(thumbUrl, &b, &l, false);
                if (strcmp(name, apiStation) == 0) {
                  if (ok) publishImg(thumbSlot, b, l); else publishImg(thumbSlot, nullptr, 0);
                } else if (ok) free(b);
              } else {
                publishImg(thumbSlot, nullptr, 0);
              }
            }
          }
        }
      } else if (code != 0) {
        Serial.printf("current_song HTTP %d\n", code);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Tactile FT6336U 
void touchInit() {
  if (TP_RST >= 0) {
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);  delay(10);
    digitalWrite(TP_RST, HIGH); delay(100);
  }
  Wire.begin(TP_SDA, TP_SCL, 400000);
}

// Retourne le nombre de doigts (0, 1 ou 2) ; x,y = premier doigt, repère paysage
int readTouch(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)5) != 5) return 0;

  uint8_t n  = Wire.read() & 0x0F;
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if (n == 0 || n > 2) return 0;

  uint16_t rawX = ((xh & 0x0F) << 8) | xl;
  uint16_t rawY = ((yh & 0x0F) << 8) | yl;
  x = rawY;
  y = (PANEL_W - 1) - rawX;
  return n;
}

// Liste complète des stations (lecture en flux, stockage PSRAM) 
bool fetchStations() {
  nameBuf  = (char*)     heap_caps_malloc(NAMEBUF_SIZE,     MALLOC_CAP_SPIRAM);
  nameOff  = (uint32_t*) heap_caps_malloc(MAX_STATIONS * 4, MALLOC_CAP_SPIRAM);
  shownIdx = (int*)      heap_caps_malloc(MAX_STATIONS * 4, MALLOC_CAP_SPIRAM);
  genBuf   = (char*)     heap_caps_malloc(GENBUF_SIZE,      MALLOC_CAP_SPIRAM);
  genOff   = (uint32_t*) heap_caps_malloc(MAX_GENRES * 4,   MALLOC_CAP_SPIRAM);
  gsBuf    = (char*)     heap_caps_malloc(GSBUF_SIZE,       MALLOC_CAP_SPIRAM);
  gsOff    = (uint32_t*) heap_caps_malloc(MAX_GS * 4,       MALLOC_CAP_SPIRAM);
  if (!nameBuf || !nameOff || !shownIdx) {
    Serial.println("ERREUR : allocation PSRAM impossible (PSRAM = OPI activee ?)");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(client, "https://api.laut.fm/station_names")) return false;
  int code = http.GET();
  if (code != 200) { Serial.printf("HTTP %d\n", code); http.end(); return false; }

  WiFiClient* s = http.getStreamPtr();
  size_t pos = 0, start = 0;
  bool inQuote = false;
  nameCount = 0;
  uint8_t buf[512];
  uint32_t lastData = millis();

  while ((http.connected() || s->available()) && millis() - lastData < 10000) {
    int av = s->available();
    if (av <= 0) { delay(5); continue; }
    int n = s->readBytes(buf, min(av, (int)sizeof(buf)));
    lastData = millis();

    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '"') {
        if (!inQuote) { inQuote = true; start = pos; }
        else {
          inQuote = false;
          if (pos > start && nameCount < MAX_STATIONS && pos < NAMEBUF_SIZE - 1) {
            nameBuf[pos++] = 0;
            nameOff[nameCount++] = start;
          } else pos = start;
        }
      } else if (inQuote && pos < NAMEBUF_SIZE - 2) {
        nameBuf[pos++] = c;
      }
    }
  }
  http.end();
  Serial.printf("%d stations, %u octets\n", nameCount, (unsigned)pos);
  return nameCount > 0;
}

char letterChar() { return letterIdx < 26 ? 'A' + letterIdx : '#'; }

void filterStations() {
  shownCount = 0;
  for (int i = 0; i < nameCount; i++) {
    char c = toupper(nameBuf[nameOff[i]]);
    bool isLetter = (c >= 'A' && c <= 'Z');
    if (letterIdx < 26 ? (isLetter && c == 'A' + letterIdx) : !isLetter)
      shownIdx[shownCount++] = i;
  }
  topIdx = 0;
}

// Liste courante (selon le mode)
bool genreListView() { return mode == M_GENRE && !genreStationsView; }

int listCount() {
  if (mode == M_LETTER) return shownCount;
  if (mode == M_FAV)    return favCount;
  return genreStationsView ? gsCount : genreCount;
}

const char* listName(int i) {
  if (mode == M_LETTER) return nameBuf + nameOff[shownIdx[i]];
  if (mode == M_FAV)    return favs[i];
  return genreStationsView ? gsBuf + gsOff[i] : genBuf + genOff[i];
}

void requestGenres() {
  listFail = false;
  loadingSince = millis();
  listLoading = true;
  netReq = 1;
}

void requestGenreStations(int gi) {
  genreCur = gi;
  genreStationsView = true;
  strlcpy(reqGenre, genBuf + genOff[gi], sizeof(reqGenre));
  gsCount = 0;
  topIdx = 0;
  listFail = false;
  loadingSince = millis();
  listLoading = true;
  netReq = 2;
}

void setMode(Mode m) {
  mode = m;
  topIdx = 0;
  listFail = false;
  if (m == M_LETTER) filterStations();
  if (m == M_GENRE && genreCount == 0 && !listLoading) requestGenres();
}

// En-tête : texte d'état 
String hdrText() {
  if (!wifiOk) return "WiFi perdu - reconnexion...";
  if (curStation.length() == 0) return "Choisis une station";
  String t = "> " + curStation;
  if (curArtist.length() || curTitle.length()) t += " : " + curArtist + " - " + curTitle;
  return t;
}

// Affichage : page liste 
void drawListHeader() {
  tft.fillRect(0, 0, SCR_W, HEADER_H, TFT_NAVY);
  char v[8];
  snprintf(v, sizeof(v), "%d", volume);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(MR_DATUM);
  tft.setTextSize(1);
  tft.drawString(v, SCR_W - 8, HEADER_H / 2, 2);
  scrollSet(hdrSc, hdrText());
}

void updateHeaderText() {
  if (page != PAGE_LIST) return;
  String t = hdrText();
  if (t != hdrSc.text) scrollSet(hdrSc, t);
}

void drawList() {
  tft.fillRect(0, LIST_Y, SCR_W, ROWS * ROW_H + 22, TFT_BLACK);
  tft.setTextSize(1);

  if (listLoading || listFail || listCount() == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(listFail ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK);
    const char* msg = listLoading ? "Chargement..."
                    : listFail    ? "Erreur reseau"
                    : (mode == M_FAV ? "Aucun favori - appuie sur l'etoile" : "Aucune station");
    tft.drawString(msg, SCR_W / 2, LIST_Y + 3 * ROW_H, 4);
    return;
  }

  int cnt = listCount();
  bool genres = genreListView();
  tft.setTextDatum(ML_DATUM);
  for (int r = 0; r < ROWS; r++) {
    int idx = topIdx + r;
    if (idx >= cnt) break;
    int y = LIST_Y + r * ROW_H;
    tft.drawFastHLine(0, y + ROW_H - 1, SCR_W, TFT_DARKGREY);
    const char* nm = listName(idx);

    if (genres) {
      drawTextFit(String(nm), 10, y + ROW_H / 2, SCR_W - 30, TFT_WHITE, 1);
    } else {
      String s(nm);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(ML_DATUM);
      int maxw = SCR_W - 60;
      if (tft.textWidth(s, 4) > maxw) {
        while (s.length() > 3 && tft.textWidth(s + "~", 4) > maxw) s.remove(s.length() - 1);
        s += "~";
      }
      tft.drawString(s, 10, y + ROW_H / 2, 4);
      if (isFav(nm)) drawStar(SCR_W - 24, y + ROW_H / 2, 10, TFT_YELLOW, true);
    }
  }

  // Ligne d'état : contexte à gauche, compteur à droite
  int yl = LIST_Y + ROWS * ROW_H + 12;
  String ctx;
  if (mode == M_LETTER)      ctx = String("Lettre ") + letterChar();
  else if (mode == M_FAV)    ctx = "Favoris - appui long: suppr. / centre: vider";
  else if (genreStationsView && genreCur >= 0) ctx = String(genBuf + genOff[genreCur]);
  else                       ctx = "Genres";
  drawTextFit(ctx, 8, yl, 370, TFT_LIGHTGREY, 3);

  char buf[32];
  snprintf(buf, sizeof(buf), "%d-%d / %d", topIdx + 1, min(cnt, topIdx + ROWS), cnt);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(buf, SCR_W - 8, yl, 2);
}

void drawListButtons() {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  bool navActive = (mode == M_LETTER) || (mode == M_GENRE && genreStationsView);
  const char* modeLbl = mode == M_LETTER ? "ABC" : (mode == M_GENRE ? "GEN" : "FAV");

  for (int k = 0; k < 8; k++) {
    int x = k * BTN_W;
    uint16_t bg = TFT_DARKCYAN;
    if ((k == 1 || k == 3) && !navActive) bg = TFT_NAVY;
    tft.fillRoundRect(x + 2, BTN_Y, BTN_W - 4, BTN_H, 8, bg);
    tft.setTextColor(TFT_WHITE, bg);

    String l;
    int font = 4;
    switch (k) {
      case 0: l = modeLbl; break;
      case 1: l = "<"; break;
      case 2:
        if (mode == M_LETTER) l = String(letterChar());
        else if (mode == M_FAV) l = String(favCount);
        else if (genreStationsView) { l = "Genres"; font = 2; }
        break;
      case 3: l = ">"; break;
      case 4: l = "Up"; break;
      case 5: l = "Dn"; break;
      case 6: l = "V-"; break;
      case 7: l = "V+"; break;
    }
    tft.drawString(l, x + BTN_W / 2, BTN_Y + BTN_H / 2, font);
  }
}

void redrawList() {
  tft.fillScreen(TFT_BLACK);
  drawListHeader();
  drawList();
  drawListButtons();
}

// Affichage : page lecteur 
void drawPlayerHeader() {
  tft.fillRect(0, 0, SCR_W, HEADER_H, TFT_NAVY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("laut.fm", 10, HEADER_H / 2, 4);

  tft.setTextDatum(MC_DATUM);
  if (wifiOk) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
    tft.drawString("2 doigts : retour", SCR_W / 2 + 10, HEADER_H / 2, 2);
  } else {
    tft.setTextColor(TFT_ORANGE, TFT_NAVY);
    tft.drawString("WiFi perdu...", SCR_W / 2 + 10, HEADER_H / 2, 2);
  }

  char v[16];
  snprintf(v, sizeof(v), "Vol %d", volume);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(v, SCR_W - 10, HEADER_H / 2, 4);
}

// Efface la zone et écrit l'étiquette ; le contenu est dessiné par le Scroller
void drawField(const char* label, int top, int labelX, int x0, int x1) {
  tft.fillRect(x0, top, x1 - x0, 76, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(label, labelX, top + 10, 2);
}

void drawStationField() {
  drawField("STATION", 36, STATION_TEXT_X, LOGO_X + LOGO_S + 2, BOX_X - 4);
  scrollSet(scStation, curStation);
}
void drawArtistField() { drawField("ARTISTE", 120, 14, 0, SCR_W); scrollSet(scArtist, curArtist); }
void drawTitleField()  { drawField("MORCEAU", 198, 14, 0, SCR_W); scrollSet(scTitle,  curTitle);  }

void drawPlayerButtons() {
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.fillRoundRect(PB_VM_X, PB_Y, PB_VM_W, PB_H, 8, TFT_DARKCYAN);
  tft.fillRoundRect(PB_ST_X, PB_Y, PB_ST_W, PB_H, 8, TFT_DARKCYAN);
  tft.fillRoundRect(PB_VP_X, PB_Y, PB_VP_W, PB_H, 8, TFT_DARKCYAN);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawString("V-", PB_VM_X + PB_VM_W / 2, PB_Y + PB_H / 2, 4);
  tft.drawString("V+", PB_VP_X + PB_VP_W / 2, PB_Y + PB_H / 2, 4);

  bool fav = isFav(apiStation);
  drawStar(PB_ST_X + PB_ST_W / 2, PB_Y + PB_H / 2 + 1, 16,
           fav ? TFT_YELLOW : TFT_WHITE, fav);
}

void redrawPlayer() {
  tft.fillScreen(TFT_BLACK);
  drawPlayerHeader();
  clearLogoBox();
  drawStationField();
  drawArtistField();
  drawTitleField();
  clearThumbBox();
  drawPlayerButtons();
}

// Navigation 
void goPlayer(const String& name) {
  curStation = name;
  curArtist  = "";
  curTitle   = "Chargement...";
  snprintf(pendingUrl, sizeof(pendingUrl), "https://stream.laut.fm/%s", name.c_str());
  strlcpy(apiStation, name.c_str(), sizeof(apiStation));
  stationDirty = false;
  songDirty  = false;
  thumbReset = true;
  logoReset  = true;
  apiRefresh = true;
  newStream  = true;
  streamActive = true;
  lastAudioOk = millis();
  page = PAGE_PLAYER;
  redrawPlayer();
}

void goList() {
  page = PAGE_LIST;
  redrawList();
}

// Horloge -> radio (écran en paysage, liste)
void enterRadio() {
  if (spr.created()) spr.deleteSprite();      // libère 25 Ko de RAM
  favPress = false;
  confirmAct = 0;
  page = PAGE_LIST;
  redrawList();
}

// Radio -> horloge : coupe le flux
void enterClock() {
  streamActive = false;                       // plus de reprise automatique
  newStream = false;
  stopStream = true;                          // la tâche audio coupe le flux
  apiStation[0] = 0;                          // arrête les requêtes artiste/logo
  curStation = ""; curArtist = ""; curTitle = "";
  songDirty = false; stationDirty = false;
  favPress = false;
  confirmAct = 0;

  page = PAGE_CLOCK;
  tft.fillScreen(TFT_BLACK);
  if (!spr.created()) {
    spr.setColorDepth(16);
    if (!spr.createSprite(FACE, BAND_H)) Serial.println("Erreur : creation du sprite impossible");
  }
  lastFrame = 0;
}

// Boîte de confirmation (favoris) 
void drawConfirm() {
  tft.fillRoundRect(40, 70, 400, 180, 10, TFT_BLACK);
  tft.drawRoundRect(40, 70, 400, 180, 10, TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  if (confirmAct == 1) {
    tft.drawString("Supprimer ce favori ?", 240, 100, 4);
    drawTextFit(String(favs[confirmIdx]), 60, 140, 360, TFT_CYAN, 1);
  } else {
    tft.drawString("Vider tous les favoris ?", 240, 105, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(String(favCount) + " favoris", 240, 145, 2);
  }

  tft.fillRoundRect(90, 190, 140, 50, 8, TFT_RED);
  tft.fillRoundRect(250, 190, 140, 50, 8, TFT_DARKCYAN);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.drawString("Oui", 160, 215, 4);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawString("Non", 320, 215, 4);
}

// Gestion du toucher 
void handleTouch() {
  static uint32_t last = 0;
  static bool waitRelease = false;

  uint16_t x = 0, y = 0;
  int n = readTouch(x, y);

  // --- Horloge : 1 doigt -> radio ---
  if (page == PAGE_CLOCK) {
    if (n == 0) { waitRelease = false; return; }
    if (!waitRelease) {
      waitRelease = true;
      enterRadio();
    }
    return;
  }

  // --- Boîte de confirmation ---
  if (confirmAct) {
    if (n == 0) { waitRelease = false; return; }
    if (waitRelease || n != 1) return;
    if (y >= 190 && y < 240) {
      if (x >= 90 && x < 230) {                       // Oui
        if (confirmAct == 1) favRemoveAt(confirmIdx); else favClear();
        confirmAct = 0; waitRelease = true; topIdx = 0;
        redrawList();
      } else if (x >= 250 && x < 390) {               // Non
        confirmAct = 0; waitRelease = true;
        redrawList();
      }
    }
    return;
  }

  // --- Mode FAV : tap court = lecture, appui long = suppression ---
  if (page == PAGE_LIST && mode == M_FAV && !listLoading) {
    if (n >= 2) favPress = false;
    if (n == 0 && favPress) {                         // relâché avant 0,7 s
      favPress = false;
      waitRelease = false;
      if (favIdx >= 0 && favIdx < favCount) goPlayer(String(favs[favIdx]));
      return;
    }
    if (n == 1 && !waitRelease) {
      if (favPress) {
        if (millis() - favT0 >= 700) {                // appui long détecté
          favPress = false;
          waitRelease = true;
          confirmAct = (favIdx >= 0) ? 1 : 2;
          confirmIdx = favIdx;
          drawConfirm();
        }
        return;
      }
      if (y >= LIST_Y && y < LIST_Y + ROWS * ROW_H) {
        int idx = topIdx + (y - LIST_Y) / ROW_H;
        if (idx < favCount) { favPress = true; favT0 = millis(); favIdx = idx; return; }
      } else if (y >= BTN_Y && x / BTN_W == 2 && favCount > 0) {
        favPress = true; favT0 = millis(); favIdx = -2; return;
      }
    }
  }

  if (n == 0) { waitRelease = false; return; }
  if (waitRelease) return;

  // --- Page lecteur ---
  if (page == PAGE_PLAYER) {
    if (n >= 2) {                         // 2 doigts : retour à la liste
      waitRelease = true;
      goList();
      return;
    }
    if (millis() - last < 250) return;
    last = millis();
    if (y >= PB_Y) {
      if (x >= PB_VM_X && x < PB_VM_X + PB_VM_W) {
        volume = max(0, volume - 1);  volDirty = true; drawPlayerHeader();
      } else if (x >= PB_VP_X && x < PB_VP_X + PB_VP_W) {
        volume = min(21, volume + 1); volDirty = true; drawPlayerHeader();
      } else if (x >= PB_ST_X && x < PB_ST_X + PB_ST_W) {
        favToggle(apiStation);
        drawPlayerButtons();
      }
    }
    return;
  }

  // --- Page liste ---
  if (n >= 2) {                           // 2 doigts : retour à l'horloge (flux coupé)
    waitRelease = true;
    enterClock();
    return;
  }
  if (n != 1) return;
  if (millis() - last < 250) return;
  last = millis();

  if (y >= BTN_Y) {
    int k = x / BTN_W;
    if (k > 7) k = 7;

    if (listLoading && k < 6) return;

    switch (k) {
      case 0:   // mode : ABC -> GEN -> FAV
        setMode(mode == M_LETTER ? M_GENRE : (mode == M_GENRE ? M_FAV : M_LETTER));
        break;
      case 1:   // précédent
        if (mode == M_LETTER) { letterIdx = (letterIdx + 26) % 27; filterStations(); }
        else if (mode == M_GENRE && genreStationsView && genreCount > 0)
          requestGenreStations((genreCur + genreCount - 1) % genreCount);
        break;
      case 2:   // centre : retour à la liste des genres
        if (mode == M_GENRE && genreStationsView) { genreStationsView = false; topIdx = 0; listFail = false; }
        break;
      case 3:   // suivant
        if (mode == M_LETTER) { letterIdx = (letterIdx + 1) % 27; filterStations(); }
        else if (mode == M_GENRE && genreStationsView && genreCount > 0)
          requestGenreStations((genreCur + 1) % genreCount);
        break;
      case 4: topIdx = max(0, topIdx - ROWS); break;
      case 5: if (topIdx + ROWS < listCount()) topIdx += ROWS; break;
      case 6: volume = max(0, volume - 1);  volDirty = true; break;
      case 7: volume = min(21, volume + 1); volDirty = true; break;
    }
    drawListHeader(); drawList(); drawListButtons();

  } else if (y >= LIST_Y && y < LIST_Y + ROWS * ROW_H) {
    if (mode == M_FAV) return;            // géré plus haut (tap / appui long)
    if (listLoading || listFail) return;
    int idx = topIdx + (y - LIST_Y) / ROW_H;
    if (idx >= listCount()) return;
    waitRelease = true;
    if (genreListView()) {
      requestGenreStations(idx);
      drawList(); drawListButtons();
    } else {
      goPlayer(String(listName(idx)));
    }
  }
}

// Surveillance WiFi + flux 
void wifiWatch() {
  static uint32_t lastCheck = 0, lastTry = 0;
  uint32_t now = millis();
  if (now - lastCheck < 1000) return;
  lastCheck = now;

  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok != wifiOk) {
    wifiOk = ok;
    Serial.println(ok ? "WiFi retabli" : "WiFi perdu");
    if (page == PAGE_PLAYER) drawPlayerHeader(); else updateHeaderText();
    if (ok && streamActive) {              // reprise automatique du flux
      newStream = true;
      apiRefresh = true;
      lastAudioOk = now;
    }
  }

  if (!ok) {
    if (now - lastTry > 8000) {
      lastTry = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  // Flux interrompu alors que le WiFi est là : on relance
  if (streamActive) {
    if (audio.isRunning()) lastAudioOk = now;
    else if (!newStream && now - lastAudioOk > 20000) {
      Serial.println("Flux coupe : reconnexion");
      newStream = true;
      lastAudioOk = now;
    }
  }
}

// Setup / loop 
void setup() {
  Serial.begin(115200);
  Serial.printf("PSRAM libre : %u octets\n", (unsigned)ESP.getFreePsram());

  imgMux = xSemaphoreCreateMutex();
  favsLoad();

  tft.init();
  u8f.begin(tft);
  u8f.setFontMode(1);
  tft.setRotation(1);                     // paysage pour les messages de démarrage
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);
  touchInit();
  initColors();
  initSegments();

  // Zones de texte défilant
  scrollInit(scStation, STATION_TEXT_X, 62,  BOX_X - 8 - STATION_TEXT_X, 40, TFT_CYAN,   TFT_BLACK, 0, 1);
  scrollInit(scArtist,  14,             146, SCR_W - 28,                 40, TFT_YELLOW, TFT_BLACK, 0, 1);
  scrollInit(scTitle,   14,             224, SCR_W - 28,                 40, TFT_WHITE,  TFT_BLACK, 0, 1);
  scrollInit(hdrSc,     8,              0,   SCR_W - 60,                 HEADER_H, TFT_WHITE, TFT_NAVY, 2, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connexion WiFi...", SCR_W / 2, SCR_H / 2, 4);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  // --- Heure (NTP), puis resynchronisation automatique en arrière-plan ---
  tft.fillScreen(TFT_BLACK);
  tft.drawString("Synchronisation de l'heure...", SCR_W / 2, SCR_H / 2, 4);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "fr.pool.ntp.org");
  struct tm tinfo;
  if (!getLocalTime(&tinfo, 10000)) {
    Serial.println("NTP indisponible : heure de compilation");
    setFallbackTime();
  }

  tft.fillScreen(TFT_BLACK);
  tft.drawString("Chargement des stations...", SCR_W / 2, SCR_H / 2, 4);
  if (!fetchStations()) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Erreur chargement liste", SCR_W / 2, SCR_H / 2, 4);
    while (true) delay(1000);
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume);
  xTaskCreatePinnedToCore(audioTask, "audio", 8192,  nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(apiTask,   "api",   24576, nullptr, 1, nullptr, 0);

  filterStations();
  enterClock();                           // démarrage sur l'horloge
}

void loop() {
  handleTouch();
  wifiWatch();

  // Fin d'un chargement de liste (genres / stations d'un genre)
  if (listReady) {
    listReady = false;
    listLoading = false;
    topIdx = 0;
    if (page == PAGE_LIST) { drawList(); drawListButtons(); }
  }
  if (listLoading && millis() - loadingSince > 40000) {   // sécurité
    listLoading = false; listFail = true;
    if (page == PAGE_LIST) { drawList(); drawListButtons(); }
  }

  // Nom affiché de la station (display_name)
  if (stationDirty) {
    stationDirty = false;
    curStation = String(stationDisp);
    if (page == PAGE_PLAYER) drawStationField(); else updateHeaderText();
  }

  // Nouvel artiste / titre
  if (songDirty) {
    songDirty = false;
    String a = String(songArtist);
    String t = String(songTitle);
    if (a != curArtist || t != curTitle) {
      bool aCh = (a != curArtist), tCh = (t != curTitle);
      curArtist = a;
      curTitle  = t;
      if (page == PAGE_PLAYER) {
        if (aCh) drawArtistField();
        if (tCh) drawTitleField();
      } else updateHeaderText();
    }
  }

  // Nouvelle pochette artiste
  if (thumbSlot.ready) {
    xSemaphoreTake(imgMux, portMAX_DELAY);
    uint8_t* b = thumbSlot.buf; size_t l = thumbSlot.len;
    thumbSlot.buf = nullptr; thumbSlot.len = 0; thumbSlot.ready = false;
    xSemaphoreGive(imgMux);
    if (page == PAGE_PLAYER) drawThumb(b, l);
    if (b) free(b);
  }

  // Nouveau logo de station
  if (logoSlot.ready) {
    xSemaphoreTake(imgMux, portMAX_DELAY);
    uint8_t* b = logoSlot.buf; size_t l = logoSlot.len;
    logoSlot.buf = nullptr; logoSlot.len = 0; logoSlot.ready = false;
    xSemaphoreGive(imgMux);
    if (page == PAGE_PLAYER) drawLogo(b, l);
    if (b) free(b);
  }

  // Animation : textes défilants (radio) ou horloge
  if (page == PAGE_PLAYER)      { scrollTick(scStation); scrollTick(scArtist); scrollTick(scTitle); }
  else if (page == PAGE_LIST)   { scrollTick(hdrSc); }
  else                          { clockLoop(); }

  delay(10);
}


//
