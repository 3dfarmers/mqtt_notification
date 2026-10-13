// =============================================================================
// mqtt_notification - a Bambu Lab printer's print state on a Hue light, and a
// push to your phone when the print finishes or fails
// =============================================================================
// Copyright (c) 2026 3D Farmers GmbH - MIT License (see LICENSE)
// https://github.com/3dfarmers/mqtt_notification
//
// Firmware for any ESP32 / ESP32-S3. It does one job:
//
//   Printer (MQTT)            Hue light                        Push (ntfy)
//   PREPARE, RUNNING layer 0  white 40 %                       -
//   RUNNING                   blue, 20 % + 80 % x progress     -
//   PAUSE                     yellow                           -
//   FAILED, real error        red, 1 Hz flash 60 s, then red   urgent "Print failed" + camera photo
//   FAILED, user cancel       red 10 s, then off               -
//   FINISH                    green 30 min, then off           "Done: <file> in X h Y min"
//   IDLE                      off                              -
//
// Only the printer's finish and error signals send a push. Only transitions
// act, plus 5 % progress steps; a FINISH or FAILED that is already there at
// boot is old news (light off, no push).
//
// Connections, all TLS with the peer certificate accepted unverified (the
// printer and the bridge use self-signed ones; TODO: verify ntfy's):
//   printer  MQTT 8883, user bblp + LAN access code, device/<serial>/report
//   bridge   Hue CLIP v2 PUT /clip/v2/resource/light/<id>, kept alive while
//            commands follow each other (the 1 Hz flash)
//   ntfy     POST for text, PUT with one JPEG for the failure photo
//   camera   A1 / A1 mini / P1P / P1S only: port 6000, 80-byte auth, then a
//            16-byte header + JPEG per frame (protocol from OpenBambuAPI
//            video.md, cross-checked with PrintSphere). One frame is streamed
//            straight into the ntfy upload, so no PSRAM is needed. Other
//            models only offer RTSP video: they get the push without a photo.
//
// Everything you set up (WiFi, printer, Hue, ntfy) is stored on the board
// over USB serial at 115200 baud: type `help`. Nothing secret lives in this
// file. Setup guide: README.md. Changing it with Claude Code: CLAUDE.md.
// Library: PubSubClient (Nick O'Leary). Core: arduino-esp32 3.x.
// =============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>

// ---- board ------------------------------------------------------------------
// 3D Farmers FarmBoard 2: hold the door, bender and fan driver inputs and the
// LED LOW so nothing moves while this firmware runs. Leave 0 on any other
// board (or build with -DFARMBOARD2=1).
#ifndef FARMBOARD2
#define FARMBOARD2 0
#endif

// ---- tuning -------------------------------------------------------------------
const uint32_t SETTLE_MS       = 1500;    // FAILED -> cancel or error. P1 / A1 send
                                          // only changed fields, so the code that
                                          // names a cancel can trail by a report.
const uint32_t LATE_ERROR_MS   = 10000;   // a "cancel" read from code 0 becomes an
                                          // error if a real code follows in time
const uint32_t CANCEL_SHOW_MS  = 10000;
const uint32_t FINISH_SHOW_MS  = 30UL * 60UL * 1000UL;
const uint32_t FLASH_MS        = 60000;
const uint32_t FLASH_HALF_MS   = 500;     // 1 Hz
const uint32_t HUE_MIN_GAP_MS  = 120;     // Hue asks for <= ~10 commands/s per light
const uint32_t HUE_IDLE_MS     = 5000;    // close the kept-alive session after this
const uint32_t HUE_BACKOFF_MAX = 60000;
const uint32_t CONNECT_MS      = 5000;
const uint32_t HUE_CONNECT_MS  = 2000;    // LAN: a healthy connect takes < 100 ms;
                                          // a dropout must not freeze the loop
const uint32_t IO_MS           = 8000;
const uint32_t CAM_IO_MS       = 4000;    // per stall
const uint32_t CAM_MAX_FRAME   = 256UL * 1024UL;
const uint32_t SNAP_MIN_HEAP   = 90000;   // camera + ntfy TLS, ~40 KB each
const uint32_t MQTT_RETRY_MS   = 10000;
const uint32_t NO_DATA_WARN_MS = 30000;
const int      WEAK_RSSI       = -75;     // below this TLS to bridge / ntfy gets flaky
const uint16_t MQTT_BUF        = 20480;   // largest reports measured: H2S 18.1 KB,
                                          // P2S 13.7 KB, X1C 9.4 KB, P1S 2.4 KB.
                                          // PubSubClient drops bigger ones silently.

// print_error on FAILED that means the user cancelled (besides 0). Every
// FAILED logs its code: if your printer sends another one, add it here.
const uint32_t CANCEL_CODES[]  = { 0x0300400C, 0x0500400E };

// Light colours as CIE xy (Hue gamut C lights, e.g. Hue Play, lightstrip
// plus). Approximate: tune them to your light / camera. White uses colour
// temperature instead (mirek 153 = cold ... 500 = warm).
enum Color : uint8_t { C_OFF, C_WHITE, C_BLUE, C_YELLOW, C_GREEN, C_RED };
const char* const COLOR_NAME[] = { "off", "white", "blue", "yellow", "green", "red" };
const float COLOR_X[] = { 0, 0, 0.153f, 0.44f, 0.17f, 0.69f };
const float COLOR_Y[] = { 0, 0, 0.048f, 0.52f, 0.70f, 0.31f };
const unsigned WHITE_MIREK = 250;

#define LOG(...) Serial.printf(__VA_ARGS__)

// ---- config, stored on the board (NVS namespace "pstatus") ---------------------------
struct Config {
  char wifiSsid[33]    = "";
  char wifiPass[65]    = "";
  char printerIp[40]   = "";
  char accessCode[33]  = "";
  char serial[24]      = "";
  char hueIp[40]       = "";
  char hueKey[64]      = "";
  char hueLights[160]  = "";              // one light id, or several separated by commas
  char ntfyServer[80]  = "https://ntfy.sh";
  char ntfyTopic[80]   = "";
} cfg;

struct Key { const char* name; char* val; size_t cap; bool secret; bool reboot; };
const Key KEYS[] = {
  { "wifi_ssid",   cfg.wifiSsid,   sizeof cfg.wifiSsid,   false, true  },
  { "wifi_pass",   cfg.wifiPass,   sizeof cfg.wifiPass,   true,  true  },
  { "printer_ip",  cfg.printerIp,  sizeof cfg.printerIp,  false, true  },
  { "access_code", cfg.accessCode, sizeof cfg.accessCode, true,  true  },
  { "serial",      cfg.serial,     sizeof cfg.serial,     false, true  },
  { "hue_ip",      cfg.hueIp,      sizeof cfg.hueIp,      false, false },
  { "hue_key",     cfg.hueKey,     sizeof cfg.hueKey,     true,  false },
  { "hue_lights",  cfg.hueLights,  sizeof cfg.hueLights,  false, false },
  { "ntfy_server", cfg.ntfyServer, sizeof cfg.ntfyServer, false, false },
  { "ntfy_topic",  cfg.ntfyTopic,  sizeof cfg.ntfyTopic,  true,  false },
};

// ---- printer state, merged: P1 / A1 send only the fields that changed ------------
WiFiClientSecure mqttTls;
PubSubClient     mqtt(mqttTls);
char             topicReport[48], topicRequest[48], clientId[24];
uint32_t         mqttLastTry = 0, mqttUpSince = 0, lastRx = 0, lastPushall = 0;
bool             mqttUp = false, noDataWarned = false;

char     mState[16] = "";             // as last reported
int      mPct = -1, mLayer = -1;
uint32_t mErr = 0;
char     mJob[72] = "";

// ---- status logic -------------------------------------------------------------------
enum Phase : uint8_t { PH_UNKNOWN, PH_IDLE, PH_PREPARE, PH_RUNNING, PH_PAUSE,
                       PH_FINISH, PH_FAILED, PH_OTHER };
const char* const PHASE_NAME[] = { "unknown", "IDLE", "PREPARE", "RUNNING",
                                   "PAUSE", "FINISH", "FAILED", "other" };
// What the logic below works on: the printer's values, or the sim's.
Phase    phase = PH_UNKNOWN;
bool     sim = false;
int      pct = -1, layer = -1;
uint32_t perr = 0;
char     job[72] = "";
int      shownPct = -2, shownLayer = -2;
uint32_t startMs = 0, perrAtStart = 0, failedAt = 0;
bool     startKnown = false, classifyPending = false, lateErrorWatch = false;
bool     flashing = false, flashLit = false, offArmed = false;
uint32_t flashNext = 0, flashUntil = 0, offAt = 0;

// ---- Hue -----------------------------------------------------------------------------
struct Light { uint8_t color = C_OFF, bri = 0; bool instant = false; };
WiFiClientSecure hue;
Light    want;                  // latest requested state; sent from loop()
bool     wantSet = false, wantSent = true, hueDown = false, hueReport = false;
uint32_t hueHoldStart = 0, hueHoldMs = 0, hueBackoff = 0, hueLastIo = 0;
int      hueCode = 0, hueRefused = 0;
char     hueErr[80] = "";

// `hue lights`: streaming scan of the bridge's light list (see lightSink).
struct LightScan { char tail[8]; bool inVal, isName; char val[64]; uint8_t vlen; char id[40]; int count; };
LightScan scan;

// ---- push ----------------------------------------------------------------------------
struct Push { bool pending; char title[32], prio[10], tags[24], msg[200]; bool snap; };
Push     push = {};
int      pushCode = 0;
uint32_t pushAt = 0, snapBytes = 0;
const int SENT_NO_REPLY = 1;    // request went out whole, reply never came

uint8_t  xfer[2048];            // camera -> ntfy
char     req[1024];             // request head
char     query[610];            // "?message=" + URL-encoded push message

struct Url { bool tls; char host[64]; uint16_t port; char path[64]; };

// =============================================================================
// Small helpers
// =============================================================================
// (Types and globals all sit above this line: the Arduino builder puts its
// generated prototypes right before the first function.)
bool hueReady()  { return cfg.hueIp[0] && cfg.hueKey[0] && cfg.hueLights[0]; }
bool camReady()  { return cfg.printerIp[0] && cfg.accessCode[0]; }

// Cuts a trailing partial UTF-8 sequence: names arrive cut by byte count, and
// ntfy turns a body that is not valid UTF-8 into a file attachment.
void trimUtf8(char* s) {
  size_t n = strlen(s), i = n, cont = 0;
  while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80 && cont < 3) { --i; ++cont; }
  if (i == 0) { if (cont) s[0] = 0; return; }
  uint8_t lead = (uint8_t)s[i - 1];
  size_t need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
  if (lead < 0x80) { s[i] = 0; return; }
  if (lead < 0xC0 || cont != need) s[i - 1] = 0;
}

void putLe32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Long random secrets (Hue key, ntfy topic) show a 4-character prefix to tell
// them apart; short ones (WiFi password, access code) only their length.
String masked(const char* s) {
  size_t n = strlen(s);
  if (!n) return "(not set)";
  char b[40];
  if (n >= 16) snprintf(b, sizeof b, "%.4s... (%u chars)", s, (unsigned)n);
  else         snprintf(b, sizeof b, "set (%u chars)", (unsigned)n);
  return b;
}

// =============================================================================
// HTTP over a Client
// =============================================================================
bool writeAll(Client& c, const uint8_t* p, size_t n) {
  while (n) {
    size_t w = c.write(p, n);
    if (w == 0) return false;
    p += w;
    n -= w;
  }
  return true;
}

bool waitData(Client& c, uint32_t deadline) {
  while (!c.available()) {
    if (!c.connected() || (int32_t)(millis() - deadline) >= 0) return false;
    delay(2);
  }
  return true;
}

// Timeout is per stall, not for the whole transfer.
bool readExact(Client& c, uint8_t* p, size_t n, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (n) {
    if (!waitData(c, deadline)) return false;
    int r = c.read(p, n);
    if (r < 0) return false;
    p += r;
    n -= r;
    if (r > 0) deadline = millis() + timeoutMs;
  }
  return true;
}

int readLine(Client& c, char* out, size_t cap, uint32_t deadline) {
  size_t n = 0;
  for (;;) {
    if (!waitData(c, deadline)) return -1;
    int ch = c.read();
    if (ch < 0 || ch == '\r') continue;
    if (ch == '\n') break;
    if (n + 1 < cap) out[n++] = (char)ch;
  }
  out[n] = 0;
  return (int)n;
}

// Reads one HTTP/1.1 response to the end, so a kept-alive connection stays in
// step. Returns the status (-1 on a transport failure), the start of the body
// in `snip`, every body byte through `sink` (both optional), and whether the
// connection can carry the next request.
int readResponse(Client& c, char* snip, size_t snipCap, bool* reusable,
                 void (*sink)(const uint8_t*, size_t)) {
  uint32_t deadline = millis() + IO_MS;
  char line[160];
  size_t kept = 0;
  if (snip) snip[0] = 0;
  if (reusable) *reusable = false;

  if (readLine(c, line, sizeof line, deadline) < 0 || strncmp(line, "HTTP/1.", 7) != 0) return -1;
  int code = atoi(line + 9);
  long clen = -1;
  bool chunked = false, closing = false;
  for (;;) {
    int n = readLine(c, line, sizeof line, deadline);
    if (n < 0) return -1;
    if (n == 0) break;
    for (char* p = line; *p; ++p) *p = (char)tolower((unsigned char)*p);
    if (!strncmp(line, "content-length:", 15)) clen = atol(line + 15);
    else if (!strncmp(line, "transfer-encoding:", 18) && strstr(line, "chunked")) chunked = true;
    else if (!strncmp(line, "connection:", 11) && strstr(line, "close")) closing = true;
  }

  uint8_t buf[64];
  auto take = [&](size_t k) {
    for (size_t i = 0; i < k && snip && kept + 1 < snipCap; ++i)
      snip[kept++] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : ' ';
    if (sink) sink(buf, k);
  };
  auto drain = [&](long n) {
    while (n > 0) {
      size_t k = n > (long)sizeof buf ? sizeof buf : (size_t)n;
      if (!readExact(c, buf, k, IO_MS)) return false;
      take(k);
      n -= (long)k;
    }
    return true;
  };

  bool ok = true;
  if (chunked) {
    for (;;) {
      if (readLine(c, line, sizeof line, deadline) < 0) { ok = false; break; }
      long sz = strtol(line, nullptr, 16);
      if (sz <= 0) {
        int n;
        do { n = readLine(c, line, sizeof line, deadline); } while (n > 0);
        ok = n == 0;
        break;
      }
      if (!drain(sz) || readLine(c, line, sizeof line, deadline) != 0) { ok = false; break; }
    }
  } else if (clen >= 0) {
    ok = drain(clen);
  } else {
    while ((c.connected() || c.available()) && (int32_t)(millis() - deadline) < 0) {
      int r = c.read(buf, sizeof buf);
      if (r > 0) take((size_t)r);
      else delay(2);
    }
    closing = true;
  }
  if (snip) snip[kept] = 0;
  if (reusable) *reusable = ok && !closing;
  return code;
}

bool parseUrl(const char* s, Url& u) {
  if (!strncmp(s, "https://", 8))     { u.tls = true;  u.port = 443; s += 8; }
  else if (!strncmp(s, "http://", 7)) { u.tls = false; u.port = 80;  s += 7; }
  else return false;
  const char* slash = strchr(s, '/');
  size_t hostLen = slash ? (size_t)(slash - s) : strlen(s);
  const char* colon = (const char*)memchr(s, ':', hostLen);
  size_t nameLen = colon ? (size_t)(colon - s) : hostLen;
  if (nameLen == 0 || nameLen >= sizeof u.host) return false;
  memcpy(u.host, s, nameLen);
  u.host[nameLen] = 0;
  if (colon && (u.port = (uint16_t)atoi(colon + 1)) == 0) return false;
  u.path[0] = 0;
  if (slash) {
    strlcpy(u.path, slash, sizeof u.path);
    size_t n = strlen(u.path);
    while (n && u.path[n - 1] == '/') u.path[--n] = 0;
  }
  return true;
}

void urlEncode(const char* s, char* out, size_t cap) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  size_t n = 0;
  for (; *s; ++s) {
    uint8_t ch = (uint8_t)*s;
    if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      if (n + 1 >= cap) break;
      out[n++] = (char)ch;
    } else {
      if (n + 3 >= cap) break;
      out[n++] = '%';
      out[n++] = HEX_DIGITS[ch >> 4];
      out[n++] = HEX_DIGITS[ch & 15];
    }
  }
  out[n] = 0;
}

NetworkClient* openNet(const Url& u) {
  NetworkClient* c;
  if (u.tls) {
    WiFiClientSecure* s = new WiFiClientSecure();
    s->setInsecure();             // TODO: verify the server certificate
    s->setHandshakeTimeout(10);
    c = s;
  } else {
    c = new WiFiClient();
  }
  if (!c->connect(u.host, u.port, CONNECT_MS)) { delete c; return nullptr; }
  return c;
}

void closeNet(NetworkClient* c) {
  if (!c) return;
  c->stop();
  delete c;
}

// One-shot request for the setup helpers: JSON body optional, the Hue key
// header when `withHueKey`. Body bytes arrive in `snip` and through `sink`.
int request(const char* method, const char* url, const char* body, bool withHueKey,
            char* snip, size_t snipCap, void (*sink)(const uint8_t*, size_t)) {
  Url u;
  if (!parseUrl(url, u)) return -3;
  NetworkClient* c = openNet(u);
  if (!c) return -1;
  size_t len = body ? strlen(body) : 0;
  int k = snprintf(req, sizeof req,
                   "%s %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "%s%s%s"
                   "Content-Type: application/json\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   method, u.path[0] ? u.path : "/", u.host,
                   withHueKey ? "hue-application-key: " : "", withHueKey ? cfg.hueKey : "",
                   withHueKey ? "\r\n" : "", (unsigned)len);
  int code = -2;
  if (k > 0 && k < (int)sizeof req && writeAll(*c, (const uint8_t*)req, (size_t)k) &&
      (!len || writeAll(*c, (const uint8_t*)body, len)))
    code = readResponse(*c, snip, snipCap, nullptr, sink);
  closeNet(c);
  return code;
}

// =============================================================================
// Hue (CLIP v2)
// =============================================================================
// One PUT to one light on the kept-alive session. A request that dies on a
// reused session gets one retry on a fresh one (the bridge may have closed it
// while idle).
int huePutOne(const char* id, const char* body) {
  int n = snprintf(req, sizeof req,
                   "PUT /clip/v2/resource/light/%s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "hue-application-key: %s\r\n"
                   "Content-Type: application/json\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: keep-alive\r\n\r\n%s",
                   id, cfg.hueIp, cfg.hueKey, (unsigned)strlen(body), body);
  if (n <= 0 || n >= (int)sizeof req) { strlcpy(hueErr, "request too long", sizeof hueErr); return -3; }

  for (int attempt = 0; attempt < 2; ++attempt) {
    bool fresh = false;
    if (!hue.connected()) {
      hue.stop();
      hue.setInsecure();              // TODO: pin the bridge's Signify CA
      hue.setHandshakeTimeout(5);     // seconds; the core default is 120
      if (!hue.connect(cfg.hueIp, 443, HUE_CONNECT_MS)) {
        char why[48] = "";
        int err = hue.lastError(why, sizeof why);   // mbedTLS reason, 0 = TCP never connected
        hue.stop();
        snprintf(hueErr, sizeof hueErr, "no TLS connection to %s:443 (%d %s)", cfg.hueIp, err, why);
        return -1;
      }
      fresh = true;
    }
    bool reusable = false;
    char snip[sizeof hueErr];
    int code = -2;
    if (writeAll(hue, (const uint8_t*)req, (size_t)n)) code = readResponse(hue, snip, sizeof snip, &reusable, nullptr);
    if (code > 0) {
      if (!reusable) hue.stop();
      if (code >= 300) strlcpy(hueErr, snip, sizeof hueErr);
      return code;
    }
    hue.stop();
    strlcpy(hueErr, "no response", sizeof hueErr);
    if (fresh) return code;
  }
  return -2;
}

// The light state to every light in hue_lights. Returns 2xx when all took
// it, the first transient failure (< 200, 429, 5xx: retry later), else the
// last refusal (4xx: wrong key or id, retrying cannot help).
int huePut(const Light& l) {
  char body[200];
  const char* dyn = l.instant ? ",\"dynamics\":{\"duration\":0}" : "";
  if (l.color == C_OFF)
    snprintf(body, sizeof body, "{\"on\":{\"on\":false}%s}", dyn);
  else if (l.color == C_WHITE)
    snprintf(body, sizeof body, "{\"on\":{\"on\":true},\"dimming\":{\"brightness\":%u},"
             "\"color_temperature\":{\"mirek\":%u}%s}", l.bri, WHITE_MIREK, dyn);
  else
    snprintf(body, sizeof body, "{\"on\":{\"on\":true},\"dimming\":{\"brightness\":%u},"
             "\"color\":{\"xy\":{\"x\":%.3f,\"y\":%.3f}}%s}",
             l.bri, COLOR_X[l.color], COLOR_Y[l.color], dyn);

  char ids[sizeof cfg.hueLights];
  strlcpy(ids, cfg.hueLights, sizeof ids);
  int result = 200;
  char* save = nullptr;
  for (char* id = strtok_r(ids, ",", &save); id; id = strtok_r(nullptr, ",", &save)) {
    while (*id == ' ') ++id;
    if (!*id) continue;
    int code = huePutOne(id, body);
    if (code < 200 || code == 429 || code >= 500) return code;
    if (code >= 300) result = code;
  }
  return result;
}

bool sameLight(const Light& a, const Light& b) {
  return a.color == b.color && a.bri == b.bri && a.instant == b.instant;
}

// force: resend even if it is already showing (tests, leaving sim).
void setLight(const Light& l, bool force = false) {
  if (!force && wantSet && sameLight(l, want)) return;
  want = l;
  wantSet = true;
  wantSent = false;
}

Light L(uint8_t color, uint8_t bri, bool instant = false) {
  Light l; l.color = color; l.bri = bri; l.instant = instant; return l;
}

void serviceLight() {
  uint32_t now = millis();
  if (hue.connected() && now - hueLastIo > HUE_IDLE_MS) hue.stop();
  if (wantSent || !hueReady() || WiFi.status() != WL_CONNECTED) return;
  // Hold as start + length: unsigned subtraction survives the millis() wrap.
  if (now - hueHoldStart < hueHoldMs || now - hueLastIo < HUE_MIN_GAP_MS) return;

  Light l = want;
  int code = huePut(l);
  hueCode = code;
  hueLastIo = millis();
  bool ok = code >= 200 && code < 300;
  bool refused = code >= 400 && code < 500 && code != 429;   // wrong key / id: retrying cannot help
  if (hueReport) {
    hueReport = false;
    LOG("[HUE] %s -> HTTP %d %s\n", COLOR_NAME[l.color], code, ok ? "" : hueErr);
  }
  if (ok || refused) {
    wantSent = true;
    hueBackoff = hueHoldMs = 0;
    if (ok && hueDown) { hueDown = false; LOG("[HUE] bridge answering again\n"); }
    if (refused && code != hueRefused) LOG("[HUE] bridge refused the command: HTTP %d %s\n", code, hueErr);
    hueRefused = refused ? code : 0;
    return;
  }
  // Retry the CURRENT state after a pause, so one dropped FINISH command does
  // not leave the light blue. Logged once per outage.
  hueBackoff = hueBackoff ? (hueBackoff * 2 > HUE_BACKOFF_MAX ? HUE_BACKOFF_MAX : hueBackoff * 2) : 5000;
  hueHoldStart = millis();
  hueHoldMs = hueBackoff;
  hue.stop();
  if (!hueDown) {
    hueDown = true;
    LOG("[HUE] command failed (HTTP %d %s, WiFi %d dBm), retrying with back-off\n", code, hueErr, WiFi.RSSI());
  }
}

// ---- setup helpers: hue find / pair / lights / blink ----------------------------------
void hueFind() {
  char body[512];
  int code = request("GET", "https://discovery.meethue.com/", nullptr, false, body, sizeof body, nullptr);
  if (code != 200) {
    LOG("[HUE] discovery.meethue.com answered %d. Find the IP in the Hue app: Settings > Bridges > (i)\n", code);
    return;
  }
  int reachable = 0;
  char pick[40] = "";
  for (const char* p = body; (p = findAfter(p, "\"internalipaddress\"")); ) {
    char ip[40];
    copyQuoted(p, ip, sizeof ip);
    if (!ip[0]) continue;
    // The list holds every bridge registered from your internet address;
    // ask each one directly to see which this board can actually reach.
    char url[64], cfgBody[400], name[48] = "?";
    snprintf(url, sizeof url, "https://%s/api/0/config", ip);
    bool ok = request("GET", url, nullptr, false, cfgBody, sizeof cfgBody, nullptr) == 200 &&
              strstr(cfgBody, "\"bridgeid\"");
    const char* n = ok ? findAfter(cfgBody, "\"name\"") : nullptr;
    if (n) copyQuoted(n, name, sizeof name);
    LOG("  bridge %-15s %s\n", ip, ok ? name : "(not reachable from this board)");
    if (ok && !reachable++) strlcpy(pick, ip, sizeof pick);
  }
  if (reachable == 1) {
    cmdSet(String("hue_ip ") + pick);
    LOG("[HUE] next: press the link button on the bridge, then type `hue pair`\n");
  } else if (reachable > 1) {
    LOG("[HUE] several bridges: `set hue_ip <ip>` for the one your light is on\n");
  } else {
    LOG("[HUE] no bridge reachable. Find the IP in the Hue app: Settings > Bridges > (i)\n");
  }
}

void huePair() {
  if (!cfg.hueIp[0]) { LOG("set hue_ip first, or run `hue find`\n"); return; }
  LOG("[HUE] press the link button on the bridge now (waiting up to 30 s)\n");
  char url[64], body[300];
  snprintf(url, sizeof url, "https://%s/api", cfg.hueIp);
  for (uint32_t t0 = millis(); millis() - t0 < 30000; delay(2000)) {
    int code = request("POST", url, "{\"devicetype\":\"mqtt_notification#esp32\"}", false,
                       body, sizeof body, nullptr);
    const char* v = code == 200 ? findAfter(body, "\"username\"") : nullptr;
    if (!v) continue;                    // "link button not pressed" (or no answer)
    char key[64];
    copyQuoted(v, key, sizeof key);
    if (!key[0]) continue;
    cmdSet(String("hue_key ") + key);
    LOG("[HUE] paired. Next: `hue lights`\n");
    return;
  }
  LOG("[HUE] no key: the link button was not pressed in time. Press it, then `hue pair` again.\n");
}

// Every light object in the CLIP v2 list has one "id" followed by its
// metadata "name" (references to other resources use "rid"), so the two
// patterns pair up without a JSON parser and without holding the body.
void lightSink(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    char c = (char)p[i];
    if (scan.inVal) {
      if (c != '"') {
        if (scan.vlen + 1 < sizeof scan.val) scan.val[scan.vlen++] = c;
        continue;
      }
      scan.val[scan.vlen] = 0;
      scan.inVal = false;
      if (!scan.isName) {
        strlcpy(scan.id, scan.val, sizeof scan.id);
      } else if (scan.id[0]) {
        LOG("  %s  %s\n", scan.id, scan.val);
        scan.id[0] = 0;
        scan.count++;
      }
      continue;
    }
    memmove(scan.tail, scan.tail + 1, sizeof scan.tail - 1);
    scan.tail[sizeof scan.tail - 1] = c;
    if (!memcmp(scan.tail + 2, "\"id\":\"", 6))   { scan.inVal = true; scan.isName = false; scan.vlen = 0; }
    else if (!memcmp(scan.tail, "\"name\":\"", 8)) { scan.inVal = true; scan.isName = true;  scan.vlen = 0; }
  }
}

void hueLights() {
  if (!cfg.hueIp[0] || !cfg.hueKey[0]) { LOG("run `hue find` and `hue pair` first\n"); return; }
  hue.stop();
  memset(&scan, 0, sizeof scan);
  char url[80], snip[80];
  snprintf(url, sizeof url, "https://%s/clip/v2/resource/light", cfg.hueIp);
  LOG("[HUE] lights on the bridge (id  name):\n");
  int code = request("GET", url, nullptr, true, snip, sizeof snip, lightSink);
  if (code != 200) { LOG("[HUE] bridge answered %d %s\n", code, snip); return; }
  LOG("[HUE] %d lights. `hue blink <id>` shows which is which, then\n"
      "      `set hue_lights <id>` (several: <id>,<id>)\n", scan.count);
}

void hueBlink(const String& id) {
  if (!cfg.hueIp[0] || !cfg.hueKey[0] || !id.length()) { LOG("usage: hue blink <light id> (from `hue lights`)\n"); return; }
  hue.stop();
  char url[96], snip[80];
  snprintf(url, sizeof url, "https://%s/clip/v2/resource/light/%s", cfg.hueIp, id.c_str());
  int code = request("PUT", url, "{\"identify\":{\"action\":\"identify\"}}", true, snip, sizeof snip, nullptr);
  LOG("[HUE] blink %s -> HTTP %d %s\n", id.c_str(), code, code == 200 ? "" : snip);
}

// =============================================================================
// ntfy + camera snapshot
// =============================================================================
// Request head shared by the text POST and the snapshot PUT.
int pushHead(const Push& p, const Url& u, const char* method, const char* query,
             const char* extra, size_t len) {
  char host[72];
  if (u.port == (u.tls ? 443 : 80)) snprintf(host, sizeof host, "%s", u.host);
  else snprintf(host, sizeof host, "%s:%u", u.host, u.port);
  int k = snprintf(req, sizeof req,
                   "%s %s/%s%s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Title: %s\r\n"
                   "Priority: %s\r\n"
                   "Tags: %s\r\n"
                   "%s"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   method, u.path, cfg.ntfyTopic, query, host, p.title, p.prio, p.tags,
                   extra, (unsigned)len);
  return (k > 0 && k < (int)sizeof req) ? k : -1;
}

int sendText(const Push& p, const Url& u) {
  size_t len = strlen(p.msg);
  int k = pushHead(p, u, "POST", "", "Content-Type: text/plain; charset=utf-8\r\n", len);
  if (k < 0) return -3;
  NetworkClient* c = openNet(u);
  if (!c) return -1;
  int code = -2;
  if (writeAll(*c, (const uint8_t*)req, (size_t)k) && writeAll(*c, (const uint8_t*)p.msg, len)) {
    code = readResponse(*c, nullptr, 0, nullptr, nullptr);
    if (code < 0) code = SENT_NO_REPLY;
  }
  closeNet(c);
  return code;
}

// One camera frame streamed straight into an ntfy PUT: the frame header gives
// the length up front, so the JPEG is never held in RAM. Returns the ntfy
// status, or < 0 before anything reached ntfy (the caller then sends text).
int sendSnapshot(const Push& p, const Url& u) {
  uint32_t heap0 = ESP.getFreeHeap();
  if (heap0 < SNAP_MIN_HEAP) {
    LOG("[PUSH] snapshot skipped: %u B heap free, needs %u\n", (unsigned)heap0, (unsigned)SNAP_MIN_HEAP);
    return -10;
  }
  WiFiClientSecure cam;
  cam.setInsecure();              // the printer's certificate is self-signed
  cam.setHandshakeTimeout(8);
  NetworkClient* net = nullptr;
  uint32_t len = 0, heap1 = 0;
  int result = -11;

  do {
    if (!cam.connect(cfg.printerIp, 6000, 7000)) {
      LOG("[PUSH] camera %s:6000 not answering (A1 / P1 only; LAN Only Liveview on?)\n", cfg.printerIp);
      break;
    }
    // 80-byte auth: header 0x40, 0x3000, 0, 0 (little endian), then "bblp"
    // and the LAN access code, each zero-padded to 32 bytes.
    uint8_t auth[80] = {0};
    putLe32(auth, 0x40);
    putLe32(auth + 4, 0x3000);
    memcpy(auth + 16, "bblp", 4);
    memcpy(auth + 48, cfg.accessCode, strnlen(cfg.accessCode, 32));
    result = -12;
    if (!writeAll(cam, auth, sizeof auth)) { LOG("[PUSH] camera auth write failed\n"); break; }

    // Each frame: a 16-byte header (JPEG length, 0, a 0/1 marker, 0), then
    // the JPEG. A frame that does not start with FF D8 is skipped and the
    // next one taken, up to 3: a P1S has been seen sending one.
    size_t first = 0;
    bool got = false;
    for (int frame = 1; frame <= 3 && !got; ++frame) {
      uint8_t hdr[16];
      result = -12;
      if (!readExact(cam, hdr, sizeof hdr, CAM_IO_MS)) { LOG("[PUSH] camera sent no frame (access code wrong?)\n"); break; }
      len = le32(hdr);
      uint32_t marker = le32(hdr + 8);
      result = -13;
      if (le32(hdr + 4) || le32(hdr + 12) || marker > 1 || len < 1024 || len > CAM_MAX_FRAME) {
        LOG("[PUSH] camera frame header unexpected: size %u marker %u (%02X %02X %02X %02X)\n",
            (unsigned)len, (unsigned)marker, hdr[0], hdr[1], hdr[2], hdr[3]);
        break;
      }
      first = len < sizeof xfer ? len : sizeof xfer;
      result = -14;
      if (!readExact(cam, xfer, first, CAM_IO_MS)) { LOG("[PUSH] camera frame read failed\n"); break; }
      if (xfer[0] == 0xFF && xfer[1] == 0xD8) { got = true; break; }
      LOG("[PUSH] frame %d (%u B) starts %02X %02X %02X %02X, not a JPEG: skipping it\n",
          frame, (unsigned)len, xfer[0], xfer[1], xfer[2], xfer[3]);
      uint32_t left = len - first;
      while (left) {
        size_t k = left < sizeof xfer ? left : sizeof xfer;
        if (!readExact(cam, xfer, k, CAM_IO_MS)) break;
        left -= k;
      }
      if (left) { LOG("[PUSH] camera stalled while skipping a frame\n"); break; }
    }
    if (!got) break;
    heap1 = ESP.getFreeHeap();

    // The message rides in the query string, URL-encoded: as a header it
    // would need RFC 2047 for a file name with umlauts.
    strcpy(query, "?message=");
    urlEncode(p.msg, query + 9, sizeof query - 9);
    int k = pushHead(p, u, "PUT", query, "Filename: snapshot.jpg\r\nContent-Type: image/jpeg\r\n", len);
    net = k < 0 ? nullptr : openNet(u);
    result = -15;
    if (!net) { LOG("[PUSH] ntfy %s not reachable\n", u.host); break; }
    if (!writeAll(*net, (const uint8_t*)req, (size_t)k) || !writeAll(*net, xfer, first)) break;

    uint8_t tail[2] = { xfer[first - 2], xfer[first - 1] };
    uint32_t left = len - first;
    while (left) {
      size_t chunk = left < sizeof xfer ? left : sizeof xfer;
      if (!readExact(cam, xfer, chunk, CAM_IO_MS) || !writeAll(*net, xfer, chunk)) break;
      if (chunk >= 2) { tail[0] = xfer[chunk - 2]; tail[1] = xfer[chunk - 1]; }
      else            { tail[0] = tail[1];          tail[1] = xfer[0]; }
      left -= chunk;
    }
    result = -16;
    if (left) { LOG("[PUSH] snapshot stalled at %u / %u B\n", (unsigned)(len - left), (unsigned)len); break; }
    cam.stop();
    if (tail[0] != 0xFF || tail[1] != 0xD9) LOG("[PUSH] snapshot does not end in FF D9, may be cut off\n");
    result = readResponse(*net, nullptr, 0, nullptr, nullptr);
    if (result < 0) result = SENT_NO_REPLY;   // whole frame went out
    snapBytes = len;
  } while (false);

  cam.stop();
  closeNet(net);
  LOG("[PUSH] snapshot: frame %u B, heap %u before, %u camera open, %u after\n",
      (unsigned)len, (unsigned)heap0, (unsigned)heap1, (unsigned)ESP.getFreeHeap());
  return result;
}

void queuePush(const char* title, const char* prio, const char* tags, const char* msg, bool snap) {
  if (!cfg.ntfyTopic[0]) { LOG("[PUSH] '%s' skipped: no ntfy_topic set\n", title); return; }
  if (push.pending) LOG("[PUSH] '%s' replaced before it went out\n", push.title);
  strlcpy(push.title, title, sizeof push.title);
  strlcpy(push.prio, prio, sizeof push.prio);
  strlcpy(push.tags, tags, sizeof push.tags);
  strlcpy(push.msg, msg, sizeof push.msg);
  trimUtf8(push.msg);
  push.snap = snap && camReady();
  push.pending = true;
}

// After serviceLight() in loop(), so the light changes before the phone buzzes.
void servicePush() {
  if (!push.pending || WiFi.status() != WL_CONNECTED) return;
  push.pending = false;
  hue.stop();                     // one TLS session fewer during the upload
  snapBytes = 0;
  Url u;
  if (!parseUrl(cfg.ntfyServer, u)) { LOG("[PUSH] ntfy_server is not http(s)://host[:port][/path]\n"); return; }

  auto sent = [](int c) { return c == SENT_NO_REPLY || (c >= 200 && c < 300); };
  int code = -1;
  bool ok = false;
  if (push.snap) {
    code = sendSnapshot(push, u);
    ok = sent(code);
    if (!ok) LOG("[PUSH] snapshot failed (%d), sending text only\n", code);
  }
  for (int attempt = 0; !ok && attempt < 2; ++attempt) {
    if (attempt) delay(3000);
    code = sendText(push, u);
    ok = sent(code);
  }
  pushCode = code;
  pushAt = millis();
  LOG("[PUSH] '%s' %s (%s %d)%s\n", push.title, ok ? "sent" : "FAILED",
      code == SENT_NO_REPLY ? "no reply," : "HTTP", code, snapBytes ? " with snapshot" : "");
}

// =============================================================================
// Print state -> light + push
// =============================================================================
Phase parsePhase(const char* s) {
  if (!s[0])                 return PH_UNKNOWN;
  if (!strcmp(s, "IDLE"))    return PH_IDLE;
  if (!strcmp(s, "PREPARE")) return PH_PREPARE;
  if (!strcmp(s, "RUNNING")) return PH_RUNNING;
  if (!strcmp(s, "PAUSE"))   return PH_PAUSE;
  if (!strcmp(s, "FINISH"))  return PH_FINISH;
  if (!strcmp(s, "FAILED"))  return PH_FAILED;
  return PH_OTHER;           // SLICING, INIT, ...: leave the light alone
}

bool isActive(Phase p) { return p == PH_PREPARE || p == PH_RUNNING || p == PH_PAUSE; }

bool isCancelCode(uint32_t c) {
  for (uint32_t k : CANCEL_CODES) if (c == k) return true;
  return false;
}

// print_error as it stands for THIS print: P1 / A1 report only changes, so a
// code left over from an earlier print may never have been reset to 0.
uint32_t printErrorNow() { return (perr != 0 && perr == perrAtStart) ? 0 : perr; }

void jobLabel(char* out, size_t cap) {
  strlcpy(out, job[0] ? job : "(unnamed)", cap);
  trimUtf8(out);
  static const char* const EXT[] = { ".gcode.3mf", ".3mf", ".gcode" };
  size_t n = strlen(out);
  for (const char* e : EXT) {
    size_t k = strlen(e);
    if (n > k && !strcasecmp(out + n - k, e)) { out[n - k] = 0; break; }
  }
}

void notifyFail() {
  char name[64], msg[200];
  jobLabel(name, sizeof name);
  uint32_t err = printErrorNow();
  if (err) snprintf(msg, sizeof msg, "Print failed: %s\nError %04X_%04X", name,
                    (unsigned)(err >> 16), (unsigned)(err & 0xFFFF));
  else     snprintf(msg, sizeof msg, "Print failed: %s", name);
  queuePush("Print failed", "urgent", "rotating_light", msg, true);
}

void notifyFinish() {
  char name[64], msg[200];
  jobLabel(name, sizeof name);
  uint32_t mins = (millis() - startMs) / 60000UL;
  if (!startKnown)    snprintf(msg, sizeof msg, "Done: %s", name);   // joined mid-print
  else if (mins >= 60) snprintf(msg, sizeof msg, "Done: %s in %u h %u min", name,
                                (unsigned)(mins / 60), (unsigned)(mins % 60));
  else                snprintf(msg, sizeof msg, "Done: %s in %u min", name, (unsigned)mins);
  queuePush("Print done", "default", "white_check_mark", msg, false);
}

void showForPhase() {
  if (phase == PH_PREPARE || (phase == PH_RUNNING && layer == 0)) {
    setLight(L(C_WHITE, 40));            // heating and levelling: no layer yet
  } else if (phase == PH_RUNNING) {
    int p = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    p -= p % 5;                          // 5 % steps only
    setLight(L(C_BLUE, (uint8_t)(20 + p * 80 / 100)));
  } else if (phase == PH_PAUSE) {
    setLight(L(C_YELLOW, 100));
  }
}

void startFailure(uint32_t now) {
  offArmed = false;
  flashing = flashLit = true;            // red is already on
  flashNext = now + FLASH_HALF_MS;
  flashUntil = now + FLASH_MS;
  notifyFail();
}

void onTransition(Phase prev) {
  uint32_t now = millis();
  classifyPending = lateErrorWatch = flashing = offArmed = false;
  LOG("[PRINT] %s -> %s\n", PHASE_NAME[prev], PHASE_NAME[phase]);
  switch (phase) {
    case PH_PREPARE:
    case PH_RUNNING:
    case PH_PAUSE:
      if (!isActive(prev)) {             // a new print, or joined mid-print
        startMs = now;
        startKnown = prev != PH_UNKNOWN;
        perrAtStart = perr;
      }
      shownPct = pct;
      shownLayer = layer;
      showForPhase();
      break;
    case PH_FINISH:
      // Seen without the print before it (boot, reconnect): old news.
      if (!isActive(prev)) { setLight(L(C_OFF, 0)); break; }
      setLight(L(C_GREEN, 100));
      offArmed = true;
      offAt = now + FINISH_SHOW_MS;
      notifyFinish();
      break;
    case PH_FAILED:
      if (!isActive(prev)) { setLight(L(C_OFF, 0)); break; }
      setLight(L(C_RED, 100));           // at once; flash or not after SETTLE_MS
      classifyPending = true;
      failedAt = now;
      break;
    case PH_IDLE:
      setLight(L(C_OFF, 0));
      break;
    default:
      break;
  }
}

void feed(Phase p) {
  if (p == PH_UNKNOWN || p == PH_OTHER) return;
  if (p != phase) {
    Phase prev = phase;
    phase = p;
    onTransition(prev);
  } else if (phase == PH_RUNNING && (pct != shownPct || layer != shownLayer)) {
    shownPct = pct;
    shownLayer = layer;
    showForPhase();                      // setLight() drops repeats
  }
}

void serviceTimers() {
  uint32_t now = millis();

  if (classifyPending && now - failedAt >= SETTLE_MS) {
    classifyPending = false;
    uint32_t code = printErrorNow();
    bool cancel = code == 0 || isCancelCode(code);
    LOG("[PRINT] FAILED, print_error %lu (%04X_%04X)%s: %s\n", (unsigned long)perr,
        (unsigned)(perr >> 16), (unsigned)(perr & 0xFFFF),
        code != perr ? ", left from before this print" : "",
        cancel ? "user cancel - red 10 s, no push" : "error - flash + push");
    if (cancel) {
      offArmed = true;
      offAt = now + CANCEL_SHOW_MS;
      lateErrorWatch = code == 0;
    } else {
      startFailure(now);
    }
  }

  if (lateErrorWatch) {
    if (phase != PH_FAILED || now - failedAt > LATE_ERROR_MS) {
      lateErrorWatch = false;
    } else if (printErrorNow() != 0 && !isCancelCode(perr)) {
      lateErrorWatch = false;
      LOG("[PRINT] print_error %lu arrived late: error after all\n", (unsigned long)perr);
      setLight(L(C_RED, 100));
      startFailure(now);
    }
  }

  if (flashing) {
    if ((int32_t)(now - flashUntil) >= 0) {
      flashing = false;
      setLight(L(C_RED, 100));           // solid until the next print
    } else if ((int32_t)(now - flashNext) >= 0) {
      flashLit = !flashLit;
      flashNext = now + FLASH_HALF_MS;
      setLight(flashLit ? L(C_RED, 100, true) : L(C_OFF, 0, true));
    }
  }

  if (offArmed && (int32_t)(now - offAt) >= 0) {
    offArmed = false;
    setLight(L(C_OFF, 0));
  }
}

// =============================================================================
// MQTT
// =============================================================================
// Points just past `"key":` (and spaces) in buf, or nullptr.
const char* findAfter(const char* buf, const char* key) {
  const char* p = strstr(buf, key);
  if (!p) return nullptr;
  p += strlen(key);
  while (*p == ' ' || *p == ':') ++p;
  return p;
}

void copyQuoted(const char* v, char* out, size_t cap) {
  out[0] = 0;
  if (*v++ != '"') return;
  const char* e = strchr(v, '"');
  if (!e) return;
  size_t n = (size_t)(e - v) < cap - 1 ? (size_t)(e - v) : cap - 1;
  memcpy(out, v, n);
  out[n] = 0;
  trimUtf8(out);
}

void onMqtt(char*, byte* payload, unsigned int len) {
  static char buf[MQTT_BUF + 1];
  lastRx = millis();
  if (len > MQTT_BUF) len = MQTT_BUF;
  memcpy(buf, payload, len);
  buf[len] = 0;
  const char* v;
  if ((v = findAfter(buf, "\"gcode_state\""))) copyQuoted(v, mState, sizeof mState);
  if ((v = findAfter(buf, "\"mc_percent\"")))  mPct = atoi(v + (*v == '"'));
  if ((v = findAfter(buf, "\"layer_num\"")))   mLayer = atoi(v + (*v == '"'));
  if ((v = findAfter(buf, "\"print_error\""))) mErr = strtoul(v + (*v == '"'), nullptr, 10);
  if ((v = findAfter(buf, "\"subtask_name\""))) {
    char name[sizeof mJob];
    copyQuoted(v, name, sizeof name);
    if (name[0]) strlcpy(mJob, name, sizeof mJob);
  }
}

void requestPushall() {
  // Bambu printers do not send their state on subscribe: ask for it.
  mqtt.publish(topicRequest, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}");
  lastPushall = millis();
}

void serviceMqtt() {
  if (WiFi.status() != WL_CONNECTED || !cfg.printerIp[0] || !cfg.accessCode[0] || !cfg.serial[0]) return;
  if (mqtt.connected()) {
    mqtt.loop();
    if (!mState[0] && millis() - lastPushall > 5000) requestPushall();
    // Nothing at all since connecting, not even the pushall reply: the broker
    // accepts any serial, so a wrong one subscribes to a silent topic.
    if (!noDataWarned && lastRx < mqttUpSince && millis() - mqttUpSince > NO_DATA_WARN_MS) {
      noDataWarned = true;
      LOG("[MQTT] connected but no printer data: check the serial number (%s) and LAN mode\n", cfg.serial);
    }
    return;
  }
  if (mqttUp) { mqttUp = false; LOG("[MQTT] connection lost\n"); }
  if (mqttLastTry && millis() - mqttLastTry < MQTT_RETRY_MS) return;
  mqttLastTry = millis();
  LOG("[MQTT] connecting to %s...\n", cfg.printerIp);
  // Open TLS ourselves with a short timeout; PubSubClient then reuses the
  // open socket (its own connect would wait 30 s on a printer that is off).
  bool tlsOk = mqttTls.connect(cfg.printerIp, 8883, CONNECT_MS);
  bool ok = tlsOk && mqtt.connect(clientId, "bblp", cfg.accessCode) && mqtt.subscribe(topicReport);
  if (!ok) {
    int st = mqtt.state();
    LOG("[MQTT] failed (state %d): %s\n", st,
        !tlsOk ? "printer not reachable on port 8883 - IP, LAN mode on, WiFi signal?"
        : (st == 4 || st == 5) ? "access code rejected"
        : "printer refused the connection (Developer Mode on newer firmware?)");
    mqtt.disconnect();
    mqttTls.stop();
    return;
  }
  mqttUp = true;
  mqttUpSince = millis();
  noDataWarned = false;
  LOG("[MQTT] connected, %s\n", topicReport);
  requestPushall();
}

// =============================================================================
// Serial commands
// =============================================================================
void printHelp() {
  LOG("Commands:\n");
  LOG("  show                       settings (secrets masked), WiFi, printer, light, last push\n");
  LOG("  set <key> <value>          '-' clears. Keys: wifi_ssid wifi_pass printer_ip\n");
  LOG("                             access_code serial hue_ip hue_key hue_lights\n");
  LOG("                             ntfy_server ntfy_topic\n");
  LOG("  hue find                   find the Hue bridge on your network and set hue_ip\n");
  LOG("  hue pair                   press the bridge's link button, get hue_key\n");
  LOG("  hue lights                 list the bridge's lights (id + name)\n");
  LOG("  hue blink <id>             make one light blink to tell them apart\n");
  LOG("  hue white|blue|yellow|green|red|off    light test\n");
  LOG("  notify                     text push test\n");
  LOG("  snap                       camera photo push test (A1 / P1)\n");
  LOG("  sim PREPARE|RUNNING <pct>|PAUSE|FAILED|CANCEL|FINISH|IDLE\n");
  LOG("  live                       leave sim, follow the printer again\n");
  LOG("  reboot\n");
}

void show() {
  uint32_t now = millis();
  for (const Key& k : KEYS)
    LOG("  %-12s %s\n", k.name, k.secret ? masked(k.val).c_str() : (k.val[0] ? k.val : "(not set)"));
  if (WiFi.status() == WL_CONNECTED)
    LOG("WiFi %s, %d dBm via %s", WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.BSSIDstr().c_str());
  else
    LOG("WiFi down");
  LOG(" | MQTT %s | printer %s", mqttUp ? "up" : "down", PHASE_NAME[phase]);
  if (pct >= 0) LOG(" %d%%", pct);
  if (job[0]) LOG(" '%s'", job);
  LOG("%s\n", sim ? " (SIM)" : "");
  if (wantSet) LOG("Light %s %u%% (%s)%s", COLOR_NAME[want.color], want.color ? want.bri : 0,
                   wantSent ? "sent" : "pending", flashing ? " flashing" : "");
  else LOG("Light: nothing sent yet");
  if (hueLastIo) LOG(" | Hue HTTP %d %lus ago", hueCode, (unsigned long)((now - hueLastIo) / 1000));
  LOG("\n");
  if (pushAt) LOG("Push HTTP %d %lus ago%s\n", pushCode, (unsigned long)((now - pushAt) / 1000),
                  snapBytes ? " with snapshot" : "");
  LOG("Heap %u free, %u largest block\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void cmdSet(const String& args) {
  int sp = args.indexOf(' ');
  String key = sp < 0 ? args : args.substring(0, sp);
  String val = sp < 0 ? "" : args.substring(sp + 1);
  key.toLowerCase();
  val.trim();
  if (val == "-") val = "";
  for (const Key& k : KEYS) {
    if (key != k.name) continue;
    bool wifi = key.startsWith("wifi_");
    if (val.length() >= k.cap || (!wifi && val.indexOf(' ') >= 0)) {
      LOG("%s: at most %u characters%s\n", k.name, (unsigned)k.cap - 1, wifi ? "" : ", no spaces");
      return;
    }
    if (key == "ntfy_topic") {
      for (size_t i = 0; i < val.length(); ++i)
        if (!isalnum((unsigned char)val[i]) && val[i] != '-' && val[i] != '_') {
          LOG("ntfy_topic: letters, digits, - and _ only\n");
          return;
        }
      if (val.length() && val.length() < 20) LOG("WARNING: anyone who guesses the topic reads your pushes. Use 20+ random characters.\n");
    }
    if (key == "hue_lights") {
      for (size_t i = 0; i < val.length(); ++i)
        if (!isxdigit((unsigned char)val[i]) && val[i] != '-' && val[i] != ',') {
          LOG("hue_lights: light ids from `hue lights`, separated by commas\n");
          return;
        }
    }
    if (key == "ntfy_server") {
      if (val.isEmpty()) val = "https://ntfy.sh";
      if (!val.startsWith("http://") && !val.startsWith("https://")) { LOG("ntfy_server: https://... (http:// for a LAN server)\n"); return; }
    }
    if (key == "serial") val.toUpperCase();   // MQTT topics are case-sensitive
    strlcpy(k.val, val.c_str(), k.cap);
    Preferences p;
    p.begin("pstatus", false);
    p.putString(k.name, val);
    p.end();
    if (key.startsWith("hue_")) hue.stop();
    LOG("%s = %s (saved)%s\n", k.name, k.secret ? masked(k.val).c_str() : (val.length() ? val.c_str() : "(cleared)"),
        k.reboot ? " - `reboot` to apply" : "");
    return;
  }
  LOG("unknown key '%s', see `help`\n", key.c_str());
}

void cmdSim(String a) {
  a.toUpperCase();
  int sp = a.indexOf(' ');
  String st = sp < 0 ? a : a.substring(0, sp);
  int p = sp < 0 ? -1 : a.substring(sp + 1).toInt();
  if (!sim) { sim = true; LOG("SIM: printer updates ignored until `live`\n"); }
  // FINISH / FAILED only act after a print: give the sim one if needed.
  if ((st == "FINISH" || st == "FAILED" || st == "CANCEL") && !isActive(phase)) {
    phase = PH_RUNNING;
    startKnown = false;
    perrAtStart = 0;
  }
  strlcpy(job, "sim_print", sizeof job);
  perr = 0;
  if (st == "PREPARE")      { layer = 0; feed(PH_PREPARE); }
  else if (st == "RUNNING") { if (p >= 0) pct = layer = p; feed(PH_RUNNING); }
  else if (st == "PAUSE")   feed(PH_PAUSE);
  else if (st == "FINISH")  feed(PH_FINISH);
  else if (st == "FAILED")  { perr = 1; feed(PH_FAILED); }
  else if (st == "CANCEL")  { perr = CANCEL_CODES[0]; feed(PH_FAILED); }
  else if (st == "IDLE")    feed(PH_IDLE);
  else LOG("usage: sim PREPARE|RUNNING <pct>|PAUSE|FAILED|CANCEL|FINISH|IDLE\n");
}

void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  cmd.toLowerCase();
  arg.trim();

  if (cmd == "help") printHelp();
  else if (cmd == "show") show();
  else if (cmd == "set") cmdSet(arg);
  else if (cmd == "hue") {
    String a = arg;
    a.toLowerCase();
    if (a == "find")                { hueFind(); return; }
    if (a == "pair")                { huePair(); return; }
    if (a == "lights")              { hueLights(); return; }
    if (a.startsWith("blink"))      { String id = arg.substring(5); id.trim(); hueBlink(id); return; }
    for (int i = 0; i <= C_RED; ++i) {
      if (a != COLOR_NAME[i]) continue;
      if (!hueReady()) { LOG("set hue_ip, hue_key and hue_lights first\n"); return; }
      hueReport = true;
      setLight(L((uint8_t)i, 100), true);
      return;
    }
    LOG("usage: hue find|pair|lights|blink <id>|white|blue|yellow|green|red|off\n");
  }
  else if (cmd == "notify") queuePush("Test", "default", "test_tube", "Hello from mqtt_notification", false);
  else if (cmd == "snap") {
    if (!camReady()) { LOG("set printer_ip and access_code first\n"); return; }
    queuePush("Snapshot test", "default", "camera", "Camera snapshot test", true);
  }
  else if (cmd == "sim") cmdSim(arg);
  else if (cmd == "live") {
    sim = false;
    phase = PH_UNKNOWN;                  // resync from the printer, no pushes
    classifyPending = lateErrorWatch = flashing = offArmed = false;
    setLight(L(C_OFF, 0), true);
    LOG("live: following the printer again\n");
  }
  else if (cmd == "reboot") ESP.restart();
  else LOG("unknown command, type `help`\n");
}

void serviceSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { handleLine(line); line = ""; }
    else if (line.length() < 200) line += c;
  }
}

// =============================================================================
void setup() {
#if FARMBOARD2
  for (int pin : { 7, 15, 12, 13, 14, 38 }) {   // door, bender, fan, LED
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
#endif
  Serial.begin(115200);
  delay(500);
  LOG("\n=== mqtt_notification: print state on a Hue light + push to your phone ===\n");

  Preferences p;
  p.begin("pstatus", true);
  for (const Key& k : KEYS) p.getString(k.name, k.val, k.cap);   // missing key: default stays
  p.end();
  if (!cfg.ntfyServer[0]) strlcpy(cfg.ntfyServer, "https://ntfy.sh", sizeof cfg.ntfyServer);

  snprintf(topicReport, sizeof topicReport, "device/%s/report", cfg.serial);
  snprintf(topicRequest, sizeof topicRequest, "device/%s/request", cfg.serial);
  snprintf(clientId, sizeof clientId, "mqttnotify-%06X", (unsigned)(ESP.getEfuseMac() & 0xFFFFFF));
  mqttTls.setInsecure();                 // Bambu printers use self-signed certificates
  mqttTls.setHandshakeTimeout(10);
  mqtt.setServer(cfg.printerIp, 8883);
  mqtt.setBufferSize(MQTT_BUF);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(onMqtt);

  if (cfg.wifiSsid[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                // no modem sleep: quicker reaction
    // Scan every channel and join the STRONGEST access point. The default
    // stops at the first one that matches, which in a mesh can be a far node,
    // and the ESP32 never roams away from it.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
    LOG("WiFi: connecting to '%s'\n", cfg.wifiSsid);
  } else {
    LOG("Not configured yet: type `help` (115200 baud, newline)\n");
  }
}

void loop() {
  static bool wifiUp = false;
  if ((WiFi.status() == WL_CONNECTED) != wifiUp) {
    wifiUp = !wifiUp;
    if (wifiUp) {
      int rssi = WiFi.RSSI();
      LOG("WiFi: connected, IP %s, %d dBm via %s\n", WiFi.localIP().toString().c_str(),
          rssi, WiFi.BSSIDstr().c_str());
      if (rssi < WEAK_RSSI)
        LOG("WiFi: weak signal. The light and the push need about -70 dBm or better: move the board closer to an access point.\n");
    } else {
      LOG("WiFi: lost, reconnecting\n");
    }
  }

  serviceSerial();
  serviceMqtt();
  if (!sim) {
    pct = mPct;
    layer = mLayer;
    perr = mErr;
    strlcpy(job, mJob, sizeof job);
    feed(parsePhase(mState));
  }
  serviceTimers();
  serviceLight();                        // light first, then the phone
  servicePush();
  delay(5);
}
