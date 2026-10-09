#pragma once

#include <gtk/gtk.h>
#include <functional>

// A GNOME Shell screenshot UI style capture panel (record only): a dimmed fullscreen overlay with a panel at the bottom
// to select what to record (selection, screen or window), toggle the cursor and start recording.

enum class CaptureMode {
    SELECTION,
    SCREEN,
    WINDOW
};

// In gdk coordinates, relative to the whole desktop
struct CaptureRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct CapturePanelOptions {
    bool selection_available = true;
    bool screen_available = true;
    bool window_available = true;
    // Text shown in the middle of the screen in window mode
    const char *window_hint = nullptr;
};

struct CapturePanelCallbacks {
    // Called after the overlay has been hidden. |area| is the selected region in selection mode and the selected monitor area in screen mode.
    // |monitor| is the monitor that contains |area|. Both are unset in window mode.
    std::function<void(CaptureMode mode, CaptureRect area, GdkMonitor *monitor)> record;
    std::function<void()> open_settings;
    std::function<void()> close;
    std::function<void(bool show_pointer)> show_pointer_changed;
};

void capture_panel_init(const CapturePanelOptions &options, const CapturePanelCallbacks &callbacks, CaptureMode mode, CaptureRect selection);
void capture_panel_show(bool show_pointer);
void capture_panel_hide();
bool capture_panel_is_visible();
CaptureMode capture_panel_get_mode();
CaptureRect capture_panel_get_selection();

// Small "recording" pill with the record time and a stop button, shown at the top of |monitor| while recording
void recording_indicator_show(GdkMonitor *monitor, std::function<void()> stop_callback);
void recording_indicator_hide();
void recording_indicator_set_text(const char *text);

// Converts gdk coordinates to display server coordinates (which is what gpu screen recorder expects for -region)
CaptureRect capture_rect_to_display_server(CaptureRect rect, GdkMonitor *monitor);

const char* capture_mode_to_string(CaptureMode mode);
bool capture_mode_from_string(const char *str, CaptureMode *mode);
