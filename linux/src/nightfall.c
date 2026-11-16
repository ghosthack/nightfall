/*
 * Nightfall for Linux (GTK4, Wayland or X11) - a tiny screen blanker.
 *
 *   nightfall      blank every monitor now; exits on wake
 *
 * While blanked: a black fullscreen window per monitor, cursor hidden, and a dim
 * clock that jumps to a random monitor/spot every 20 s (no burn-in).
 * Wake with any key or mouse click; moving the mouse alone does not wake it.
 * Nothing stays resident: launch it from the dock/app grid or the Ctrl+Alt+B
 * GNOME shortcut that install.sh sets up.
 */
#include <gtk/gtk.h>

#define GRACE_US   (800 * 1000)   /* ignore input right after blanking */
#define MOVE_SEC   20             /* move the clock this often */
#define MAX_MON    16

typedef struct {
    GtkWidget *window;
    GtkWidget *area;
    int width, height;
} Screen;

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
    for (int i = 0; i < g_nscreens; i++) gtk_window_destroy(GTK_WINDOW(g_screens[i].window));
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
    s->window = gtk_application_window_new(g_app);
    gtk_window_set_title(GTK_WINDOW(s->window), "Nightfall");
    gtk_window_set_decorated(GTK_WINDOW(s->window), FALSE);
    gtk_widget_add_css_class(s->window, "nightfall");

    s->area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->area), draw, s, NULL);
    gtk_widget_set_cursor_from_name(s->area, "none");
    gtk_widget_set_cursor_from_name(s->window, "none");
    gtk_window_set_child(GTK_WINDOW(s->window), s->area);

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
    dbg("blank: %u monitor(s)", n);
    g_timeout_add_seconds(1, tick, NULL);
}

int main(int argc, char **argv)
{
    /* software rendering: a black screen with a little text needs no GPU context */
    g_setenv("GSK_RENDERER", "cairo", FALSE);
    g_debug = g_getenv("NIGHTFALL_DEBUG");
    g_app = gtk_application_new("io.github.nightfall", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(g_app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(g_app), argc, argv);
    dbg("exit rc=%d", rc);
    g_object_unref(g_app);
    return rc;
}
