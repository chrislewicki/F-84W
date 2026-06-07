#include <pebble.h>

// ---------------------------------------------------------------------------
// F-84W - a watchface that emulates the look of the Casio F-84W.
//
// The LCD numerals are drawn as vector seven-segment shapes and the day-of-week
// as vector fourteen-segment letters, so everything scales natively to every
// rectangular Pebble (144x168 and emery's 200x228). On colour models the unlit
// "ghost" segments are drawn faintly, like a real LCD; B&W models drop them.
//
// The bezel text (CASIO, F-84W, WATER RESIST, ...) is printed text on the real
// watch, so it uses the system font. The side button labels follow the
// "91 Dub" Pebble adaptation: LIGHT / PREV / NEXT pointing at the buttons.
// ---------------------------------------------------------------------------

static Window *s_window;
static Layer *s_layer;

// The static bezel is rendered once into this bitmap and then blitted each
// tick; only the LCD contents are redrawn. s_lbl are temporary, used only
// while baking the bezel (the rotated side labels).
static GBitmap *s_bezel = NULL;
static GBitmap *s_lbl[3] = { NULL, NULL, NULL };
static const char *kBtnLabels[3] = { "LIGHT", "PREV", "NEXT" };

// ---- user settings (configured from the Clay page) ------------------------
typedef struct {
  bool show_seconds;
  bool hourly_vibe;
  bool hourly_chime;
  bool alarm_enabled;
  bool alarm_vibe;
  bool alarm_chime;
  uint8_t alarm_hour;
  uint8_t alarm_min;
  uint8_t chime_volume;   // 0-100, speaker models (keep last for upgrade safety)
} Settings;

static Settings s_cfg = {
  .show_seconds  = true,
  .hourly_vibe   = false,
  .hourly_chime  = false,
  .alarm_enabled = false,
  .alarm_vibe    = true,
  .alarm_chime   = true,
  .alarm_hour    = 7,
  .alarm_min     = 0,
  .chime_volume  = 50,
};

#define SETTINGS_PERSIST_KEY 1

// ---- seven-segment numerals ----------------------------------------------
//  bit0=a(top) b=top-right c=bottom-right d=bottom e=bottom-left f=top-left g=mid
#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40

static const uint8_t kSeg[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static const char *kDays[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

// Horizontal segment: a flattened hexagon `len` wide and `t` thick.
static void fill_hseg(GContext *ctx, int x, int y, int len, int t) {
  int h = t / 2;
  GPoint pts[6] = {
    {x + h, y}, {x + len - h, y}, {x + len, y + h},
    {x + len - h, y + t}, {x + h, y + t}, {x, y + h}
  };
  GPathInfo gi = { 6, pts };
  GPath *p = gpath_create(&gi);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// Trapezoid horizontal bar across width `w`, thickness `t`: the top edge is
// inset `ti` per side, the bottom edge `bi`. ti<bi gives a top-heavy "\==/"
// bar; ti>bi gives a bottom-heavy "/==\" bar.
static void fill_hbar(GContext *ctx, int x, int y, int w, int t, int ti, int bi) {
  GPoint pts[4] = {
    {x + ti, y}, {x + w - ti, y},
    {x + w - bi, y + t}, {x + bi, y + t}
  };
  GPathInfo gi = { 4, pts };
  GPath *p = gpath_create(&gi);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// Vertical segment `len` tall, `t` thick. Each end is either pointed (lean 0)
// or bevelled so one corner reaches the far end: lean>0 -> the right corner is
// the long one, lean<0 -> the left corner. Lets the inner corner of a segment
// "angle in" toward the middle bar.
static void fill_vseg2(GContext *ctx, int x, int y, int len, int t,
                       int top_lean, int bot_lean) {
  int h = t / 2;
  GPoint pts[6];
  int n = 0;
  if (top_lean == 0) {                       // pointed top
    pts[n++] = GPoint(x, y + h);
    pts[n++] = GPoint(x + h, y);
    pts[n++] = GPoint(x + t, y + h);
  } else if (top_lean > 0) {                 // right corner up
    pts[n++] = GPoint(x, y + t);
    pts[n++] = GPoint(x + t, y);
  } else {                                   // left corner up
    pts[n++] = GPoint(x, y);
    pts[n++] = GPoint(x + t, y + t);
  }
  if (bot_lean == 0) {                       // pointed bottom
    pts[n++] = GPoint(x + t, y + len - h);
    pts[n++] = GPoint(x + h, y + len);
    pts[n++] = GPoint(x, y + len - h);
  } else if (bot_lean > 0) {                 // right corner down
    pts[n++] = GPoint(x + t, y + len);
    pts[n++] = GPoint(x, y + len - t);
  } else {                                   // left corner down
    pts[n++] = GPoint(x + t, y + len - t);
    pts[n++] = GPoint(x, y + len);
  }
  GPathInfo gi = { (uint32_t)n, pts };
  GPath *p = gpath_create(&gi);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// Draw one seven-segment digit. digit 0-9 lit; anything else = blank (all off).
// When `ghosts` is set, every unlit segment is drawn in `ghost` so the cell
// shows a faint "8", just like a real liquid-crystal display.
// `t` is the horizontal-segment thickness; `vt` the vertical-segment thickness
// (so the verticals can be made a touch heavier than the horizontals).
static void draw_digit(GContext *ctx, int x, int y, int w, int h, int t, int vt,
                       int digit, GColor on, GColor ghost, bool ghosts) {
  uint8_t m = (digit >= 0 && digit <= 9) ? kSeg[digit] : 0x00;
  int gp = (t >= 12) ? 2 : 1;            // thin gap between adjacent segments
  // Real-Casio segment shapes (applied to every 7-seg numeral, big or small):
  // the top bar is wider along its top edge (\==/), the bottom bar wider along
  // its bottom (/==\), and the middle bar a symmetric hexagon (<==>). Each
  // vertical's outer edge runs full length while both ends bevel inward, so the
  // tops reach into 'a'/'d' and the inner ends reach into 'g' -- the wide/angled
  // edges jut in while the narrow edges hold the 1px gap.
  int ti = vt + gp - t; if (ti < gp) ti = gp;   // wide-edge inset (jut)
  int bi = vt + gp;                             // narrow-edge inset (gap)
  int midy = y + (h - t) / 2;
  int ext = (t + gp) / 2;                       // how far verticals grow into the bars
  int uv_y = y;                          // upper verticals: from the very top...
  int uv_len = (midy - gp + ext) - uv_y; if (uv_len < 1) uv_len = 1;  // ...into 'g'
  int lv_y = midy + t + gp - ext;        // lower verticals: from 'g'...
  int lv_len = (y + h) - lv_y; if (lv_len < 1) lv_len = 1;            // ...to the bottom
  int gw = w - 2 * ti; if (gw < 1) gw = 1;
  #define SEGF(bit, call) do { \
      if (m & (bit)) { graphics_context_set_fill_color(ctx, on); call; } \
      else if (ghosts) { graphics_context_set_fill_color(ctx, ghost); call; } \
    } while (0)
  SEGF(SEG_A, fill_hbar(ctx, x, y, w, t, ti, bi));            // top-heavy bar
  SEGF(SEG_G, fill_hseg(ctx, x + ti, midy, gw, t));           // symmetric hexagon
  SEGF(SEG_D, fill_hbar(ctx, x, y + h - t, w, t, bi, ti));    // bottom-heavy bar
  // Both ends bevel toward the centre line (outer = left for the left pair,
  // right for the right pair), so each vertical leans into the bar it meets.
  SEGF(SEG_F, fill_vseg2(ctx, x, uv_y, uv_len, vt, -1, -1));
  SEGF(SEG_B, fill_vseg2(ctx, x + w - vt, uv_y, uv_len, vt, 1, 1));
  SEGF(SEG_E, fill_vseg2(ctx, x, lv_y, lv_len, vt, -1, -1));
  SEGF(SEG_C, fill_vseg2(ctx, x + w - vt, lv_y, lv_len, vt, 1, 1));
  #undef SEGF
}

static void draw_2digit(GContext *ctx, int x, int y, int w, int h, int t, int vt,
                        int gap, int value, bool blank_lead,
                        GColor on, GColor ghost, bool ghosts) {
  int d0 = (value / 10) % 10;
  int d1 = value % 10;
  draw_digit(ctx, x, y, w, h, t, vt, (blank_lead && d0 == 0) ? -1 : d0, on, ghost, ghosts);
  draw_digit(ctx, x + w + gap, y, w, h, t, vt, d1, on, ghost, ghosts);
}

// ---- fourteen-segment letters --------------------------------------------
// Segment order: a b c d e f g1 g2 h i j k l m  (outer, two middles, 4 diags,
// 2 centre verticals). Drawn as thick strokes - crisp enough for short words.
enum { LA,LB,LC,LD,LE,LF,LG1,LG2,LH,LI,LJ,LK,LL,LM };

#define B(s) (1 << (s))
static uint16_t letter_mask(char ch) {
  switch (ch) {
    case 'A': return B(LA)|B(LB)|B(LC)|B(LE)|B(LF)|B(LG1)|B(LG2);
    case 'E': return B(LA)|B(LD)|B(LE)|B(LF)|B(LG1)|B(LG2);
    case 'F': return B(LA)|B(LE)|B(LF)|B(LG1)|B(LG2);
    case 'H': return B(LB)|B(LC)|B(LE)|B(LF)|B(LG1)|B(LG2);
    case 'M': return B(LB)|B(LC)|B(LE)|B(LF)|B(LH)|B(LJ);
    case 'O': return B(LA)|B(LB)|B(LC)|B(LD)|B(LE)|B(LF);
    case 'P': return B(LA)|B(LB)|B(LE)|B(LF)|B(LG1)|B(LG2);
    case 'R': return B(LA)|B(LB)|B(LE)|B(LF)|B(LG1)|B(LG2)|B(LK);
    case 'S': return B(LA)|B(LC)|B(LD)|B(LF)|B(LG1)|B(LG2);
    case 'T': return B(LA)|B(LI)|B(LL);
    case 'U': return B(LB)|B(LC)|B(LD)|B(LE)|B(LF);
    case 'W': return B(LB)|B(LC)|B(LE)|B(LF)|B(LK)|B(LM);
    default:  return 0;
  }
}
#undef B

// Draw a line shortened by `g` at each end (so neighbouring 14-seg strokes
// don't touch -- gives the small inter-segment gaps of a real LCD).
static void seg_line(GContext *ctx, GPoint a, GPoint b, int g) {
  int sx = (b.x > a.x) - (b.x < a.x);
  int sy = (b.y > a.y) - (b.y < a.y);
  graphics_draw_line(ctx, GPoint(a.x + sx * g, a.y + sy * g),
                          GPoint(b.x - sx * g, b.y - sy * g));
}

static void draw_letter(GContext *ctx, int x, int y, int w, int h, int t,
                        char ch, GColor on, GColor ghost, bool ghosts) {
  uint16_t m = letter_mask(ch);
  int x2 = x + w / 2, xr = x + w;
  int y2 = y + h / 2, yb = y + h;
  GPoint TL = {x, y},  TC = {x2, y},  TR = {xr, y};
  GPoint ML = {x, y2}, CC = {x2, y2}, MR = {xr, y2};
  GPoint BL = {x, yb}, BC = {x2, yb}, BR = {xr, yb};
  GPoint A[14] = { TL, TR, MR, BL, ML, TL, ML, CC, TL, TC, TR, BR, BC, BL };
  GPoint B[14] = { TR, MR, BR, BR, BL, ML, CC, MR, CC, CC, CC, CC, CC, CC };
  graphics_context_set_stroke_width(ctx, t);
  for (int i = 0; i < 14; i++) {
    bool lit = m & (1 << i);
    if (!lit && !ghosts) continue;
    graphics_context_set_stroke_color(ctx, lit ? on : ghost);
    seg_line(ctx, A[i], B[i], 1);
  }
  graphics_context_set_stroke_width(ctx, 1);
}

static int draw_word(GContext *ctx, int x, int y, int w, int h, int t,
                     int gap, const char *s, GColor on, GColor ghost, bool ghosts) {
  for (int i = 0; s[i]; i++) {
    draw_letter(ctx, x + i * (w + gap), y, w, h, t, s[i], on, ghost, ghosts);
  }
  return x;
}

// ---- misc helpers ---------------------------------------------------------

static void draw_text(GContext *ctx, const char *s, GFont f, GColor col,
                      int x, int y, int w, int h, GTextAlignment al) {
  graphics_context_set_text_color(ctx, col);
  graphics_draw_text(ctx, s, f, GRect(x, y, w, h),
                     GTextOverflowModeFill, al, NULL);
}


// ---- LCD status icons -----------------------------------------------------

// A "D" half-disc: flat left edge, bulging to the right.
static void fill_halfdisc(GContext *ctx, int x, int cy, int r) {
  GPoint pts[5] = {
    {x, cy - r}, {x + r * 7 / 10, cy - r * 7 / 10}, {x + r, cy},
    {x + r * 7 / 10, cy + r * 7 / 10}, {x, cy + r}
  };
  GPathInfo gi = { 5, pts };
  GPath *p = gpath_create(&gi);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// The hourly-signal icon: a solid half-disc emitter followed by four literal
// ")" parentheses -> D))))  exactly as printed on the real F-84W.
static void draw_chime(GContext *ctx, int x, int y, int s, GColor col) {
  int cy = y + s / 2;
  int rd = s * 48 / 100; if (rd < 3) rd = 3;
  graphics_context_set_fill_color(ctx, col);
  fill_halfdisc(ctx, x, cy, rd);
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, s >= 14 ? 2 : 1);
  // Each ")" is an identical right-bulging arc (same radius), translated along
  // x -- so they read as four discrete parentheses, not concentric rings.
  // Pebble angles run clockwise from the top; 55..125 deg gives a gentle ")".
  int r_p = rd * 7 / 5;                 // paren radius (a touch shorter than the disc)
  int sp = s * 19 / 100; if (sp < 4) sp = 4;
  int lx = x + rd + 1;                  // left edge of the first ")", close to the disc
  for (int i = 0; i < 4; i++) {
    int cx = lx + i * sp - (r_p * 819 / 1000);
    graphics_draw_arc(ctx, GRect(cx - r_p, cy - r_p, 2 * r_p, 2 * r_p),
                      GOvalScaleModeFitCircle,
                      DEG_TO_TRIGANGLE(55), DEG_TO_TRIGANGLE(125));
  }
}

// The alarm icon: a small bell (dome + flared body + clapper).
static void draw_bell(GContext *ctx, int x, int y, int s, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = x + s / 2;
  int rd = s * 26 / 100; if (rd < 2) rd = 2;
  int top = y + s * 12 / 100;
  graphics_fill_circle(ctx, GPoint(cx, top + rd), rd);            // dome
  // flared body: trapezoid from under the dome out to the base
  GPoint body[4] = {
    {cx - rd, top + rd}, {cx + rd, top + rd},
    {x + s * 88 / 100, y + s * 74 / 100}, {x + s * 12 / 100, y + s * 74 / 100}
  };
  GPathInfo bgi = { 4, body };
  GPath *bp = gpath_create(&bgi);
  gpath_draw_filled(ctx, bp);
  gpath_destroy(bp);
  graphics_fill_rect(ctx, GRect(cx - 1, y + s * 78 / 100, 2, s * 16 / 100), 0, GCornerNone); // clapper
}

// The bezel "lithium" mark: a central O with concentric arcs -> ((((O)))).
static void draw_lithium_icon(GContext *ctx, int cx, int cy, int r_max, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_fill_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_fill_circle(ctx, GPoint(cx, cy), r_max * 22 / 100);   // central O
  for (int i = 2; i <= 4; i++) {
    int r = r_max * i / 4;
    GRect rr = GRect(cx - r, cy - r, 2 * r, 2 * r);
    graphics_draw_arc(ctx, rr, GOvalScaleModeFitCircle,
                      DEG_TO_TRIGANGLE(52), DEG_TO_TRIGANGLE(128));   // right  )
    graphics_draw_arc(ctx, rr, GOvalScaleModeFitCircle,
                      DEG_TO_TRIGANGLE(232), DEG_TO_TRIGANGLE(308));  // left   (
  }
}

// Render a short label to a framebuffer scratch area and copy it out to a
// standalone bitmap, so it can be drawn rotated 90deg on the case side.
static GBitmap *make_label_bitmap(GContext *ctx, const char *s, GFont font,
                                  GColor fg, GColor bg, GRect scratch, bool block_after) {
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, scratch, 0, GCornerNone);
  // Lay out the text and a solid white "button" block, centred in the scratch.
  GSize ts = graphics_text_layout_get_content_size(s, font, scratch,
               GTextOverflowModeFill, GTextAlignmentLeft);
  int blk = scratch.size.h * 45 / 100;   // small button block
  int sep = 4;
  int total = ts.w + sep + blk;
  int x0 = scratch.origin.x + (scratch.size.w - total) / 2;
  int cy = scratch.origin.y + scratch.size.h / 2;
  int tx = block_after ? x0 : (x0 + blk + sep);
  int bx = block_after ? (x0 + ts.w + sep) : x0;
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, s, font, GRect(tx, scratch.origin.y, ts.w + 4, scratch.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(bx, cy - blk / 2, blk, blk), 0, GCornerNone);
  GBitmap *out = NULL;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (fb) {
    out = gbitmap_create_blank(scratch.size, GBitmapFormat8Bit);
    if (out) {
      uint16_t fbs = gbitmap_get_bytes_per_row(fb);
      uint16_t os  = gbitmap_get_bytes_per_row(out);
      uint8_t *fd  = gbitmap_get_data(fb);
      uint8_t *od  = gbitmap_get_data(out);
      for (int r = 0; r < scratch.size.h; r++) {
        memcpy(od + r * os,
               fd + (scratch.origin.y + r) * fbs + scratch.origin.x,
               scratch.size.w);
      }
    }
    graphics_release_frame_buffer(ctx, fb);
  }
  return out;
}

static void draw_rot_label(GContext *ctx, GBitmap *bmp, int deg, GPoint dest) {
  if (!bmp) return;
  GSize sz = gbitmap_get_bounds(bmp).size;
  graphics_draw_rotated_bitmap(ctx, bmp, GPoint(sz.w / 2, sz.h / 2),
                               DEG_TO_TRIGANGLE(deg), dest);
}

// ---- main draw ------------------------------------------------------------

// Capture the framebuffer into a standalone bitmap (to bake the static bezel).
static GBitmap *bake_screen(GContext *ctx) {
  GBitmap *out = NULL;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (fb) {
    GRect bnd = gbitmap_get_bounds(fb);
    out = gbitmap_create_blank(bnd.size, GBitmapFormat8Bit);
    if (out) {
      uint16_t fbs = gbitmap_get_bytes_per_row(fb);
      uint16_t os  = gbitmap_get_bytes_per_row(out);
      uint8_t *fd  = gbitmap_get_data(fb);
      uint8_t *od  = gbitmap_get_data(out);
      for (int r = 0; r < bnd.size.h; r++)
        memcpy(od + r * os, fd + r * fbs, bnd.size.w);
    }
    graphics_release_frame_buffer(ctx, fb);
  }
  return out;
}

// The static bezel -- rendered once, then blitted from s_bezel each tick.
static void draw_bezel(GContext *ctx, GRect b) {
  int W = b.size.w;
  int H = b.size.h;

  GColor c_bezel  = GColorBlack;
  GColor c_label  = GColorLightGray;
  GColor c_red    = GColorOrange;
  GColor c_blue   = GColorVividCerulean;
  GColor c_green  = GColorIslamicGreen;
  GColor c_yellow = GColorYellow;
  GColor c_lcdbg  = GColorFromRGB(0xaa, 0xaa, 0x55);

  GFont f_casio = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GFont f_model = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont f_small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont f_lith  = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GFont f_botb  = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont f_btn   = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  graphics_context_set_fill_color(ctx, c_bezel);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // LCD geometry
  int lcd_x = W * 11 / 100;
  int lcd_w = W - 2 * lcd_x;
  int lcd_y = H * 30 / 100;
  int lcd_h = H * 44 / 100;
  GRect lcd = GRect(lcd_x, lcd_y, lcd_w, lcd_h);

  // Build the rotated side labels once (scratch is hidden by the LCD glass).
  // LIGHT/PREV put the white "button" block after the text (so it ends up at
  // the top); NEXT puts it before the text (block at the bottom).
  if (!s_lbl[0]) {
    GRect scr = GRect(lcd_x + 6, lcd_y + 6, 54, 20);
    bool after[3] = { true, true, false };   // LIGHT, PREV, NEXT
    for (int i = 0; i < 3; i++)
      s_lbl[i] = make_label_bitmap(ctx, kBtnLabels[i], f_btn, c_label, c_bezel, scr, after[i]);
  }

  // rotated 90deg, in the physical screen corners by the buttons:
  // LIGHT top-left, PREV top-right, NEXT bottom-right
  draw_rot_label(ctx, s_lbl[0], 270, GPoint(11, H * 14 / 100 + 15));     // LIGHT, top-left
  draw_rot_label(ctx, s_lbl[1], 270, GPoint(W - 11, H * 14 / 100 + 15)); // PREV, top-right
  draw_rot_label(ctx, s_lbl[2], 270, GPoint(W - 11, H * 86 / 100 - 15)); // NEXT, bottom-right

  // outer case frame line, inset from the screen edge
  graphics_context_set_stroke_color(ctx, c_label);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(ctx, GRect(2, 2, W - 4, H - 4), 8);

  // top bezel branding: CASIO (left) / F-84W (right)
  draw_text(ctx, "CASIO", f_casio, c_label, lcd_x, 6,
            lcd_w, H * 16 / 100, GTextAlignmentLeft);
  draw_text(ctx, "F-84W", f_model, c_label, lcd_x, 10,
            lcd_w, H * 14 / 100, GTextAlignmentRight);

  // top brick-red line: between the CASIO/F-84W line and the Lithium line
  int lith_y = lcd_y - 22;
  graphics_context_set_fill_color(ctx, c_red);
  graphics_fill_rect(ctx, GRect(lcd_x, lith_y - 7, lcd_w, 2), 0, GCornerNone);

  // "Lithium" + ((O)) mark (yellow)
  draw_text(ctx, "Lithium", f_lith, c_yellow, lcd_x, lith_y,
            lcd_w, 16, GTextAlignmentLeft);
  draw_lithium_icon(ctx, lcd_x + lcd_w - 12, lith_y + 9, 9, c_yellow);

  // LCD glass + white perimeter stripe
  graphics_context_set_fill_color(ctx, c_lcdbg);
  graphics_fill_rect(ctx, lcd, 4, GCornersAll);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_round_rect(ctx, GRect(lcd_x - 2, lcd_y - 2, lcd_w + 4, lcd_h + 4), 6);

  // bottom bezel text, with the bottom brick-red line between the two lines
  draw_text(ctx, "WATER RESIST", f_botb, c_green, 0,
            lcd_y + lcd_h + H * 12 / 1000, W, H * 9 / 100, GTextAlignmentCenter);
  graphics_context_set_fill_color(ctx, c_red);
  graphics_fill_rect(ctx, GRect(lcd_x, lcd_y + lcd_h + H * 110 / 1000, lcd_w, 2),
                     0, GCornerNone);
  draw_text(ctx, "ALARM CHRONOGRAPH", f_botb, c_yellow, 0,
            lcd_y + lcd_h + H * 125 / 1000, W, H * 10 / 100, GTextAlignmentCenter);
}

// ---- LCD contents (dynamic, drawn over the baked bezel each tick) ----------
static void draw_lcd(GContext *ctx, GRect b) {
  // Anti-alias the segment fills so the angled bevels (and the thin gaps they
  // bound) blend with the LCD background instead of hard-stepping -- makes the
  // 1px inter-segment gaps read softer on the smaller numerals.
  graphics_context_set_antialiased(ctx, true);
  int W = b.size.w;
  int H = b.size.h;
  GColor c_lcdfg = GColorBlack;
  GColor c_ghost = GColorFromRGB(0xaa, 0xaa, 0x00);
  GFont f_ind = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  int lcd_x = W * 11 / 100;
  int lcd_w = W - 2 * lcd_x;
  int lcd_y = H * 30 / 100;
  int lcd_h = H * 44 / 100;

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  bool h24 = clock_is_24h_style();
  int hour = t->tm_hour;
  int disp_hour = h24 ? hour : (hour % 12 == 0 ? 12 : hour % 12);
  bool blank_lead = !h24 && disp_hour < 10;

  int ins = lcd_w * 5 / 100;
  int gx = lcd_x + ins;
  int gr = lcd_x + lcd_w - ins;

  // chime (hourly) + bell (alarm) icons, top-left; faint when their feature is off
  int icon_y = lcd_y + lcd_h * 5 / 100;
  int icon_s = lcd_h * 17 / 100; if (icon_s < 10) icon_s = 10;
  bool chime_on = s_cfg.hourly_vibe || s_cfg.hourly_chime;
  draw_chime(ctx, gx, icon_y, icon_s, chime_on ? c_lcdfg : c_ghost);
  int bell_s = icon_s * 11 / 10;          // alarm bell a touch larger
  int bell_x = gx + icon_s * 16 / 10;     // moved a little left, after the chime
  draw_bell(ctx, bell_x, icon_y, bell_s, s_cfg.alarm_enabled ? c_lcdfg : c_ghost);

  // PM and 24H indicators. On the real watch these are their own fixed-shape
  // LCD cells (not built from segments), so they're drawn as small glyphs and
  // the "off" ghost is simply the same glyph in the faint colour.
  int ind_y = lcd_y + lcd_h * 24 / 100;
  {
    bool pm_active = !h24 && hour >= 12;
    draw_text(ctx, "PM", f_ind, pm_active ? c_lcdfg : c_ghost,
              gx, ind_y, 40, 18, GTextAlignmentLeft);
    draw_text(ctx, "24H", f_ind, h24 ? c_lcdfg : c_ghost,
              gx + lcd_w * 16 / 100, ind_y, 48, 18, GTextAlignmentLeft);
  }

  // day-of-week (centre, 14-seg) and date (right, 7-seg), top-aligned with icons
  int drow_y = lcd_y + lcd_h * 3 / 100;
  int lh = lcd_h * 18 / 100;
  int lw = lh * 58 / 100;
  int lt = lh * 14 / 100; if (lt < 2) lt = 2;
  int lgap = lw; if (lgap < 4) lgap = 4;   // space between day letters
  draw_word(ctx, gx + lcd_w * 36 / 100, drow_y, lw, lh, lt, lgap,
            kDays[t->tm_wday], c_lcdfg, c_ghost, false);

  int dh_date = lcd_h * 20 / 100 + 2;   // date a hair larger than the day-of-week
  int dw_date = dh_date * 55 / 100;
  int t_date  = dh_date * 16 / 100; if (t_date < 2) t_date = 2;
  int gap_date = dw_date * 16 / 100; if (gap_date < 1) gap_date = 1;
  int date_x = gr - (2 * dw_date + gap_date);
  draw_2digit(ctx, date_x, drow_y, dw_date, dh_date, t_date, t_date + 1, gap_date,
              t->tm_mday, false, c_lcdfg, c_ghost, true);

  // main time (large, fixed position, spans the glass)
  int dh = lcd_h * 47 / 100;
  int avail = gr - gx;
  int dw = avail * 1000 / 5900;        // sized so HH:MM:SS spans the width
  int max_dw = dh * 60 / 100;
  if (dw > max_dw) dw = max_dw;
  int tt = dw * 18 / 100; if (tt < 3) tt = 3;   // thinner strokes, like the real LCD
  int gap = dw * 10 / 100; if (gap < 1) gap = 1;
  int cw = dw * 45 / 100;
  int dh_s = dh * 52 / 100 + 2;
  int dw_s = dw * 50 / 100 + 1;
  int t_s = dh_s * 16 / 100; if (t_s < 2) t_s = 2;
  int gap_s = dw_s * 14 / 100; if (gap_s < 1) gap_s = 1;
  int sec_gap = dw * 18 / 100;

  int w_hh = 2 * dw + gap;
  int w_mm = 2 * dw + gap;
  int y_main = lcd_y + lcd_h * 43 / 100;

  draw_2digit(ctx, gx, y_main, dw, dh, tt, tt + 1, gap, disp_hour, blank_lead,
              c_lcdfg, c_ghost, true);
  int x_colon = gx + w_hh;
  int cs = tt;
  int ccx = x_colon + (cw - cs) / 2;
  // colon blinks once a second -- only when seconds are shown (second-tick on)
  GColor colon_col = (s_cfg.show_seconds && (t->tm_sec % 2)) ? c_ghost : c_lcdfg;
  graphics_context_set_fill_color(ctx, colon_col);
  graphics_fill_rect(ctx, GRect(ccx, y_main + dh * 28 / 100, cs, cs), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(ccx, y_main + dh * 60 / 100, cs, cs), 0, GCornerNone);
  int x_mm = x_colon + cw;
  draw_2digit(ctx, x_mm, y_main, dw, dh, tt, tt + 1, gap, t->tm_min, false,
              c_lcdfg, c_ghost, true);

  // seconds: a fixed slot; when hidden, show empty (ghost) cells instead
  int x_sec = x_mm + w_mm + sec_gap;
  int y_sec = y_main + (dh - dh_s);
  if (s_cfg.show_seconds) {
    draw_2digit(ctx, x_sec, y_sec, dw_s, dh_s, t_s, t_s + 1, gap_s, t->tm_sec, false,
                c_lcdfg, c_ghost, true);
  } else {
    draw_digit(ctx, x_sec, y_sec, dw_s, dh_s, t_s, t_s + 1, -1, c_lcdfg, c_ghost, true);
    draw_digit(ctx, x_sec + dw_s + gap_s, y_sec, dw_s, dh_s, t_s, t_s + 1, -1,
               c_lcdfg, c_ghost, true);
  }
}

static void layer_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (!s_bezel) {
    // First frame: render the bezel, bake it, and free the temporary labels.
    draw_bezel(ctx, b);
    s_bezel = bake_screen(ctx);
    for (int i = 0; i < 3; i++) {
      if (s_lbl[i]) { gbitmap_destroy(s_lbl[i]); s_lbl[i] = NULL; }
    }
  } else {
    graphics_draw_bitmap_in_rect(ctx, s_bezel, b);
  }
  draw_lcd(ctx, b);
}

// ---- Casio "beep beep" chime (recorded PCM) -------------------------------
// Plays a real F-84W "beep beep" recording (16 kHz / 16-bit signed mono PCM,
// stored as a raw resource) by streaming it to the speaker. The hourly signal
// plays it once; the alarm loops it seamlessly until dismissed. We stream it in
// small chunks straight from the resource, so the whole clip never sits in RAM.
#if defined(PBL_SPEAKER)
#define PCM_CHUNK 2048
static ResHandle s_pcm_res = NULL;
static uint32_t  s_pcm_size = 0;
static uint32_t  s_pcm_pos = 0;        // next byte to read from the resource
static bool      s_pcm_loop = false;
static bool      s_pcm_active = false;
static AppTimer *s_pcm_timer = NULL;
static uint8_t   s_pcm_buf[PCM_CHUNK];
static uint32_t  s_pcm_have = 0;       // valid bytes in s_pcm_buf
static uint32_t  s_pcm_sent = 0;       // bytes of s_pcm_buf already written

static void casio_chime_stop(void) {
  if (s_pcm_timer) { app_timer_cancel(s_pcm_timer); s_pcm_timer = NULL; }
  if (s_pcm_active) { speaker_stop(); s_pcm_active = false; }
  s_pcm_pos = s_pcm_have = s_pcm_sent = 0;
}

// Keep the speaker's buffer fed; runs off a short repeating timer.
static void pcm_pump(void *ctx) {
  s_pcm_timer = NULL;
  if (!s_pcm_active) return;
  uint32_t written = 0;
  while (written < PCM_CHUNK * 8) {              // cap per tick (we'll resume next tick)
    if (s_pcm_sent >= s_pcm_have) {              // local buffer drained -> refill
      if (s_pcm_pos >= s_pcm_size) {
        if (!s_pcm_loop) {                        // one-shot: all data handed over
          speaker_stream_close();                 // plays out what's buffered, then stops
          s_pcm_active = false;
          return;
        }
        s_pcm_pos = 0;                            // loop seamlessly back to the start
      }
      uint32_t want = s_pcm_size - s_pcm_pos;
      if (want > PCM_CHUNK) want = PCM_CHUNK;
      resource_load_byte_range(s_pcm_res, s_pcm_pos, s_pcm_buf, want);
      s_pcm_have = want; s_pcm_sent = 0; s_pcm_pos += want;
    }
    uint32_t n = speaker_stream_write(s_pcm_buf + s_pcm_sent, s_pcm_have - s_pcm_sent);
    s_pcm_sent += n;
    written += n;
    if (n == 0) break;                            // speaker buffer full -> wait
  }
  s_pcm_timer = app_timer_register(40, pcm_pump, NULL);
}

static void casio_chime_play(bool loop) {
  if (!s_pcm_res) {
    s_pcm_res = resource_get_handle(RESOURCE_ID_BEEP_BEEP);
    s_pcm_size = s_pcm_res ? resource_size(s_pcm_res) : 0;
  }
  if (s_pcm_size == 0) return;
  casio_chime_stop();                             // restart cleanly if already playing
  if (!speaker_stream_open(SpeakerPcmFormat_16kHz_16bit, s_cfg.chime_volume)) return;
  s_pcm_pos = s_pcm_have = s_pcm_sent = 0;
  s_pcm_loop = loop;
  s_pcm_active = true;
  pcm_pump(NULL);
}
#else
static void casio_chime_play(bool loop) { (void) loop; }
static void casio_chime_stop(void) {}
#endif

// ---- alarm ----------------------------------------------------------------
static bool s_alarm_active = false;
static bool s_alarm_fired = false;        // already fired during the matching minute
static AppTimer *s_alarm_timer = NULL;
static int s_alarm_cycles = 0;

// Vibration mirroring the "beep beep": two 160ms pulses 80ms apart. One call is
// one set; the ~600ms silence before the next set is what's left of the 1000ms
// alarm cycle (the hourly signal is just a single set).
static void casio_buzz(void) {
  static const uint32_t segs[] = { 160, 80, 160 };  // on, off, on
  VibePattern pat = { .durations = segs, .num_segments = 3 };
  vibes_enqueue_custom_pattern(pat);
}

static void alarm_stop(void) {
  if (!s_alarm_active) return;
  s_alarm_active = false;
  if (s_alarm_timer) { app_timer_cancel(s_alarm_timer); s_alarm_timer = NULL; }
  casio_chime_stop();
  vibes_cancel();
  accel_tap_service_unsubscribe();
}

static void alarm_cycle(void *ctx) {
  s_alarm_timer = NULL;
  if (!s_alarm_active) return;
  if (s_alarm_cycles <= 0) { alarm_stop(); return; }
  s_alarm_cycles--;
  if (s_cfg.alarm_vibe) casio_buzz();                     // chime loops on its own
  s_alarm_timer = app_timer_register(1000, alarm_cycle, NULL);  // 400ms buzz + ~600ms gap
}

static void alarm_tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_alarm_active) alarm_stop();                       // shake to silence
}

static void alarm_start(void) {
  if (s_alarm_active) return;
  if (!s_cfg.alarm_vibe && !s_cfg.alarm_chime) return;
  s_alarm_active = true;
  s_alarm_cycles = 24;                                    // ~24s at 1000ms/cycle
  accel_tap_service_subscribe(alarm_tap_handler);
  if (s_cfg.alarm_chime) casio_chime_play(true);          // loops until dismissed
  alarm_cycle(NULL);                                      // vibration + timeout
}

// ---------------------------------------------------------------------------

static void tick_handler(struct tm *t, TimeUnits units_changed) {
  // hourly signal at the top of the hour
  if (t->tm_min == 0 && t->tm_sec == 0) {
    if (s_cfg.hourly_vibe) casio_buzz();
    if (s_cfg.hourly_chime) casio_chime_play(false);
  }
  // alarm (fires once when the clock enters the configured minute)
  if (s_cfg.alarm_enabled && t->tm_hour == s_cfg.alarm_hour && t->tm_min == s_cfg.alarm_min) {
    if (!s_alarm_fired) { s_alarm_fired = true; alarm_start(); }
  } else {
    s_alarm_fired = false;
  }
  if (s_layer) layer_mark_dirty(s_layer);
}

static void apply_tick_unit(void) {
  // Subscribing to SECOND_UNIT only when the seconds are shown saves battery.
  tick_timer_service_unsubscribe();
  tick_timer_service_subscribe(s_cfg.show_seconds ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *tp;
  if ((tp = dict_find(iter, MESSAGE_KEY_SHOW_SECONDS)))  s_cfg.show_seconds  = tp->value->int32 != 0;
  if ((tp = dict_find(iter, MESSAGE_KEY_HOURLY_VIBE)))   s_cfg.hourly_vibe   = tp->value->int32 != 0;
  if ((tp = dict_find(iter, MESSAGE_KEY_HOURLY_CHIME)))  s_cfg.hourly_chime  = tp->value->int32 != 0;
  if ((tp = dict_find(iter, MESSAGE_KEY_ALARM_ENABLED))) s_cfg.alarm_enabled = tp->value->int32 != 0;
  if ((tp = dict_find(iter, MESSAGE_KEY_ALARM_VIBE)))    s_cfg.alarm_vibe    = tp->value->int32 != 0;
  if ((tp = dict_find(iter, MESSAGE_KEY_ALARM_CHIME)))   s_cfg.alarm_chime   = tp->value->int32 != 0;
  if ((tp = dict_find(iter, MESSAGE_KEY_CHIME_VOLUME))) {
    int32_t v = tp->value->int32;
    s_cfg.chime_volume = v < 0 ? 0 : (v > 100 ? 100 : (uint8_t)v);
  }
  if ((tp = dict_find(iter, MESSAGE_KEY_ALARM_TIME))) {
    const char *c = tp->value->cstring;                   // "HH:MM"
    if (c && c[0] && c[1] && c[2] == ':' && c[3] && c[4]) {
      s_cfg.alarm_hour = (c[0] - '0') * 10 + (c[1] - '0');
      s_cfg.alarm_min  = (c[3] - '0') * 10 + (c[4] - '0');
    }
  }
  persist_write_data(SETTINGS_PERSIST_KEY, &s_cfg, sizeof(s_cfg));
  s_alarm_fired = false;
  apply_tick_unit();
  if (s_layer) layer_mark_dirty(s_layer);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_layer = layer_create(bounds);
  layer_set_update_proc(s_layer, layer_update);
  layer_add_child(root, s_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_layer);
  s_layer = NULL;
  if (s_bezel) { gbitmap_destroy(s_bezel); s_bezel = NULL; }
  for (int i = 0; i < 3; i++) {
    if (s_lbl[i]) { gbitmap_destroy(s_lbl[i]); s_lbl[i] = NULL; }
  }
}

static void init(void) {
  if (persist_exists(SETTINGS_PERSIST_KEY)) {
    persist_read_data(SETTINGS_PERSIST_KEY, &s_cfg, sizeof(s_cfg));
  }

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
  apply_tick_unit();
}

static void deinit(void) {
  alarm_stop();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
