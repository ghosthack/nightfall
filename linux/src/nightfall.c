/*
 * Nightfall for Linux (GTK4, Wayland or X11) - a tiny screen blanker.
 *
 *   nightfall                 blank every monitor now with the saved style; exits on wake
 *   nightfall --STYLE         blank now with that style (this time only), e.g. --rain
 *   nightfall --set-mode STYLE   save the default style (~/.config/nightfall/mode)
 *   nightfall --list          list the styles
 *
 * While blanked: a black fullscreen window per monitor, cursor hidden, and either a dim
 * clock that jumps to a random monitor/spot every 20 s (no burn-in), a dim green
 * "digital rain", a slowly shifting gray mosaic, or one of the styles drawn by the shared
 * scene engine (scene.c: stars, analog and word clocks, curves, maze, life, ripples,
 * plasma, fireflies, starfield). The animated styles redraw only what changed.
 * Wake with any key or mouse click; moving the mouse alone does not wake it.
 * Nothing stays resident: launch it from the dock/app grid or the Ctrl+Alt+B
 * GNOME shortcut that install.sh sets up.
 */
#include <gtk/gtk.h>
#include <math.h>
#include "rain.h"
#include "mosaic.h"
#include "scene.h"

#define GRACE_US   (800 * 1000)   /* ignore input right after blanking */
#define MOVE_SEC   20             /* move the clock this often */
#define MAX_MON    16

typedef struct {
    GtkWidget *window;
    GtkWidget *area;
    int width, height;
    /* rain mode: the scene is one small texture node per lit cell, grouped per column, so
     * GTK's node diff repaints only the cells that changed instead of the whole window */
    GtkWidget *rainview;
    Rain rain;
    GdkTexture *tex[RAIN_LEVELS][RAIN_GLYPHS];
    int cell, view_w, view_h;          /* cell in device pixels */
    double scale;
    /* mosaic mode: one color node per lit cell; GTK repaints only the cells that changed */
    Mosaic mosaic;
    /* scene styles: one texture node per lit 64-px tile, re-uploaded only when it changed */
    NfScene *scene;
    GdkTexture **tile;
    unsigned char *pending;            /* changed since the last snapshot */
} Screen;

enum { MODE_CLOCK = NF_CLOCK, MODE_RAIN = NF_RAIN, MODE_MOSAIC = NF_MOSAIC };
static int g_mode = MODE_CLOCK;

static GtkApplication *g_app;
static Screen  g_screens[MAX_MON];
static int     g_nscreens;
static int     g_clock_screen;          /* which screen shows the clock */
static double  g_fx = -1, g_fy = -1;    /* clock position as a fraction of the free space */
static gint64  g_start_us;
static int     g_last_minute = -1;
static int     g_since_move;
static const char *g_debug;            /* NIGHTFALL_DEBUG=<dir>: log state + save PNGs of what we draw */

static void dbg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void dbg(const char *fmt, ...)
{
    if (!g_debug) return;
    va_list ap;
    va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    char *path = g_build_filename(g_debug, "nightfall.log", NULL);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%.3f %s\n", g_get_monotonic_time() / 1e6, m); fclose(f); }
    g_free(path); g_free(m);
}

static void debug_dump(const char *tag)
{
    if (!g_debug) return;
    for (int i = 0; i < g_nscreens; i++) {
        Screen *s = &g_screens[i];
        GtkWindow *w = GTK_WINDOW(s->window);
        int pw = gtk_widget_get_width(s->window), ph = gtk_widget_get_height(s->window);
        dbg("%s screen=%d size=%dx%d fullscreen=%d active=%d clock_here=%d", tag, i,
            pw, ph, gtk_window_is_fullscreen(w), gtk_window_is_active(w), i == g_clock_screen);
        if (s->scene) {
            int lit = 0;
            for (int ty = 0; ty < nf_scene_tile_rows(s->scene); ty++)
                for (int tx = 0; tx < nf_scene_tile_cols(s->scene); tx++) lit += nf_scene_tile_lit(s->scene, tx, ty);
            dbg("%s screen=%d lit_tiles=%d", tag, i, lit);
        }
        GdkPaintable *p = gtk_widget_paintable_new(s->window);
        GtkSnapshot *snap = gtk_snapshot_new();
        gdk_paintable_snapshot(p, snap, pw, ph);
        GskRenderNode *node = gtk_snapshot_free_to_node(snap);
        GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(s->window));
        if (node && r) {
            GdkTexture *t = gsk_renderer_render_texture(r, node, &GRAPHENE_RECT_INIT(0, 0, pw, ph));
            char name[64];
            g_snprintf(name, sizeof name, "%s-screen%d.png", tag, i);
            char *path = g_build_filename(g_debug, name, NULL);
            gdk_texture_save_to_png(t, path);
            g_free(path);
            g_object_unref(t);
        }
        if (node) gsk_render_node_unref(node);
        g_object_unref(p);
    }
}

static void queue_all(void)
{
    for (int i = 0; i < g_nscreens; i++) gtk_widget_queue_draw(g_screens[i].area);
}

static void move_clock(void)
{
    g_clock_screen = g_nscreens ? g_random_int_range(0, g_nscreens) : 0;
    g_fx = g_random_double();
    g_fy = g_random_double();
    queue_all();
}

/* ---------- digital rain ---------- */

static void rain_free_screen(Screen *s)
{
    for (int l = 0; l < RAIN_LEVELS; l++)
        for (int g = 0; g < RAIN_GLYPHS; g++) g_clear_object(&s->tex[l][g]);
    rain_free(&s->rain);
    mosaic_free(&s->mosaic);
    if (s->scene) {
        int n = nf_scene_tile_cols(s->scene) * nf_scene_tile_rows(s->scene);
        for (int i = 0; i < n; i++) g_clear_object(&s->tile[i]);
        nf_scene_free(s->scene);
        s->scene = NULL;
    }
    g_clear_pointer(&s->tile, g_free);
    g_clear_pointer(&s->pending, g_free);
}

/* Render every glyph at every brightness once, c x c device pixels each. */
static void make_textures(Screen *s, GtkWidget *widget)
{
    int c = s->cell;
    cairo_surface_t *cell = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, c, c);
    PangoLayout *l = gtk_widget_create_pango_layout(widget, NULL);
    PangoFontDescription *f = pango_font_description_from_string("Monospace");
    pango_font_description_set_absolute_size(f, c * 0.8 * PANGO_SCALE);
    pango_layout_set_font_description(l, f);
    for (int g = 0; g < RAIN_GLYPHS; g++) {
        char utf8[8] = {0};
        g_unichar_to_utf8(rain_glyph_cp(g), utf8);
        pango_layout_set_text(l, utf8, -1);
        int tw, th;
        pango_layout_get_pixel_size(l, &tw, &th);
        for (int lv = 1; lv < RAIN_LEVELS; lv++) {
            int r, gr, b;
            rain_color(lv, &r, &gr, &b);
            cairo_t *cr = cairo_create(cell);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_paint(cr);
            cairo_set_source_rgb(cr, r / 255.0, gr / 255.0, b / 255.0);
            cairo_move_to(cr, (c - tw) / 2.0, (c - th) / 2.0);
            pango_cairo_show_layout(cr, l);
            cairo_destroy(cr);
            cairo_surface_flush(cell);
            GBytes *bytes = g_bytes_new(cairo_image_surface_get_data(cell),
                                        (gsize)cairo_image_surface_get_stride(cell) * c);
            s->tex[lv][g] = gdk_memory_texture_new(c, c, GDK_MEMORY_DEFAULT, bytes,
                                                   cairo_image_surface_get_stride(cell));
            g_bytes_unref(bytes);
        }
    }
    pango_font_description_free(f);
    g_object_unref(l);
    cairo_surface_destroy(cell);
}

static double widget_scale(GtkWidget *w)
{
    GtkNative *n = gtk_widget_get_native(w);
    GdkSurface *surf = n ? gtk_native_get_surface(n) : NULL;
    double sc = surf ? gdk_surface_get_scale(surf) : 0;
    return sc > 0 ? sc : gtk_widget_get_scale_factor(w);
}

static void rain_snapshot(Screen *s, GtkWidget *widget, GtkSnapshot *snap)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    double scale = widget_scale(widget);
    if (w <= 0 || h <= 0) return;
    if (!s->rain.col || s->view_w != w || s->view_h != h || s->scale != scale) {
        rain_free_screen(s);
        s->scale = scale; s->view_w = w; s->view_h = h;
        int pw = (int)ceil(w * scale), ph = (int)ceil(h * scale);
        s->cell = ph / 36 > 14 ? ph / 36 : 14;
        make_textures(s, widget);
        rain_init(&s->rain, pw / s->cell + 1, ph / s->cell + 1, g_random_int());
        dbg("rain: %dx%d cells of %d device px, scale %.2f", s->rain.cols, s->rain.rows, s->cell, scale);
    }
    Rain *r = &s->rain;
    double c = s->cell / scale;
    for (int ci = 0; ci < r->cols; ci++) {
        RainCol *col = &r->col[ci];
        int from = col->head - col->len + 1, to = col->head;
        if (from < 0) from = 0;
        if (to >= r->rows) to = r->rows - 1;
        for (int row = from; row <= to; row++) {
            int lv = rain_level(col, row);
            if (!lv) continue;
            gtk_snapshot_append_texture(snap, s->tex[lv][r->glyph[row * r->cols + ci]],
                                        &GRAPHENE_RECT_INIT(ci * c, row * c, c, c));
        }
    }
}

/* ---------- gray mosaic ---------- */

static void mosaic_snapshot(Screen *s, GtkWidget *widget, GtkSnapshot *snap)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    double scale = widget_scale(widget);
    if (w <= 0 || h <= 0) return;
    if (!s->mosaic.cell || s->view_w != w || s->view_h != h || s->scale != scale) {
        mosaic_free(&s->mosaic);
        s->scale = scale; s->view_w = w; s->view_h = h;
        int pw = (int)ceil(w * scale), ph = (int)ceil(h * scale);
        mosaic_init(&s->mosaic, pw, ph, g_random_int());
        dbg("mosaic: %dx%d cells for %dx%d device px, scale %.2f", s->mosaic.cols, s->mosaic.rows, pw, ph, scale);
    }
    Mosaic *m = &s->mosaic;
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            int g = m->cell[r * m->cols + c].gray;
            if (!g) continue;
            GdkRGBA rgba = { g / 255.0f, g / 255.0f, g / 255.0f, 1 };
            gtk_snapshot_append_color(snap, &rgba,
                &GRAPHENE_RECT_INIT(m->xs[c] / scale, m->ys[r] / scale,
                                    (m->xs[c + 1] - m->xs[c]) / scale, (m->ys[r + 1] - m->ys[r]) / scale));
        }
}

/* ---------- scene styles ---------- */

static unsigned char *text_mask(const char *utf8, int px, int *w, int *h, void *ctx)
{
    PangoLayout *l = gtk_widget_create_pango_layout(GTK_WIDGET(ctx), utf8);
    PangoFontDescription *f = pango_font_description_from_string("Sans Light");
    pango_font_description_set_absolute_size(f, px * PANGO_SCALE);
    pango_layout_set_font_description(l, f);
    pango_font_description_free(f);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    tw = tw > 0 ? tw : 1; th = th > 0 ? th : 1;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, tw, th);
    cairo_t *cr = cairo_create(surf);
    pango_cairo_show_layout(cr, l);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    unsigned char *out = malloc((size_t)tw * th);
    if (out) {
        int stride = cairo_image_surface_get_stride(surf);
        const unsigned char *src = cairo_image_surface_get_data(surf);
        for (int y = 0; y < th; y++) memcpy(out + (size_t)y * tw, src + (size_t)y * stride, (size_t)tw);
    }
    cairo_surface_destroy(surf);
    g_object_unref(l);
    *w = tw; *h = th;
    return out;
}

static void scene_snapshot(Screen *s, GtkWidget *widget, GtkSnapshot *snap)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    double scale = widget_scale(widget);
    if (w <= 0 || h <= 0) return;
    if (!s->scene || s->view_w != w || s->view_h != h || s->scale != scale) {
        rain_free_screen(s);
        s->scale = scale; s->view_w = w; s->view_h = h;
        int pw = (int)ceil(w * scale), ph = (int)ceil(h * scale);
        s->scene = nf_scene_new(g_mode, pw, ph, NULL, g_random_int(), text_mask, widget);
        if (!s->scene) return;
        int n = nf_scene_tile_cols(s->scene) * nf_scene_tile_rows(s->scene);
        s->tile = g_new0(GdkTexture *, n);
        s->pending = g_new0(unsigned char, n);
        dbg("scene: %s %dx%d device px, scale %.2f, %d fps", nf_style_name(g_mode), pw, ph, scale, nf_scene_fps(s->scene));
        nf_scene_step(s->scene);
        memset(s->pending, 1, n);
    }
    NfScene *sc = s->scene;
    int tc = nf_scene_tile_cols(sc), tr = nf_scene_tile_rows(sc), pw = nf_scene_width(sc), ph = nf_scene_height(sc);
    const uint32_t *px = nf_scene_pixels(sc);
    for (int ty = 0; ty < tr; ty++)
        for (int tx = 0; tx < tc; tx++) {
            int i = ty * tc + tx;
            if (s->pending[i]) {
                s->pending[i] = 0;
                g_clear_object(&s->tile[i]);
                if (nf_scene_tile_lit(sc, tx, ty)) {
                    int x0 = tx * NF_TILE, y0 = ty * NF_TILE;
                    int tw = MIN(NF_TILE, pw - x0), th = MIN(NF_TILE, ph - y0);
                    guchar *buf = g_malloc((gsize)tw * th * 4);
                    for (int y = 0; y < th; y++) memcpy(buf + (gsize)y * tw * 4, px + (gsize)(y0 + y) * pw + x0, (gsize)tw * 4);
                    GBytes *b = g_bytes_new_take(buf, (gsize)tw * th * 4);
                    s->tile[i] = gdk_memory_texture_new(tw, th, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, b, (gsize)tw * 4);
                    g_bytes_unref(b);
                }
            }
            if (s->tile[i]) {
                int x0 = tx * NF_TILE, y0 = ty * NF_TILE;
                int tw = gdk_texture_get_width(s->tile[i]), th = gdk_texture_get_height(s->tile[i]);
                gtk_snapshot_append_texture(snap, s->tile[i],
                    &GRAPHENE_RECT_INIT(x0 / scale, y0 / scale, tw / scale, th / scale));
            }
        }
}

/* the animated styles draw into this widget */
typedef struct { GtkWidget parent; Screen *screen; } NfRainView;
typedef struct { GtkWidgetClass parent_class; } NfRainViewClass;
G_DEFINE_TYPE(NfRainView, nf_rain_view, GTK_TYPE_WIDGET)
static void nf_rain_view_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    if (g_mode >= NF_FIRST_SCENE) scene_snapshot(((NfRainView *)w)->screen, w, snap);
    else if (g_mode == MODE_MOSAIC) mosaic_snapshot(((NfRainView *)w)->screen, w, snap);
    else rain_snapshot(((NfRainView *)w)->screen, w, snap);
}
static void nf_rain_view_class_init(NfRainViewClass *k) { GTK_WIDGET_CLASS(k)->snapshot = nf_rain_view_snapshot; }
static void nf_rain_view_init(NfRainView *v) { (void)v; }

static gboolean rain_tick(gpointer data)
{
    (void)data;
    if (!g_nscreens) return G_SOURCE_REMOVE;
    for (int i = 0; i < g_nscreens; i++) {
        Screen *s = &g_screens[i];
        if (s->rain.col) rain_step(&s->rain);
        else if (s->mosaic.cell) mosaic_step(&s->mosaic);
        else if (s->scene) {
            if (!nf_scene_step(s->scene)) continue;          /* nothing changed: no redraw */
            int tc = nf_scene_tile_cols(s->scene), tr = nf_scene_tile_rows(s->scene);
            for (int ty = 0; ty < tr; ty++)
                for (int tx = 0; tx < tc; tx++)
                    if (nf_scene_tile_dirty(s->scene, tx, ty)) s->pending[ty * tc + tx] = 1;
        }
        else continue;
        gtk_widget_queue_draw(s->rainview);
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- clock ---------- */

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    Screen *s = data;
    (void)area;
    s->width = w; s->height = h;
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (s != &g_screens[g_clock_screen] || g_fx < 0) return;

    GDateTime *now = g_date_time_new_now_local();
    g_last_minute = g_date_time_get_minute(now);
    char *tstr = g_date_time_format(now, "%H:%M");
    char *dstr = g_date_time_format(now, "%A, %B %-d, %Y");
    g_date_time_unref(now);

    int time_px = h / 7 > 24 ? h / 7 : 24;
    PangoLayout *tl = gtk_widget_create_pango_layout(GTK_WIDGET(area), tstr);
    PangoLayout *dl = gtk_widget_create_pango_layout(GTK_WIDGET(area), dstr);
    PangoFontDescription *tf = pango_font_description_from_string("Sans Light");
    PangoFontDescription *df = pango_font_description_from_string("Sans");
    pango_font_description_set_absolute_size(tf, time_px * PANGO_SCALE);
    pango_font_description_set_weight(tf, PANGO_WEIGHT_LIGHT);
    pango_font_description_set_absolute_size(df, (time_px / 4) * PANGO_SCALE);
    pango_layout_set_font_description(tl, tf);
    pango_layout_set_font_description(dl, df);

    int tw, th, dw, dh;
    pango_layout_get_pixel_size(tl, &tw, &th);
    pango_layout_get_pixel_size(dl, &dw, &dh);
    int bw = (tw > dw ? tw : dw) + time_px;   /* slack so 9:59 -> 10:00 never clips */
    int bh = th + dh;
    double x = (w > bw ? (w - bw) * g_fx : 0);
    double y = (h > bh ? (h - bh) * g_fy : 0);

    cairo_set_source_rgb(cr, 78 / 255.0, 78 / 255.0, 84 / 255.0);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, tl);
    cairo_set_source_rgb(cr, 52 / 255.0, 52 / 255.0, 58 / 255.0);
    cairo_move_to(cr, x + time_px * 0.04, y + th);
    pango_cairo_show_layout(cr, dl);

    pango_font_description_free(tf);
    pango_font_description_free(df);
    g_object_unref(tl);
    g_object_unref(dl);
    g_free(tstr);
    g_free(dstr);
}

static gboolean tick(gpointer data)
{
    (void)data;
    if (g_debug && (g_since_move == 2 || g_since_move == MOVE_SEC - 1)) debug_dump(g_since_move == 2 ? "start" : "before-move");
    if (g_mode != MODE_CLOCK) { ++g_since_move; return G_SOURCE_CONTINUE; }
    if (++g_since_move >= MOVE_SEC) {
        g_since_move = 0;
        move_clock();
        dbg("clock moved to screen %d (%.2f, %.2f)", g_clock_screen, g_fx, g_fy);
    } else {
        GDateTime *now = g_date_time_new_now_local();
        if (g_date_time_get_minute(now) != g_last_minute) queue_all();
        g_date_time_unref(now);
    }
    return G_SOURCE_CONTINUE;
}

static void wake(void)
{
    if (g_get_monotonic_time() - g_start_us < GRACE_US) { dbg("input ignored (grace)"); return; }
    dbg("wake");
    for (int i = 0; i < g_nscreens; i++) {
        gtk_window_destroy(GTK_WINDOW(g_screens[i].window));
        rain_free_screen(&g_screens[i]);
    }
    g_nscreens = 0;
}

static gboolean on_key(GtkEventControllerKey *c, guint kv, guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)code; (void)st; (void)d;
    dbg("key %s", gdk_keyval_name(kv));
    wake();
    return TRUE;
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)n; (void)x; (void)y; (void)d;
    dbg("click button %u", gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)));
    wake();
}

static gboolean on_close_request(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    dbg("close-request from compositor");
    return FALSE;
}

static void add_screen(GdkMonitor *mon)
{
    if (g_nscreens >= MAX_MON) return;
    Screen *s = &g_screens[g_nscreens++];
    memset(s, 0, sizeof *s);
    s->window = gtk_application_window_new(g_app);
    gtk_window_set_title(GTK_WINDOW(s->window), "Nightfall");
    gtk_window_set_decorated(GTK_WINDOW(s->window), FALSE);
    gtk_widget_add_css_class(s->window, "nightfall");

    s->area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->area), draw, s, NULL);
    gtk_widget_set_cursor_from_name(s->area, "none");
    gtk_widget_set_cursor_from_name(s->window, "none");
    if (g_mode != MODE_CLOCK) {
        s->rainview = g_object_new(nf_rain_view_get_type(), NULL);
        ((NfRainView *)s->rainview)->screen = s;
        gtk_widget_set_cursor_from_name(s->rainview, "none");
        gtk_window_set_child(GTK_WINDOW(s->window), s->rainview);
    } else {
        gtk_window_set_child(GTK_WINDOW(s->window), s->area);
    }

    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(s->window, k);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);   /* any button */
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), NULL);
    gtk_widget_add_controller(s->window, GTK_EVENT_CONTROLLER(click));
    g_signal_connect(s->window, "close-request", G_CALLBACK(on_close_request), NULL);

    if (mon) gtk_window_fullscreen_on_monitor(GTK_WINDOW(s->window), mon);
    else gtk_window_fullscreen(GTK_WINDOW(s->window));
    gtk_window_present(GTK_WINDOW(s->window));
}

static void activate(GtkApplication *app, gpointer data)
{
    (void)data;
    if (g_nscreens) {             /* launched again while blanked: just re-raise */
        gtk_window_present(GTK_WINDOW(g_screens[0].window));
        return;
    }
    (void)app;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window.nightfall, window.nightfall * { background: black; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
    guint n = g_list_model_get_n_items(mons);
    for (guint i = 0; i < n; i++) {
        GdkMonitor *m = g_list_model_get_item(mons, i);
        add_screen(m);
        g_object_unref(m);
    }
    if (n == 0) add_screen(NULL);

    g_start_us = g_get_monotonic_time();
    move_clock();
    dbg("blank: %u monitor(s) mode=%s", n, nf_style_name(g_mode));
    g_timeout_add_seconds(1, tick, NULL);
    if (g_mode == MODE_RAIN) g_timeout_add(1000 / RAIN_FPS, rain_tick, NULL);
    if (g_mode == MODE_MOSAIC) g_timeout_add(1000 / MOSAIC_FPS, rain_tick, NULL);
    if (g_mode >= NF_FIRST_SCENE) {
        /* same frame rate as the scene; a probe scene tells it without knowing the size yet */
        NfScene *probe = nf_scene_new(g_mode, 64, 64, NULL, 1, NULL, NULL);
        int fps = probe ? nf_scene_fps(probe) : 15;
        nf_scene_free(probe);
        g_timeout_add(1000 / fps, rain_tick, NULL);
    }
}

#ifndef NIGHTFALL_VERSION
#define NIGHTFALL_VERSION "0.0.0"
#endif

static char *mode_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "nightfall", "mode", NULL);
}

static int parse_mode(const char *m)
{
    return m ? nf_style_from_name(m) : -1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && g_strcmp0(argv[1], "--version") == 0) {
        g_print("nightfall %s\n", NIGHTFALL_VERSION);
        return 0;
    }
    if (argc > 1 && g_strcmp0(argv[1], "--list") == 0) {
        for (int i = 0; i < NF_STYLE_COUNT; i++) g_print("%-10s %s\n", nf_style_name(i), nf_style_title(i));
        return 0;
    }
    if (argc > 1 && g_strcmp0(argv[1], "--set-mode") == 0) {
        if (argc < 3 || parse_mode(argv[2]) < 0) { g_printerr("usage: nightfall --set-mode STYLE (see --list)\n"); return 2; }
        char *path = mode_file(), *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0755);
        GError *err = NULL;
        if (!g_file_set_contents(path, argv[2], -1, &err)) { g_printerr("%s\n", err->message); return 1; }
        g_print("Nightfall will blank with: %s\n", nf_style_title(parse_mode(argv[2])));
        g_free(path); g_free(dir);
        return 0;
    }
    /* saved style, then a one-off --STYLE (removed before GApplication sees argv) */
    char *path = mode_file(), *saved = NULL;
    if (g_file_get_contents(path, &saved, NULL, NULL)) {
        int m = parse_mode(g_strstrip(saved));
        if (m >= 0) g_mode = m;
    }
    g_free(saved); g_free(path);
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        int m = g_str_has_prefix(argv[i], "--") ? nf_style_from_name(argv[i] + 2) : -1;
        if (m >= 0) g_mode = m;
        else argv[kept++] = argv[i];
    }
    argc = kept; argv[argc] = NULL;
    /* the clock is a black screen with a little text: software rendering, no GPU context.
     * The rain and mosaic animate a few hundred cells 15x a second, which GTK's GPU renderer composites
     * for a fraction of the CPU its software renderer needs. */
    if (g_mode == MODE_CLOCK || g_mode == NF_ANALOG || g_mode == NF_WORDS)   /* 1-2 frames a second */
        g_setenv("GSK_RENDERER", "cairo", FALSE);
    g_debug = g_getenv("NIGHTFALL_DEBUG");
    g_app = gtk_application_new("io.github.nightfall", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(g_app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(g_app), argc, argv);
    dbg("exit rc=%d", rc);
    g_object_unref(g_app);
    return rc;
}
