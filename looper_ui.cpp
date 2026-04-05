/*
 * Guitar Looper LV2 Plugin — X11/Cairo UI
 *
 * Pure Xlib + Cairo: zero GTK dependency.
 * Works in any LV2 host that supports ui:X11UI.
 */

#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#define UI_URI "urn:rafa:Looper#ui"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Layout constants (all in pixels)
 * ---------------------------------------------------------------------- */
#define UI_W        280
#define UI_H        378

/* State LED */
#define LED_CX      140
#define LED_CY       85
#define LED_R        58

/* Separator y positions */
#define SEP1_Y      153
#define SEP2_Y      255
#define SEP3_Y      312

/* Knobs */
#define LKNOB_CX     80
#define RKNOB_CX    200
#define KNOB_CY     200
#define KNOB_R       34
#define KLBL_Y      247

/* MIDI mode */
#define MODE_LBL_Y  268
#define BTN_Y       280
#define BTN_H        24
#define BTN_NON_X    14
#define BTN_NON_W   118
#define BTN_CC_X    142
#define BTN_CC_W     60

/* Note rows (center y of each row) */
#define ROW0_Y      327
#define ROW1_Y      347
#define ROW2_Y      367

/* Note row widget x positions */
#define NOTE_DEC_X   86
#define NOTE_VAL_X  110
#define NOTE_INC_X  150
#define NOTE_BTN_W   20
#define NOTE_BTN_H   20

/* -------------------------------------------------------------------------
 * Port indices
 * ---------------------------------------------------------------------- */
enum {
    PORT_AUDIO_IN   = 0,
    PORT_AUDIO_OUT  = 1,
    PORT_MIDI_IN    = 2,
    PORT_REC_NOTE   = 3,
    PORT_CLR_NOTE   = 4,
    PORT_PAUSE_NOTE = 5,
    PORT_LEVEL      = 6,
    PORT_STATE_OUT  = 7,
    PORT_MIDI_MODE  = 8,
    PORT_REC_LEVEL  = 9,
};

/* -------------------------------------------------------------------------
 * Plugin UI state
 * ---------------------------------------------------------------------- */
typedef struct {
    LV2UI_Write_Function write;
    LV2UI_Controller     controller;

    Display*         dpy;
    Window           win;
    cairo_surface_t* surf;

    /* Port values */
    float state;
    float level;
    float rec_level;
    int   midi_mode;
    int   rec_note;
    int   clr_note;
    int   pause_note;

    /* Knob drag */
    int    dragging;   /* 0=none, 1=level, 2=rec_level */
    double drag_y;
    double drag_v0;

    int needs_redraw;
} LooperUI;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static void note_name(int n, char* buf, int sz)
{
    static const char* nm[] =
        {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (n < 0 || n > 127) snprintf(buf, sz, "?");
    else                   snprintf(buf, sz, "%s%d", nm[n%12], n/12 - 1);
}

static void write_f(LooperUI* ui, uint32_t port, float v)
{
    ui->write(ui->controller, port, sizeof(float), 0, &v);
}

/* -------------------------------------------------------------------------
 * Hit tests
 * ---------------------------------------------------------------------- */
static int hit_lknob(int x, int y)
{
    double dx = x - LKNOB_CX, dy = y - KNOB_CY;
    return (dx*dx + dy*dy <= (double)(KNOB_R+10)*(KNOB_R+10));
}

static int hit_rknob(int x, int y)
{
    double dx = x - RKNOB_CX, dy = y - KNOB_CY;
    return (dx*dx + dy*dy <= (double)(KNOB_R+10)*(KNOB_R+10));
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx+rw && y >= ry && y < ry+rh;
}

static int near_row(int y, int row_cy)
{
    return y >= row_cy - NOTE_BTN_H/2 && y <= row_cy + NOTE_BTN_H/2;
}

/* -------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------- */
#define KNOB_START  (2.0 * M_PI / 3.0)   /* 7 o'clock */
#define KNOB_SWEEP  (5.0 * M_PI / 3.0)   /* 300° sweep */

static void draw_state_led(cairo_t* cr, float state)
{
    double cx = LED_CX, cy = LED_CY, r = LED_R;

    typedef struct { double r, g, b; const char* name; } SI;
    static const SI S[5] = {
        {0.22, 0.22, 0.22, "IDLE"        },
        {0.85, 0.20, 0.20, "RECORDING"   },
        {0.18, 0.72, 0.18, "PLAYING"     },
        {0.88, 0.48, 0.08, "OVERDUBBING" },
        {0.78, 0.78, 0.08, "PAUSED"      },
    };
    int s = (int)(state + 0.5f);
    if (s < 0 || s > 4) s = 0;
    double sr = S[s].r, sg = S[s].g, sb = S[s].b;

    /* Glow */
    if (s > 0) {
        cairo_pattern_t* g =
            cairo_pattern_create_radial(cx, cy, r*0.5, cx, cy, r*1.4);
        cairo_pattern_add_color_stop_rgba(g, 0.0, sr, sg, sb, 0.28);
        cairo_pattern_add_color_stop_rgba(g, 1.0, sr, sg, sb, 0.00);
        cairo_arc(cr, cx, cy, r*1.4, 0, 2*M_PI);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* Sphere body */
    {
        double hi = s ? 1.5 : 1.1;
#define C1(x) ((x)*hi > 1.0 ? 1.0 : (x)*hi)
        cairo_pattern_t* b =
            cairo_pattern_create_radial(cx-r*0.28, cy-r*0.32, r*0.05, cx, cy, r);
        cairo_pattern_add_color_stop_rgb(b, 0.0, C1(sr), C1(sg), C1(sb));
        cairo_pattern_add_color_stop_rgb(b, 1.0, sr*0.35, sg*0.35, sb*0.35);
#undef C1
        cairo_arc(cr, cx, cy, r, 0, 2*M_PI);
        cairo_set_source(cr, b);
        cairo_fill(cr);
        cairo_pattern_destroy(b);
    }

    /* Rim */
    cairo_arc(cr, cx, cy, r, 0, 2*M_PI);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    /* Specular */
    {
        double hx = cx-r*0.30, hy = cy-r*0.35;
        cairo_pattern_t* sp =
            cairo_pattern_create_radial(hx, hy, 0, hx, hy, r*0.28);
        cairo_pattern_add_color_stop_rgba(sp, 0.0, 1, 1, 1, 0.38);
        cairo_pattern_add_color_stop_rgba(sp, 1.0, 1, 1, 1, 0.00);
        cairo_arc(cr, hx, hy, r*0.28, 0, 2*M_PI);
        cairo_set_source(cr, sp);
        cairo_fill(cr);
        cairo_pattern_destroy(sp);
    }

    /* State label */
    cairo_set_source_rgba(cr, 1, 1, 1, s ? 0.92 : 0.45);
    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    double fs = r * 0.25;
    if (fs < 9) fs = 9;
    cairo_set_font_size(cr, fs);
    cairo_text_extents_t te;
    cairo_text_extents(cr, S[s].name, &te);
    cairo_move_to(cr,
        cx - te.width/2  - te.x_bearing,
        cy - te.height/2 - te.y_bearing);
    cairo_show_text(cr, S[s].name);
}

static void draw_knob_at(cairo_t* cr, double cx, double cy, double R,
                         float val, float vmin, float vmax)
{
    double norm = (double)(val - vmin) / (vmax - vmin);
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;
    double va = KNOB_START + norm * KNOB_SWEEP;

    /* Track background */
    cairo_set_line_width(cr, 3.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, R-4, KNOB_START, KNOB_START + KNOB_SWEEP);
    cairo_set_source_rgba(cr, 0.16, 0.16, 0.16, 1);
    cairo_stroke(cr);

    /* Value arc */
    if (norm > 0.002) {
        cairo_arc(cr, cx, cy, R-4, KNOB_START, va);
        cairo_set_source_rgba(cr, 0.29, 0.56, 0.85, 1);
        cairo_stroke(cr);
    }

    /* Knob body */
    double r = R * 0.70;
    {
        cairo_pattern_t* bd =
            cairo_pattern_create_radial(cx-r*0.25, cy-r*0.28, r*0.05, cx, cy, r);
        cairo_pattern_add_color_stop_rgb(bd, 0.0, 0.55, 0.55, 0.55);
        cairo_pattern_add_color_stop_rgb(bd, 1.0, 0.17, 0.17, 0.17);
        cairo_arc(cr, cx, cy, r, 0, 2*M_PI);
        cairo_set_source(cr, bd);
        cairo_fill(cr);
        cairo_pattern_destroy(bd);
    }

    /* Rim */
    cairo_arc(cr, cx, cy, r, 0, 2*M_PI);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    /* Specular */
    {
        double hx = cx-r*0.22, hy = cy-r*0.26;
        cairo_pattern_t* sp =
            cairo_pattern_create_radial(hx, hy, 0, hx, hy, r*0.32);
        cairo_pattern_add_color_stop_rgba(sp, 0.0, 1, 1, 1, 0.28);
        cairo_pattern_add_color_stop_rgba(sp, 1.0, 1, 1, 1, 0.00);
        cairo_arc(cr, hx, hy, r*0.32, 0, 2*M_PI);
        cairo_set_source(cr, sp);
        cairo_fill(cr);
        cairo_pattern_destroy(sp);
    }

    /* Indicator line */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.88);
    cairo_set_line_width(cr, 2.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx + cos(va)*r*0.36, cy + sin(va)*r*0.36);
    cairo_line_to(cr, cx + cos(va)*r*0.80, cy + sin(va)*r*0.80);
    cairo_stroke(cr);

    /* Value text */
    char buf[12];
    snprintf(buf, sizeof(buf), "%.2f", val);
    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9);
    cairo_set_source_rgba(cr, 0.65, 0.65, 0.65, 0.85);
    cairo_text_extents_t te;
    cairo_text_extents(cr, buf, &te);
    cairo_move_to(cr,
        cx - te.width/2  - te.x_bearing,
        cy + r*0.26 - te.height/2 - te.y_bearing);
    cairo_show_text(cr, buf);
}

/* Rounded rectangle fill helper */
static void rrect(cairo_t* cr, double x, double y, double w, double h, double rad)
{
    cairo_move_to(cr, x+rad, y);
    cairo_line_to(cr, x+w-rad, y);
    cairo_arc(cr, x+w-rad, y+rad,   rad, -M_PI/2, 0);
    cairo_line_to(cr, x+w, y+h-rad);
    cairo_arc(cr, x+w-rad, y+h-rad, rad, 0,       M_PI/2);
    cairo_line_to(cr, x+rad, y+h);
    cairo_arc(cr, x+rad,   y+h-rad, rad, M_PI/2,  M_PI);
    cairo_line_to(cr, x, y+rad);
    cairo_arc(cr, x+rad,   y+rad,   rad, M_PI,    3*M_PI/2);
    cairo_close_path(cr);
}

static void draw_btn(cairo_t* cr, double x, double y, double w, double h,
                     const char* lbl, int active)
{
    rrect(cr, x, y, w, h, 4);
    if (active)
        cairo_set_source_rgba(cr, 0.10, 0.25, 0.44, 1);
    else
        cairo_set_source_rgba(cr, 0.17, 0.17, 0.17, 1);
    cairo_fill_preserve(cr);
    if (active)
        cairo_set_source_rgba(cr, 0.23, 0.47, 0.74, 1);
    else
        cairo_set_source_rgba(cr, 0.30, 0.30, 0.30, 1);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, active ? 1.0 : 0.58,
                              active ? 1.0 : 0.58,
                              active ? 1.0 : 0.58, 1);
    cairo_text_extents_t te;
    cairo_text_extents(cr, lbl, &te);
    cairo_move_to(cr,
        x + w/2 - te.width/2  - te.x_bearing,
        y + h/2 - te.height/2 - te.y_bearing);
    cairo_show_text(cr, lbl);
}

static void draw_note_row(cairo_t* cr, const char* lbl, int val, int ry)
{
    /* Row label */
    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, 0.50, 0.50, 0.50, 1);
    cairo_text_extents_t te;
    cairo_text_extents(cr, lbl, &te);
    cairo_move_to(cr,
        NOTE_DEC_X - 4 - te.width - te.x_bearing,
        ry - te.height/2 - te.y_bearing);
    cairo_show_text(cr, lbl);

    /* Dec / Inc buttons */
    draw_btn(cr, NOTE_DEC_X, ry - NOTE_BTN_H/2, NOTE_BTN_W, NOTE_BTN_H, "<", 0);
    draw_btn(cr, NOTE_INC_X, ry - NOTE_BTN_H/2, NOTE_BTN_W, NOTE_BTN_H, ">", 0);

    /* Value box */
    cairo_set_source_rgba(cr, 0.13, 0.13, 0.13, 1);
    cairo_rectangle(cr, NOTE_VAL_X, ry-10, 36, 20);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.28, 0.28, 0.28, 1);
    cairo_rectangle(cr, NOTE_VAL_X, ry-10, 36, 20);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);

    char num[8];
    snprintf(num, sizeof(num), "%d", val);
    cairo_set_source_rgba(cr, 0.82, 0.82, 0.82, 1);
    cairo_set_font_size(cr, 12);
    cairo_text_extents(cr, num, &te);
    cairo_move_to(cr,
        NOTE_VAL_X + 18 - te.width/2 - te.x_bearing,
        ry - te.height/2 - te.y_bearing);
    cairo_show_text(cr, num);

    /* Note name */
    char nb[8];
    note_name(val, nb, sizeof(nb));
    cairo_set_source_rgba(cr, 0.29, 0.56, 0.85, 1);
    cairo_set_font_size(cr, 11);
    cairo_text_extents(cr, nb, &te);
    cairo_move_to(cr,
        NOTE_INC_X + NOTE_BTN_W + 8,
        ry - te.height/2 - te.y_bearing);
    cairo_show_text(cr, nb);
}

static void draw_sep(cairo_t* cr, int y)
{
    cairo_set_source_rgba(cr, 0.22, 0.22, 0.22, 1);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 14, y);
    cairo_line_to(cr, UI_W-14, y);
    cairo_stroke(cr);
}

static void draw_all(LooperUI* ui)
{
    cairo_t* cr = cairo_create(ui->surf);

    /* Background */
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.11);
    cairo_paint(cr);

    /* Title */
    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 1);
    {
        cairo_text_extents_t te;
        cairo_text_extents(cr, "LOOPER PEDAL", &te);
        cairo_move_to(cr,
            UI_W/2 - te.width/2  - te.x_bearing,
            18     - te.height/2 - te.y_bearing);
        cairo_show_text(cr, "LOOPER PEDAL");
    }

    /* State LED */
    draw_state_led(cr, ui->state);

    draw_sep(cr, SEP1_Y);

    /* Knobs */
    draw_knob_at(cr, LKNOB_CX, KNOB_CY, KNOB_R, ui->level,     0.0f, 2.0f);
    draw_knob_at(cr, RKNOB_CX, KNOB_CY, KNOB_R, ui->rec_level, 0.0f, 2.0f);

    /* Knob labels */
    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_set_source_rgba(cr, 0.50, 0.50, 0.50, 1);
    {
        cairo_text_extents_t te;
        cairo_text_extents(cr, "Loop Volume", &te);
        cairo_move_to(cr,
            LKNOB_CX - te.width/2  - te.x_bearing,
            KLBL_Y   - te.height/2 - te.y_bearing);
        cairo_show_text(cr, "Loop Volume");

        cairo_text_extents(cr, "Rec Level", &te);
        cairo_move_to(cr,
            RKNOB_CX - te.width/2  - te.x_bearing,
            KLBL_Y   - te.height/2 - te.y_bearing);
        cairo_show_text(cr, "Rec Level");
    }

    draw_sep(cr, SEP2_Y);

    /* MIDI Mode */
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, 0.50, 0.50, 0.50, 1);
    {
        cairo_text_extents_t te;
        const char* ml = "MIDI Mode:";
        cairo_text_extents(cr, ml, &te);
        cairo_move_to(cr, 14, MODE_LBL_Y - te.height/2 - te.y_bearing);
        cairo_show_text(cr, ml);
    }

    draw_btn(cr, BTN_NON_X, BTN_Y, BTN_NON_W, BTN_H, "Note On", ui->midi_mode == 0);
    draw_btn(cr, BTN_CC_X,  BTN_Y, BTN_CC_W,  BTN_H, "CC",      ui->midi_mode == 1);

    draw_sep(cr, SEP3_Y);

    /* Note rows */
    draw_note_row(cr, "Record:", ui->rec_note,   ROW0_Y);
    draw_note_row(cr, "Clear:",  ui->clr_note,   ROW1_Y);
    draw_note_row(cr, "Pause:",  ui->pause_note, ROW2_Y);

    cairo_destroy(cr);
    XFlush(ui->dpy);
}

/* -------------------------------------------------------------------------
 * Event handling
 * ---------------------------------------------------------------------- */
static void try_note_btn(LooperUI* ui, int x, int y, int row_cy,
                         int* val, uint32_t port)
{
    if (!near_row(y, row_cy)) return;

    int dec_hit = in_rect(x, y, NOTE_DEC_X, row_cy-NOTE_BTN_H/2,
                          NOTE_BTN_W, NOTE_BTN_H);
    int inc_hit = in_rect(x, y, NOTE_INC_X, row_cy-NOTE_BTN_H/2,
                          NOTE_BTN_W, NOTE_BTN_H);

    if (dec_hit || inc_hit) {
        *val = (*val + (dec_hit ? -1 : 1) + 128) % 128;
        write_f(ui, port, (float)*val);
        ui->needs_redraw = 1;
    }
}

static void handle_scroll(LooperUI* ui, int x, int y, int up)
{
    float step = up ? 0.02f : -0.02f;

    if (hit_lknob(x, y)) {
        ui->level += step;
        if (ui->level < 0) ui->level = 0;
        if (ui->level > 2) ui->level = 2;
        write_f(ui, PORT_LEVEL, ui->level);
        ui->needs_redraw = 1;
    } else if (hit_rknob(x, y)) {
        ui->rec_level += step;
        if (ui->rec_level < 0) ui->rec_level = 0;
        if (ui->rec_level > 2) ui->rec_level = 2;
        write_f(ui, PORT_REC_LEVEL, ui->rec_level);
        ui->needs_redraw = 1;
    } else {
        int delta = up ? 1 : -1;
        if (near_row(y, ROW0_Y)) {
            ui->rec_note = (ui->rec_note + delta + 128) % 128;
            write_f(ui, PORT_REC_NOTE, (float)ui->rec_note);
            ui->needs_redraw = 1;
        } else if (near_row(y, ROW1_Y)) {
            ui->clr_note = (ui->clr_note + delta + 128) % 128;
            write_f(ui, PORT_CLR_NOTE, (float)ui->clr_note);
            ui->needs_redraw = 1;
        } else if (near_row(y, ROW2_Y)) {
            ui->pause_note = (ui->pause_note + delta + 128) % 128;
            write_f(ui, PORT_PAUSE_NOTE, (float)ui->pause_note);
            ui->needs_redraw = 1;
        }
    }
}

static void handle_button_press(LooperUI* ui, int x, int y, unsigned int button)
{
    if (button == 4) { handle_scroll(ui, x, y, 1); return; }
    if (button == 5) { handle_scroll(ui, x, y, 0); return; }
    if (button != 1) return;

    /* Knob drag start */
    if (hit_lknob(x, y)) {
        ui->dragging = 1;
        ui->drag_y   = y;
        ui->drag_v0  = ui->level;
        return;
    }
    if (hit_rknob(x, y)) {
        ui->dragging = 2;
        ui->drag_y   = y;
        ui->drag_v0  = ui->rec_level;
        return;
    }

    /* MIDI mode */
    if (in_rect(x, y, BTN_NON_X, BTN_Y, BTN_NON_W, BTN_H)) {
        ui->midi_mode = 0;
        write_f(ui, PORT_MIDI_MODE, 0.0f);
        ui->needs_redraw = 1;
        return;
    }
    if (in_rect(x, y, BTN_CC_X, BTN_Y, BTN_CC_W, BTN_H)) {
        ui->midi_mode = 1;
        write_f(ui, PORT_MIDI_MODE, 1.0f);
        ui->needs_redraw = 1;
        return;
    }

    /* Note rows */
    try_note_btn(ui, x, y, ROW0_Y, &ui->rec_note,   PORT_REC_NOTE);
    try_note_btn(ui, x, y, ROW1_Y, &ui->clr_note,   PORT_CLR_NOTE);
    try_note_btn(ui, x, y, ROW2_Y, &ui->pause_note, PORT_PAUSE_NOTE);
}

static void handle_motion(LooperUI* ui, int /*x*/, int y)
{
    if (!ui->dragging) return;

    float delta = (float)((ui->drag_y - y) / 160.0 * 2.0);
    float newval = (float)ui->drag_v0 + delta;
    if (newval < 0) newval = 0;
    if (newval > 2) newval = 2;

    if (ui->dragging == 1) {
        ui->level = newval;
        write_f(ui, PORT_LEVEL, newval);
    } else {
        ui->rec_level = newval;
        write_f(ui, PORT_REC_LEVEL, newval);
    }
    ui->needs_redraw = 1;
}

static void process_events(LooperUI* ui)
{
    XEvent ev;
    while (XPending(ui->dpy)) {
        XNextEvent(ui->dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                ui->needs_redraw = 1;
            break;
        case ButtonPress:
            handle_button_press(ui, ev.xbutton.x, ev.xbutton.y,
                                ev.xbutton.button);
            break;
        case ButtonRelease:
            if (ev.xbutton.button == 1)
                ui->dragging = 0;
            break;
        case MotionNotify:
            handle_motion(ui, ev.xmotion.x, ev.xmotion.y);
            break;
        default:
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * LV2 UI interface
 * ---------------------------------------------------------------------- */
static LV2UI_Handle ui_instantiate(
    const LV2UI_Descriptor*   descriptor,
    const char*               plugin_uri,
    const char*               bundle_path,
    LV2UI_Write_Function      write_function,
    LV2UI_Controller          controller,
    LV2UI_Widget*             widget,
    const LV2_Feature* const* features)
{
    (void)descriptor; (void)plugin_uri; (void)bundle_path;

    LooperUI* ui = (LooperUI*)calloc(1, sizeof(LooperUI));
    if (!ui) return NULL;

    ui->write        = write_function;
    ui->controller   = controller;
    ui->state        = 0.0f;
    ui->level        = 1.0f;
    ui->rec_level    = 1.0f;
    ui->midi_mode    = 0;
    ui->rec_note     = 60;
    ui->clr_note     = 62;
    ui->pause_note   = 64;
    ui->needs_redraw = 1;

    /* Collect parent XID from host */
    Window parent = 0;
    for (int i = 0; features[i]; i++) {
        if (!strcmp(features[i]->URI, LV2_UI__parent)) {
            parent = (Window)(uintptr_t)features[i]->data;
            break;
        }
    }

    /* Open our own X display connection */
    ui->dpy = XOpenDisplay(NULL);
    if (!ui->dpy) { free(ui); return NULL; }

    int scr  = DefaultScreen(ui->dpy);
    Window root = (parent != 0) ? parent : RootWindow(ui->dpy, scr);

    /* Create child window */
    XSetWindowAttributes wa;
    wa.background_pixel = BlackPixel(ui->dpy, scr);
    wa.border_pixel     = 0;
    wa.event_mask       = ExposureMask | ButtonPressMask |
                          ButtonReleaseMask | Button1MotionMask;

    ui->win = XCreateWindow(
        ui->dpy, root,
        0, 0, UI_W, UI_H, 0,
        DefaultDepth(ui->dpy, scr),
        InputOutput,
        DefaultVisual(ui->dpy, scr),
        CWBackPixel | CWBorderPixel | CWEventMask,
        &wa);

    /* Declare fixed size to the host */
    XSizeHints hints;
    hints.flags      = PMinSize | PMaxSize;
    hints.min_width  = hints.max_width  = UI_W;
    hints.min_height = hints.max_height = UI_H;
    XSetWMNormalHints(ui->dpy, ui->win, &hints);

    XMapWindow(ui->dpy, ui->win);
    XFlush(ui->dpy);

    /* Cairo surface backed by the X window */
    ui->surf = cairo_xlib_surface_create(
        ui->dpy, ui->win,
        DefaultVisual(ui->dpy, scr),
        UI_W, UI_H);

    *widget = (LV2UI_Widget)(uintptr_t)ui->win;
    return (LV2UI_Handle)ui;
}

static void ui_cleanup(LV2UI_Handle handle)
{
    LooperUI* ui = (LooperUI*)handle;
    if (ui->surf) cairo_surface_destroy(ui->surf);
    if (ui->win)  XDestroyWindow(ui->dpy, ui->win);
    if (ui->dpy)  XCloseDisplay(ui->dpy);
    free(ui);
}

static void ui_port_event(LV2UI_Handle handle,
                          uint32_t port, uint32_t size,
                          uint32_t format, const void* buf)
{
    LooperUI* ui = (LooperUI*)handle;
    if (format != 0 || size != sizeof(float)) return;

    float v = *(const float*)buf;
    switch (port) {
    case PORT_STATE_OUT:  ui->state      = v;              break;
    case PORT_LEVEL:      ui->level      = v;              break;
    case PORT_REC_LEVEL:  ui->rec_level  = v;              break;
    case PORT_MIDI_MODE:  ui->midi_mode  = (int)(v+0.5f); break;
    case PORT_REC_NOTE:   ui->rec_note   = (int)(v+0.5f); break;
    case PORT_CLR_NOTE:   ui->clr_note   = (int)(v+0.5f); break;
    case PORT_PAUSE_NOTE: ui->pause_note = (int)(v+0.5f); break;
    default: return;
    }
    ui->needs_redraw = 1;
}

/* Called periodically by the host — process X events and redraw. */
static int ui_idle(LV2UI_Handle handle)
{
    LooperUI* ui = (LooperUI*)handle;
    process_events(ui);
    if (ui->needs_redraw) {
        draw_all(ui);
        ui->needs_redraw = 0;
    }
    return 0;
}

static const void* ui_extension_data(const char* uri)
{
    if (!strcmp(uri, LV2_UI__idleInterface)) {
        static const LV2UI_Idle_Interface idle_iface = { ui_idle };
        return &idle_iface;
    }
    return NULL;
}

static const LV2UI_Descriptor ui_descriptor = {
    UI_URI,
    ui_instantiate,
    ui_cleanup,
    ui_port_event,
    ui_extension_data,
};

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
    return (index == 0) ? &ui_descriptor : NULL;
}
