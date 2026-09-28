// lgtm for the Miyoo Mini Plus: chimes when Claude Code wants permission,
// shows a decision card for the request at the head of the queue, and sends
// back allow / deny / defer. X opens the full detail, paged and loaded in
// chunks as you read.
//
// Networking is a bundled static curl run through popen. The device protocol
// is plain text for the same reason: no TLS stack or JSON parser to carry.

#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#include <SDL/SDL_ttf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Miyoo Mini button -> SDL key mapping (stock firmware / Onion).
#define BTN_UP SDLK_UP
#define BTN_DOWN SDLK_DOWN
#define BTN_LEFT SDLK_LEFT
#define BTN_RIGHT SDLK_RIGHT
#define BTN_A SDLK_SPACE
#define BTN_B SDLK_LCTRL
#define BTN_X SDLK_LSHIFT
#define BTN_Y SDLK_LALT
#define BTN_L1 SDLK_e
#define BTN_R1 SDLK_t
#define BTN_SELECT SDLK_RCTRL
#define BTN_MENU SDLK_ESCAPE

#define W 640
#define H 480
#define POLL_SECONDS 2
// A new request can arrive right as you press A for the previous one. Input
// is ignored for this long after the request on screen changes, so you never
// approve something you haven't seen.
#define ARM_DELAY_MS 700
// One chunk of detail (200 lines on the server side) plus the card fields.
#define RESP_MAX (256 * 1024)
#define MAX_FACTS 8
#define MAX_LINES 50000

// --- shared state between the network thread and the UI ---

typedef struct {
  int count;  // requests waiting, including the one shown
  char id[40];
  char profile[33];
  char tool[64];
  char project[128];
  char host[48];  // machine that asked
  char headline[160];
  char intent[700];
  int total;  // detail lines on the server
  int nfacts;
  char facts[MAX_FACTS][220];
  char first[RESP_MAX];  // first chunk of detail
  char status[64];       // "" when healthy, otherwise what went wrong
} Snapshot;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static Snapshot shared;
static int shared_version;  // bumped whenever `shared` changes
static char outbox_id[40];  // decision waiting to be sent, if id is non-empty
static char outbox_behavior[8];
static char outbox_profile[33];
// A detail chunk the UI wants, and the one the network thread fetched.
static char chunk_want_id[40], chunk_want_profile[33];
static int chunk_want = -1;
static char chunk_got_id[40];
static int chunk_got = -1;
static char chunk_buf[RESP_MAX];
static volatile int running = 1;

static char base_url[512];
static pthread_mutex_t url_mu = PTHREAD_MUTEX_INITIALIZER;  // base_url can be edited on the device
static int refetch_profiles;

// Profiles come from the server; "all" is always offered first. The selected
// one is read by the network thread on every poll.
#define MAX_PROFILES 16
static char profiles[MAX_PROFILES + 1][33] = {"all"};
static int profile_count = 1;
static char selected[33] = "all";
static const char *HEADER_FILE = "/tmp/lgtm.hdr";

// --- alerts: rumble (Mini Plus motor on GPIO 48, active low) and chime ---

static void write_file(const char *path, const char *s) {
  FILE *f = fopen(path, "w");
  if (f) {
    fputs(s, f);
    fclose(f);
  }
}

static void rumble(int ms) {
  if (access("/sys/class/gpio/gpio48", F_OK) != 0) {
    write_file("/sys/class/gpio/export", "48");
    write_file("/sys/class/gpio/gpio48/direction", "out");
  }
  write_file("/sys/class/gpio/gpio48/value", "0");
  SDL_Delay(ms);
  write_file("/sys/class/gpio/gpio48/value", "1");
}

static Mix_Chunk *chime;

static void open_audio(void) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0 || Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0) {
    fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    return;
  }
  chime = Mix_LoadWAV("chime.wav");
  if (!chime) fprintf(stderr, "chime.wav: %s\n", Mix_GetError());
}

static const char *ALERTS[] = {"chime + buzz", "chime", "buzz", "off"};
#define ALERT_COUNT 4
static int alert_i = 0;

static void alert(void) {
  if ((alert_i == 0 || alert_i == 1) && chime) Mix_PlayChannel(-1, chime, 0);
  if (alert_i == 0 || alert_i == 2) rumble(120);
}

// --- config ---

static int load_config(void) {
  FILE *f = fopen("config.txt", "r");
  if (!f) return 0;
  char line[1024], token[512] = "";
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!strncmp(line, "URL=", 4)) snprintf(base_url, sizeof base_url, "%s", line + 4);
    if (!strncmp(line, "TOKEN=", 6)) snprintf(token, sizeof token, "%s", line + 6);
  }
  fclose(f);
  size_t n = strlen(base_url);
  while (n && base_url[n - 1] == '/') base_url[--n] = 0;
  // The URL goes into a shell command inside single quotes.
  if (strchr(base_url, '\'')) base_url[0] = 0, n = 0;
  if (!n || !token[0]) return 0;
  // The token goes in a header file rather than argv, so it isn't in ps.
  FILE *h = fopen(HEADER_FILE, "w");
  if (!h) return 0;
  fprintf(h, "Authorization: Bearer %s\n", token);
  fclose(h);
  return 1;
}

static int net_ready(void);

// --- networking ---

// Runs curl and returns the HTTP status (0 on transport failure). The response
// body is written into out.
static int http(const char *method_args, const char *path, char *out, size_t cap) {
  char cmd[1024];
  pthread_mutex_lock(&url_mu);
  snprintf(cmd, sizeof cmd,
           "./bin/curl -sS -m 8 --cacert ./bin/cacert.pem -H @%s %s -w '\\n%%{http_code}' '%s%s' 2>/dev/null",
           HEADER_FILE, method_args, base_url, path);
  pthread_mutex_unlock(&url_mu);
  FILE *p = popen(cmd, "r");
  if (!p) return 0;
  size_t n = fread(out, 1, cap - 1, p);
  out[n] = 0;
  pclose(p);
  // Last line is the status code from -w.
  char *nl = strrchr(out, '\n');
  if (!nl) return 0;
  int code = atoi(nl + 1);
  *nl = 0;
  return code;
}

static void set_status(Snapshot *s, int code) {
  if (code == 200) s->status[0] = 0;
  else if (code == 0) snprintf(s->status, sizeof s->status, "offline");
  else if (code == 401) snprintf(s->status, sizeof s->status, "bad token");
  else snprintf(s->status, sizeof s->status, "HTTP %d", code);
}

// Returns the next line of *cur (without the newline) and advances past it.
static const char *next_line(const char **cur, int *len) {
  const char *l = *cur;
  if (!l || !*l) return NULL;
  const char *nl = strchr(l, '\n');
  *len = nl ? (int)(nl - l) : (int)strlen(l);
  *cur = nl ? nl + 1 : l + *len;
  return l;
}

static void parse_next(const char *resp, Snapshot *s) {
  const char *cur = resp, *l;
  int n;
#define FIELD(dst)                                       \
  do {                                                   \
    if (!(l = next_line(&cur, &n))) return;              \
    snprintf(dst, sizeof dst, "%.*s", n, l);             \
  } while (0)
  char num[16];
  FIELD(num);
  s->count = atoi(num);
  if (!s->count) return;
  FIELD(s->id);
  FIELD(s->profile);
  FIELD(s->tool);
  FIELD(s->project);
  FIELD(s->host);
  FIELD(s->headline);
  FIELD(s->intent);
  FIELD(num);
  s->total = atoi(num);
  FIELD(num);
  int nfacts = atoi(num);
  for (int i = 0; i < nfacts; i++) {
    char fact[220];
    FIELD(fact);
    if (s->nfacts < MAX_FACTS) snprintf(s->facts[s->nfacts++], sizeof s->facts[0], "%s", fact);
  }
#undef FIELD
  snprintf(s->first, sizeof s->first, "%s", cur);
}

// Profile names end up in URLs and shell commands, so only [a-z0-9-] passes.
static int valid_name(const char *n) {
  if (!n[0] || strlen(n) > 32) return 0;
  for (; *n; n++)
    if (!((*n >= 'a' && *n <= 'z') || (*n >= '0' && *n <= '9') || *n == '-')) return 0;
  return 1;
}

// Request ids are UUIDs; checked before they go into a URL.
static int valid_id(const char *id) {
  if (strlen(id) != 36) return 0;
  for (; *id; id++)
    if (!((*id >= '0' && *id <= '9') || (*id >= 'a' && *id <= 'f') || *id == '-')) return 0;
  return 1;
}

static void fetch_profiles(char *resp, size_t cap) {
  if (http("", "/v1/device/profiles", resp, cap) != 200) return;
  pthread_mutex_lock(&mu);
  profile_count = 1;
  for (char *line = strtok(resp, "\n"); line && profile_count <= MAX_PROFILES; line = strtok(NULL, "\n"))
    if (valid_name(line) && strcmp(line, "all")) snprintf(profiles[profile_count++], 33, "%s", line);
  pthread_mutex_unlock(&mu);
}

static void *net_loop(void *arg) {
  (void)arg;
  static char resp[RESP_MAX + 4096];
  static Snapshot next;
  int have_profiles = 0;
  while (running) {
    // Retried each loop until it works, so starting offline is fine.
    pthread_mutex_lock(&mu);
    int refetch = refetch_profiles;
    refetch_profiles = 0;
    pthread_mutex_unlock(&mu);
    if (!have_profiles || refetch) {
      fetch_profiles(resp, sizeof resp);
      have_profiles = profile_count > 1;
    }
    char id[40] = "", behavior[8] = "", profile[33] = "", want[33];
    char cid[40] = "", cprofile[33] = "";
    int cn = -1;
    pthread_mutex_lock(&mu);
    if (outbox_id[0]) {
      snprintf(id, sizeof id, "%s", outbox_id);
      snprintf(behavior, sizeof behavior, "%s", outbox_behavior);
      snprintf(profile, sizeof profile, "%s", outbox_profile);
    }
    if (chunk_want >= 0) {
      cn = chunk_want;
      snprintf(cid, sizeof cid, "%s", chunk_want_id);
      snprintf(cprofile, sizeof cprofile, "%s", chunk_want_profile);
    }
    snprintf(want, sizeof want, "%s", selected);
    pthread_mutex_unlock(&mu);

    if (id[0] && valid_id(id) && valid_name(profile)) {
      char args[160];
      snprintf(args, sizeof args, "-d 'id=%s&profile=%s&behavior=%s'", id, profile, behavior);
      int code = http(args, "/v1/device/decide", resp, sizeof resp);
      pthread_mutex_lock(&mu);
      // 404 means the hook already gave up; either way the decision is done.
      if (code == 200 || code == 404) outbox_id[0] = 0;
      pthread_mutex_unlock(&mu);
    }

    if (cn >= 0 && valid_id(cid) && valid_name(cprofile)) {
      char path[160];
      snprintf(path, sizeof path, "/v1/device/body?profile=%s&id=%s&chunk=%d", cprofile, cid, cn);
      int code = http("", path, resp, sizeof resp);
      pthread_mutex_lock(&mu);
      if (code == 200 && chunk_want == cn && !strcmp(chunk_want_id, cid)) {
        snprintf(chunk_buf, sizeof chunk_buf, "%.*s", (int)sizeof chunk_buf - 1, resp);
        snprintf(chunk_got_id, sizeof chunk_got_id, "%s", cid);
        chunk_got = cn;
        chunk_want = -1;
      }
      pthread_mutex_unlock(&mu);
    }

    char path[96];
    snprintf(path, sizeof path, "/v1/device/next?profile=%s", valid_name(want) ? want : "all");
    int code = http("", path, resp, sizeof resp);
    memset(&next, 0, sizeof next);
    if (code == 200) parse_next(resp, &next);
    // A response for a profile that's no longer selected would flash the
    // wrong queue; drop it and poll again.
    pthread_mutex_lock(&mu);
    int stale = strcmp(want, selected);
    pthread_mutex_unlock(&mu);
    if (stale) continue;
    if (code == 404) {
      // The saved profile was removed on the server; fall back to all.
      pthread_mutex_lock(&mu);
      snprintf(selected, sizeof selected, "all");
      pthread_mutex_unlock(&mu);
      continue;
    }
    set_status(&next, code);
    pthread_mutex_lock(&mu);
    // Keep showing the last request while offline instead of blanking it.
    if (code == 200) shared = next;
    else snprintf(shared.status, sizeof shared.status, "%s", next.status);
    shared_version++;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += POLL_SECONDS;
    if (!outbox_id[0] && chunk_want < 0) pthread_cond_timedwait(&wake, &mu, &ts);
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

static void decide(const char *id, const char *profile, const char *behavior) {
  pthread_mutex_lock(&mu);
  snprintf(outbox_id, sizeof outbox_id, "%s", id);
  snprintf(outbox_profile, sizeof outbox_profile, "%s", profile);
  snprintf(outbox_behavior, sizeof outbox_behavior, "%s", behavior);
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&mu);
}

static void want_chunk(const char *id, const char *profile, int n) {
  pthread_mutex_lock(&mu);
  if (chunk_want != n || strcmp(chunk_want_id, id)) {
    chunk_want = n;
    snprintf(chunk_want_id, sizeof chunk_want_id, "%s", id);
    snprintf(chunk_want_profile, sizeof chunk_want_profile, "%s", profile);
    pthread_cond_signal(&wake);
  }
  pthread_mutex_unlock(&mu);
}

// --- look: themes, fonts, sizes ---

typedef struct {
  const char *name;
  Uint32 bg, fg, dim, add, del, cmd;
  Uint32 bar_bg, bar_fg, bar_ok, bar_warn, bar_err, footer_bg;
  Uint32 cap_on, cap_on_fg, cap_off;
  Uint32 panel, border, select, scrollbar;
} Theme;

static const Theme THEMES[] = {
    {"dark", 0x141416, 0xE6E6E1, 0x82827D, 0x78DC82, 0xF06E64, 0xFFD26E, 0xE8E4D8, 0x141416, 0x1F7A32, 0x8A5A00,
     0xB3261E, 0x222226, 0xE8E4D8, 0x141416, 0x3A3A3E, 0x1C1C1F, 0xE8E4D8, 0x3A3A3E, 0x5A5A5A},
    {"light", 0xF4F1E8, 0x1C1C1F, 0x7A776E, 0x1F7A32, 0xB3261E, 0x8A5A00, 0x1C1C1F, 0xF4F1E8, 0x78DC82, 0xFFD26E,
     0xF06E64, 0xE6E1D3, 0x1C1C1F, 0xF4F1E8, 0xCFC9B8, 0xFBF9F3, 0x1C1C1F, 0xE0DACB, 0xA8A396},
};
#define THEME_COUNT 2

static const struct {
  const char *name, *file;
} FONTS[] = {
    {"Atkinson Mono", "fonts/atkinson.ttf"}, {"IBM Plex Mono", "fonts/plex.ttf"}, {"Fira Mono", "fonts/fira.ttf"},
    {"Space Mono", "fonts/space.ttf"},       {"VT323", "fonts/vt323.ttf"},
};
#define FONT_COUNT 5

static const int SIZES[] = {16, 18, 20, 22, 24, 28};
#define SIZE_COUNT 6

static int theme_i = 0, font_i = 0, size_i = 2;
static const Theme *T = &THEMES[0];

static void load_settings(void) {
  FILE *f = fopen("settings.txt", "r");
  if (!f) return;
  char line[128];
  while (fgets(line, sizeof line, f)) {
    int v;
    if (sscanf(line, "theme=%d", &v) == 1 && v >= 0 && v < THEME_COUNT) theme_i = v;
    if (sscanf(line, "font=%d", &v) == 1 && v >= 0 && v < FONT_COUNT) font_i = v;
    if (sscanf(line, "size=%d", &v) == 1 && v >= 0 && v < SIZE_COUNT) size_i = v;
    if (sscanf(line, "alert=%d", &v) == 1 && v >= 0 && v < ALERT_COUNT) alert_i = v;
    char name[33];
    if (sscanf(line, "profile=%32s", name) == 1 && valid_name(name)) snprintf(selected, sizeof selected, "%s", name);
  }
  fclose(f);
}

static void save_settings(void) {
  FILE *f = fopen("settings.txt", "w");
  if (!f) return;
  fprintf(f, "theme=%d\nfont=%d\nsize=%d\nalert=%d\nprofile=%s\n", theme_i, font_i, size_i, alert_i, selected);
  fclose(f);
}

// --- drawing primitives ---

// UI text is always Atkinson Hyperlegible Next (proportional, easiest to read);
// code (diffs, commands) uses the chosen monospace font.
static const char *UI_FONT = "fonts/atkinson-next.ttf";
static TTF_Font *font, *font_big, *mono;
// Layout, recomputed whenever the font or size changes. line_h is UI text;
// code_h and code_w are the monospace line height and character width.
static int line_h, big_h, header_h, cap_h, code_w, code_h, cols;

static int apply_font(void) {
  TTF_Font *f = TTF_OpenFont(UI_FONT, SIZES[size_i]);
  TTF_Font *fb = TTF_OpenFont(UI_FONT, SIZES[size_i] + 4);
  TTF_Font *fm = TTF_OpenFont(FONTS[font_i].file, SIZES[size_i]);
  if (!f || !fb || !fm) {
    if (f) TTF_CloseFont(f);
    if (fb) TTF_CloseFont(fb);
    if (fm) TTF_CloseFont(fm);
    return 0;
  }
  if (font) TTF_CloseFont(font);
  if (font_big) TTF_CloseFont(font_big);
  if (mono) TTF_CloseFont(mono);
  font = f;
  font_big = fb;
  mono = fm;
  TTF_SizeUTF8(mono, "M", &code_w, NULL);
  code_h = TTF_FontLineSkip(mono);
  line_h = TTF_FontLineSkip(font);
  big_h = TTF_FontLineSkip(font_big);
  header_h = big_h + 12;
  cap_h = line_h + 4;
  cols = (W - 28) / code_w;
  return 1;
}

static SDL_Color rgb(Uint32 c) {
  SDL_Color s = {c >> 16, (c >> 8) & 0xff, c & 0xff, 0};
  return s;
}

static void text(SDL_Surface *dst, TTF_Font *f, const char *s, int x, int y, Uint32 c) {
  if (!s[0]) return;
  SDL_Surface *t = TTF_RenderUTF8_Blended(f, s, rgb(c));
  if (!t) return;
  SDL_Rect r = {x, y, 0, 0};
  SDL_BlitSurface(t, NULL, dst, &r);
  SDL_FreeSurface(t);
}

static int text_w(TTF_Font *f, const char *s) {
  int w = 0;
  TTF_SizeUTF8(f, s, &w, NULL);
  return w;
}

static void fill(SDL_Surface *dst, int x, int y, int w, int h, Uint32 c) {
  SDL_Rect r = {x, y, w, h};
  SDL_FillRect(dst, &r, SDL_MapRGB(dst->format, c >> 16, (c >> 8) & 0xff, c & 0xff));
}

// Word-wraps s to `width` pixels in font f, at most max_lines rows, drawing
// each; the last row gets "..." if text remains. Returns the y below it.
static int draw_wrapped(SDL_Surface *dst, TTF_Font *f, const char *s, int x, int y, int width, int max_lines,
                        Uint32 c) {
  char row[1024];
  int skip = TTF_FontLineSkip(f), drawn = 0;
  while (*s && drawn < max_lines) {
    // Grow the row a word at a time while it fits.
    int cut = 0;
    for (int i = 0;; i++) {
      if (s[i] == ' ' || !s[i]) {
        if (i >= (int)sizeof row - 4) break;
        snprintf(row, sizeof row, "%.*s", i, s);
        if (text_w(f, row) > width && cut) break;
        cut = i;
        if (!s[i] || text_w(f, row) > width) break;
      }
    }
    // A single word wider than the line (or the row buffer): break it by
    // characters.
    if (!cut) cut = strlen(s) < 256 ? (int)strlen(s) : 256;
    snprintf(row, sizeof row, "%.*s", cut, s);
    while (cut > 1 && text_w(f, row) > width) {
      do cut--; while (cut > 1 && (s[cut] & 0xC0) == 0x80);
      snprintf(row, sizeof row, "%.*s", cut, s);
    }
    if (drawn == max_lines - 1 && s[cut]) {
      while (cut > 1 && text_w(f, row) + text_w(f, "...") > width) {
        do cut--; while (cut > 1 && (s[cut] & 0xC0) == 0x80);
        snprintf(row, sizeof row, "%.*s", cut, s);
      }
      snprintf(row + strlen(row), sizeof row - strlen(row), "...");
    }
    text(dst, f, row, x, y, c);
    y += skip;
    drawn++;
    s += cut;
    while (*s == ' ') s++;
  }
  return y;
}

// A button as a key cap followed by what it does; returns its width.
static int legend_w(const char *key, const char *action) {
  return text_w(font, key) + 12 + (action[0] ? 8 + text_w(font, action) : 0);
}

static void legend(SDL_Surface *s, int x, int y, const char *key, const char *action, Uint32 cap, Uint32 cap_fg,
                   Uint32 c) {
  int kw = text_w(font, key);
  fill(s, x, y, kw + 12, cap_h, cap);
  text(s, font, key, x + 6, y + 2, cap_fg);
  if (action[0]) text(s, font, action, x + kw + 20, y + 2, c);
}

// --- detail body: loaded chunks, wrapped to the screen ---

static char *body;  // loaded detail lines, '\n'-separated
static size_t body_len, body_cap;
static int body_lines, body_chunks;  // server lines and chunks loaded so far
static char *wrapped;
static char *lines[MAX_LINES];
static Uint32 line_colors[MAX_LINES];
static int nlines, wrap_dirty = 1;

static void body_reset(void) {
  body_len = 0;
  body_lines = body_chunks = 0;
  if (body) body[0] = 0;
  wrap_dirty = 1;
}

static void body_append(const char *chunk) {
  size_t n = strlen(chunk);
  if (body_len + n + 2 > body_cap) {
    body_cap = (body_len + n + 2) * 2;
    body = realloc(body, body_cap);
  }
  memcpy(body + body_len, chunk, n);
  body_len += n;
  if (body_len && body[body_len - 1] != '\n') body[body_len++] = '\n';
  body[body_len] = 0;
  for (const char *p = chunk; *p; p++)
    if (*p == '\n') body_lines++;
  if (n && chunk[n - 1] != '\n') body_lines++;
  body_chunks++;
  wrap_dirty = 1;
}

static Uint32 line_color(const char *l) {
  if (!strncmp(l, "+ ", 2)) return T->add;
  if (!strncmp(l, "- ", 2)) return T->del;
  if (!strncmp(l, "$ ", 2)) return T->cmd;
  if (!strncmp(l, "  ...", 5)) return T->dim;
  return T->fg;
}

// Wraps the loaded body to the screen width. Continuations keep the color of
// the line they came from.
static void rewrap(void) {
  wrapped = realloc(wrapped, body_len + MAX_LINES + 1);
  nlines = 0;
  char *w = wrapped;
  const char *p = body ? body : "";
  Uint32 color = 0;
  int fresh = 1;
  while (*p && nlines < MAX_LINES) {
    if (fresh) color = line_color(p);
    line_colors[nlines] = color;
    lines[nlines++] = w;
    int col = 0;
    while (*p && *p != '\n') {
      if ((*p & 0xC0) != 0x80) {
        if (col == cols) break;
        col++;
      }
      *w++ = *p++;
    }
    *w++ = 0;
    fresh = *p == '\n';
    if (fresh) p++;
  }
  wrap_dirty = 0;
}

// --- screens ---

enum { VIEW_CARD, VIEW_DETAIL };

static void header(SDL_Surface *s, const Snapshot *snap) {
  fill(s, 0, 0, W, header_h, T->bar_bg);
  int x = 14;
  text(s, font_big, "lgtm", x, (header_h - big_h) / 2, T->bar_fg);
  x += text_w(font_big, "lgtm") + 14;
  legend(s, x, (header_h - cap_h) / 2, "SELECT", "", T->bar_fg, T->bar_bg, T->bar_fg);

  // Right side: "<status> · <profile>", status in color.
  char st[64];
  Uint32 sc;
  if (snap->status[0]) snprintf(st, sizeof st, "%s", snap->status), sc = T->bar_err;
  else if (snap->count) snprintf(st, sizeof st, "%d waiting", snap->count), sc = T->bar_warn;
  else snprintf(st, sizeof st, "online"), sc = T->bar_ok;
  char tail[48];
  snprintf(tail, sizeof tail, " · %s", selected);
  int tw = text_w(font, tail), sw = text_w(font, st), y = (header_h - line_h) / 2;
  text(s, font, tail, W - 14 - tw, y, T->bar_fg);
  text(s, font, st, W - 14 - tw - sw, y, sc);
}

// The action row. Wraps to a second row at big font sizes; returns its top.
static int footer(SDL_Surface *s, int view, int armed, const char *host) {
  // "answer on <host>" goes last: it's the longest and its length varies.
  char defer[64];
  snprintf(defer, sizeof defer, "answer on %.24s", host[0] ? host : "computer");
  const char *keys[] = {"A", "B", "Y", "X"};
  const char *acts[] = {"approve", "deny", view == VIEW_CARD ? "details" : "card", defer};
  Uint32 colors[] = {T->add, T->del, T->fg, T->fg};
  int rows = 1, x = 14;
  for (int i = 0; i < 4; i++) {
    int w = legend_w(keys[i], acts[i]);
    if (x + w > W - 14) rows++, x = 14;
    x += w + 22;
  }
  int fh = rows * (cap_h + 8) + 8, top = H - fh;
  fill(s, 0, top, W, fh, T->footer_bg);
  x = 14;
  int y = top + 8;
  for (int i = 0; i < 4; i++) {
    int w = legend_w(keys[i], acts[i]);
    if (x + w > W - 14) y += cap_h + 8, x = 14;
    // Y (details) always works; the decisions wait out the arm delay.
    int on = armed || i == 2;
    legend(s, x, y, keys[i], acts[i], on ? T->cap_on : T->cap_off, on ? T->cap_on_fg : T->dim,
           on ? colors[i] : T->dim);
    x += w + 22;
  }
  return top;
}

static void draw_idle(SDL_Surface *s) {
  int ok = net_ready();
  const char *msg = ok ? "nothing to approve" : "not set up";
  text(s, font_big, msg, (W - text_w(font_big, msg)) / 2, (H + header_h - big_h) / 2 - line_h, ok ? T->dim : T->del);
  if (!ok) {
    const char *how = "config.txt needs URL= and TOKEN=";
    text(s, font, how, (W - text_w(font, how)) / 2, (H + header_h) / 2 + 8, T->dim);
  }
}

static void draw_card(SDL_Surface *s, const Snapshot *snap, int bottom) {
  int x = 14, y = header_h + 12, width = W - 28;
  text(s, font_big, snap->headline, x, y, T->cmd);
  y += big_h;
  char where[200];
  snprintf(where, sizeof where, "%s%s%s%s", snap->project[0] ? "in " : "", snap->project,
           strcmp(selected, "all") ? "" : " · ", strcmp(selected, "all") ? "" : snap->profile);
  text(s, font, where, x, y, T->dim);
  y += line_h + 12;

  if (snap->intent[0]) {
    // Claude's own words, set off like a quote.
    int top = y;
    int room = (bottom - y - (snap->nfacts + 1) * line_h - 16) / line_h;
    if (room < 2) room = 2;
    y = draw_wrapped(s, font, snap->intent, x + 14, y, width - 14, room, T->fg);
    fill(s, x, top, 4, y - top - 4, T->cmd);
    y += 12;
  }

  for (int i = 0; i < snap->nfacts && y + line_h <= bottom; i++) {
    int warn = !strncmp(snap->facts[i], "! ", 2);
    y = draw_wrapped(s, font, snap->facts[i] + (warn ? 2 : 0), x + (warn ? 14 : 0), y, width - (warn ? 14 : 0), 2,
                     warn ? T->del : T->dim);
    if (warn) fill(s, x, y - line_h + 4, 4, line_h - 8, T->del);
  }

  // Show as much of the detail as fits. When all of it does, there's nothing
  // behind X worth pointing at.
  y += 8;
  int room = (bottom - y - 6) / code_h;
  int all_fits = body_lines >= snap->total && nlines <= room;
  int show = all_fits ? nlines : (bottom - y - line_h - 12) / code_h;
  for (int i = 0; i < show && i < nlines; i++) text(s, mono, lines[i], x, y + i * code_h, line_colors[i]);
  if (!all_fits && snap->total) {
    char more[64];
    snprintf(more, sizeof more, "Y for all %d lines", snap->total);
    text(s, font, more, x, bottom - line_h - 6, T->dim);
  }
}

static void draw_detail(SDL_Surface *s, const Snapshot *snap, int scroll, int rows, int loading) {
  int x = 14, y = header_h + 8;
  text(s, font, snap->headline, x, y, T->cmd);
  // "page 2/5", with a + while more of the body is still on the server.
  int pages = nlines ? (nlines + rows - 1) / rows : 1;
  int page = scroll / (rows ? rows : 1) + 1;
  if (scroll + rows >= nlines && page < pages) page = pages;
  char pg[48];
  snprintf(pg, sizeof pg, "page %d/%d%s", page, pages, body_lines < snap->total ? "+" : "");
  fill(s, W - 14 - text_w(font, pg) - 8, y, text_w(font, pg) + 8, line_h, T->bg);
  text(s, font, pg, W - 14 - text_w(font, pg), y, T->dim);
  y += line_h + 6;

  for (int i = 0; i < rows && scroll + i < nlines; i++)
    text(s, mono, lines[scroll + i], x, y + i * code_h, line_colors[scroll + i]);
  if (nlines > rows) {
    int track = rows * code_h, bar = track * rows / nlines;
    if (bar < 12) bar = 12;
    int pos = nlines > rows ? (track - bar) * scroll / (nlines - rows) : 0;
    fill(s, W - 6, y + pos, 4, bar, T->scrollbar);
  }
  if (scroll + rows >= nlines && body_lines < snap->total) {
    char msg[80];
    if (loading) snprintf(msg, sizeof msg, "loading more of %d lines...", snap->total);
    else snprintf(msg, sizeof msg, "lines 1-%d of %d, page on for more", body_lines, snap->total);
    int my = y + (nlines - scroll < rows ? nlines - scroll : rows - 1) * code_h;
    text(s, font, msg, x, my, T->dim);
  }
}

// --- menu and the screens it opens ---

enum { M_RESUME, M_PROFILE, M_PROFILES, M_SERVER, M_ALERT, M_THEME, M_FONT, M_SIZE, M_QUIT, MENU_COUNT };
static const char *MENU_LABELS[] = {"resume", "watching", "profiles", "server", "alert",
                                    "theme",  "code font", "size",    "quit lgtm"};

enum { MODE_MAIN, MODE_MENU, MODE_PROFILES, MODE_KEYBOARD, MODE_PAIR, MODE_CONFIRM };

static const char *url_host(void) {
  const char *h = strstr(base_url, "://");
  return h ? h + 3 : base_url;
}

static void draw_menu(SDL_Surface *s, int sel) {
  int item_h = line_h + 6;
  int mw = W - 60, mh = big_h + 18 + MENU_COUNT * item_h + 10;
  int x = (W - mw) / 2, y = (H - mh) / 2;
  fill(s, x - 2, y - 2, mw + 4, mh + 4, T->border);
  fill(s, x, y, mw, mh, T->panel);
  text(s, font_big, "menu", x + 18, y + 8, T->cmd);

  for (int i = 0; i < MENU_COUNT; i++) {
    int iy = y + big_h + 18 + i * item_h;
    if (i == sel) fill(s, x + 12, iy - 2, mw - 24, line_h + 4, T->select);
    text(s, font, MENU_LABELS[i], x + 24, iy, i == sel ? T->fg : T->dim);
    char val[80] = "";
    if (i == M_PROFILE) snprintf(val, sizeof val, "< %s >", selected);
    if (i == M_PROFILES) snprintf(val, sizeof val, "%d paired  >", profile_count - 1);
    if (i == M_SERVER) snprintf(val, sizeof val, "%.22s%s  >", url_host(), strlen(url_host()) > 22 ? "..." : "");
    if (i == M_ALERT) snprintf(val, sizeof val, "< %s >", ALERTS[alert_i]);
    if (i == M_THEME) snprintf(val, sizeof val, "< %s >", THEMES[theme_i].name);
    if (i == M_FONT) snprintf(val, sizeof val, "< %s >", FONTS[font_i].name);
    if (i == M_SIZE) snprintf(val, sizeof val, "< %d >", SIZES[size_i]);
    text(s, font, val, x + mw - 24 - text_w(font, val), iy, i == sel ? T->fg : T->dim);
  }
}

// A full-screen panel under the header, with a hint row at the bottom.
static int panel(SDL_Surface *s, const char *title, const char *hint) {
  fill(s, 0, header_h, W, H - header_h, T->bg);
  text(s, font_big, title, 14, header_h + 10, T->cmd);
  fill(s, 0, H - cap_h - 16, W, cap_h + 16, T->footer_bg);
  text(s, font, hint, 14, H - cap_h - 8 + 2, T->dim);
  return header_h + 10 + big_h + 8;
}

// A short-lived line at the bottom of a panel: "removed work", errors, etc.
static char toast[96];
static Uint32 toast_at;

static void say(const char *msg) {
  snprintf(toast, sizeof toast, "%s", msg);
  toast_at = SDL_GetTicks();
}

static void draw_toast(SDL_Surface *s) {
  if (!toast[0] || SDL_GetTicks() - toast_at > 3500) return;
  text(s, font, toast, 14, H - cap_h - 16 - line_h - 6, T->cmd);
}

// Blocking calls from the UI thread are fine for these rare actions, but the
// screen should say something is happening first.
static void working(SDL_Surface *screen, SDL_Surface *video, const char *what) {
  int w = text_w(font, what) + 32;
  fill(screen, (W - w) / 2 - 2, H / 2 - line_h - 2, w + 4, 2 * line_h + 4, T->border);
  fill(screen, (W - w) / 2, H / 2 - line_h, w, 2 * line_h, T->panel);
  text(screen, font, what, (W - w) / 2 + 16, H / 2 - line_h / 2, T->fg);
  SDL_BlitSurface(screen, NULL, video, NULL);
  SDL_Flip(video);
}

// --- on-screen keyboard ---

static const char *KB_ROWS[] = {"abcdefghij", "klmnopqrst", "uvwxyz0123", "456789-._:"};
#define KB_ROW_COUNT 4
// Bottom row: "/", backspace, done.
#define KB_SPECIALS 3
static const char *KB_SPECIAL[] = {"/", "del", "done"};

static struct {
  int url;  // URL mode allows . _ : / ; name mode only a-z 0-9 -
  char buf[200];
  int row, col;
  char title[48];
} kb;

static int kb_allowed(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') return 1;
  return kb.url && (c == '.' || c == '_' || c == ':' || c == '/');
}

static void kb_open(const char *title, const char *initial, int url) {
  memset(&kb, 0, sizeof kb);
  kb.url = url;
  snprintf(kb.title, sizeof kb.title, "%s", title);
  snprintf(kb.buf, sizeof kb.buf, "%s", initial);
}

static void kb_type(const char *s) {
  size_t n = strlen(kb.buf);
  for (; *s && n < (kb.url ? sizeof kb.buf - 1 : 32); s++)
    if (kb_allowed(*s)) kb.buf[n++] = *s;
  kb.buf[n] = 0;
}

static void kb_backspace(void) {
  size_t n = strlen(kb.buf);
  if (n) kb.buf[n - 1] = 0;
}

// Returns 1 when the user pressed done.
static int kb_key(SDLKey k) {
  int cols = kb.row == KB_ROW_COUNT ? KB_SPECIALS : 10;
  if (k == BTN_UP) kb.row = (kb.row + KB_ROW_COUNT) % (KB_ROW_COUNT + 1);
  else if (k == BTN_DOWN) kb.row = (kb.row + 1) % (KB_ROW_COUNT + 1);
  else if (k == BTN_LEFT) kb.col = (kb.col + cols - 1) % cols;
  else if (k == BTN_RIGHT) kb.col = (kb.col + 1) % cols;
  else if (k == BTN_B) kb_backspace();
  else if (k == SDLK_RETURN) return 1;  // START
  else if (kb.url && k == BTN_L1) kb_type("https://");
  else if (kb.url && k == BTN_R1) kb_type(".workers.dev");
  else if (k == BTN_A) {
    if (kb.row < KB_ROW_COUNT) {
      char c[2] = {KB_ROWS[kb.row][kb.col], 0};
      kb_type(c);
    } else if (kb.col == 0) kb_type("/");
    else if (kb.col == 1) kb_backspace();
    else return 1;
  }
  if (kb.row == KB_ROW_COUNT && kb.col >= KB_SPECIALS) kb.col = KB_SPECIALS - 1;
  return 0;
}

static void draw_keyboard(SDL_Surface *s) {
  int y = panel(s, kb.title,
                kb.url ? "A type  B del  START done  L1 https://  R1 .workers.dev" : "A type  B del  START done");
  // The text so far, with a block cursor, wrapped if it's a long URL.
  char shown[210];
  snprintf(shown, sizeof shown, "%s_", kb.buf);
  fill(s, 10, y - 2, W - 20, 2 * line_h + 8, T->panel);
  draw_wrapped(s, mono, shown, 18, y + 2, W - 36, 2, T->fg);
  y += 2 * line_h + 18;

  int cw = (W - 40) / 10, ch = cap_h + 8;
  for (int r = 0; r <= KB_ROW_COUNT; r++) {
    int n = r < KB_ROW_COUNT ? 10 : KB_SPECIALS;
    for (int c = 0; c < n; c++) {
      char label[8];
      int allowed = 1, span = 1, cx;
      if (r < KB_ROW_COUNT) {
        label[0] = KB_ROWS[r][c], label[1] = 0;
        allowed = kb_allowed(label[0]);
        cx = 20 + c * cw;
      } else {
        snprintf(label, sizeof label, "%s", KB_SPECIAL[c]);
        allowed = c != 0 || kb.url;
        span = c == 0 ? 2 : 4;
        cx = 20 + (c == 0 ? 0 : c == 1 ? 2 : 6) * cw;
      }
      int on = r == kb.row && c == kb.col;
      fill(s, cx + 2, y + 2, span * cw - 4, ch - 4, on ? T->cap_on : T->footer_bg);
      text(s, font, label, cx + (span * cw - text_w(font, label)) / 2, y + (ch - line_h) / 2,
           on ? T->cap_on_fg : allowed ? T->fg : T->scrollbar);
    }
    y += ch;
  }
}

// --- pairing ---

static struct {
  char profile[33];
  char code[16];
  Uint32 expires_at;  // SDL ticks
  Uint32 checked_at;
  char status[16];  // pending | paired | expired
} pr;
static TTF_Font *font_pin;

static void draw_pair(SDL_Surface *s) {
  char title[64];
  snprintf(title, sizeof title, "pair \"%s\"", pr.profile);
  int y = panel(s, title, "B back");
  int left = pr.expires_at > SDL_GetTicks() ? (pr.expires_at - SDL_GetTicks()) / 1000 : 0;

  if (!strcmp(pr.status, "paired")) {
    text(s, font_big, "paired!", 14, y + 20, T->add);
    char msg[80];
    snprintf(msg, sizeof msg, "\"%s\" can now send requests here.", pr.profile);
    draw_wrapped(s, font, msg, 14, y + 20 + big_h + 8, W - 28, 2, T->fg);
    return;
  }
  if (!strcmp(pr.status, "expired") || !left) {
    text(s, font_big, "code expired", 14, y + 20, T->del);
    draw_wrapped(s, font, "Start again from profiles. Codes last 10 minutes, work once, and too many wrong tries cancel them.",
                 14, y + 20 + big_h + 8, W - 28, 3, T->dim);
    return;
  }

  text(s, font, "On the new computer, run:", 14, y, T->dim);
  y += line_h + 4;
  char cmd[640];
  snprintf(cmd, sizeof cmd, "lgtm-hook.mjs pair %s %s", pr.code, base_url);
  y = draw_wrapped(s, mono, cmd, 14, y, W - 28, 3, T->fg) + 10;
  TTF_Font *f = font_pin ? font_pin : font_big;
  text(s, f, pr.code, (W - text_w(f, pr.code)) / 2, y, T->cmd);
  y += TTF_FontLineSkip(f) + 6;
  char exp[48];
  snprintf(exp, sizeof exp, "expires in %d:%02d  ·  waiting...", left / 60, left % 60);
  text(s, font, exp, (W - text_w(font, exp)) / 2, y, T->dim);
}

// --- profile manager ---

static void draw_profiles(SDL_Surface *s, int sel) {
  int y = panel(s, "profiles", "A re-pair  X remove  B back");
  int item_h = line_h + 8;
  // Row 0 is "add"; then each paired profile.
  for (int i = 0; i < profile_count && y + item_h < H - cap_h - 16 - line_h; i++) {
    const char *label = i == 0 ? "+ add profile" : profiles[i];
    if (i == sel) fill(s, 10, y - 3, W - 20, line_h + 6, T->select);
    text(s, font, label, 24, y, i == 0 ? T->add : i == sel ? T->fg : T->dim);
    y += item_h;
  }
  draw_toast(s);
}

static void draw_confirm(SDL_Surface *s, const char *profile) {
  char title[64];
  snprintf(title, sizeof title, "remove \"%s\"?", profile);
  int y = panel(s, title, "A remove  B cancel");
  draw_wrapped(s, font, "Its hook token stops working right away. That computer's prompts go back to its own terminal until you pair it again.",
               14, y + 8, W - 28, 4, T->fg);
}

// Saves the server URL, keeping the token line as it was.
static int save_url(const char *url) {
  char token[512] = "", line[1024];
  FILE *f = fopen("config.txt", "r");
  if (f) {
    while (fgets(line, sizeof line, f))
      if (!strncmp(line, "TOKEN=", 6)) snprintf(token, sizeof token, "%s", line + 6);
    fclose(f);
  }
  token[strcspn(token, "\r\n")] = 0;
  f = fopen("config.txt", "w");
  if (!f) return 0;
  fprintf(f, "URL=%s\nTOKEN=%s\n", url, token);
  fclose(f);
  return 1;
}

// --- main ---

static int wrap_n(int i, int d, int n) { return (i + d + n) % n; }

static int net_started;
static int net_ready(void) { return net_started; }

static void start_net(void) {
  if (net_started) return;
  pthread_t t;
  pthread_create(&t, NULL, net_loop, NULL);
  pthread_detach(t);
  net_started = 1;
}

int main(void) {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) return 1;
  SDL_ShowCursor(SDL_DISABLE);
  SDL_Surface *video = SDL_SetVideoMode(W, H, 32, SDL_HWSURFACE);
  SDL_Surface *screen = SDL_CreateRGBSurface(SDL_HWSURFACE, W, H, 32, 0, 0, 0, 0);
  TTF_Init();
  open_audio();
  load_settings();
  T = &THEMES[theme_i];
  if (!apply_font()) {
    // A saved font that won't open shouldn't brick the app.
    font_i = 0;
    size_i = 2;
    if (!apply_font()) return 1;
  }

  if (load_config()) start_net();
  else snprintf(shared.status, sizeof shared.status, "no config");

  static Snapshot snap;
  static char resp[64 * 1024];
  int seen_version = -1;
  char shown_id[40] = "", answered_id[40] = "";
  Uint32 shown_at = 0;
  int view = VIEW_CARD, scroll = 0, rows = 1;
  int mode = MODE_MAIN, menu_sel = 0, prof_sel = 0;
  int kb_for = 0;  // what the keyboard is for: M_PROFILES (new name) or M_SERVER

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = 0;
      if (e.type != SDL_KEYDOWN) continue;
      SDLKey k = e.key.keysym.sym;

      // SELECT always backs out to the request; from there it opens the menu.
      if (k == BTN_SELECT || k == BTN_MENU) {
        mode = mode == MODE_MAIN ? MODE_MENU : MODE_MAIN;
        menu_sel = 0;
        continue;
      }

      if (mode == MODE_MENU) {
        int d = k == BTN_LEFT ? -1 : (k == BTN_RIGHT || k == BTN_A) ? 1 : 0;
        if (k == BTN_UP) menu_sel = wrap_n(menu_sel, -1, MENU_COUNT);
        else if (k == BTN_DOWN) menu_sel = wrap_n(menu_sel, 1, MENU_COUNT);
        else if (k == BTN_B) mode = MODE_MAIN;
        else if (k == BTN_A && menu_sel == M_RESUME) mode = MODE_MAIN;
        else if (k == BTN_A && menu_sel == M_QUIT) running = 0;
        else if ((k == BTN_A || k == BTN_RIGHT) && menu_sel == M_PROFILES) mode = MODE_PROFILES, prof_sel = 0;
        else if ((k == BTN_A || k == BTN_RIGHT) && menu_sel == M_SERVER) {
          kb_open("server url", base_url, 1);
          kb_for = M_SERVER;
          mode = MODE_KEYBOARD;
        } else if (d && menu_sel == M_PROFILE) {
          pthread_mutex_lock(&mu);
          int cur = 0;
          for (int i = 0; i < profile_count; i++)
            if (!strcmp(profiles[i], selected)) cur = i;
          snprintf(selected, sizeof selected, "%s", profiles[wrap_n(cur, d, profile_count)]);
          // Clear the old profile's queue so it doesn't linger until the next poll.
          memset(&shared, 0, sizeof shared);
          shared_version++;
          pthread_cond_signal(&wake);
          pthread_mutex_unlock(&mu);
          save_settings();
        } else if (d && menu_sel == M_ALERT) {
          alert_i = wrap_n(alert_i, d, ALERT_COUNT);
          save_settings();
          alert();  // preview
        } else if (d && menu_sel == M_THEME) {
          theme_i = wrap_n(theme_i, d, THEME_COUNT);
          T = &THEMES[theme_i];
          wrap_dirty = 1;  // line colors come from the theme
          save_settings();
        } else if (d && (menu_sel == M_FONT || menu_sel == M_SIZE)) {
          int old_font = font_i, old_size = size_i;
          if (menu_sel == M_FONT) font_i = wrap_n(font_i, d, FONT_COUNT);
          else size_i = wrap_n(size_i, d, SIZE_COUNT);
          if (apply_font()) {
            save_settings();
            wrap_dirty = 1;
            scroll = 0;
          } else {
            font_i = old_font;
            size_i = old_size;
          }
        }
        continue;
      }

      if (mode == MODE_PROFILES) {
        if (k == BTN_UP) prof_sel = wrap_n(prof_sel, -1, profile_count);
        else if (k == BTN_DOWN) prof_sel = wrap_n(prof_sel, 1, profile_count);
        else if (k == BTN_B) mode = MODE_MENU;
        else if (k == BTN_A && prof_sel == 0) {
          kb_open("name the new profile", "", 0);
          kb_for = M_PROFILES;
          mode = MODE_KEYBOARD;
        } else if (k == BTN_A) {
          snprintf(pr.profile, sizeof pr.profile, "%s", profiles[prof_sel]);
          kb_for = -1;  // re-pair: skip naming
          mode = MODE_KEYBOARD;
          snprintf(kb.buf, sizeof kb.buf, "%s", pr.profile);
          SDL_Event done = {.type = SDL_KEYDOWN};
          done.key.keysym.sym = SDLK_RETURN;
          SDL_PushEvent(&done);
        } else if (k == BTN_X && prof_sel > 0) {
          snprintf(pr.profile, sizeof pr.profile, "%s", profiles[prof_sel]);
          mode = MODE_CONFIRM;
        }
        continue;
      }

      if (mode == MODE_CONFIRM) {
        if (k == BTN_B) mode = MODE_PROFILES;
        else if (k == BTN_A) {
          working(screen, video, "removing...");
          char args[80];
          snprintf(args, sizeof args, "-d 'profile=%s'", pr.profile);
          int code = valid_name(pr.profile) ? http(args, "/v1/device/remove", resp, sizeof resp) : 400;
          if (code == 200) {
            char msg[80];
            snprintf(msg, sizeof msg, "removed %s", pr.profile);
            say(msg);
            if (!strcmp(selected, pr.profile)) snprintf(selected, sizeof selected, "all"), save_settings();
            fetch_profiles(resp, sizeof resp);
            prof_sel = 0;
          } else say(code == 409 ? "set in keys.json; remove it there" : "couldn't remove it");
          mode = MODE_PROFILES;
        }
        continue;
      }

      if (mode == MODE_KEYBOARD) {
        if (!kb_key(k)) continue;
        if (kb_for == M_SERVER) {
          if (strncmp(kb.buf, "https://", 8) && strncmp(kb.buf, "http://", 7)) {
            say("needs http:// or https://");
            continue;
          }
          pthread_mutex_lock(&url_mu);
          snprintf(base_url, sizeof base_url, "%s", kb.buf);
          size_t n = strlen(base_url);
          while (n && base_url[n - 1] == '/') base_url[--n] = 0;
          pthread_mutex_unlock(&url_mu);
          save_url(base_url);
          if (load_config()) start_net();
          pthread_mutex_lock(&mu);
          refetch_profiles = 1;
          pthread_cond_signal(&wake);
          pthread_mutex_unlock(&mu);
          mode = MODE_MENU;
          continue;
        }
        // New or re-paired profile: ask the server for a code.
        if (!valid_name(kb.buf) || !strcmp(kb.buf, "all")) {
          say("use a-z, 0-9 and -");
          continue;
        }
        snprintf(pr.profile, sizeof pr.profile, "%.32s", kb.buf);
        working(screen, video, "getting a code...");
        char args[80];
        snprintf(args, sizeof args, "-d 'profile=%s'", pr.profile);
        if (http(args, "/v1/device/pair", resp, sizeof resp) == 200) {
          char *nl = strchr(resp, '\n');
          int secs = nl ? atoi(nl + 1) : 0;
          if (nl) *nl = 0;
          snprintf(pr.code, sizeof pr.code, "%.9s", resp);
          snprintf(pr.status, sizeof pr.status, "pending");
          pr.expires_at = SDL_GetTicks() + secs * 1000;
          pr.checked_at = SDL_GetTicks();
          if (!font_pin) font_pin = TTF_OpenFont(FONTS[font_i].file, 56);
          mode = MODE_PAIR;
        } else {
          say("couldn't reach the server");
          mode = MODE_PROFILES;
        }
        continue;
      }

      if (mode == MODE_PAIR) {
        if (k == BTN_B || k == BTN_A) {
          if (font_pin) TTF_CloseFont(font_pin), font_pin = NULL;
          fetch_profiles(resp, sizeof resp);
          mode = MODE_PROFILES;
        }
        continue;
      }

      // MODE_MAIN: the request.
      int armed = snap.count && SDL_GetTicks() - shown_at > ARM_DELAY_MS && strcmp(snap.id, answered_id);
      if (snap.count && k == BTN_Y) {
        view = view == VIEW_CARD ? VIEW_DETAIL : VIEW_CARD;
        scroll = 0;
      } else if (armed && (k == BTN_A || k == BTN_B || k == BTN_X)) {
        decide(snap.id, snap.profile, k == BTN_A ? "allow" : k == BTN_B ? "deny" : "defer");
        snprintf(answered_id, sizeof answered_id, "%s", snap.id);
      } else if (view == VIEW_DETAIL) {
        if (k == BTN_DOWN) scroll++;
        else if (k == BTN_UP) scroll--;
        else if (k == BTN_R1 || k == BTN_RIGHT) scroll += rows;
        else if (k == BTN_L1 || k == BTN_LEFT) scroll -= rows;
      }
    }

    // While a pairing code is up, check on it every couple of seconds.
    if (mode == MODE_PAIR && !strcmp(pr.status, "pending") && SDL_GetTicks() - pr.checked_at > 2000) {
      char path[96];
      snprintf(path, sizeof path, "/v1/device/pair?profile=%s&code=%s", pr.profile, pr.code);
      if (valid_name(pr.profile) && http("", path, resp, sizeof resp) == 200) {
        resp[strcspn(resp, "\n")] = 0;
        snprintf(pr.status, sizeof pr.status, "%.15s", resp);
        if (!strcmp(pr.status, "paired")) alert();
      }
      pr.checked_at = SDL_GetTicks();
    }

    pthread_mutex_lock(&mu);
    if (shared_version != seen_version) {
      snap = shared;
      seen_version = shared_version;
    }
    // A fetched chunk is only kept if it's the next one for this request.
    if (chunk_got >= 0) {
      if (!strcmp(chunk_got_id, snap.id) && chunk_got == body_chunks) body_append(chunk_buf);
      chunk_got = -1;
    }
    int loading = chunk_want >= 0;
    pthread_mutex_unlock(&mu);

    // Until the server drops an answered request, show the queue as if it has.
    int count = snap.count && !strcmp(snap.id, answered_id) ? 0 : snap.count;

    if (strcmp(snap.id, shown_id)) {
      snprintf(shown_id, sizeof shown_id, "%s", snap.id);
      shown_at = SDL_GetTicks();
      view = VIEW_CARD;
      scroll = 0;
      body_reset();
      if (snap.id[0]) body_append(snap.first);
      if (count) alert();
    }

    fill(screen, 0, 0, W, H, T->bg);
    int saved = snap.count;
    snap.count = count;
    header(screen, &snap);
    if (mode == MODE_PROFILES) draw_profiles(screen, prof_sel);
    else if (mode == MODE_KEYBOARD) draw_keyboard(screen), draw_toast(screen);
    else if (mode == MODE_PAIR) draw_pair(screen);
    else if (mode == MODE_CONFIRM) draw_confirm(screen, pr.profile);
    else if (!count) {
      view = VIEW_CARD;
      draw_idle(screen);
    } else {
      int armed = SDL_GetTicks() - shown_at > ARM_DELAY_MS && mode == MODE_MAIN;
      int top = footer(screen, view, armed, snap.host);
      if (wrap_dirty) rewrap();
      if (view == VIEW_CARD) {
        draw_card(screen, &snap, top - 4);
      } else {
        rows = (top - header_h - line_h - 20) / code_h;
        if (rows < 1) rows = 1;
        // Reading past what's loaded asks for the next chunk.
        if (scroll + rows >= nlines && body_lines < snap.total) want_chunk(snap.id, snap.profile, body_chunks);
        int max_scroll = nlines > rows ? nlines - rows : 0;
        if (body_lines < snap.total) max_scroll = nlines > 1 ? nlines - 1 : 0;
        if (scroll > max_scroll) scroll = max_scroll;
        if (scroll < 0) scroll = 0;
        draw_detail(screen, &snap, scroll, rows, loading);
      }
    }
    snap.count = saved;
    if (mode == MODE_MENU) draw_menu(screen, menu_sel);
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
    SDL_Delay(33);
  }

  Mix_CloseAudio();
  TTF_Quit();
  SDL_Quit();
  return 0;
}
