#include "capture_panel.hpp"

#include <gdk/gdkx.h>
#include <gdk/gdkwayland.h>
#include <math.h>
#include <string.h>
#include <vector>

#ifdef GSR_LAYER_SHELL
#include <gtk-layer-shell/gtk-layer-shell.h>
#endif

enum class PanelIcon {
    SELECTION,
    SCREEN,
    WINDOW,
    POINTER,
    SETTINGS,
    CLOSE,
    STOP
};

enum class DragAction {
    NONE,
    CREATE,
    MOVE,
    RESIZE
};

static const int EDGE_LEFT   = 1 << 0;
static const int EDGE_RIGHT  = 1 << 1;
static const int EDGE_TOP    = 1 << 2;
static const int EDGE_BOTTOM = 1 << 3;
static const int INSIDE      = 1 << 4;

// How close the cursor has to be to the edge of the selection to resize it
static const int SELECTION_GRAB_DISTANCE = 12;
// How far the cursor has to move before a click turns into a new selection
static const int SELECTION_DRAG_THRESHOLD = 3;
// Time to wait after hiding the overlay before recording starts, to make sure the overlay isn't visible in the recording
static const int RECORD_DELAY_MS = 250;

static const char *panel_css = R"css(
.gsr-transparent {
    background-color: transparent;
}

.gsr-panel {
    background-color: #1e1e1e;
    border-radius: 28px;
    padding: 12px;
    color: #ffffff;
}

.gsr-panel button,
.gsr-close-button,
.gsr-indicator button {
    background-image: none;
    background-color: transparent;
    border-style: none;
    border-radius: 12px;
    box-shadow: none;
    text-shadow: none;
    -gtk-icon-shadow: none;
    outline-style: none;
    color: #ffffff;
    min-width: 0;
    min-height: 0;
    padding: 0;
    transition: background-color 100ms ease-out;
}

.gsr-panel button label {
    color: #ffffff;
    font-size: 12px;
}

.gsr-panel button:hover {
    background-color: alpha(#ffffff, 0.07);
}

.gsr-panel button:active,
.gsr-panel button:checked {
    background-color: alpha(#ffffff, 0.12);
}

.gsr-panel button:checked:hover {
    background-color: alpha(#ffffff, 0.16);
}

.gsr-panel button:disabled,
.gsr-panel button:disabled label {
    background-color: transparent;
    color: alpha(#ffffff, 0.3);
}

.gsr-panel button.gsr-mode-button {
    padding: 10px 6px 8px 6px;
    min-width: 76px;
}

.gsr-panel button.gsr-small-button {
    border-radius: 10px;
    min-width: 40px;
    min-height: 32px;
}

.gsr-panel button.gsr-record-button,
.gsr-panel button.gsr-record-button:hover,
.gsr-panel button.gsr-record-button:active {
    background-color: transparent;
    border-radius: 9999px;
}

.gsr-close-button {
    background-color: #3a3a3a;
    border-radius: 9999px;
    min-width: 32px;
    min-height: 32px;
}

.gsr-close-button:hover {
    background-color: #4a4a4a;
}

.gsr-close-button:active {
    background-color: #575757;
}

.gsr-hint {
    background-color: alpha(#1e1e1e, 0.95);
    color: #ffffff;
    border-radius: 14px;
    padding: 14px 20px;
    font-size: 15px;
}

.gsr-indicator {
    background-color: alpha(#1e1e1e, 0.95);
    color: #ffffff;
    border-radius: 9999px;
    padding: 4px 4px 4px 12px;
}

.gsr-indicator label {
    color: #ffffff;
    font-weight: bold;
}

.gsr-indicator button {
    border-radius: 9999px;
    min-width: 28px;
    min-height: 28px;
    background-color: alpha(#ffffff, 0.1);
}

.gsr-indicator button:hover {
    background-color: alpha(#ffffff, 0.2);
}

.gsr-indicator button:active {
    background-color: alpha(#ffffff, 0.28);
}
)css";

struct Overlay {
    GtkWidget *window = nullptr;
    GtkWidget *canvas = nullptr;
    GdkMonitor *monitor = nullptr;
    GdkRectangle geometry = {0, 0, 0, 0};
    // Screenshot of the monitor, used as the background when the display server can't show transparent windows (X11 without a compositor)
    cairo_surface_t *backdrop = nullptr;
    const char *cursor_name = nullptr;
};

struct CapturePanel {
    bool initialized = false;
    CapturePanelOptions options;
    CapturePanelCallbacks callbacks;

    bool visible = false;
    std::vector<Overlay> overlays;
    GdkMonitor *panel_monitor = nullptr;
    GtkWidget *mode_buttons[3] = { nullptr, nullptr, nullptr };
    GtkWidget *window_hint = nullptr;
    bool updating_mode_buttons = false;

    CaptureMode mode = CaptureMode::SELECTION;
    CaptureRect selection;
    GdkMonitor *selected_monitor = nullptr;
    bool show_pointer = true;

    DragAction drag_action = DragAction::NONE;
    int drag_edges = 0;
    int drag_overlay = -1;
    bool drag_moved = false;
    double drag_start_x = 0.0;
    double drag_start_y = 0.0;
    CaptureRect drag_start_selection;

    guint record_timeout_id = 0;
    CaptureMode record_mode = CaptureMode::SELECTION;
    CaptureRect record_area;
    GdkMonitor *record_monitor = nullptr;
};

static CapturePanel panel;

struct RecordingIndicator {
    GtkWidget *window = nullptr;
    GtkWidget *label = nullptr;
    std::function<void()> stop_callback;
};

static RecordingIndicator indicator;

const char* capture_mode_to_string(CaptureMode mode) {
    switch(mode) {
        case CaptureMode::SELECTION: return "selection";
        case CaptureMode::SCREEN:    return "screen";
        case CaptureMode::WINDOW:    return "window";
    }
    return "selection";
}

bool capture_mode_from_string(const char *str, CaptureMode *mode) {
    if(strcmp(str, "selection") == 0) {
        *mode = CaptureMode::SELECTION;
        return true;
    } else if(strcmp(str, "screen") == 0) {
        *mode = CaptureMode::SCREEN;
        return true;
    } else if(strcmp(str, "window") == 0) {
        *mode = CaptureMode::WINDOW;
        return true;
    }
    return false;
}

CaptureRect capture_rect_to_display_server(CaptureRect rect, GdkMonitor *monitor) {
#ifdef GDK_WINDOWING_X11
    // Gdk coordinates on X11 are divided by the window scaling factor (GDK_SCALE), on Wayland they are already in the logical coordinates the compositor uses
    if(GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        const int scale = monitor ? gdk_monitor_get_scale_factor(monitor) : 1;
        rect.x *= scale;
        rect.y *= scale;
        rect.width *= scale;
        rect.height *= scale;
    }
#else
    (void)monitor;
#endif
    return rect;
}

static bool use_layer_shell() {
#ifdef GSR_LAYER_SHELL
    return gtk_layer_is_supported();
#else
    return false;
#endif
}

static void set_transparent_visual(GtkWidget *window) {
    GdkScreen *screen = gtk_widget_get_screen(window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if(visual && gdk_screen_is_composited(screen))
        gtk_widget_set_visual(window, visual);
    gtk_widget_set_app_paintable(window, true);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "gsr-transparent");
}

static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -G_PI_2, 0.0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0.0, G_PI_2);
    cairo_arc(cr, x + radius, y + height - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, G_PI * 1.5);
    cairo_close_path(cr);
}

// Icons are designed on a 16x16 grid, like symbolic icons
static void draw_icon(cairo_t *cr, PanelIcon icon, double size) {
    cairo_scale(cr, size / 16.0, size / 16.0);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch(icon) {
        case PanelIcon::SELECTION: {
            // Dashed rectangle with handles in the corners
            const double corners[2] = { 2.75, 13.25 };
            for(double y : corners) {
                for(double x : corners) {
                    cairo_rectangle(cr, x - 1.75, y - 1.75, 3.5, 3.5);
                }
            }
            cairo_fill(cr);

            cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
            const double dashes[2][2] = { { 5.75, 7.25 }, { 8.75, 10.25 } };
            for(const auto &dash : dashes) {
                for(double edge : corners) {
                    cairo_move_to(cr, dash[0], edge);
                    cairo_line_to(cr, dash[1], edge);
                    cairo_move_to(cr, edge, dash[0]);
                    cairo_line_to(cr, edge, dash[1]);
                }
            }
            cairo_stroke(cr);
            break;
        }
        case PanelIcon::SCREEN: {
            cairo_rounded_rectangle(cr, 1.75, 2.25, 12.5, 9.0, 1.5);
            cairo_move_to(cr, 8.0, 11.25);
            cairo_line_to(cr, 8.0, 13.75);
            cairo_move_to(cr, 5.0, 13.75);
            cairo_line_to(cr, 11.0, 13.75);
            cairo_stroke(cr);
            break;
        }
        case PanelIcon::WINDOW: {
            // Back window, hidden behind the front window
            cairo_save(cr);
            cairo_rectangle(cr, 0.0, 0.0, 16.0, 16.0);
            cairo_rectangle(cr, 0.25, 4.75, 12.0, 11.0);
            cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
            cairo_clip(cr);
            cairo_rounded_rectangle(cr, 5.25, 1.75, 9.0, 8.0, 1.5);
            cairo_stroke(cr);
            cairo_restore(cr);

            // Front window with a title bar
            cairo_rounded_rectangle(cr, 1.75, 6.25, 9.0, 8.0, 1.5);
            cairo_stroke(cr);
            cairo_rectangle(cr, 1.75, 6.25, 9.0, 2.5);
            cairo_fill(cr);
            break;
        }
        case PanelIcon::POINTER: {
            cairo_move_to(cr, 4.0, 1.5);
            cairo_line_to(cr, 4.0, 13.0);
            cairo_line_to(cr, 6.9, 10.4);
            cairo_line_to(cr, 8.8, 14.4);
            cairo_line_to(cr, 10.6, 13.6);
            cairo_line_to(cr, 8.7, 9.7);
            cairo_line_to(cr, 12.5, 9.7);
            cairo_close_path(cr);
            cairo_set_line_width(cr, 1.0);
            cairo_fill_preserve(cr);
            cairo_stroke(cr);
            break;
        }
        case PanelIcon::SETTINGS: {
            const int num_teeth = 8;
            const double step = 2.0 * G_PI / num_teeth;
            const double outer_radius = 7.25;
            const double inner_radius = 5.5;
            const double tooth_bevel = 0.07;

            cairo_new_path(cr);
            for(int i = 0; i < num_teeth; ++i) {
                const double tooth_start = i * step - step * 0.25;
                const double tooth_end = i * step + step * 0.25;
                cairo_arc(cr, 8.0, 8.0, inner_radius, tooth_start - step * 0.5, tooth_start);
                cairo_arc(cr, 8.0, 8.0, outer_radius, tooth_start + tooth_bevel, tooth_end - tooth_bevel);
                cairo_line_to(cr, 8.0 + cos(tooth_end) * inner_radius, 8.0 + sin(tooth_end) * inner_radius);
            }
            cairo_close_path(cr);
            cairo_new_sub_path(cr);
            cairo_arc(cr, 8.0, 8.0, 2.25, 0.0, 2.0 * G_PI);
            cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
            cairo_fill(cr);
            break;
        }
        case PanelIcon::CLOSE: {
            cairo_set_line_width(cr, 1.75);
            cairo_move_to(cr, 4.5, 4.5);
            cairo_line_to(cr, 11.5, 11.5);
            cairo_move_to(cr, 11.5, 4.5);
            cairo_line_to(cr, 4.5, 11.5);
            cairo_stroke(cr);
            break;
        }
        case PanelIcon::STOP: {
            cairo_rounded_rectangle(cr, 4.0, 4.0, 8.0, 8.0, 1.5);
            cairo_fill(cr);
            break;
        }
    }
}

static gboolean on_icon_draw(GtkWidget *widget, cairo_t *cr, gpointer userdata) {
    const PanelIcon icon = (PanelIcon)GPOINTER_TO_INT(userdata);

    // Use the color of the button so that the icon follows the button state (hover, disabled, etc)
    GtkWidget *color_source = gtk_widget_get_ancestor(widget, GTK_TYPE_BUTTON);
    if(!color_source)
        color_source = widget;

    GtkStyleContext *style_context = gtk_widget_get_style_context(color_source);
    GdkRGBA color;
    gtk_style_context_get_color(style_context, gtk_style_context_get_state(style_context), &color);

    const int width = gtk_widget_get_allocated_width(widget);
    const int height = gtk_widget_get_allocated_height(widget);
    const double size = MIN(width, height);
    cairo_translate(cr, floor((width - size) * 0.5), floor((height - size) * 0.5));
    gdk_cairo_set_source_rgba(cr, &color);
    draw_icon(cr, icon, size);
    return false;
}

static GtkWidget* create_icon(PanelIcon icon, int size) {
    GtkWidget *drawing_area = gtk_drawing_area_new();
    // Without a window of its own the drawing area doesn't steal input events from the button it's inside
    gtk_widget_set_has_window(drawing_area, false);
    gtk_widget_set_size_request(drawing_area, size, size);
    gtk_widget_set_halign(drawing_area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(drawing_area, GTK_ALIGN_CENTER);
    g_signal_connect(drawing_area, "draw", G_CALLBACK(on_icon_draw), GINT_TO_POINTER((int)icon));
    return drawing_area;
}

static gboolean on_record_button_draw(GtkWidget *widget, cairo_t *cr, gpointer) {
    GtkWidget *button = gtk_widget_get_ancestor(widget, GTK_TYPE_BUTTON);
    const GtkStateFlags state = button ? gtk_widget_get_state_flags(button) : GTK_STATE_FLAG_NORMAL;

    const double width = gtk_widget_get_allocated_width(widget);
    const double height = gtk_widget_get_allocated_height(widget);
    const double radius = MIN(width, height) * 0.5;
    const double opacity = (state & GTK_STATE_FLAG_INSENSITIVE) ? 0.3 : 1.0;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, opacity);
    cairo_set_line_width(cr, 3.0);
    cairo_arc(cr, width * 0.5, height * 0.5, radius - 1.5, 0.0, 2.0 * G_PI);
    cairo_stroke(cr);

    double dot_radius = radius - 7.0;
    if(state & GTK_STATE_FLAG_ACTIVE) {
        dot_radius -= 1.0;
        cairo_set_source_rgba(cr, 0.75, 0.11, 0.16, opacity);
    } else if(state & GTK_STATE_FLAG_PRELIGHT) {
        cairo_set_source_rgba(cr, 0.93, 0.2, 0.23, opacity);
    } else {
        cairo_set_source_rgba(cr, 0.88, 0.11, 0.14, opacity);
    }
    cairo_arc(cr, width * 0.5, height * 0.5, dot_radius, 0.0, 2.0 * G_PI);
    cairo_fill(cr);
    return false;
}

static void on_record_button_state_flags_changed(GtkWidget*, GtkStateFlags, gpointer userdata) {
    gtk_widget_queue_draw(GTK_WIDGET(userdata));
}

static bool is_mode_available(CaptureMode mode) {
    switch(mode) {
        case CaptureMode::SELECTION: return panel.options.selection_available;
        case CaptureMode::SCREEN:    return panel.options.screen_available;
        case CaptureMode::WINDOW:    return panel.options.window_available;
    }
    return false;
}

static void queue_draw_overlays() {
    for(Overlay &overlay : panel.overlays) {
        gtk_widget_queue_draw(overlay.canvas);
    }
}

static void set_mode(CaptureMode mode) {
    if(!is_mode_available(mode))
        return;

    panel.mode = mode;
    panel.drag_action = DragAction::NONE;

    panel.updating_mode_buttons = true;
    for(int i = 0; i < 3; ++i) {
        if(panel.mode_buttons[i])
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(panel.mode_buttons[i]), i == (int)mode);
    }
    panel.updating_mode_buttons = false;

    if(panel.window_hint)
        gtk_widget_set_visible(panel.window_hint, mode == CaptureMode::WINDOW && panel.options.window_hint);

    queue_draw_overlays();
}

static GdkMonitor* get_panel_monitor(GdkDisplay *display) {
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if(!monitor)
        monitor = gdk_display_get_monitor(display, 0);
    return monitor;
}

static bool display_has_monitor(GdkDisplay *display, GdkMonitor *monitor) {
    const int num_monitors = gdk_display_get_n_monitors(display);
    for(int i = 0; i < num_monitors; ++i) {
        if(gdk_display_get_monitor(display, i) == monitor)
            return true;
    }
    return false;
}

static GdkRectangle clamp_rect_to_area(GdkRectangle rect, const GdkRectangle &area) {
    rect.width = CLAMP(rect.width, 1, area.width);
    rect.height = CLAMP(rect.height, 1, area.height);
    rect.x = CLAMP(rect.x, area.x, area.x + area.width - rect.width);
    rect.y = CLAMP(rect.y, area.y, area.y + area.height - rect.height);
    return rect;
}

static GdkRectangle capture_rect_to_gdk_rect(const CaptureRect &rect) {
    return { rect.x, rect.y, rect.width, rect.height };
}

static CaptureRect gdk_rect_to_capture_rect(const GdkRectangle &rect) {
    CaptureRect capture_rect;
    capture_rect.x = rect.x;
    capture_rect.y = rect.y;
    capture_rect.width = rect.width;
    capture_rect.height = rect.height;
    return capture_rect;
}

// A selection is always inside a single monitor because gpu screen recorder can only record a region of one monitor
static void validate_selection(GdkDisplay *display) {
    GdkMonitor *monitor = nullptr;
    if(panel.selection.width > 0 && panel.selection.height > 0) {
        monitor = gdk_display_get_monitor_at_point(display,
            panel.selection.x + panel.selection.width / 2, panel.selection.y + panel.selection.height / 2);
    }

    GdkRectangle monitor_geometry;
    if(!monitor) {
        gdk_monitor_get_geometry(panel.panel_monitor, &monitor_geometry);
        panel.selection.width = monitor_geometry.width / 2;
        panel.selection.height = monitor_geometry.height / 2;
        panel.selection.x = monitor_geometry.x + (monitor_geometry.width - panel.selection.width) / 2;
        panel.selection.y = monitor_geometry.y + (monitor_geometry.height - panel.selection.height) / 2;
        return;
    }

    gdk_monitor_get_geometry(monitor, &monitor_geometry);
    panel.selection = gdk_rect_to_capture_rect(clamp_rect_to_area(capture_rect_to_gdk_rect(panel.selection), monitor_geometry));
}

static bool overlay_has_selection(const Overlay &overlay) {
    if(panel.selection.width <= 0 || panel.selection.height <= 0)
        return false;

    const int center_x = panel.selection.x + panel.selection.width / 2;
    const int center_y = panel.selection.y + panel.selection.height / 2;
    return center_x >= overlay.geometry.x && center_x < overlay.geometry.x + overlay.geometry.width
        && center_y >= overlay.geometry.y && center_y < overlay.geometry.y + overlay.geometry.height;
}

// Returns the area that is going to be recorded on this overlay, in overlay coordinates
static bool get_highlighted_area(const Overlay &overlay, GdkRectangle *area) {
    switch(panel.mode) {
        case CaptureMode::SELECTION: {
            if(!overlay_has_selection(overlay))
                return false;
            area->x = panel.selection.x - overlay.geometry.x;
            area->y = panel.selection.y - overlay.geometry.y;
            area->width = panel.selection.width;
            area->height = panel.selection.height;
            return true;
        }
        case CaptureMode::SCREEN: {
            if(overlay.monitor != panel.selected_monitor)
                return false;
            area->x = 0;
            area->y = 0;
            area->width = overlay.geometry.width;
            area->height = overlay.geometry.height;
            return true;
        }
        case CaptureMode::WINDOW: {
            return false;
        }
    }
    return false;
}

static gboolean on_canvas_draw(GtkWidget *widget, cairo_t *cr, gpointer userdata) {
    const int overlay_index = GPOINTER_TO_INT(userdata);
    if(overlay_index < 0 || overlay_index >= (int)panel.overlays.size())
        return false;

    const Overlay &overlay = panel.overlays[overlay_index];
    const int width = gtk_widget_get_allocated_width(widget);
    const int height = gtk_widget_get_allocated_height(widget);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if(overlay.backdrop)
        cairo_set_source_surface(cr, overlay.backdrop, 0.0, 0.0);
    else
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Dim everything except the area that is going to be recorded
    GdkRectangle area;
    const bool has_area = get_highlighted_area(overlay, &area);
    cairo_rectangle(cr, 0.0, 0.0, width, height);
    if(has_area)
        cairo_rectangle(cr, area.x, area.y, area.width, area.height);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
    cairo_fill(cr);

    if(!has_area)
        return false;

    if(panel.mode == CaptureMode::SELECTION) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, area.x + 0.5, area.y + 0.5, area.width - 1.0, area.height - 1.0);
        cairo_stroke(cr);

        const double handle_x[2] = { (double)area.x, (double)(area.x + area.width) };
        const double handle_y[2] = { (double)area.y, (double)(area.y + area.height) };
        for(double y : handle_y) {
            for(double x : handle_x) {
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.3);
                cairo_arc(cr, x, y, 7.0, 0.0, 2.0 * G_PI);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_arc(cr, x, y, 5.5, 0.0, 2.0 * G_PI);
                cairo_fill(cr);
            }
        }
    } else if(panel.mode == CaptureMode::SCREEN) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_set_line_width(cr, 4.0);
        cairo_rectangle(cr, area.x + 2.0, area.y + 2.0, area.width - 4.0, area.height - 4.0);
        cairo_stroke(cr);
    }

    return false;
}

// Returns a combination of EDGE_* or INSIDE, or 0 if the position (in gdk coordinates) is outside the selection
static int selection_hit_test(const Overlay &overlay, double x, double y) {
    if(!overlay_has_selection(overlay))
        return 0;

    const double left = panel.selection.x;
    const double right = panel.selection.x + panel.selection.width;
    const double top = panel.selection.y;
    const double bottom = panel.selection.y + panel.selection.height;

    if(x < left - SELECTION_GRAB_DISTANCE || x > right + SELECTION_GRAB_DISTANCE || y < top - SELECTION_GRAB_DISTANCE || y > bottom + SELECTION_GRAB_DISTANCE)
        return 0;

    int edges = 0;
    const double distance_left = fabs(x - left);
    const double distance_right = fabs(x - right);
    if(MIN(distance_left, distance_right) <= SELECTION_GRAB_DISTANCE)
        edges |= distance_left <= distance_right ? EDGE_LEFT : EDGE_RIGHT;

    const double distance_top = fabs(y - top);
    const double distance_bottom = fabs(y - bottom);
    if(MIN(distance_top, distance_bottom) <= SELECTION_GRAB_DISTANCE)
        edges |= distance_top <= distance_bottom ? EDGE_TOP : EDGE_BOTTOM;

    if(edges != 0)
        return edges;

    if(x > left && x < right && y > top && y < bottom)
        return INSIDE;

    return 0;
}

static const char* get_cursor_name(const Overlay &overlay, double x, double y) {
    switch(panel.mode) {
        case CaptureMode::SELECTION: {
            switch(selection_hit_test(overlay, x, y)) {
                case INSIDE:                   return "move";
                case EDGE_LEFT | EDGE_TOP:     return "nw-resize";
                case EDGE_RIGHT | EDGE_TOP:    return "ne-resize";
                case EDGE_LEFT | EDGE_BOTTOM:  return "sw-resize";
                case EDGE_RIGHT | EDGE_BOTTOM: return "se-resize";
                case EDGE_LEFT:                return "w-resize";
                case EDGE_RIGHT:               return "e-resize";
                case EDGE_TOP:                 return "n-resize";
                case EDGE_BOTTOM:              return "s-resize";
                default:                       return "crosshair";
            }
        }
        case CaptureMode::SCREEN: {
            return "pointer";
        }
        case CaptureMode::WINDOW: {
            return "default";
        }
    }
    return "default";
}

static void update_cursor(Overlay &overlay, const char *cursor_name) {
    if(overlay.cursor_name && strcmp(overlay.cursor_name, cursor_name) == 0)
        return;

    GdkWindow *gdk_window = gtk_widget_get_window(overlay.canvas);
    if(!gdk_window)
        return;

    overlay.cursor_name = cursor_name;
    GdkCursor *cursor = gdk_cursor_new_from_name(gtk_widget_get_display(overlay.canvas), cursor_name);
    gdk_window_set_cursor(gdk_window, cursor);
    if(cursor)
        g_object_unref(cursor);
}

static void update_drag(double x, double y) {
    if(panel.drag_overlay < 0 || panel.drag_overlay >= (int)panel.overlays.size())
        return;

    const GdkRectangle &area = panel.overlays[panel.drag_overlay].geometry;
    const double dx = x - panel.drag_start_x;
    const double dy = y - panel.drag_start_y;

    if(!panel.drag_moved && fabs(dx) < SELECTION_DRAG_THRESHOLD && fabs(dy) < SELECTION_DRAG_THRESHOLD)
        return;
    panel.drag_moved = true;

    x = CLAMP(x, area.x, area.x + area.width);
    y = CLAMP(y, area.y, area.y + area.height);

    switch(panel.drag_action) {
        case DragAction::NONE: {
            return;
        }
        case DragAction::CREATE: {
            const int left = (int)floor(MIN(panel.drag_start_x, x));
            const int top = (int)floor(MIN(panel.drag_start_y, y));
            const int right = (int)ceil(MAX(panel.drag_start_x, x));
            const int bottom = (int)ceil(MAX(panel.drag_start_y, y));
            panel.selection.x = left;
            panel.selection.y = top;
            panel.selection.width = MAX(1, right - left);
            panel.selection.height = MAX(1, bottom - top);
            break;
        }
        case DragAction::MOVE: {
            GdkRectangle moved = capture_rect_to_gdk_rect(panel.drag_start_selection);
            moved.x += (int)round(dx);
            moved.y += (int)round(dy);
            panel.selection = gdk_rect_to_capture_rect(clamp_rect_to_area(moved, area));
            break;
        }
        case DragAction::RESIZE: {
            const CaptureRect &start = panel.drag_start_selection;
            int left = start.x;
            int right = start.x + start.width;
            int top = start.y;
            int bottom = start.y + start.height;

            if(panel.drag_edges & EDGE_LEFT)
                left = (int)round(x);
            if(panel.drag_edges & EDGE_RIGHT)
                right = (int)round(x);
            if(panel.drag_edges & EDGE_TOP)
                top = (int)round(y);
            if(panel.drag_edges & EDGE_BOTTOM)
                bottom = (int)round(y);

            GdkRectangle resized;
            resized.x = MIN(left, right);
            resized.y = MIN(top, bottom);
            resized.width = MAX(1, abs(right - left));
            resized.height = MAX(1, abs(bottom - top));
            panel.selection = gdk_rect_to_capture_rect(clamp_rect_to_area(resized, area));
            break;
        }
    }

    queue_draw_overlays();
}

static gboolean on_canvas_button_press(GtkWidget*, GdkEventButton *event, gpointer userdata) {
    const int overlay_index = GPOINTER_TO_INT(userdata);
    if(event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY || overlay_index >= (int)panel.overlays.size())
        return false;

    Overlay &overlay = panel.overlays[overlay_index];
    const double x = overlay.geometry.x + event->x;
    const double y = overlay.geometry.y + event->y;

    switch(panel.mode) {
        case CaptureMode::SELECTION: {
            const int hit = selection_hit_test(overlay, x, y);
            panel.drag_overlay = overlay_index;
            panel.drag_moved = false;
            panel.drag_start_x = x;
            panel.drag_start_y = y;
            panel.drag_start_selection = panel.selection;
            panel.drag_edges = hit & ~INSIDE;
            if(hit == INSIDE)
                panel.drag_action = DragAction::MOVE;
            else if(hit != 0)
                panel.drag_action = DragAction::RESIZE;
            else
                panel.drag_action = DragAction::CREATE;
            break;
        }
        case CaptureMode::SCREEN: {
            panel.selected_monitor = overlay.monitor;
            queue_draw_overlays();
            break;
        }
        case CaptureMode::WINDOW: {
            break;
        }
    }
    return true;
}

static gboolean on_canvas_motion(GtkWidget*, GdkEventMotion *event, gpointer userdata) {
    const int overlay_index = GPOINTER_TO_INT(userdata);
    if(overlay_index >= (int)panel.overlays.size())
        return false;

    Overlay &overlay = panel.overlays[overlay_index];
    const double x = overlay.geometry.x + event->x;
    const double y = overlay.geometry.y + event->y;

    if(panel.drag_action != DragAction::NONE)
        update_drag(x, y);
    else
        update_cursor(overlay, get_cursor_name(overlay, x, y));
    return true;
}

static gboolean on_canvas_button_release(GtkWidget*, GdkEventButton *event, gpointer userdata) {
    const int overlay_index = GPOINTER_TO_INT(userdata);
    if(event->button != GDK_BUTTON_PRIMARY || overlay_index >= (int)panel.overlays.size())
        return false;

    if(panel.drag_action != DragAction::NONE) {
        Overlay &overlay = panel.overlays[overlay_index];
        update_drag(overlay.geometry.x + event->x, overlay.geometry.y + event->y);
        panel.drag_action = DragAction::NONE;
        panel.drag_overlay = -1;
    }
    return true;
}

static gboolean record_timeout_handler(gpointer) {
    panel.record_timeout_id = 0;
    GdkMonitor *monitor = panel.record_monitor;
    panel.record_monitor = nullptr;

    if(panel.callbacks.record)
        panel.callbacks.record(panel.record_mode, panel.record_area, monitor);

    if(monitor)
        g_object_unref(monitor);
    return G_SOURCE_REMOVE;
}

static void trigger_record() {
    if(!is_mode_available(panel.mode))
        return;

    CaptureRect area;
    GdkMonitor *monitor = nullptr;
    switch(panel.mode) {
        case CaptureMode::SELECTION: {
            if(panel.selection.width < 2 || panel.selection.height < 2)
                return;
            area = panel.selection;
            monitor = gdk_display_get_monitor_at_point(gdk_display_get_default(), area.x + area.width / 2, area.y + area.height / 2);
            break;
        }
        case CaptureMode::SCREEN: {
            monitor = panel.selected_monitor;
            if(!monitor || !display_has_monitor(gdk_display_get_default(), monitor))
                return;
            GdkRectangle geometry;
            gdk_monitor_get_geometry(monitor, &geometry);
            area = gdk_rect_to_capture_rect(geometry);
            break;
        }
        case CaptureMode::WINDOW: {
            break;
        }
    }

    panel.record_mode = panel.mode;
    panel.record_area = area;
    panel.record_monitor = monitor ? GDK_MONITOR(g_object_ref(monitor)) : nullptr;

    capture_panel_hide();
    panel.record_timeout_id = g_timeout_add(RECORD_DELAY_MS, record_timeout_handler, nullptr);
}

static void set_show_pointer(bool show_pointer) {
    panel.show_pointer = show_pointer;
    if(panel.callbacks.show_pointer_changed)
        panel.callbacks.show_pointer_changed(show_pointer);
}

static gboolean on_overlay_key_press(GtkWidget*, GdkEventKey *event, gpointer) {
    switch(event->keyval) {
        case GDK_KEY_Escape: {
            if(panel.callbacks.close)
                panel.callbacks.close();
            return true;
        }
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_space: {
            trigger_record();
            return true;
        }
        case GDK_KEY_s:
        case GDK_KEY_S: {
            set_mode(CaptureMode::SELECTION);
            return true;
        }
        case GDK_KEY_c:
        case GDK_KEY_C: {
            set_mode(CaptureMode::SCREEN);
            return true;
        }
        case GDK_KEY_w:
        case GDK_KEY_W: {
            set_mode(CaptureMode::WINDOW);
            return true;
        }
        case GDK_KEY_p:
        case GDK_KEY_P: {
            // The pointer button exists on the panel overlay only
            for(const Overlay &overlay : panel.overlays) {
                GtkWidget *pointer_button = (GtkWidget*)g_object_get_data(G_OBJECT(overlay.window), "pointer-button");
                if(pointer_button) {
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pointer_button), !panel.show_pointer);
                    return true;
                }
            }
            set_show_pointer(!panel.show_pointer);
            return true;
        }
    }
    return false;
}

static void on_mode_button_toggled(GtkToggleButton *button, gpointer userdata) {
    if(panel.updating_mode_buttons)
        return;

    const CaptureMode mode = (CaptureMode)GPOINTER_TO_INT(userdata);
    if(!gtk_toggle_button_get_active(button) && mode == panel.mode) {
        // The active mode can't be deselected, only another mode can be selected
        panel.updating_mode_buttons = true;
        gtk_toggle_button_set_active(button, true);
        panel.updating_mode_buttons = false;
        return;
    }

    set_mode(mode);
}

static void on_pointer_button_toggled(GtkToggleButton *button, gpointer) {
    set_show_pointer(gtk_toggle_button_get_active(button));
}

static void on_record_button_clicked(GtkButton*, gpointer) {
    trigger_record();
}

static void on_settings_button_clicked(GtkButton*, gpointer) {
    if(panel.callbacks.open_settings)
        panel.callbacks.open_settings();
}

static void on_close_button_clicked(GtkButton*, gpointer) {
    if(panel.callbacks.close)
        panel.callbacks.close();
}

static GtkWidget* create_panel_button(GtkWidget *button, const char *css_class, const char *tooltip) {
    gtk_widget_set_can_focus(button, false);
    gtk_widget_set_focus_on_click(button, false);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), css_class);
    if(tooltip)
        gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

static GtkWidget* create_mode_button(PanelIcon icon, const char *label, CaptureMode mode, const char *tooltip) {
    GtkWidget *button = create_panel_button(gtk_toggle_button_new(), "gsr-mode-button", tooltip);
    gtk_widget_set_sensitive(button, is_mode_available(mode));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), create_icon(icon, 24), false, false, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), false, false, 0);
    gtk_container_add(GTK_CONTAINER(button), box);

    g_signal_connect(button, "toggled", G_CALLBACK(on_mode_button_toggled), GINT_TO_POINTER((int)mode));
    panel.mode_buttons[(int)mode] = button;
    return button;
}

static GtkWidget* create_panel(GtkWidget *overlay_window) {
    // The outer overlay is only there to place the close button on the top right corner of the panel
    GtkWidget *panel_overlay = gtk_overlay_new();
    gtk_widget_set_halign(panel_overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(panel_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(panel_overlay, 48);

    GtkWidget *panel_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel_box), "gsr-panel");
    gtk_widget_set_margin_top(panel_box, 16);
    gtk_widget_set_margin_end(panel_box, 16);
    gtk_container_add(GTK_CONTAINER(panel_overlay), panel_box);

    GtkWidget *modes_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(modes_box), true);
    gtk_box_pack_start(GTK_BOX(modes_box), create_mode_button(PanelIcon::SELECTION, "Selection", CaptureMode::SELECTION, "Record a selected area (S)"), true, true, 0);
    gtk_box_pack_start(GTK_BOX(modes_box), create_mode_button(PanelIcon::SCREEN, "Screen", CaptureMode::SCREEN, "Record a whole screen (C)"), true, true, 0);
    gtk_box_pack_start(GTK_BOX(modes_box), create_mode_button(PanelIcon::WINDOW, "Window", CaptureMode::WINDOW, "Record a window (W)"), true, true, 0);
    gtk_box_pack_start(GTK_BOX(panel_box), modes_box, false, false, 0);

    GtkWidget *bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(panel_box), bottom_box, false, false, 0);

    GtkWidget *settings_button = create_panel_button(gtk_button_new(), "gsr-small-button", "Settings");
    gtk_container_add(GTK_CONTAINER(settings_button), create_icon(PanelIcon::SETTINGS, 18));
    gtk_widget_set_valign(settings_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(settings_button, 4);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(on_settings_button_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(bottom_box), settings_button, false, false, 0);

    GtkWidget *record_button = create_panel_button(gtk_button_new(), "gsr-record-button", "Start recording (Enter)");
    GtkWidget *record_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_has_window(record_drawing_area, false);
    gtk_widget_set_size_request(record_drawing_area, 56, 56);
    g_signal_connect(record_drawing_area, "draw", G_CALLBACK(on_record_button_draw), nullptr);
    g_signal_connect(record_button, "state-flags-changed", G_CALLBACK(on_record_button_state_flags_changed), record_drawing_area);
    gtk_container_add(GTK_CONTAINER(record_button), record_drawing_area);
    g_signal_connect(record_button, "clicked", G_CALLBACK(on_record_button_clicked), nullptr);
    gtk_box_set_center_widget(GTK_BOX(bottom_box), record_button);

    GtkWidget *pointer_button = create_panel_button(gtk_toggle_button_new(), "gsr-small-button", "Show pointer (P)");
    gtk_container_add(GTK_CONTAINER(pointer_button), create_icon(PanelIcon::POINTER, 18));
    gtk_widget_set_valign(pointer_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(pointer_button, 4);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pointer_button), panel.show_pointer);
    g_signal_connect(pointer_button, "toggled", G_CALLBACK(on_pointer_button_toggled), nullptr);
    gtk_box_pack_end(GTK_BOX(bottom_box), pointer_button, false, false, 0);
    g_object_set_data(G_OBJECT(overlay_window), "pointer-button", pointer_button);

    GtkWidget *close_button = create_panel_button(gtk_button_new(), "gsr-close-button", "Close (Esc)");
    gtk_container_add(GTK_CONTAINER(close_button), create_icon(PanelIcon::CLOSE, 16));
    gtk_widget_set_halign(close_button, GTK_ALIGN_END);
    gtk_widget_set_valign(close_button, GTK_ALIGN_START);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_button_clicked), nullptr);
    gtk_overlay_add_overlay(GTK_OVERLAY(panel_overlay), close_button);

    return panel_overlay;
}

static void create_overlay_window(int overlay_index, int monitor_index, bool has_panel, bool layer_shell) {
    Overlay &overlay = panel.overlays[overlay_index];

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    overlay.window = window;
    gtk_window_set_title(GTK_WINDOW(window), "GPU Screen Recorder");
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), true);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), true);
    gtk_window_set_default_size(GTK_WINDOW(window), overlay.geometry.width, overlay.geometry.height);
    set_transparent_visual(window);

    if(!gdk_screen_is_composited(gtk_widget_get_screen(window))) {
        GdkPixbuf *screenshot = gdk_pixbuf_get_from_window(gdk_get_default_root_window(),
            overlay.geometry.x, overlay.geometry.y, overlay.geometry.width, overlay.geometry.height);
        if(screenshot) {
            overlay.backdrop = gdk_cairo_surface_create_from_pixbuf(screenshot, gdk_monitor_get_scale_factor(overlay.monitor), nullptr);
            g_object_unref(screenshot);
        }
    }

#ifdef GSR_LAYER_SHELL
    if(layer_shell) {
        gtk_layer_init_for_window(GTK_WINDOW(window));
        gtk_layer_set_namespace(GTK_WINDOW(window), "gpu-screen-recorder");
        gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_monitor(GTK_WINDOW(window), overlay.monitor);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, true);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, true);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, true);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, true);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window), has_panel ? GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE : GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    }
#endif
    if(!layer_shell) {
        gtk_window_set_keep_above(GTK_WINDOW(window), true);
        gtk_window_move(GTK_WINDOW(window), overlay.geometry.x, overlay.geometry.y);
        gtk_window_fullscreen_on_monitor(GTK_WINDOW(window), gtk_widget_get_screen(window), monitor_index);
    }

    GtkWidget *window_overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(window), window_overlay);

    overlay.canvas = gtk_drawing_area_new();
    gtk_widget_set_hexpand(overlay.canvas, true);
    gtk_widget_set_vexpand(overlay.canvas, true);
    gtk_widget_add_events(overlay.canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(overlay.canvas, "draw", G_CALLBACK(on_canvas_draw), GINT_TO_POINTER(overlay_index));
    g_signal_connect(overlay.canvas, "button-press-event", G_CALLBACK(on_canvas_button_press), GINT_TO_POINTER(overlay_index));
    g_signal_connect(overlay.canvas, "button-release-event", G_CALLBACK(on_canvas_button_release), GINT_TO_POINTER(overlay_index));
    g_signal_connect(overlay.canvas, "motion-notify-event", G_CALLBACK(on_canvas_motion), GINT_TO_POINTER(overlay_index));
    gtk_container_add(GTK_CONTAINER(window_overlay), overlay.canvas);

    if(has_panel) {
        if(panel.options.window_hint) {
            panel.window_hint = gtk_label_new(panel.options.window_hint);
            gtk_style_context_add_class(gtk_widget_get_style_context(panel.window_hint), "gsr-hint");
            gtk_widget_set_halign(panel.window_hint, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(panel.window_hint, GTK_ALIGN_CENTER);
            gtk_widget_set_no_show_all(panel.window_hint, true);
            gtk_overlay_add_overlay(GTK_OVERLAY(window_overlay), panel.window_hint);
            gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(window_overlay), panel.window_hint, true);
        }

        gtk_overlay_add_overlay(GTK_OVERLAY(window_overlay), create_panel(window));
    }

    g_signal_connect(window, "key-press-event", G_CALLBACK(on_overlay_key_press), nullptr);
    gtk_widget_show_all(window);

    if(has_panel)
        gtk_window_present(GTK_WINDOW(window));
}

static void install_css() {
    GtkCssProvider *css_provider = gtk_css_provider_new();
    GError *error = nullptr;
    if(!gtk_css_provider_load_from_data(css_provider, panel_css, -1, &error)) {
        fprintf(stderr, "gsr error: failed to load capture panel css: %s\n", error ? error->message : "unknown error");
        if(error)
            g_error_free(error);
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);
}

void capture_panel_init(const CapturePanelOptions &options, const CapturePanelCallbacks &callbacks, CaptureMode mode, CaptureRect selection) {
    if(!panel.initialized) {
        panel.initialized = true;
        install_css();
    }

    panel.options = options;
    panel.callbacks = callbacks;
    panel.mode = mode;
    panel.selection = selection;
}

void capture_panel_show(bool show_pointer) {
    if(panel.visible)
        return;

    if(panel.record_timeout_id) {
        g_source_remove(panel.record_timeout_id);
        panel.record_timeout_id = 0;
        if(panel.record_monitor) {
            g_object_unref(panel.record_monitor);
            panel.record_monitor = nullptr;
        }
    }

    GdkDisplay *display = gdk_display_get_default();
    const int num_monitors = gdk_display_get_n_monitors(display);
    if(num_monitors <= 0) {
        fprintf(stderr, "gsr error: no monitors found to show the capture panel on\n");
        return;
    }

    panel.panel_monitor = get_panel_monitor(display);
    if(!panel.selected_monitor || !display_has_monitor(display, panel.selected_monitor))
        panel.selected_monitor = panel.panel_monitor;
    validate_selection(display);

    if(!is_mode_available(panel.mode)) {
        if(is_mode_available(CaptureMode::SELECTION))
            panel.mode = CaptureMode::SELECTION;
        else if(is_mode_available(CaptureMode::SCREEN))
            panel.mode = CaptureMode::SCREEN;
        else
            panel.mode = CaptureMode::WINDOW;
    }

    panel.show_pointer = show_pointer;
    panel.drag_action = DragAction::NONE;
    panel.visible = true;

    panel.overlays.clear();
    panel.overlays.resize(num_monitors);
    int panel_overlay_index = 0;
    for(int i = 0; i < num_monitors; ++i) {
        Overlay &overlay = panel.overlays[i];
        overlay.monitor = gdk_display_get_monitor(display, i);
        gdk_monitor_get_geometry(overlay.monitor, &overlay.geometry);
        if(overlay.monitor == panel.panel_monitor)
            panel_overlay_index = i;
    }

    const bool layer_shell = use_layer_shell();
    for(int i = 0; i < num_monitors; ++i) {
        // The panel overlay is created last so that it gets keyboard focus
        if(i != panel_overlay_index)
            create_overlay_window(i, i, false, layer_shell);
    }
    create_overlay_window(panel_overlay_index, panel_overlay_index, true, layer_shell);

    set_mode(panel.mode);
}

static gboolean destroy_windows_idle(gpointer userdata) {
    std::vector<GtkWidget*> *windows = (std::vector<GtkWidget*>*)userdata;
    for(GtkWidget *window : *windows) {
        gtk_widget_destroy(window);
    }
    delete windows;
    return G_SOURCE_REMOVE;
}

void capture_panel_hide() {
    if(!panel.visible)
        return;

    panel.visible = false;
    panel.drag_action = DragAction::NONE;
    panel.drag_overlay = -1;

    // The windows are hidden right away but destroyed later since this can be called from a signal handler of a widget inside them
    std::vector<GtkWidget*> *windows = new std::vector<GtkWidget*>();
    for(Overlay &overlay : panel.overlays) {
        gtk_widget_hide(overlay.window);
        windows->push_back(overlay.window);
        if(overlay.backdrop)
            cairo_surface_destroy(overlay.backdrop);
    }
    panel.overlays.clear();
    for(GtkWidget *&mode_button : panel.mode_buttons) {
        mode_button = nullptr;
    }
    panel.window_hint = nullptr;
    g_idle_add(destroy_windows_idle, windows);
}

bool capture_panel_is_visible() {
    return panel.visible;
}

CaptureMode capture_panel_get_mode() {
    return panel.mode;
}

CaptureRect capture_panel_get_selection() {
    return panel.selection;
}

static gboolean on_indicator_dot_draw(GtkWidget *widget, cairo_t *cr, gpointer) {
    const double width = gtk_widget_get_allocated_width(widget);
    const double height = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 0.88, 0.11, 0.14);
    cairo_arc(cr, width * 0.5, height * 0.5, MIN(width, height) * 0.5, 0.0, 2.0 * G_PI);
    cairo_fill(cr);
    return false;
}

static void on_indicator_stop_clicked(GtkButton*, gpointer) {
    // Copy since the callback can hide (and reset) the indicator
    std::function<void()> stop_callback = indicator.stop_callback;
    if(stop_callback)
        stop_callback();
}

static void on_indicator_size_allocate(GtkWidget *window, GdkRectangle *allocation, gpointer userdata) {
    GdkMonitor *monitor = GDK_MONITOR(userdata);
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    gtk_window_move(GTK_WINDOW(window), geometry.x + (geometry.width - allocation->width) / 2, geometry.y + 8);
}

static void unref_monitor(gpointer monitor, GClosure*) {
    g_object_unref(monitor);
}

void recording_indicator_show(GdkMonitor *monitor, std::function<void()> stop_callback) {
    recording_indicator_hide();
    indicator.stop_callback = std::move(stop_callback);

    if(!monitor)
        monitor = get_panel_monitor(gdk_display_get_default());

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    indicator.window = window;
    gtk_window_set_title(GTK_WINDOW(window), "GPU Screen Recorder");
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    gtk_window_set_resizable(GTK_WINDOW(window), false);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), true);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), true);
    gtk_window_set_accept_focus(GTK_WINDOW(window), false);
    gtk_window_set_focus_on_map(GTK_WINDOW(window), false);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_UTILITY);
    set_transparent_visual(window);

    const bool layer_shell = use_layer_shell();
#ifdef GSR_LAYER_SHELL
    if(layer_shell) {
        gtk_layer_init_for_window(GTK_WINDOW(window));
        gtk_layer_set_namespace(GTK_WINDOW(window), "gpu-screen-recorder-indicator");
        gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_TOP);
        if(monitor)
            gtk_layer_set_monitor(GTK_WINDOW(window), monitor);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, true);
        gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, 8);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    }
#endif
    if(!layer_shell) {
        gtk_window_set_keep_above(GTK_WINDOW(window), true);
        if(monitor)
            g_signal_connect_data(window, "size-allocate", G_CALLBACK(on_indicator_size_allocate), g_object_ref(monitor), unref_monitor, (GConnectFlags)0);
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "gsr-indicator");
    gtk_container_add(GTK_CONTAINER(window), box);

    GtkWidget *dot = gtk_drawing_area_new();
    gtk_widget_set_size_request(dot, 10, 10);
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    g_signal_connect(dot, "draw", G_CALLBACK(on_indicator_dot_draw), nullptr);
    gtk_box_pack_start(GTK_BOX(box), dot, false, false, 0);

    indicator.label = gtk_label_new("00:00:00");
    gtk_box_pack_start(GTK_BOX(box), indicator.label, false, false, 0);

    GtkWidget *stop_button = gtk_button_new();
    gtk_widget_set_can_focus(stop_button, false);
    gtk_widget_set_tooltip_text(stop_button, "Stop recording");
    gtk_container_add(GTK_CONTAINER(stop_button), create_icon(PanelIcon::STOP, 16));
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_indicator_stop_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(box), stop_button, false, false, 0);

    gtk_widget_show_all(window);
}

void recording_indicator_hide() {
    indicator.stop_callback = nullptr;
    indicator.label = nullptr;
    if(indicator.window) {
        gtk_widget_hide(indicator.window);
        g_idle_add(destroy_windows_idle, new std::vector<GtkWidget*>{ indicator.window });
        indicator.window = nullptr;
    }
}

void recording_indicator_set_text(const char *text) {
    if(indicator.label)
        gtk_label_set_text(GTK_LABEL(indicator.label), text);
}
