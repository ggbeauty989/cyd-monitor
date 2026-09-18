/*
 * CYD Hardware Monitor - firmware ESP32-2432S028
 *
 * Riceve righe JSON via seriale (115200) dallo script monitor.py
 * e mostra una dashboard testuale stile terminale (LVGL 9 + font unscii).
 *
 * Tre pagine, si scorrono toccando lo schermo (o col tasto BOOT):
 *   0) valori  -> numeri grandi, barre ASCII, RAM/VRAM, rete
 *   1) grafici -> storico 60 s di carico e temperatura CPU/GPU (lv_chart)
 *   2) top     -> processi più impegnativi, stile htop (PID CPU% MEM% nome)
 * Tenendo premuto per 2 s la schermata ruota di 180 gradi (scelta salvata in flash).
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

#define SCREEN_W 240  // orientamento nativo del pannello (verticale)
#define SCREEN_H 320
#define DATA_TIMEOUT_MS 3000
#define HISTORY_POINTS 60  // 1 punto al secondo -> 60 s

// Touch XPT2046 del CYD (bus SPI separato dal display)
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define BOOT_BUTTON  0
#define LONG_PRESS_MS 2000       // pressione lunga -> ruota lo schermo
#define RELEASE_DEBOUNCE_MS 80   // il touch può "sfarfallare" mentre è premuto

#define F8  (&lv_font_unscii_8)   // 8x8 monospace -> 40 colonne
#define F16 (&lv_font_unscii_16)
#define CW 8                      // larghezza carattere F8
#define LINE_CHARS 40
#define TOP_ROWS 13               // righe di processi nella pagina "top" (14 toccano la riga ==== in basso)
#define TOP_ROW_H 12              // passo verticale (8 px di font + 4 di aria)

static uint8_t draw_buf[SCREEN_W * SCREEN_H / 10 * (LV_COLOR_DEPTH / 8)];
static SPIClass touch_spi(VSPI);
static XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);

// ----------------------------- Palette "fosfori verdi" ---------------------
#define COL_BG    lv_color_hex(0x000000)
#define COL_TEXT  lv_color_hex(0x39FF6A)
#define COL_DIM   lv_color_hex(0x1C6B34)
#define COL_GRID  lv_color_hex(0x0F3A1C)
#define COL_AMBER lv_color_hex(0xFFB000)
#define COL_CYAN  lv_color_hex(0x00E5FF)
#define COL_WARN  lv_color_hex(0xFFE600)
#define COL_HOT   lv_color_hex(0xFF3B3B)

// ----------------------------- Widget --------------------------------------
struct AsciiBar {
  lv_obj_t *fill, *empty, *tail;
  int x, cells;
};

#define PAGE_COUNT 3
static lv_obj_t *pages[PAGE_COUNT];  // 0 valori, 1 grafici, 2 top
static lv_obj_t *lbl_top;            // unica label multilinea con i processi
static int current_page = 0;
static lv_display_t *disp;
static Preferences prefs;
static bool flipped = false;  // true = ruotato di 180 gradi
static char host[16] = "cyd";

static AsciiBar bar_ram, bar_vram;
static lv_obj_t *lbl_prompt, *cursor, *lbl_clock, *lbl_rx, *lbl_tx, *lbl_status, *lbl_link;
static lv_obj_t *overlay, *lbl_nocarrier;
static uint32_t last_data_ms = 0, packets = 0;
static bool have_data = false;

// ----------------------------- Helper UI -----------------------------------
static lv_obj_t *txt(lv_obj_t *p, int x, int y, const lv_font_t *f, lv_color_t c, const char *s) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, s);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t *make_page(lv_obj_t *scr) {
  lv_obj_t *p = lv_obj_create(scr);
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, lv_pct(100), lv_pct(100));
  lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  return p;
}

// Intestazione di sezione: --[NOME]------------------ (nome modificabile)
struct Section {
  lv_obj_t *name, *tail;
};

static void set_section(Section &s, const char *name) {
  if (strcmp(lv_label_get_text(s.name), name) == 0) return;  // evita ridisegni inutili
  int n = constrain((int)strlen(name), 0, LINE_CHARS - 5);
  char tail[LINE_CHARS + 1];
  int rest = LINE_CHARS - 3 - n;
  tail[0] = ']';
  memset(tail + 1, '-', rest - 1);
  tail[rest] = '\0';
  lv_label_set_text(s.name, name);
  lv_label_set_text(s.tail, tail);
  lv_obj_set_x(s.tail, (3 + n) * CW);
}

static Section section(lv_obj_t *p, int y, const char *name) {
  Section s;
  txt(p, 0, y, F8, COL_DIM, "--[");
  s.name = txt(p, 3 * CW, y, F8, COL_AMBER, "");
  s.tail = txt(p, 3 * CW, y, F8, COL_DIM, "");
  set_section(s, name);
  return s;
}

struct Block {
  Section header;
  lv_obj_t *temp, *load, *extra;
  AsciiBar bar;
};
struct History {
  Section header;
  lv_obj_t *chart, *leg_load, *leg_temp;
  lv_chart_series_t *load, *temp;
};

static Block b_cpu, b_gpu;
static History h_cpu, h_gpu;

// Barra ASCII: [#####.......] testo
static AsciiBar make_bar(lv_obj_t *p, int x, int y, int cells, lv_color_t col) {
  AsciiBar b;
  b.x = x;
  b.cells = cells;
  txt(p, x, y, F8, COL_DIM, "[");
  b.fill = txt(p, x + CW, y, F8, col, "");
  char e[LINE_CHARS + 1];
  memset(e, '.', cells);
  e[cells] = '\0';
  b.empty = txt(p, x + CW, y, F8, COL_DIM, e);
  txt(p, x + (cells + 1) * CW, y, F8, COL_DIM, "]");
  b.tail = txt(p, x + (cells + 2) * CW, y, F8, COL_TEXT, "");
  return b;
}

static void set_bar(AsciiBar &b, float pct, const char *tail) {
  int n = constrain((int)lround(pct * b.cells / 100.0f), 0, b.cells);
  char f[LINE_CHARS + 1], e[LINE_CHARS + 1];
  memset(f, '#', n);
  f[n] = '\0';
  memset(e, '.', b.cells - n);
  e[b.cells - n] = '\0';
  lv_label_set_text(b.fill, f);
  lv_label_set_text(b.empty, e);
  lv_obj_set_x(b.empty, b.x + (n + 1) * CW);
  lv_label_set_text(b.tail, tail);
}

static Block make_block(lv_obj_t *p, int y, const char *name, const char *extra_tag) {
  Block b;
  b.header = section(p, y, name);
  txt(p, 0, y + 11, F8, COL_DIM, "temp/C");
  txt(p, 112, y + 11, F8, COL_DIM, "load/%");
  txt(p, 224, y + 11, F8, COL_DIM, extra_tag);
  b.temp = txt(p, 0, y + 21, F16, COL_DIM, "--.-");
  b.load = txt(p, 112, y + 21, F16, COL_TEXT, "--.-");
  b.extra = txt(p, 224, y + 21, F16, COL_CYAN, "--");
  b.bar = make_bar(p, 0, y + 40, 33, COL_TEXT);
  return b;
}

// Etichetta di legenda con sfondo nero, "incisa" sopra la riga di trattini
static lv_obj_t *legend(lv_obj_t *p, int x, int y, lv_color_t c, const char *s) {
  lv_obj_t *l = txt(p, x, y, F8, c, s);
  lv_obj_set_style_bg_color(l, COL_BG, 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(l, 4, 0);
  lv_obj_set_style_pad_right(l, 2, 0);
  return l;
}

// Grafico storico (60 s): --[CPU]------------ L  12% T  54C
static History make_history(lv_obj_t *p, int y, const char *name) {
  History h;
  h.header = section(p, y, name);
  h.leg_load = legend(p, 192, y, COL_TEXT, "L  --%");
  h.leg_temp = legend(p, 252, y, COL_AMBER, "T  --C");

  lv_obj_t *ch = lv_chart_create(p);
  h.chart = ch;
  lv_obj_set_size(ch, 320, 76);
  lv_obj_set_pos(ch, 0, y + 10);
  lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(ch, HISTORY_POINTS);
  lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(ch, 5, 7);  // griglia a 25% e ogni 10 s

  // Sfondo nero, cornice sottile, griglia tratteggiata stile oscilloscopio
  lv_obj_set_style_bg_color(ch, COL_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(ch, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(ch, COL_DIM, LV_PART_MAIN);
  lv_obj_set_style_border_width(ch, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ch, 1, LV_PART_MAIN);
  lv_obj_set_style_line_color(ch, COL_GRID, LV_PART_MAIN);
  lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);
  lv_obj_set_style_line_dash_width(ch, 2, LV_PART_MAIN);
  lv_obj_set_style_line_dash_gap(ch, 2, LV_PART_MAIN);
  // Linee dati sottili, nessun pallino sui punti
  lv_obj_set_style_line_width(ch, 1, LV_PART_ITEMS);
  lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);

  h.temp = lv_chart_add_series(ch, COL_AMBER, LV_CHART_AXIS_PRIMARY_Y);
  h.load = lv_chart_add_series(ch, COL_TEXT, LV_CHART_AXIS_PRIMARY_Y);  // disegnata sopra
  lv_chart_set_all_values(ch, h.temp, LV_CHART_POINT_NONE);
  lv_chart_set_all_values(ch, h.load, LV_CHART_POINT_NONE);
  return h;
}

// ----------------------------- Boot log ------------------------------------
static lv_obj_t *boot_scr, *boot_lbl;

static void boot_timer_cb(lv_timer_t *t) {
  static char lines[9][LINE_CHARS + 1];
  static char buf[440] = "";
  static int idx = 0;
  if (idx == 0) {
    snprintf(lines[0], sizeof(lines[0]), "CYD-BIOS v1.1 (c) 2026");
    snprintf(lines[1], sizeof(lines[1]), "cpu0: xtensa lx6 2-core    [ OK ]");
    snprintf(lines[2], sizeof(lines[2]), "heap: %4u KB free          [ OK ]", (unsigned)(ESP.getFreeHeap() / 1024));
    snprintf(lines[3], sizeof(lines[3]), "lcd0: ili9341 320x240 spi  [ OK ]");
    snprintf(lines[4], sizeof(lines[4]), "tsc0: xpt2046 touch        [ OK ]");
    snprintf(lines[5], sizeof(lines[5]), "gfx0: lvgl %d.%d.%d            [ OK ]",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    snprintf(lines[6], sizeof(lines[6]), "tty0: uart 115200 8N1      [ OK ]");
    snprintf(lines[7], sizeof(lines[7]), "hint: tap=page  hold 2s=rotate");
    snprintf(lines[8], sizeof(lines[8]), "exec /bin/sysmon ...");
  }
  if (idx < 9) {
    strncat(buf, lines[idx], sizeof(buf) - strlen(buf) - 2);
    strcat(buf, "\n");
    lv_label_set_text(boot_lbl, buf);
  } else if (idx > 14) {
    lv_obj_delete(boot_scr);
    lv_timer_delete(t);
    return;
  }
  idx++;
}

// ----------------------------- Costruzione UI ------------------------------
static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // --- Parti comuni: prompt, orologio, riga di stato ---
  lbl_prompt = txt(scr, 0, 2, F8, COL_TEXT, "root@cyd:~$ ./sysmon");
  cursor = lv_obj_create(scr);
  lv_obj_remove_style_all(cursor);
  lv_obj_set_size(cursor, 7, 8);
  lv_obj_set_style_bg_color(cursor, COL_TEXT, 0);
  lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
  lv_obj_set_pos(cursor, 20 * CW + 2, 2);
  lbl_clock = txt(scr, 0, 2, F8, COL_AMBER, "--:--");
  lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, 0, 2);
  txt(scr, 0, 12, F8, COL_DIM, "========================================");
  txt(scr, 0, 210, F8, COL_DIM, "========================================");
  lbl_status = txt(scr, 0, 222, F8, COL_DIM, "up --  procs --  pkt 0");
  lbl_link = txt(scr, 0, 222, F8, COL_HOT, "[DOWN]");
  lv_obj_align(lbl_link, LV_ALIGN_TOP_RIGHT, 0, 222);

  // --- Pagina 0: valori ---
  lv_obj_t *page_values = pages[0] = make_page(scr);
  b_cpu = make_block(page_values, 22, "CPU", "freq/GHz");
  b_gpu = make_block(page_values, 76, "GPU", "power/W");

  section(page_values, 130, "MEM");
  txt(page_values, 0, 141, F8, COL_DIM, "ram");
  bar_ram = make_bar(page_values, 40, 141, 18, COL_TEXT);
  txt(page_values, 0, 152, F8, COL_DIM, "vram");
  bar_vram = make_bar(page_values, 40, 152, 18, COL_CYAN);

  section(page_values, 166, "NET");
  txt(page_values, 0, 177, F8, COL_DIM, "rx/down");
  txt(page_values, 160, 177, F8, COL_DIM, "tx/up");
  lbl_rx = txt(page_values, 0, 187, F16, COL_TEXT, "--");
  lbl_tx = txt(page_values, 160, 187, F16, COL_CYAN, "--");

  // --- Pagina 1: grafici storici ---
  lv_obj_t *page_graph = pages[1] = make_page(scr);
  h_cpu = make_history(page_graph, 22, "CPU");
  h_gpu = make_history(page_graph, 116, "GPU");
  lv_obj_add_flag(page_graph, LV_OBJ_FLAG_HIDDEN);

  // --- Pagina 2: top processi (una sola label multilinea: 14 righe da 40 col) ---
  lv_obj_t *page_top = pages[2] = make_page(scr);
  section(page_top, 22, "TOP");
  txt(page_top, 0, 33, F8, COL_DIM, "  PID  CPU%  MEM%  COMMAND");
  lbl_top = txt(page_top, 0, 44, F8, COL_TEXT, "n/a");
  lv_obj_set_style_text_line_space(lbl_top, TOP_ROW_H - 8, 0);
  lv_obj_add_flag(page_top, LV_OBJ_FLAG_HIDDEN);

  // --- "NO CARRIER" quando il PC non trasmette ---
  overlay = lv_obj_create(scr);
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, 256, 64);
  lv_obj_center(overlay);
  lv_obj_set_style_bg_color(overlay, COL_BG, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(overlay, COL_HOT, 0);
  lv_obj_set_style_border_width(overlay, 1, 0);
  lbl_nocarrier = txt(overlay, 0, 0, F16, COL_HOT, "NO CARRIER");
  lv_obj_align(lbl_nocarrier, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_t *sub = txt(overlay, 0, 0, F8, COL_DIM, "awaiting host @ uart0:115200");
  lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -12);

  // --- Boot log (sopra a tutto, si autodistrugge) ---
  boot_scr = lv_obj_create(scr);
  lv_obj_remove_style_all(boot_scr);
  lv_obj_set_size(boot_scr, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(boot_scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(boot_scr, LV_OPA_COVER, 0);
  boot_lbl = txt(boot_scr, 0, 4, F8, COL_TEXT, "");
  lv_obj_set_style_text_line_space(boot_lbl, 3, 0);
  lv_timer_create(boot_timer_cb, 140, NULL);
}

// ----------------------------- Pagine --------------------------------------
static void update_prompt() {
  char buf[48];
  static const char *flags[PAGE_COUNT] = {"", " -g", " -t"};
  snprintf(buf, sizeof(buf), "root@%.12s:~$ ./sysmon%s", host, flags[current_page]);
  lv_label_set_text(lbl_prompt, buf);
  lv_obj_set_x(cursor, strlen(buf) * CW + 2);
}

static void show_page(int page) {
  current_page = page;
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (i == page) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
  }
  update_prompt();
}

// Orizzontale 320x240: 270 = orientamento standard, 90 = capovolto
static void apply_rotation() {
  lv_display_set_rotation(disp, flipped ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_270);
  lv_obj_invalidate(lv_screen_active());
}

static void flip_screen() {
  flipped = !flipped;
  prefs.putBool("flip", flipped);
  apply_rotation();
}

// Tocco o tasto BOOT:
//   breve (al rilascio)           -> pagina successiva (0 -> 1 -> 2 -> 0)
//   lungo (tenuto per 2 secondi)  -> ruota la schermata di 180 gradi
static void poll_input() {
  static bool held = false, long_done = false;
  static uint32_t press_start = 0, last_seen = 0;
  uint32_t now = millis();
  bool raw = (touch.tirqTouched() && touch.touched()) || digitalRead(BOOT_BUTTON) == LOW;

  if (raw) {
    last_seen = now;
    if (!held) {
      held = true;
      long_done = false;
      press_start = now;
    } else if (!long_done && now - press_start >= LONG_PRESS_MS) {
      long_done = true;
      flip_screen();
    }
  } else if (held && now - last_seen > RELEASE_DEBOUNCE_MS) {
    held = false;
    if (!long_done) show_page((current_page + 1) % PAGE_COUNT);
  }
}

// ----------------------------- Aggiornamento dati --------------------------
static bool getf(JsonDocument &doc, const char *k, float &out) {
  if (doc[k].isNull()) return false;
  out = doc[k].as<float>();
  return true;
}

static lv_color_t temp_color(float t) {
  if (t < 60) return COL_TEXT;
  if (t < 80) return COL_WARN;
  return COL_HOT;
}

static void fmt_rate(char *out, size_t n, float bps) {
  const char *units[] = {"B/s", "K/s", "M/s", "G/s"};
  int i = 0;
  while (bps >= 1024 && i < 3) { bps /= 1024; i++; }
  snprintf(out, n, i == 0 ? "%.0f%s" : "%.1f%s", bps, units[i]);
}

static void update_block(Block &b, JsonDocument &doc, const char *kt, const char *ku) {
  char buf[16];
  float v;
  if (getf(doc, kt, v)) {
    snprintf(buf, sizeof(buf), "%.1f", v);
    lv_label_set_text(b.temp, buf);
    lv_obj_set_style_text_color(b.temp, temp_color(v), 0);
  } else {
    lv_label_set_text(b.temp, "--.-");
    lv_obj_set_style_text_color(b.temp, COL_DIM, 0);
  }
  if (getf(doc, ku, v)) {
    snprintf(buf, sizeof(buf), "%.1f", v);
    lv_label_set_text(b.load, buf);
    snprintf(buf, sizeof(buf), "%3d%%", (int)lround(v));
    set_bar(b.bar, v, buf);
  } else {
    lv_label_set_text(b.load, "--.-");
    set_bar(b.bar, 0, " --%");
  }
}

// Aggiunge un punto al grafico (scorre a sinistra) e aggiorna la legenda
static void push_history(History &h, JsonDocument &doc, const char *kt, const char *ku) {
  char buf[12];
  float v;
  if (getf(doc, ku, v)) {
    int u = constrain((int)lround(v), 0, 100);
    lv_chart_set_next_value(h.chart, h.load, u);
    snprintf(buf, sizeof(buf), "L%4d%%", u);
  } else {
    lv_chart_set_next_value(h.chart, h.load, LV_CHART_POINT_NONE);
    strcpy(buf, "L  --%");
  }
  lv_label_set_text(h.leg_load, buf);

  if (getf(doc, kt, v)) {
    int t = (int)lround(v);
    lv_chart_set_next_value(h.chart, h.temp, constrain(t, 0, 100));
    snprintf(buf, sizeof(buf), "T%4dC", t);
    lv_obj_set_style_text_color(h.leg_temp, t < 60 ? COL_AMBER : temp_color(v), 0);
  } else {
    lv_chart_set_next_value(h.chart, h.temp, LV_CHART_POINT_NONE);
    strcpy(buf, "T  --C");
  }
  lv_label_set_text(h.leg_temp, buf);
}

static void update_mem(AsciiBar &bar, JsonDocument &doc, const char *kused, const char *ktot) {
  char buf[24];
  float used, tot;
  if (!getf(doc, kused, used) || !getf(doc, ktot, tot) || tot <= 0) {
    set_bar(bar, 0, "n/a");
    return;
  }
  float pct = used * 100.0f / tot;
  snprintf(buf, sizeof(buf), "%.1f/%.0fG %3d%%", used, tot, (int)lround(pct));
  set_bar(bar, pct, buf);
}

// Pagina top: "top": [[pid, cpu%, mem%, "nome"], ...] -> una riga per processo
static void update_top(JsonDocument &doc) {
  JsonArray top = doc["top"].as<JsonArray>();
  if (top.isNull()) {
    lv_label_set_text(lbl_top, "n/a");
    return;
  }
  static char buf[TOP_ROWS * (LINE_CHARS + 1) + 1];
  size_t len = 0;
  int rows = 0;
  for (JsonArray r : top) {
    if (rows >= TOP_ROWS || r.size() < 4) break;
    len += snprintf(buf + len, sizeof(buf) - len, "%s%5d %5.1f %5.1f %-20.20s", rows ? "\n" : "",
                    (int)r[0].as<long>(), r[1].as<float>(), r[2].as<float>(), r[3] | "?");
    rows++;
  }
  lv_label_set_text(lbl_top, rows ? buf : "n/a");
}

// "CPU" + nome (max 16 caratteri) sulla pagina valori e su quella dei grafici
static void set_titles(Section &values, Section &graph, const char *prefix, const char *name) {
  char title[24];
  if (name[0]) snprintf(title, sizeof(title), "%s %.16s", prefix, name);
  else strlcpy(title, prefix, sizeof(title));
  set_section(values, title);
  set_section(graph, title);
}

static void apply_data(JsonDocument &doc) {
  char buf[64];
  float v;

  strlcpy(host, doc["host"] | "cyd", sizeof(host));
  update_prompt();
  if (doc["time"].is<const char *>()) lv_label_set_text(lbl_clock, doc["time"].as<const char *>());

  update_block(b_cpu, doc, "cpu_t", "cpu_u");
  update_block(b_gpu, doc, "gpu_t", "gpu_u");
  push_history(h_cpu, doc, "cpu_t", "cpu_u");
  push_history(h_gpu, doc, "gpu_t", "gpu_u");

  // Nomi nelle intestazioni: --[CPU Ryzen 7 9800X3D]--  --[GPU RX 9070 XT]--
  set_titles(b_cpu.header, h_cpu.header, "CPU", doc["cpu_name"] | "");
  set_titles(b_gpu.header, h_gpu.header, "GPU", doc["gpu_name"] | "");

  if (getf(doc, "cpu_f", v)) snprintf(buf, sizeof(buf), "%.2f", v / 1000.0f);
  else strcpy(buf, "--");
  lv_label_set_text(b_cpu.extra, buf);
  if (getf(doc, "gpu_p", v)) snprintf(buf, sizeof(buf), "%.0f", v);
  else strcpy(buf, "--");
  lv_label_set_text(b_gpu.extra, buf);

  update_mem(bar_ram, doc, "ram_used", "ram_tot");
  update_mem(bar_vram, doc, "vram_used", "vram_tot");
  update_top(doc);

  fmt_rate(buf, sizeof(buf), doc["net_dn"] | 0.0f);
  lv_label_set_text(lbl_rx, buf);
  fmt_rate(buf, sizeof(buf), doc["net_up"] | 0.0f);
  lv_label_set_text(lbl_tx, buf);

  packets++;
  uint32_t up = doc["up"] | 0;
  snprintf(buf, sizeof(buf), "up %ud%02uh%02um procs %u pkt %05u",
           (unsigned)(up / 86400), (unsigned)(up % 86400 / 3600), (unsigned)(up % 3600 / 60),
           (unsigned)(doc["procs"] | 0), (unsigned)(packets % 100000));
  lv_label_set_text(lbl_status, buf);
}

static void read_serial() {
  static char line[1536];  // ~800 byte con la lista "top", margine per nomi lunghi
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      line[len] = '\0';
      JsonDocument doc;
      if (len > 0 && deserializeJson(doc, line, len) == DeserializationError::Ok) {
        apply_data(doc);
        last_data_ms = millis();
        have_data = true;
      }
      len = 0;
    } else if (c != '\r') {
      if (len < sizeof(line) - 1) line[len++] = c;
      else len = 0;  // riga troppo lunga: scarta
    }
  }
}

static bool online() { return have_data && (millis() - last_data_ms) < DATA_TIMEOUT_MS; }

static void blink_timer_cb(lv_timer_t *) {
  static bool on = true;
  on = !on;
  if (on) lv_obj_remove_flag(cursor, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_opa(lbl_nocarrier, on ? LV_OPA_COVER : LV_OPA_20, 0);
}

static void update_connection_state() {
  static int last = -1;
  int now = online();
  if (now == last) return;
  last = now;
  lv_label_set_text(lbl_link, now ? "[UP]" : "[DOWN]");
  lv_obj_set_style_text_color(lbl_link, now ? COL_TEXT : COL_HOT, 0);
  if (now) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

// ----------------------------- Arduino -------------------------------------
void setup() {
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);

  touch_spi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touch_spi);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  prefs.begin("cyd", false);
  flipped = prefs.getBool("flip", false);

  disp = lv_tft_espi_create(SCREEN_W, SCREEN_H, draw_buf, sizeof(draw_buf));
  apply_rotation();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  build_ui();
  lv_timer_create(blink_timer_cb, 500, NULL);
}

void loop() {
  read_serial();
  poll_input();
  update_connection_state();
  lv_timer_handler();
  delay(5);
}
