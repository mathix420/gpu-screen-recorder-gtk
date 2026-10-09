#include "config.hpp"
#include "capture_panel.hpp"
extern "C" {
#include "global_shortcuts.h"
}
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkwayland.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <assert.h>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <functional>
#include <vector>
#include <libayatana-appindicator/app-indicator.h>

#define GSR_CURRENT_GLOBAL_HOTKEYS_CODE_VERSION 6

#ifndef GSR_VERSION
#define GSR_VERSION "unknown"
#endif

// Start/stop recording also means start/stop streaming and start/stop replay
#define SHORTCUT_ID_START_STOP_RECORDING    "gpu_screen_recorder_start_stop_recording"
#define SHORTCUT_ID_PAUSE_UNPAUSE_RECORDING "gpu_screen_recorder_pause_unpause_recording"
#define SHORTCUT_ID_SAVE_REPLAY             "gpu_screen_recorder_save_replay"

typedef struct {
    Display *display;
    GtkApplication *app;
    GtkButton *select_window_button;
    Window selected_window;
} SelectWindowUserdata;

typedef struct {
    GtkApplication *app;
    GtkStack *stack;
    GtkWidget *common_settings_page;
    GtkWidget *replay_page;
    GtkWidget *recording_page;
    GtkWidget *streaming_page;
} PageNavigationUserdata;

static bool window_hidden = true; // The settings window, the capture panel is shown on startup instead
static GtkWidget *window;
static SelectWindowUserdata select_window_userdata;
static PageNavigationUserdata page_navigation_userdata;
static Cursor crosshair_cursor;
static GtkSpinButton *fps_entry;
static GtkSpinButton *video_bitrate_entry;
static GtkGrid *area_size_grid;
static GtkWidget *change_video_resolution_button;
static GtkGrid *video_resolution_grid;
static GtkSpinButton *area_width_entry;
static GtkSpinButton *area_height_entry;
static GtkSpinButton *video_width_entry;
static GtkSpinButton *video_height_entry;
static GtkComboBox *record_area_selection_menu;
static GtkTreeModel *record_area_selection_model;
static GtkComboBoxText *quality_input_menu;
static GtkComboBoxText *audio_codec_input_menu;
static GtkComboBoxText *color_range_input_menu;
static GtkComboBoxText *framerate_mode_input_menu;
static GtkComboBoxText *stream_service_input_menu;
static GtkComboBoxText *record_container;
static GtkComboBoxText *replay_container;
static GtkComboBoxText *custom_stream_container;
static GtkComboBox *video_codec_selection_menu;
static GtkTreeModel *video_codec_selection_model;
static GtkLabel *stream_key_label;
static GtkButton *record_file_chooser_button;
static GtkButton *replay_file_chooser_button;
static GtkButton *stream_button;
static GtkButton *record_button;
static GtkButton *replay_button;
static GtkButton *replay_back_button;
static GtkButton *record_back_button;
static GtkButton *stream_back_button;
static GtkButton *replay_save_button;
static GtkButton *start_recording_button;
static GtkButton *pause_recording_button;
static GtkButton *start_replay_button;
static GtkButton *start_streaming_button;
static GtkEntry *youtube_stream_id_entry;
static GtkEntry *twitch_stream_id_entry;
static GtkEntry *custom_stream_url_entry;
static GtkSpinButton *replay_time_entry;
static GtkButton *select_window_button;
static GtkBox *audio_devices_items_box;
static GtkWidget *record_start_stop_hotkey_button;
static GtkWidget *pause_unpause_hotkey_button;
static GtkWidget *replay_start_stop_hotkey_button;
static GtkWidget *replay_save_hotkey_button;
static GtkWidget *streaming_start_stop_hotkey_button;
static GtkWidget *record_app_audio_inverted_button;
static GtkWidget *split_audio_button;
static GtkFrame *notifications_frame;
static GtkFrame *general_frame;
static GtkWidget *show_recording_started_notification_button;
static GtkWidget *show_recording_stopped_notification_button;
static GtkWidget *show_recording_saved_notification_button;
static GtkWidget *exclude_metadata_button;
static GtkWidget *record_cursor_button;
static GtkWidget *restore_portal_session_button;
static GtkGrid *audio_devices_grid;
static GtkWidget *add_application_audio_button;
static GtkWidget *add_custom_application_audio_button;
static GtkGrid *video_codec_grid;
static GtkGrid *audio_codec_grid;
static GtkGrid *color_range_grid;
static GtkGrid *framerate_mode_grid;
static GtkGrid *video_bitrate_grid;
static GtkComboBoxText *view_combo_box;
static GtkGrid *recording_bottom_panel_grid;
static GtkWidget *recording_record_time_label;
static GtkWidget *recording_record_icon;
static GtkGrid *custom_stream_container_grid;
static GtkGrid *streaming_bottom_panel_grid;
static GtkWidget *streaming_record_time_label;
static GtkGrid *replay_bottom_panel_grid;
static GtkWidget *replay_record_time_label;
static GtkWidget *show_hide_menu_item;
static GtkWidget *recording_menu_separator;
static GtkWidget *start_stop_streaming_menu_item;
static GtkWidget *start_stop_recording_menu_item;
static GtkWidget *pause_recording_menu_item;
static GtkWidget *start_stop_replay_menu_item;
static GtkWidget *save_replay_menu_item;
static GtkWidget *hide_window_when_recording_menu_item;
static GtkGrid *recording_hotkeys_grid;
static GtkGrid *replay_hotkeys_grid;
static GtkGrid *streaming_hotkeys_grid;
static GtkWidget *recording_hotkeys_not_supported_label;
static GtkWidget *replay_hotkeys_not_supported_label;
static GtkWidget *streaming_hotkeys_not_supported_label;

static double record_start_time_sec = 0.0;
static double pause_start_sec = 0.0;
static double paused_time_offset_sec = 0.0;

static XIM xim;
static XIC xic;

static bool replaying = false;
static bool recording = false;
static bool paused = false;
static bool streaming = false;
static pid_t gpu_screen_recorder_process = -1;
static int prev_exit_status = -1;
static bool config_empty = false;
static Config config;
static std::string record_file_current_filename;
static bool nvfbc_installed = false;

static Display *dpy = NULL;
static bool flatpak = false;

static bool showing_notification = false;
static double notification_timeout_seconds = 0.0;
static double notification_start_seconds = 0.0;

static AppIndicator *app_indicator;

static gsr_global_shortcuts global_shortcuts;
static bool global_shortcuts_initialized = false;

// Set while recording something that was selected in the capture panel
struct PanelRecording {
    bool active = false;
    std::string capture_target; // The record area menu id this matches ("region", "portal", "window" or a monitor name)
    std::string window_arg;     // gpu-screen-recorder -w value
    std::string region_arg;     // gpu-screen-recorder -region value, if needed
    bool restore_portal_session = true;
};
static PanelRecording panel_recording;
static bool app_activated = false;
static bool start_with_settings = false;
static guint quit_timeout_id = 0;

struct AudioInput {
    std::string name;
    std::string description;
};

static std::vector<AudioInput> audio_inputs;
static std::vector<std::string> application_audio;

enum class HotkeyMode {
    NoAction,
    NewHotkey,
    Record
};

static HotkeyMode hotkey_mode = HotkeyMode::NoAction;

typedef gboolean (*hotkey_trigger_handler)(GtkButton *button, gpointer userdata);
struct Hotkey {
    uint32_t modkey_mask = 0;
    KeySym keysym = None;
    GtkWidget *hotkey_entry = nullptr;
    GtkWidget *hotkey_active_label = nullptr;
    ConfigHotkey *config = nullptr;
    bool grab_success = false;
    GtkWidget *page = nullptr;
    hotkey_trigger_handler trigger_handler = nullptr;
    GtkButton *associated_button = nullptr;
    const char *shortcut_id = nullptr;
};

static Hotkey *current_hotkey = nullptr;
static Hotkey pressed_hotkey;
static Hotkey latest_hotkey;
static Hotkey streaming_start_stop_hotkey;
static Hotkey record_start_stop_hotkey;
static Hotkey pause_unpause_hotkey;
static Hotkey replay_start_stop_hotkey;
static Hotkey replay_save_hotkey;

static Hotkey *hotkeys[] = {
    &streaming_start_stop_hotkey,
    &record_start_stop_hotkey,
    &pause_unpause_hotkey,
    &replay_start_stop_hotkey,
    &replay_save_hotkey
};
static int num_hotkeys = 5;

struct SupportedVideoCodecs {
    bool h264 = false;
    bool h264_software = false;
    bool hevc = false;
    bool hevc_hdr = false;
    bool hevc_10bit = false;
    bool av1 = false;
    bool av1_hdr = false;
    bool av1_10bit = false;
    bool vp8 = false;
    bool vp9 = false;
};

struct vec2i {
    int x = 0;
    int y = 0;
};

struct GsrMonitor {
    std::string name;
    vec2i size;
};

struct SupportedCaptureOptions {
    bool window = false;
    bool focused = false;
    bool portal = false;
    bool region = false;
    std::vector<GsrMonitor> monitors;
};

enum class DisplayServer {
    UNKNOWN,
    X11,
    WAYLAND
};

struct SystemInfo {
    DisplayServer display_server = DisplayServer::UNKNOWN;
    bool supports_app_audio = false;
    bool is_steam_deck = false;
};

enum class GpuVendor {
    UNKNOWN,
    AMD,
    INTEL,
    NVIDIA,
    BROADCOM,
    APPLE
};

struct GpuInfo {
    GpuVendor vendor = GpuVendor::UNKNOWN;
};

struct GsrInfo {
    SystemInfo system_info;
    GpuInfo gpu_info;
    SupportedVideoCodecs supported_video_codecs;
    SupportedCaptureOptions supported_capture_options;
};

static GsrInfo gsr_info;

enum class GsrInfoExitStatus {
    OK,
    FAILED_TO_RUN_COMMAND,
    OPENGL_FAILED,
    NO_DRM_CARD
};

static GsrInfoExitStatus gsr_info_exit_status;

enum class WaylandCompositor {
    UNKNOWN,
    HYPRLAND,
    KDE_OLD, // kwin
    KDE_NEW  // kwin 6.7.4 or newer
};
static WaylandCompositor wayland_compositor = WaylandCompositor::UNKNOWN;

static bool wayland_compositor_is_kde(WaylandCompositor wayland_compositor) {
    return wayland_compositor == WaylandCompositor::KDE_OLD || wayland_compositor == WaylandCompositor::KDE_NEW;
}

static int make_version(int major, int minor, int patch) {
    return major * 10000 + minor * 100 + patch;
}

// Returns 0 if the kwin version couldn't be retrieved
static int kwin_get_version() {
    GDBusConnection *gdbus_con = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if(!gdbus_con) {
        fprintf(stderr, "Warning: kwin_get_version: failed to connect to the session dbus\n");
        return 0;
    }

    // KWin doesn't expose its version directly, the only way to get it is from this support information text
    GVariant *ret = g_dbus_connection_call_sync(gdbus_con, "org.kde.KWin", "/KWin", "org.kde.KWin", "supportInformation", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    if(!ret) {
        fprintf(stderr, "Warning: kwin_get_version: org.kde.KWin.supportInformation dbus call failed\n");
        g_object_unref(gdbus_con);
        return 0;
    }

    int version = 0;
    const char *support_information = NULL;
    g_variant_get(ret, "(&s)", &support_information);
    if(support_information) {
        const char *version_prefix = "KWin version: ";
        const char *version_str = strstr(support_information, version_prefix);
        int major = 0;
        int minor = 0;
        int patch = 0;
        if(version_str && sscanf(version_str + strlen(version_prefix), "%d.%d.%d", &major, &minor, &patch) >= 2)
            version = make_version(major, minor, patch);
    }

    if(version == 0)
        fprintf(stderr, "Warning: kwin_get_version: failed to find the kwin version in the kwin support information\n");

    g_variant_unref(ret);
    g_object_unref(gdbus_con);
    return version;
}

struct Container {
    const char *container_name;
    const char *file_extension;
};

static const Container supported_containers[] = {
    { "mp4", "mp4" },
    { "flv", "flv" },
    { "matroska", "mkv" }, // TODO: Default to this on amd/intel, add (Recommended on AMD/Intel)
    { "mov", "mov" },
    { "mpegts", "ts" },
    { "hls", "m3u8" }
};

// Dumb hacks below!! why dont these fking paths work outside flatpak.. except in kde. TODO: fix this!
static const char* get_tray_idle_icon_name() {
    if(flatpak)
        return "com.dec05eba.gpu_screen_recorder.tray-idle";
    else
        return "/usr/share/icons/hicolor/32x32/status/com.dec05eba.gpu_screen_recorder.tray-idle.png";
}

static const char* get_tray_recording_icon_name() {
    if(flatpak)
        return "com.dec05eba.gpu_screen_recorder.tray-recording";
    else
        return "/usr/share/icons/hicolor/32x32/status/com.dec05eba.gpu_screen_recorder.tray-recording.png";
}

static const char* get_tray_paused_icon_name() {
    if(flatpak)
        return "com.dec05eba.gpu_screen_recorder.tray-paused";
    else
        return "/usr/share/icons/hicolor/32x32/status/com.dec05eba.gpu_screen_recorder.tray-paused.png";
}

static bool is_program_installed(const StringView program_name) {
    const char *path = getenv("PATH");
    if(!path)
        return false;

    bool program_installed = false;
    char full_program_path[PATH_MAX];
    string_split_char(path, ':', [&](StringView line) -> bool {
        snprintf(full_program_path, sizeof(full_program_path), "%.*s/%.*s", (int)line.size, line.str, (int)program_name.size, program_name.str);
        if(access(full_program_path, F_OK) == 0) {
            program_installed = true;
            return false;
        }
        return true;
    });
    return program_installed;
}

static bool is_inside_flatpak(void) {
    return getenv("FLATPAK_ID") != NULL;
}

static bool is_pkexec_installed() {
    if(flatpak)
        return system("flatpak-spawn --host pkexec --version") == 0;
    else
        return is_program_installed({ "pkexec", 6 });
}

static bool flatpak_is_installed_as_system(void) {
    static bool installed_as_system = false;
    static bool checked = false;
    if(!checked) {
        checked = true;
        installed_as_system = system("flatpak-spawn --host flatpak run --system --command=pwd com.dec05eba.gpu_screen_recorder") == 0;
    }
    return installed_as_system;
}

static double clock_get_monotonic_seconds(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

static void show_window() {
    gtk_widget_show(window);
    gtk_window_present(GTK_WINDOW(window));
    gtk_menu_item_set_label(GTK_MENU_ITEM(show_hide_menu_item), "Hide settings");
    window_hidden = false;
}

static void hide_window() {
    gtk_widget_hide(window);
    gtk_menu_item_set_label(GTK_MENU_ITEM(show_hide_menu_item), "Show settings");
    window_hidden = true;
}

static void open_settings();
static void close_settings();
static void quit_application();

static void systray_show_hide_callback(GtkMenuItem*, gpointer) {
    if(window_hidden) {
        open_settings();
    } else {
        close_settings();
    }
}

static void hide_window_when_recording_systray_callback(GtkMenuItem*, gpointer) {
    config.main_config.hide_window_when_recording = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(hide_window_when_recording_menu_item));
}

static void set_label_selectable(gpointer data, gpointer) {
    GtkWidget *widget = GTK_WIDGET(data);
    if(GTK_IS_LABEL(widget))
        gtk_label_set_selectable(GTK_LABEL(widget), true);
}

static void set_dialog_selectable(GtkWidget *dialog) {
    GtkWidget *area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
    GList *children = gtk_container_get_children(GTK_CONTAINER(area));
    g_list_foreach(children, set_label_selectable, nullptr);
    g_list_free(children);
}

static void start_stop_streaming_menu_item_systray_callback(GtkMenuItem*, gpointer userdata);
static void start_stop_recording_systray_callback(GtkMenuItem*, gpointer userdata);
static void pause_recording_systray_callback(GtkMenuItem*, gpointer userdata);
static void start_stop_replay_systray_callback(GtkMenuItem*, gpointer userdata);
static void save_replay_systray_callback(GtkMenuItem*, gpointer userdata);

static void systray_exit_callback(GtkMenuItem*, gpointer) {
    quit_application();
}

enum class SystrayPage {
    FRONT,
    STREAMING,
    RECORDING,
    REPLAY
};

static GtkMenuShell* create_systray_menu(GtkApplication *app, SystrayPage systray_page) {
    GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());

    show_hide_menu_item = gtk_menu_item_new_with_label(window_hidden ? "Show settings" : "Hide settings");
    g_signal_connect(show_hide_menu_item, "activate", G_CALLBACK(systray_show_hide_callback), app);
    gtk_menu_shell_append(menu, show_hide_menu_item);

    {
        GtkMenuShell *options_menu = GTK_MENU_SHELL(gtk_menu_new());
        hide_window_when_recording_menu_item = gtk_check_menu_item_new_with_label("Hide window when recording starts");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hide_window_when_recording_menu_item), config.main_config.hide_window_when_recording);
        g_signal_connect(hide_window_when_recording_menu_item, "activate", G_CALLBACK(hide_window_when_recording_systray_callback), nullptr);
        gtk_menu_shell_append(options_menu, hide_window_when_recording_menu_item);

        GtkWidget *options_menu_item = gtk_menu_item_new_with_label("Options");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(options_menu_item), GTK_WIDGET(options_menu));
        gtk_menu_shell_append(menu, options_menu_item);
    }

    recording_menu_separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(menu, recording_menu_separator);

    start_stop_streaming_menu_item = gtk_menu_item_new_with_label("Start streaming");
    g_signal_connect(start_stop_streaming_menu_item, "activate", G_CALLBACK(start_stop_streaming_menu_item_systray_callback), app);
    gtk_menu_shell_append(menu, start_stop_streaming_menu_item);

    start_stop_recording_menu_item = gtk_menu_item_new_with_label("Start recording");
    g_signal_connect(start_stop_recording_menu_item, "activate", G_CALLBACK(start_stop_recording_systray_callback), app);
    gtk_menu_shell_append(menu, start_stop_recording_menu_item);

    pause_recording_menu_item = gtk_menu_item_new_with_label("Pause recording");
    g_signal_connect(pause_recording_menu_item, "activate", G_CALLBACK(pause_recording_systray_callback), app);
    gtk_menu_shell_append(menu, pause_recording_menu_item);

    start_stop_replay_menu_item = gtk_menu_item_new_with_label("Start replay");
    g_signal_connect(start_stop_replay_menu_item, "activate", G_CALLBACK(start_stop_replay_systray_callback), app);
    gtk_menu_shell_append(menu, start_stop_replay_menu_item);

    save_replay_menu_item = gtk_menu_item_new_with_label("Save replay");
    g_signal_connect(save_replay_menu_item, "activate", G_CALLBACK(save_replay_systray_callback), app);
    gtk_menu_shell_append(menu, save_replay_menu_item);

    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());

    GtkWidget *exit_menu_item = gtk_menu_item_new_with_label("Exit");
    g_signal_connect(exit_menu_item, "activate", G_CALLBACK(systray_exit_callback), nullptr);
    gtk_menu_shell_append(menu, exit_menu_item);

    gtk_widget_show_all(GTK_WIDGET(menu));
    gtk_widget_set_visible(recording_menu_separator, false);
    gtk_widget_set_visible(start_stop_streaming_menu_item, false);
    gtk_widget_set_visible(start_stop_recording_menu_item, false);
    gtk_widget_set_visible(pause_recording_menu_item, false);
    gtk_widget_set_visible(start_stop_replay_menu_item, false);
    gtk_widget_set_visible(save_replay_menu_item, false);

    switch(systray_page) {
        case SystrayPage::FRONT:
            break;
        case SystrayPage::STREAMING:
            gtk_widget_set_visible(recording_menu_separator, true);
            gtk_widget_set_visible(start_stop_streaming_menu_item, true);
            break;
        case SystrayPage::RECORDING:
            gtk_widget_set_visible(recording_menu_separator, true);
            gtk_widget_set_visible(start_stop_recording_menu_item, true);
            gtk_widget_set_visible(pause_recording_menu_item, true);
            gtk_widget_set_sensitive(pause_recording_menu_item, false);
            break;
        case SystrayPage::REPLAY:
            gtk_widget_set_visible(recording_menu_separator, true);
            gtk_widget_set_visible(start_stop_replay_menu_item, true);
            gtk_widget_set_visible(save_replay_menu_item, true);
            gtk_widget_set_sensitive(save_replay_menu_item, false);
            break;
    }
    return menu;
}

static void setup_systray(GtkApplication *app) {
    app_indicator = app_indicator_new("com.dec05eba.gpu_screen_recorder", "", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_icon_full(app_indicator, get_tray_idle_icon_name(), "Idle");
    // This triggers Gdk assert: gdk_window_thaw_toplevel_updates: assertion 'window->update_and_descendants_freeze_count > 0' failed,
    // dont know why but it works anyways
    app_indicator_set_title(app_indicator, "GPU Screen Recorder");
    app_indicator_set_status(app_indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(app_indicator, GTK_MENU(create_systray_menu(app, SystrayPage::FRONT)));
}

static AudioInput parse_audio_device_line(const std::string &line) {
    AudioInput audio_input;
    const size_t space_index = line.find('|');
    if(space_index == std::string::npos)
        return audio_input;

    const StringView audio_input_name = {line.c_str(), space_index};
    const StringView audio_input_description = {line.c_str() + space_index + 1, line.size() - (space_index + 1)};
    audio_input.name.assign(audio_input_name.str, audio_input_name.size);
    audio_input.description.assign(audio_input_description.str, audio_input_description.size);
    return audio_input;
}

static std::vector<AudioInput> get_audio_devices() {
    std::vector<AudioInput> inputs;

    FILE *f = popen("gpu-screen-recorder --list-audio-devices", "r");
    if(!f) {
        fprintf(stderr, "error: 'gpu-screen-recorder --list-audio-devices' failed\n");
        return inputs;
    }

    char output[16384];
    ssize_t bytes_read = fread(output, 1, sizeof(output) - 1, f);
    if(bytes_read < 0 || ferror(f)) {
        fprintf(stderr, "error: failed to read 'gpu-screen-recorder --list-audio-devices' output\n");
        pclose(f);
        return inputs;
    }
    output[bytes_read] = '\0';

    string_split_char(output, '\n', [&](StringView line) {
        const std::string line_str(line.str, line.size);
        inputs.push_back(parse_audio_device_line(line_str));
        return true;
    });

    return inputs;
}

static std::vector<std::string> get_application_audio() {
    std::vector<std::string> application_audio;

    FILE *f = popen("gpu-screen-recorder --list-application-audio", "r");
    if(!f) {
        fprintf(stderr, "error: 'gpu-screen-recorder --list-application-audio' failed\n");
        return application_audio;
    }

    char output[16384];
    ssize_t bytes_read = fread(output, 1, sizeof(output) - 1, f);
    if(bytes_read < 0 || ferror(f)) {
        fprintf(stderr, "error: failed to read 'gpu-screen-recorder --list-application-audio' output\n");
        pclose(f);
        return application_audio;
    }
    output[bytes_read] = '\0';

    string_split_char(output, '\n', [&](StringView line) {
        std::string line_str(line.str, line.size);
        application_audio.emplace_back(std::move(line_str));
        return true;
    });

    return application_audio;
}

static bool is_video_capture_option_enabled(const char *str) {
    bool enabled = true;

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        enabled = strcmp(str, "window") != 0 && strcmp(str, "focused") != 0;

    if(strcmp(str, "portal") == 0 && !gsr_info.supported_capture_options.portal)
        enabled = false;

    return enabled;
}

static bool is_video_codec_enabled(const char *str) {
    bool enabled = true;

    if(strcmp(str, "h264") == 0 && !gsr_info.supported_video_codecs.h264)
        enabled = false;

    if(strcmp(str, "h264_software") == 0 && !gsr_info.supported_video_codecs.h264_software)
        enabled = false;

    if(strcmp(str, "hevc") == 0 && !gsr_info.supported_video_codecs.hevc)
        enabled = false;

    if(strcmp(str, "hevc_hdr") == 0 && !gsr_info.supported_video_codecs.hevc_hdr)
        enabled = false;

    if(strcmp(str, "hevc_10bit") == 0 && !gsr_info.supported_video_codecs.hevc_10bit)
        enabled = false;

    if(strcmp(str, "av1") == 0 && !gsr_info.supported_video_codecs.av1)
        enabled = false;

    if(strcmp(str, "av1_hdr") == 0 && !gsr_info.supported_video_codecs.av1_hdr)
        enabled = false;

    if(strcmp(str, "av1_10bit") == 0 && !gsr_info.supported_video_codecs.av1_10bit)
        enabled = false;

    if(strcmp(str, "vp8") == 0 && !gsr_info.supported_video_codecs.vp8)
        enabled = false;

    if(strcmp(str, "vp9") == 0 && !gsr_info.supported_video_codecs.vp9)
        enabled = false;

    return enabled;
}

static std::string record_area_selection_menu_get_active_id() {
    std::string id_str;
    GtkTreeIter iter;
    if(!gtk_combo_box_get_active_iter(record_area_selection_menu, &iter))
        return id_str;

    gchar *id;
    gtk_tree_model_get(record_area_selection_model, &iter, 1, &id, -1);
    id_str = id;
    g_free(id);
    return id_str;
}

static void record_area_selection_menu_set_active_id(const gchar *id) {
    if(!is_video_capture_option_enabled(id))
        return;

    GtkTreeIter iter;
    if(!gtk_tree_model_get_iter_first(record_area_selection_model, &iter))
        return;

    do {
        gchar *row_id = nullptr;
        gtk_tree_model_get(record_area_selection_model, &iter, 1, &row_id, -1);

        const bool found_row = strcmp(row_id, id) == 0;
        g_free(row_id);
        if(found_row) {
            gtk_combo_box_set_active_iter(record_area_selection_menu, &iter);
            break;
        }
    } while(gtk_tree_model_iter_next(record_area_selection_model, &iter));
}

static std::string video_codec_selection_menu_get_active_id() {
    std::string id_str;
    GtkTreeIter iter;
    if(!gtk_combo_box_get_active_iter(video_codec_selection_menu, &iter))
        return id_str;

    gchar *id;
    gtk_tree_model_get(video_codec_selection_model, &iter, 1, &id, -1);
    id_str = id;
    g_free(id);
    return id_str;
}

static void video_codec_selection_menu_set_active_id(const gchar *id) {
    if(!is_video_codec_enabled(id))
        return;

    GtkTreeIter iter;
    if(!gtk_tree_model_get_iter_first(video_codec_selection_model, &iter))
        return;

    do {
        gchar *row_id = nullptr;
        gtk_tree_model_get(video_codec_selection_model, &iter, 1, &row_id, -1);

        const bool found_row = strcmp(row_id, id) == 0;
        g_free(row_id);
        if(found_row) {
            gtk_combo_box_set_active_iter(video_codec_selection_menu, &iter);
            break;
        }
    } while(gtk_tree_model_iter_next(video_codec_selection_model, &iter));
}

static void enable_stream_record_button_if_info_filled() {
    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND) {
        const std::string selected_window_area = record_area_selection_menu_get_active_id();
        if(strcmp(selected_window_area.c_str(), "window") == 0 && select_window_userdata.selected_window == None) {
            gtk_widget_set_sensitive(GTK_WIDGET(replay_button), false);
            gtk_widget_set_sensitive(GTK_WIDGET(record_button), false);
            gtk_widget_set_sensitive(GTK_WIDGET(stream_button), false);
            return;
        }
    }

    gtk_widget_set_sensitive(GTK_WIDGET(replay_button), true);
    gtk_widget_set_sensitive(GTK_WIDGET(record_button), true);
    gtk_widget_set_sensitive(GTK_WIDGET(stream_button), true);
}

static gboolean scroll_event_ignore(GtkWidget*, GdkEvent*, void*) {
    return TRUE;
}

// Return true from |callback_func| to continue to the next row
static void for_each_item_in_combo_box(GtkComboBox *combo_box, std::function<bool(gint row_index, const gchar *row_id, const gchar *row_text)> callback_func) {
    const int id_column = gtk_combo_box_get_id_column(GTK_COMBO_BOX(combo_box));
    const int text_column = gtk_combo_box_get_entry_text_column(GTK_COMBO_BOX(combo_box));

    GtkTreeModel *tree_model = gtk_combo_box_get_model(combo_box);
    GtkTreeIter tree_iter;
    if(!gtk_tree_model_get_iter_first(tree_model, &tree_iter))
        return;

    gint row_index = 0;
    do {
        gchar *row_id = nullptr;
        gtk_tree_model_get(tree_model, &tree_iter, id_column, &row_id, -1);

        gchar *row_text = nullptr;
        gtk_tree_model_get(tree_model, &tree_iter, text_column, &row_text, -1);

        bool cont = true;
        if(row_id && row_text)
            cont = callback_func(row_index, row_id, row_text);

        g_free(row_id);
        g_free(row_text);

        if(!cont)
            break;

        ++row_index;
    } while(gtk_tree_model_iter_next(tree_model, &tree_iter));
}

static gint combo_box_text_get_row_by_label(GtkComboBox *combo_box, const char *label, std::string &id) {
    gint found_index = -1;
    for_each_item_in_combo_box(combo_box, [&found_index, &label, &id](gint row_index, const gchar *row_id, const gchar *row_text) {
        if(strcmp(row_text, label) == 0) {
            id = row_id;
            found_index = row_index;
            return false;
        }
        return true;
    });
    return found_index;
}

static GtkWidget* create_audio_device_combo_box_row(const std::string &selected_row_text) {
    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    g_object_set_data(G_OBJECT(grid), "audio-track-type", (void*)"device");
    gtk_grid_set_column_spacing(grid, 10);
    gtk_widget_set_hexpand(GTK_WIDGET(grid), true);

    GtkLabel *label = GTK_LABEL(gtk_label_new("Device:"));
    gtk_grid_attach(grid, GTK_WIDGET(label), 0, 0, 1, 1);

    GtkComboBoxText *audio_device_combo_box = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(audio_device_combo_box, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    for(const auto &audio_input : audio_inputs) {
        gtk_combo_box_text_append(audio_device_combo_box, audio_input.name.c_str(), audio_input.description.c_str());
    }

    if(!audio_inputs.empty() && selected_row_text.empty()) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(audio_device_combo_box), 0);
    } else if(!selected_row_text.empty()) {
        std::string audio_id;
        const gint target_combo_box_index = combo_box_text_get_row_by_label(GTK_COMBO_BOX(audio_device_combo_box), selected_row_text.c_str(), audio_id);
        if(target_combo_box_index != -1)
            gtk_combo_box_set_active(GTK_COMBO_BOX(audio_device_combo_box), target_combo_box_index);
        else if(!audio_inputs.empty())
            gtk_combo_box_set_active(GTK_COMBO_BOX(audio_device_combo_box), 0);
    }

    gtk_widget_set_hexpand(GTK_WIDGET(audio_device_combo_box), true);
    gtk_grid_attach(grid, GTK_WIDGET(audio_device_combo_box), 1, 0, 1, 1);

    GtkButton *remove_button = GTK_BUTTON(gtk_button_new_with_label("Remove"));
    gtk_grid_attach(grid, GTK_WIDGET(remove_button), 2, 0, 1, 1);

    g_signal_connect(remove_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userdata){
        GtkGrid *grid = (GtkGrid*)userdata;
        gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(GTK_WIDGET(grid))), GTK_WIDGET(grid));
        return true;
    }), grid);

    return GTK_WIDGET(grid);
}

static GtkWidget* create_application_audio_combo_box_row(const std::string &selected_row_id) {
    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    g_object_set_data(G_OBJECT(grid), "audio-track-type", (void*)"app");
    gtk_grid_set_column_spacing(grid, 10);
    gtk_widget_set_hexpand(GTK_WIDGET(grid), true);

    GtkLabel *label = GTK_LABEL(gtk_label_new("Application:"));
    gtk_grid_attach(grid, GTK_WIDGET(label), 0, 0, 1, 1);

    GtkComboBoxText *application_audio_combo_box = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(application_audio_combo_box, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    for(const std::string &app_audio : application_audio) {
        gtk_combo_box_text_append(application_audio_combo_box, app_audio.c_str(), app_audio.c_str());
    }

    if(!application_audio.empty() && selected_row_id.empty())
        gtk_combo_box_set_active(GTK_COMBO_BOX(application_audio_combo_box), 0);
    else if(!selected_row_id.empty())
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(application_audio_combo_box), selected_row_id.c_str());

    gtk_widget_set_hexpand(GTK_WIDGET(application_audio_combo_box), true);
    gtk_grid_attach(grid, GTK_WIDGET(application_audio_combo_box), 1, 0, 1, 1);

    GtkButton *remove_button = GTK_BUTTON(gtk_button_new_with_label("Remove"));
    gtk_grid_attach(grid, GTK_WIDGET(remove_button), 2, 0, 1, 1);

    g_signal_connect(remove_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userdata){
        GtkGrid *grid = (GtkGrid*)userdata;
        gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(GTK_WIDGET(grid))), GTK_WIDGET(grid));
        return true;
    }), grid);

    return GTK_WIDGET(grid);
}

static GtkWidget* create_application_audio_custom_row(const std::string &text) {
    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    g_object_set_data(G_OBJECT(grid), "audio-track-type", (void*)"app-custom");
    gtk_grid_set_column_spacing(grid, 10);
    gtk_widget_set_hexpand(GTK_WIDGET(grid), true);

    GtkLabel *label = GTK_LABEL(gtk_label_new("Application:"));
    gtk_grid_attach(grid, GTK_WIDGET(label), 0, 0, 1, 1);

    GtkEntry *application_audio_entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(application_audio_entry), true);
    gtk_entry_set_text(application_audio_entry, text.c_str());
    gtk_grid_attach(grid, GTK_WIDGET(application_audio_entry), 1, 0, 1, 1);

    GtkButton *remove_button = GTK_BUTTON(gtk_button_new_with_label("Remove"));
    gtk_grid_attach(grid, GTK_WIDGET(remove_button), 2, 0, 1, 1);

    g_signal_connect(remove_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userdata){
        GtkGrid *grid = (GtkGrid*)userdata;
        gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(GTK_WIDGET(grid))), GTK_WIDGET(grid));
        return true;
    }), grid);

    return GTK_WIDGET(grid);
}

static bool is_directory(const char *filepath) {
    struct stat file_stat;
    memset(&file_stat, 0, sizeof(file_stat));
    int ret = stat(filepath, &file_stat);
    if(ret == 0)
        return S_ISDIR(file_stat.st_mode);
    return false;
}

static std::string get_date_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
    return str;
}

static void save_configs() {
    config.main_config.record_area_option = record_area_selection_menu_get_active_id();
    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND) {
        config.main_config.record_area_width = gtk_spin_button_get_value_as_int(area_width_entry);
        config.main_config.record_area_height = gtk_spin_button_get_value_as_int(area_height_entry);
    }
    config.main_config.video_width = gtk_spin_button_get_value_as_int(video_width_entry);
    config.main_config.video_height = gtk_spin_button_get_value_as_int(video_height_entry);
    config.main_config.fps = gtk_spin_button_get_value_as_int(fps_entry);
    config.main_config.video_bitrate = gtk_spin_button_get_value_as_int(video_bitrate_entry);
    config.main_config.merge_audio_tracks = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(split_audio_button));
    config.main_config.record_app_audio_inverted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(record_app_audio_inverted_button));
    config.main_config.change_video_resolution = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(change_video_resolution_button));

    config.main_config.audio_input.clear();
    gtk_container_foreach(GTK_CONTAINER(audio_devices_items_box), [](GtkWidget *widget, gpointer) {
        const char *audio_track_type = (const char*)g_object_get_data(G_OBJECT(widget), "audio-track-type");
        GtkWidget *row_item_widget = gtk_grid_get_child_at(GTK_GRID(widget), 1, 0);
        if(strcmp(audio_track_type, "device") == 0) {
            std::string audio_input_name = "device:";
            audio_input_name += gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(row_item_widget));
            config.main_config.audio_input.push_back(std::move(audio_input_name));
        } else if(strcmp(audio_track_type, "app") == 0) {
            std::string audio_input_name = "app:";
            audio_input_name += gtk_combo_box_get_active_id(GTK_COMBO_BOX(row_item_widget));
            config.main_config.audio_input.push_back(std::move(audio_input_name));
        } else if(strcmp(audio_track_type, "app-custom") == 0) {
            std::string audio_input_name = "app:";
            audio_input_name += gtk_entry_get_text(GTK_ENTRY(row_item_widget));
            config.main_config.audio_input.push_back(std::move(audio_input_name));
        }
    }, nullptr);

    config.main_config.color_range = gtk_combo_box_get_active_id(GTK_COMBO_BOX(color_range_input_menu));
    config.main_config.quality = gtk_combo_box_get_active_id(GTK_COMBO_BOX(quality_input_menu));
    config.main_config.codec = video_codec_selection_menu_get_active_id();
    config.main_config.audio_codec = gtk_combo_box_get_active_id(GTK_COMBO_BOX(audio_codec_input_menu));
    config.main_config.framerate_mode = gtk_combo_box_get_active_id(GTK_COMBO_BOX(framerate_mode_input_menu));
    config.main_config.advanced_view = strcmp(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view_combo_box)), "advanced") == 0;
    config.main_config.show_recording_started_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_started_notification_button));
    config.main_config.show_recording_stopped_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_stopped_notification_button));
    config.main_config.show_recording_saved_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_saved_notification_button));
    config.main_config.exclude_metadata = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(exclude_metadata_button));
    config.main_config.record_cursor = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(record_cursor_button));
    config.main_config.hide_window_when_recording = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(hide_window_when_recording_menu_item));
    config.main_config.restore_portal_session = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_portal_session_button));

    config.streaming_config.streaming_service = gtk_combo_box_get_active_id(GTK_COMBO_BOX(stream_service_input_menu));
    config.streaming_config.youtube.stream_key = gtk_entry_get_text(youtube_stream_id_entry);
    config.streaming_config.twitch.stream_key = gtk_entry_get_text(twitch_stream_id_entry);
    config.streaming_config.custom.url = gtk_entry_get_text(custom_stream_url_entry);
    config.streaming_config.custom.container = gtk_combo_box_get_active_id(GTK_COMBO_BOX(custom_stream_container));

    config.record_config.save_directory = gtk_button_get_label(record_file_chooser_button);
    config.record_config.container = gtk_combo_box_get_active_id(GTK_COMBO_BOX(record_container));

    config.replay_config.save_directory = gtk_button_get_label(replay_file_chooser_button);
    config.replay_config.container = gtk_combo_box_get_active_id(GTK_COMBO_BOX(replay_container));
    config.replay_config.replay_time = gtk_spin_button_get_value_as_int(replay_time_entry);

    config.main_config.capture_panel_mode = capture_mode_to_string(capture_panel_get_mode());
    const CaptureRect capture_panel_selection = capture_panel_get_selection();
    char capture_panel_selection_str[128];
    snprintf(capture_panel_selection_str, sizeof(capture_panel_selection_str), "%dx%d+%d+%d",
        capture_panel_selection.width, capture_panel_selection.height, capture_panel_selection.x, capture_panel_selection.y);
    config.main_config.capture_panel_selection = capture_panel_selection_str;

    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND) {
        for(int i = 0; i < num_hotkeys; ++i) {
            // This can also happen if we run multiple instances of gpu screen recorder, in which case it will fail to grab keys for the other windows.
            // We dont want to overwrite hotkeys in that case.
            if(hotkeys[i]->grab_success) {
                hotkeys[i]->config->keysym = hotkeys[i]->keysym;
                hotkeys[i]->config->modifiers = hotkeys[i]->modkey_mask;
            }
        }
    }

    save_config(config);
}

static void show_notification(GtkApplication *app, const char *title, const char *body, GNotificationPriority priority) {
    if(priority < G_NOTIFICATION_PRIORITY_URGENT) {
        notification_timeout_seconds = 3.0;
    } else {
        notification_timeout_seconds = 10.0;
    }

    // KDE doesn't show notifications when using desktop portal capture unless either DoNotDisturb.WhenScreenSharing kde config
    // has been changed by the user or if the priority for the notification is set as urgent
    if((recording || replaying || streaming) && wayland_compositor_is_kde(wayland_compositor))
        priority = G_NOTIFICATION_PRIORITY_URGENT;

    fprintf(stderr, "Notification: title: %s, body: %s\n", title, body);
    GNotification *notification = g_notification_new(title);
    g_notification_set_body(notification, body);
    g_notification_set_priority(notification, priority);
    g_application_send_notification(&app->parent, "gpu-screen-recorder", notification);

    notification_start_seconds = clock_get_monotonic_seconds();
    showing_notification = true;
}

static bool window_has_atom(Display *display, Window _window, Atom atom) {
    Atom type;
    unsigned long len, bytes_left;
    int format;
    unsigned char *properties = nullptr;
    if(XGetWindowProperty(display, _window, atom, 0, 1024, False, AnyPropertyType, &type, &format, &len, &bytes_left, &properties) < Success)
        return false;

    if(properties)
        XFree(properties);

    return type != None;
}

static Window window_get_target_window_child(Display *display, Window _window) {
    if(_window == None)
        return None;

    Atom wm_state_atom = XInternAtom(display, "_NET_WM_STATE", False);
    if(!wm_state_atom)
        return None;

    if(window_has_atom(display, _window, wm_state_atom))
        return _window;

    Window root;
    Window parent;
    Window *children = nullptr;
    unsigned int num_children = 0;
    if(!XQueryTree(display, _window, &root, &parent, &children, &num_children) || !children)
        return None;

    Window found_window = None;
    for(int i = num_children - 1; i >= 0; --i) {
        if(children[i] && window_has_atom(display, children[i], wm_state_atom)) {
            found_window = children[i];
            goto finished;
        }
    }

    for(int i = num_children - 1; i >= 0; --i) {
        if(children[i]) {
            Window win = window_get_target_window_child(display, children[i]);
            if(win) {
                found_window = win;
                goto finished;
            }
        }
    }

    finished:
    XFree(children);
    return found_window;
}

// Called once with the window that was clicked on (or None) after on_select_window_button_click
static std::function<void(Window)> window_selected_callback;

static void call_window_selected_callback(Window selected_window) {
    if(!window_selected_callback)
        return;

    std::function<void(Window)> callback = std::move(window_selected_callback);
    window_selected_callback = nullptr;
    callback(selected_window);
}

static GdkFilterReturn filter_callback(GdkXEvent *xevent, GdkEvent*, gpointer userdata) {
    SelectWindowUserdata *_select_window_userdata = (SelectWindowUserdata*)userdata;
    XEvent *ev = (XEvent*)xevent;
    //assert(ev->type == ButtonPress);
    if(ev->type != ButtonPress)
        return GDK_FILTER_CONTINUE;

    Window target_win = ev->xbutton.subwindow;
    // TODO: Fix, this is incorrect when trying to record steam window. For steam window
    Window new_window = window_get_target_window_child(_select_window_userdata->display, target_win);
    if(new_window)
        target_win = new_window;

    int status = XUngrabPointer(_select_window_userdata->display, CurrentTime);
    if(!status) {
        fprintf(stderr, "failed to ungrab pointer!\n");
        show_notification(_select_window_userdata->app, "GPU Screen Recorder", "Failed to ungrab pointer!", G_NOTIFICATION_PRIORITY_URGENT);
        exit(1);
    }

    if(target_win == None) {
        show_notification(_select_window_userdata->app, "GPU Screen Recorder", "No window selected!", G_NOTIFICATION_PRIORITY_URGENT);
        GdkScreen *screen = gdk_screen_get_default();
        GdkWindow *root_window = gdk_screen_get_root_window(screen);
        gdk_window_remove_filter(root_window, filter_callback, _select_window_userdata);
        call_window_selected_callback(None);
        return GDK_FILTER_REMOVE;
    }

    std::string window_name;
    XTextProperty wm_name_prop;
    if(XGetWMName(_select_window_userdata->display, target_win, &wm_name_prop) && wm_name_prop.nitems > 0) {
        char **list_return = NULL;
        int num_items = 0;
        int ret = XmbTextPropertyToTextList(_select_window_userdata->display, &wm_name_prop, &list_return, &num_items);
        if((ret == Success || ret > 0) && list_return) {
            for(int i = 0; i < num_items; ++i) {
                window_name += list_return[i];
            }
            XFreeStringList(list_return);
        } else {
            window_name += (char*)wm_name_prop.value;
        }
    } else {
        window_name += "(no name)";
    }

    fprintf(stderr, "window name: %s, window id: %ld\n", window_name.c_str(), target_win);
    gtk_button_set_label(_select_window_userdata->select_window_button, window_name.c_str());
    _select_window_userdata->selected_window = target_win;

    GdkScreen *screen = gdk_screen_get_default();
    GdkWindow *root_window = gdk_screen_get_root_window(screen);
    gdk_window_remove_filter(root_window, filter_callback, _select_window_userdata);

    enable_stream_record_button_if_info_filled();
    call_window_selected_callback(target_win);

    return GDK_FILTER_REMOVE;
}

static gboolean on_select_window_button_click(GtkButton *button, gpointer) {
    Display *display = gdk_x11_get_default_xdisplay();
    select_window_userdata.display = display;
    select_window_userdata.select_window_button = button;
    select_window_userdata.selected_window = None;

    GdkScreen *screen = gdk_screen_get_default();
    GdkWindow *root_window = gdk_screen_get_root_window(screen);
    gdk_window_set_events(root_window, GDK_BUTTON_PRESS_MASK);
    gdk_window_add_filter(root_window, filter_callback, &select_window_userdata);

    Window root = GDK_WINDOW_XID(root_window);

    /* Grab the pointer using target cursor, letting it room all over */
    int status = XGrabPointer(display, root, False,
                ButtonPressMask, GrabModeAsync,
                GrabModeAsync, root, crosshair_cursor, CurrentTime);
    if (status != GrabSuccess) {
        fprintf(stderr, "failed to grab pointer!\n");
        //GNotification *notification = g_notification_new("Failed to grab pointer to window selection!");
        //g_notification_set_priority(notification, G_NOTIFICATION_PRIORITY_HIGH);
        //g_application_send_notification(app, "select-window", notification);
    }

    return true;
}

static gboolean on_change_video_resolution_button_click(GtkButton *button, gpointer) {
    const bool clicked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
    const std::string window_str = record_area_selection_menu_get_active_id();
    gtk_widget_set_visible(GTK_WIDGET(video_resolution_grid), clicked && window_str != "focused");
    return true;
}

static bool key_is_modifier(KeySym key_sym) {
    return key_sym >= XK_Shift_L && key_sym <= XK_Super_R && key_sym != XK_Caps_Lock && key_sym != XK_Shift_Lock;
}

static uint32_t modkey_to_mask(KeySym key_sym) {
    assert(key_is_modifier(key_sym));
    return 1 << (key_sym - XK_Shift_L);
}

static uint32_t key_mod_mask_to_x11_mask(uint32_t mask) {
    uint32_t key_mod_masks = 0;
    if(mask & (modkey_to_mask(XK_Control_L) | modkey_to_mask(XK_Control_R)))
        key_mod_masks |= ControlMask;
    if(mask & (modkey_to_mask(XK_Alt_L) | modkey_to_mask(XK_Alt_R)))
        key_mod_masks |= Mod1Mask;
    if(mask & (modkey_to_mask(XK_Shift_L) | modkey_to_mask(XK_Shift_R)))
        key_mod_masks |= ShiftMask;
    if(mask & (modkey_to_mask(XK_Super_L) | modkey_to_mask(XK_Super_R) | modkey_to_mask(XK_Meta_L)| modkey_to_mask(XK_Meta_R)))
        key_mod_masks |= Mod4Mask;
    //if(mask & (modkey_to_mask(XK_Caps_Lock) | modkey_to_mask(XK_Shift_Lock)))
    //    key_mod_masks |= LockMask;
    return key_mod_masks;
}

static unsigned int key_state_without_locks(unsigned int key_state) {
    return key_state & ~(Mod2Mask|LockMask);
}

struct CustomKeyName {
    KeySym key_sym;
    const char *name;
};

static int key_get_name(KeySym key_sym, char *buffer, int buffer_size) {
    if(buffer_size == 0 || key_sym == None)
        return 0;

    #define CUSTOM_KEY_NAME_LEN 23
    const CustomKeyName key_names[CUSTOM_KEY_NAME_LEN] = {
        { XK_Caps_Lock,      "Caps Lock"     },
        { XK_Shift_Lock,     "Caps Lock"     },
        { XK_Return,         "Return"        },
        { XK_BackSpace,      "BackSpace"     },
        { XK_Tab,            "Tab"           },
        { XK_Delete,         "Delete"        },
        { XK_dead_acute,     "`"             },
        { XK_dead_diaeresis, "^"             },
        { XK_Prior,          "PageUp"        },
        { XK_Next,           "PageDown"      },
        { ' ',               "Space"         },
        { XK_KP_Insert,      "KeyPad 0"      },
        { XK_KP_End,         "KeyPad 1"      },
        { XK_KP_Down,        "KeyPad 2"      },
        { XK_KP_Next,        "KeyPad 3"      },
        { XK_KP_Left,        "KeyPad 4"      },
        { XK_KP_Begin,       "KeyPad 5"      },
        { XK_KP_Right,       "KeyPad 6"      },
        { XK_KP_Home,        "KeyPad 7"      },
        { XK_KP_Up,          "KeyPad 8"      },
        { XK_KP_Prior,       "KeyPad 9"      },
        { XK_KP_Enter,       "KeyPad Return" },
        { XK_KP_Delete,      "KeyPad Delete" }
    };

    for(int i = 0; i < CUSTOM_KEY_NAME_LEN; ++i) {
        const CustomKeyName custom_key_name = key_names[i];
        if(key_sym == custom_key_name.key_sym) {
            const int key_len = strlen(custom_key_name.name);
            if(buffer_size < key_len)
                return 0;

            memcpy(buffer, custom_key_name.name, key_len);
            return key_len;
        }
    }

    XKeyPressedEvent event;
    event.type = KeyPress;
    event.display = gdk_x11_get_default_xdisplay();
    event.state = 0;
    event.keycode = XKeysymToKeycode(event.display, key_sym);

    KeySym ignore;
    Status return_status;
    int buflen = Xutf8LookupString(xic, &event, buffer, buffer_size, &ignore, &return_status);
    if(return_status != XBufferOverflow && buflen > 0)
        return buflen;

    const char *keysym_str = XKeysymToString(key_sym);
    if(keysym_str) {
        int keysym_str_len = strlen(keysym_str);
        if(buffer_size >= keysym_str_len) {
            memcpy(buffer, keysym_str, keysym_str_len);
            return keysym_str_len;
        }
    }

    return 0;
}

static int xerror_dummy(Display*, XErrorEvent*) {
    return 0;
}

static bool x_failed = false;
static int xerror_grab_error(Display*, XErrorEvent*) {
    x_failed = true;
    return 0;
}

static void ungrab_keyboard(Display *display) {
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        return;

    if(current_hotkey) {
        gtk_grab_remove(current_hotkey->hotkey_entry);
        gtk_widget_set_visible(current_hotkey->hotkey_active_label, false);
    }

    XErrorHandler prev_error_handler = XSetErrorHandler(xerror_dummy);
    XUngrabKeyboard(display, CurrentTime);
    XSync(display, False);
    XSetErrorHandler(prev_error_handler);
}

static bool grab_ungrab_hotkey_combo(Display *display, Hotkey hotkey, bool grab) {
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        return true;

    if(hotkey.keysym == None && hotkey.modkey_mask == 0)
        return true;

    unsigned int numlockmask = 0;
    KeyCode numlock_keycode = XKeysymToKeycode(display, XK_Num_Lock);
    XModifierKeymap *modmap = XGetModifierMapping(display);
    if(modmap) {
        for(int i = 0; i < 8; ++i) {
            for(int j = 0; j < modmap->max_keypermod; ++j) {
                if(modmap->modifiermap[i * modmap->max_keypermod + j] == numlock_keycode)
                    numlockmask = (1 << i);
            }
        }
        XFreeModifiermap(modmap);
    }

    unsigned int key_mod_masks = 0;
    KeySym key_sym = hotkey.keysym;
    if(key_sym == None) {
        // TODO: Set key_sym to one of the modkey mask values and set key_mod_masks to the other modkeys
    } else {
        key_mod_masks = key_mod_mask_to_x11_mask(hotkey.modkey_mask);
    }

    XSync(display, False);
    x_failed = false;
    XErrorHandler prev_error_handler = XSetErrorHandler(xerror_grab_error);

	Window root_window = DefaultRootWindow(display);
    unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
    if(key_sym != None) {
        for(int i = 0; i < 4; ++i) {
            if(grab) {
                XGrabKey(display, XKeysymToKeycode(display, key_sym), key_mod_masks|modifiers[i], root_window, False, GrabModeAsync, GrabModeAsync);
            } else {
                XUngrabKey(display, XKeysymToKeycode(display, key_sym), key_mod_masks|modifiers[i], root_window);
            }
        }
    }
    XSync(display, False);

    bool success = !x_failed;

    if(!success && key_sym != None) {
        for(int i = 0; i < 4; ++i) {
            XUngrabKey(display, XKeysymToKeycode(display, key_sym), key_mod_masks|modifiers[i], root_window);
        }
    }
    XSync(display, False);

    XSetErrorHandler(prev_error_handler);
    return success;
}

static void ungrab_keys(Display *display) {
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        return;

    for(int i = 0; i < num_hotkeys; ++i) {
        grab_ungrab_hotkey_combo(display, *hotkeys[i], false);
    }
}

static void set_hotkey_text_from_hotkey_data(GtkEntry *entry, Hotkey hotkey) {
    struct ModkeyName {
        KeySym key_sym;
        const char *name;
    };

    const ModkeyName modkey_names[] = {
        { XK_Control_L, "Ctrl" },
        { XK_Control_R, "Ctrl" },
        { XK_Super_L, "Super" },
        { XK_Super_R, "Super" },
        { XK_Meta_L, "Super" },
        { XK_Meta_R, "Super" },
        { XK_Shift_L, "Shift" },
        { XK_Shift_R, "Shift" },
        { XK_Alt_L, "Alt" },
        { XK_Alt_R, "Alt" },
    };

    std::string hotkey_combo_str;

    for(auto modkey_name : modkey_names) {
        if(hotkey.modkey_mask & modkey_to_mask(modkey_name.key_sym)) {
            if(!hotkey_combo_str.empty())
                hotkey_combo_str += " + ";
            hotkey_combo_str += modkey_name.name;
        }
    }

    if(!hotkey_combo_str.empty())
        hotkey_combo_str += " + ";

    char buffer[128];
    int buflen = key_get_name(hotkey.keysym, buffer, sizeof(buffer));
    if(buflen > 0)
        hotkey_combo_str.append(buffer, buflen);

    gtk_entry_set_text(entry, hotkey_combo_str.c_str());
}

static bool replace_grabbed_keys_depending_on_active_page() {
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        return true;

    for(int i = 0; i < num_hotkeys; ++i) {
        hotkeys[i]->grab_success = false;
    }

    ungrab_keys(gdk_x11_get_default_xdisplay());
    const GtkWidget *visible_page = gtk_stack_get_visible_child(page_navigation_userdata.stack);

    bool keys_successfully_grabbed = true;
    for(int i = 0; i < num_hotkeys; ++i) {
        if(visible_page == hotkeys[i]->page) {
            hotkey_mode = HotkeyMode::Record;
            hotkeys[i]->grab_success = grab_ungrab_hotkey_combo(gdk_x11_get_default_xdisplay(), *hotkeys[i], true);
            if(hotkeys[i]->grab_success) {
                set_hotkey_text_from_hotkey_data(GTK_ENTRY(hotkeys[i]->hotkey_entry), *hotkeys[i]);
            } else {
                gtk_entry_set_text(GTK_ENTRY(hotkeys[i]->hotkey_entry), "");
                hotkeys[i]->keysym = 0;
                hotkeys[i]->modkey_mask = 0;
                keys_successfully_grabbed = false;
            }
        }
    }
    return keys_successfully_grabbed;
}

static bool is_monitor_capture_drm() {
    return gsr_info.system_info.display_server == DisplayServer::WAYLAND || gsr_info.gpu_info.vendor != GpuVendor::NVIDIA;
}

static bool show_pkexec_flatpak_error_if_needed() {
    if(!flatpak)
        return false;

    const std::string window_str = panel_recording.active ? panel_recording.capture_target : record_area_selection_menu_get_active_id();
    if(is_monitor_capture_drm() && window_str != "window" && window_str != "focused" && window_str != "portal") {
        if(!is_pkexec_installed()) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "pkexec needs to be installed to record a monitor with an AMD/Intel GPU. Please install and run polkit. Alternatively, record a single window or use portal option which doesn't require root access.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return true;
        }

        if(!flatpak_is_installed_as_system()) {
            if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                    "GPU Screen Recorder needs to be installed system-wide to record your monitor on Wayland when not using the portal option. You can run this command to install GPU Screen recorder system-wide:\n"
                    "flatpak install --system com.dec05eba.gpu_screen_recorder\n");
                set_dialog_selectable(dialog);
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
            } else {
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                    "GPU Screen Recorder needs to be installed system-wide to record your monitor on AMD/Intel when not using the portal option. You can run this command to install GPU Screen recorder system-wide:\n"
                    "flatpak install --system com.dec05eba.gpu_screen_recorder\n"
                    "Alternatively, record a single window which doesn't have this restriction.");
                set_dialog_selectable(dialog);
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
            }
            return true;
        }
    }
    return false;
}

static void show_bugged_driver_warning() {
    if(gsr_info.gpu_info.vendor != GpuVendor::AMD)
        return;

    const std::string video_codec = video_codec_selection_menu_get_active_id();
    if((video_codec == "hevc" || video_codec == "hevc_10bit" || video_codec == "hevc_hdr") && !config.main_config.hevc_amd_bug_warning_shown) {
        GtkWidget *dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "There is an AMD driver bug and FFmpeg bug that causes black bars to appear on the sides of the video at certain resolutions when using HEVC codec.\n"
            "Select H264 video codec instead if this is an issue for you.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        config.main_config.hevc_amd_bug_warning_shown = true;
    }

    if((video_codec == "av1" || video_codec == "av1_10bit" || video_codec == "av1_hdr") && !config.main_config.av1_amd_bug_warning_shown) {
        GtkWidget *dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "There is an AMD hardware bug that causes black bars to appear on the sides of the video at certain resolutions when using AV1 codec.\n"
            "Select H264 video codec instead if this is an issue for you.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        config.main_config.av1_amd_bug_warning_shown = true;
    }
}

static void replace_meta_with_super(std::string &str) {
    size_t index = str.find("meta");
    if(index != std::string::npos)
        str.replace(index, 4, "Super");

    index = str.find("Meta");
    if(index != std::string::npos)
        str.replace(index, 4, "Super");
}

static void shortcut_changed_callback(gsr_shortcut shortcut, void *userdata) {
    (void)userdata;
    std::string trigger = shortcut.trigger_description;
    replace_meta_with_super(trigger);
    for(int i = 0; i < num_hotkeys; ++i) {
        if(strcmp(shortcut.id, hotkeys[i]->shortcut_id) == 0) {
            gtk_entry_set_text(GTK_ENTRY(hotkeys[i]->hotkey_entry), trigger.c_str());
        }
    }
}

static gboolean on_register_hotkeys_button_clicked(GtkButton *button, gpointer userdata) {
    (void)button;
    (void)userdata;

    /*
        Modifier key names are defined here: https://github.com/xkbcommon/libxkbcommon/blob/master/include/xkbcommon/xkbcommon-names.h.
        Remove the XKB_MOD_NAME_ prefix from the name and use the remaining part.
        Key names are defined here: https://github.com/xkbcommon/libxkbcommon/blob/master/include/xkbcommon/xkbcommon-keysyms.h.
        Remove the XKB_KEY_ (or XKB_KEY_KP_) prefix from the name and user the remaining part.
    */
    /* Unfortunately global shortcuts cant handle same key for different shortcuts, even though GPU Screen Recorder has page specific hotkeys */
    const gsr_bind_shortcut shortcuts[3] = {
        {
            "Start/stop recording/replay/streaming",
            { SHORTCUT_ID_START_STOP_RECORDING, "ALT+1" }
        },
        {
            "Pause/unpause recording",
            { SHORTCUT_ID_PAUSE_UNPAUSE_RECORDING, "ALT+2" }
        },
        {
            "Save replay",
            { SHORTCUT_ID_SAVE_REPLAY, "ALT+3" }
        }
    };

    if(global_shortcuts_initialized) {
        if(!gsr_global_shortcuts_bind_shortcuts(&global_shortcuts, shortcuts, 3, shortcut_changed_callback, NULL)) {
            fprintf(stderr, "gsr error: failed to bind shortcuts\n");
        }
    }

    return true;
}

static void register_global_shortcuts_once() {
    static bool registered = false;
    // On KDE plasma the shortcut menu popup will show up everytime this is used, so we dont want to call it everytime.
    // On Hyprland the global shortcut desktop portal is broken on older versions and crashes the desktop portal.
    // On GNOME and KDE 6.7.4+ this needs to be called everytime to register the shortcuts. The shortcut popup menu will show the first time only.
    if((wayland_compositor == WaylandCompositor::UNKNOWN || wayland_compositor == WaylandCompositor::KDE_NEW) && !registered) {
        registered = true;
        on_register_hotkeys_button_clicked(nullptr, nullptr);
    }
}

static gboolean on_start_replay_click(GtkButton*, gpointer userdata) {
    if(show_pkexec_flatpak_error_if_needed())
        return true;

    show_bugged_driver_warning();
    register_global_shortcuts_once();

    PageNavigationUserdata *_page_navigation_userdata = (PageNavigationUserdata*)userdata;
    gtk_stack_set_visible_child(_page_navigation_userdata->stack, _page_navigation_userdata->replay_page);
    app_indicator_set_menu(app_indicator, GTK_MENU(create_systray_menu(_page_navigation_userdata->app, SystrayPage::REPLAY)));

    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND)
        replace_grabbed_keys_depending_on_active_page();

    return true;
}

static gboolean on_start_recording_click(GtkButton*, gpointer userdata) {
    if(show_pkexec_flatpak_error_if_needed())
        return true;

    show_bugged_driver_warning();
    register_global_shortcuts_once();

    PageNavigationUserdata *_page_navigation_userdata = (PageNavigationUserdata*)userdata;
    gtk_stack_set_visible_child(_page_navigation_userdata->stack, _page_navigation_userdata->recording_page);
    app_indicator_set_menu(app_indicator, GTK_MENU(create_systray_menu(_page_navigation_userdata->app, SystrayPage::RECORDING)));

    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND)
        replace_grabbed_keys_depending_on_active_page();

    return true;
}

void on_stream_key_icon_click(GtkWidget *widget, gpointer) {
    gboolean visible = gtk_entry_get_visibility(GTK_ENTRY(widget));
    gtk_entry_set_visibility(GTK_ENTRY(widget), !visible);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(widget), GTK_ENTRY_ICON_SECONDARY, visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
}

static gboolean on_start_streaming_click(GtkButton*, gpointer userdata) {
    if(show_pkexec_flatpak_error_if_needed())
        return true;

    show_bugged_driver_warning();
    register_global_shortcuts_once();

    int num_audio_tracks = 0;
    gtk_container_foreach(GTK_CONTAINER(audio_devices_items_box), [](GtkWidget *widget, gpointer userdata) {
        int &num_audio_tracks = *(int*)userdata;
        GtkWidget *row_item_widget = gtk_grid_get_child_at(GTK_GRID(widget), 0, 0);
        if(GTK_IS_COMBO_BOX_TEXT(row_item_widget))
            ++num_audio_tracks;
    }, &num_audio_tracks);

    if(num_audio_tracks > 1 && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(split_audio_button))) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Streaming doesn't work with more than 1 audio track. Please remove all audio tracks or only use 1 audio track or select to merge audio tracks.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return true;
    }

    PageNavigationUserdata *_page_navigation_userdata = (PageNavigationUserdata*)userdata;
    gtk_stack_set_visible_child(_page_navigation_userdata->stack, _page_navigation_userdata->streaming_page);
    app_indicator_set_menu(app_indicator, GTK_MENU(create_systray_menu(_page_navigation_userdata->app, SystrayPage::STREAMING)));

    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND)
        replace_grabbed_keys_depending_on_active_page();

    return true;
}

static gboolean on_streaming_recording_replay_page_back_click(GtkButton*, gpointer userdata) {
    PageNavigationUserdata *_page_navigation_userdata = (PageNavigationUserdata*)userdata;
    gtk_stack_set_visible_child(_page_navigation_userdata->stack, _page_navigation_userdata->common_settings_page);
    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND)
        ungrab_keys(gdk_x11_get_default_xdisplay());
    hotkey_mode = HotkeyMode::NoAction;
    app_indicator_set_menu(app_indicator, GTK_MENU(create_systray_menu(_page_navigation_userdata->app, SystrayPage::FRONT)));
    return true;
}

static gboolean file_choose_button_click_handler(GtkButton *button, const char *title, GtkFileChooserAction file_action, std::string container_file_ext) {
    GtkWidget *file_chooser_dialog = gtk_file_chooser_dialog_new(title, nullptr, file_action,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Save", GTK_RESPONSE_ACCEPT,
        nullptr);

    if(file_action != GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER) {
        const std::string name = "Video_" + get_date_str() + "." + container_file_ext;
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(file_chooser_dialog), name.c_str());
    }

    int res = gtk_dialog_run(GTK_DIALOG(file_chooser_dialog));
    if(res == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(file_chooser_dialog));
        gtk_button_set_label(button, filename);
        g_free(filename);
    }
    gtk_widget_destroy(file_chooser_dialog);
    return true;
}

static gboolean on_record_file_choose_button_click(GtkButton *button, gpointer) {
    gboolean res = file_choose_button_click_handler(button, "Where do you want to save the video?", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, gtk_combo_box_text_get_active_text(record_container));
    config.record_config.save_directory = gtk_button_get_label(button);
    return res;
}

static gboolean on_replay_file_chooser_button_click(GtkButton *button, gpointer) {
    gboolean res = file_choose_button_click_handler(button, "Where do you want to save the replays?", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, gtk_combo_box_text_get_active_text(replay_container));
    config.replay_config.save_directory = gtk_button_get_label(button);
    return res;
}

static bool kill_gpu_screen_recorder_get_result(bool *already_dead) {
    *already_dead = true;
    bool exit_success = true;
    if(gpu_screen_recorder_process != -1) {
        *already_dead = false;
        int status;
        int wait_result = waitpid(gpu_screen_recorder_process, &status, WNOHANG);
        if(wait_result == -1) {
            perror("waitpid failed");
            exit_success = false;
        } else if(wait_result == 0) {
            // the process is still running
            kill(gpu_screen_recorder_process, SIGINT);
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                exit_success = false;
            } else {
                exit_success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
        } else {
            exit_success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        gpu_screen_recorder_process = -1;
    }
    return exit_success;
}

static bool starts_with(const std::string &str, const char *substr) {
    size_t len = strlen(substr);
    return str.size() >= len && memcmp(str.data(), substr, len) == 0;
}

struct AudioTracksUserdata {
    std::vector<std::string> &result;
    bool application_audio_invert;
    int num_app_audio = 0;
};

static std::vector<std::string> create_audio_tracks_real_names(std::string &merge_audio_tracks) {
    std::vector<std::string> result;
    AudioTracksUserdata audio_tracks_userdata {
        result,
        (bool)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(record_app_audio_inverted_button)),
        0
    };

    gtk_container_foreach(GTK_CONTAINER(audio_devices_items_box), [](GtkWidget *widget, gpointer userdata) {
        AudioTracksUserdata &audio_tracks_userdata = *(AudioTracksUserdata*)userdata;
        const char *audio_track_type = (const char*)g_object_get_data(G_OBJECT(widget), "audio-track-type");
        GtkWidget *row_item_widget = gtk_grid_get_child_at(GTK_GRID(widget), 1, 0);
        if(strcmp(audio_track_type, "device") == 0) {
            std::string audio_input_name = "device:";
            audio_input_name += gtk_combo_box_get_active_id(GTK_COMBO_BOX(row_item_widget));
            audio_tracks_userdata.result.push_back(std::move(audio_input_name));
        } else if(strcmp(audio_track_type, "app") == 0) {
            if(!gsr_info.system_info.supports_app_audio)
                return;

            std::string audio_input_name = audio_tracks_userdata.application_audio_invert ? "app-inverse:" : "app:";
            audio_input_name += gtk_combo_box_get_active_id(GTK_COMBO_BOX(row_item_widget));
            audio_tracks_userdata.result.push_back(std::move(audio_input_name));
            ++audio_tracks_userdata.num_app_audio;
        } else if(strcmp(audio_track_type, "app-custom") == 0) {
            if(!gsr_info.system_info.supports_app_audio)
                return;

            std::string audio_input_name = audio_tracks_userdata.application_audio_invert ? "app-inverse:" : "app:";
            audio_input_name += gtk_entry_get_text(GTK_ENTRY(row_item_widget));
            audio_tracks_userdata.result.push_back(std::move(audio_input_name));
            ++audio_tracks_userdata.num_app_audio;
        }
    }, &audio_tracks_userdata);

    if(audio_tracks_userdata.num_app_audio == 0 && audio_tracks_userdata.application_audio_invert)
        audio_tracks_userdata.result.push_back("app-inverse:");

    merge_audio_tracks.clear();
    for(size_t i = 0; i < result.size(); ++i) {
        if(i > 0)
            merge_audio_tracks += '|';
        merge_audio_tracks += result[i];
    }

    return result;
}

static void add_audio_command_line_args(std::vector<const char*> &args, const std::vector<std::string> &audio_tracks, const std::string &merge_audio_tracks) {
    if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(split_audio_button))) {
        if(!merge_audio_tracks.empty())
            args.insert(args.end(), { "-a", merge_audio_tracks.c_str() });
    } else {
        for(const std::string &audio_track : audio_tracks) {
            args.insert(args.end(), { "-a", audio_track.c_str() });
        }
    }
}

static void change_container_if_codec_not_supported(const std::string &video_codec, const gchar **container_str) {
    if(strcmp(video_codec.c_str(), "vp8") == 0 || strcmp(video_codec.c_str(), "vp9") == 0) {
        if(strcmp(*container_str, "webm") != 0 && strcmp(*container_str, "matroska") != 0) {
            fprintf(stderr, "Warning: container '%s' is not compatible with video codec '%s', using webm container instead\n", *container_str, video_codec.c_str());
            *container_str = "webm";
        }
    } else if(strcmp(*container_str, "webm") == 0) {
        fprintf(stderr, "Warning: container webm is not compatible with video codec '%s', using mp4 container instead\n",  video_codec.c_str());
        *container_str = "mp4";
    }
}

static const char* get_first_usable_hardware_video_codec_name() {
    if(gsr_info.supported_video_codecs.h264)
        return "h264";
    else if(gsr_info.supported_video_codecs.hevc)
        return "hevc";
    else if(gsr_info.supported_video_codecs.av1)
        return "av1";
    else if(gsr_info.supported_video_codecs.vp8)
        return "vp8";
    else if(gsr_info.supported_video_codecs.vp9)
        return "vp9";
    return nullptr;
}

static void add_quality_command_line_args(std::vector<const char*> &args, const char *quality_input_str, const char *video_bitrate_str) {
    if(strcmp(quality_input_str, "custom") == 0) {
        args.push_back("-bm");
        args.push_back("cbr");
        args.push_back("-q");
        args.push_back(video_bitrate_str);
    } else {
        args.push_back("-q");
        args.push_back(quality_input_str);
    }
}

static void debug_print_args(const char **args) {
    fprintf(stderr, "info: running command:");
    while(*args) {
        fprintf(stderr, " %s", *args);
        ++args;
    }
    fprintf(stderr, "\n");
}

static bool validate_window(GtkApplication *app, Window window) {
    XWindowAttributes attr;
    if(XGetWindowAttributes(dpy, window, &attr)) {
        return true;
    } else {
        show_notification(app, "GPU Screen Recorder", "The window you are trying to record no longer exists", G_NOTIFICATION_PRIORITY_URGENT);
        return false;
    }
}

static gboolean on_start_replay_button_click(GtkButton *button, gpointer userdata) {
    GtkApplication *app = (GtkApplication*)userdata;
    const gchar *dir = gtk_button_get_label(replay_file_chooser_button);

    int exit_status = prev_exit_status;
    prev_exit_status = -1;
    if(replaying) {
        bool already_dead = true;
        bool exit_success = kill_gpu_screen_recorder_get_result(&already_dead);

        gtk_button_set_label(button, "Start replay");
        replaying = false;
        gtk_widget_set_sensitive(GTK_WIDGET(replay_back_button), true);
        gtk_widget_set_sensitive(GTK_WIDGET(replay_save_button), false);

        gtk_widget_set_opacity(GTK_WIDGET(replay_bottom_panel_grid), 0.5);
        gtk_label_set_text(GTK_LABEL(replay_record_time_label), "00:00:00");

        gtk_menu_item_set_label(GTK_MENU_ITEM(start_stop_replay_menu_item), "Start replay");
        gtk_widget_set_sensitive(save_replay_menu_item, false);
        app_indicator_set_icon_full(app_indicator, get_tray_idle_icon_name(), "Idle");

        if(exit_status == 10) {
            show_notification(app, "GPU Screen Recorder", "You need to have pkexec installed and have a polkit agent running to record your monitor", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_status == 50) {
            show_notification(app, "GPU Screen Recorder", "Desktop portal capture failed. Either you canceled the desktop portal or your Wayland compositor doesn't support desktop portal capture or it's incorrectly setup on your system", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_status == 60) {
            // Canceled by the user
        } else if(!exit_success || (already_dead && exit_status != 0)) {
            show_notification(app, "GPU Screen Recorder",
                "Failed to start replay. Either your graphics card doesn't support GPU Screen Recorder with the settings you used or you don't have enough disk space to record a video", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_success) {
            if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_stopped_notification_button)))
                show_notification(app, "GPU Screen Recorder", "Stopped replay", G_NOTIFICATION_PRIORITY_NORMAL);
        }

        return true;
    }

    save_configs();

    const int fps = gtk_spin_button_get_value_as_int(fps_entry);
    const int replay_time = gtk_spin_button_get_value_as_int(replay_time_entry);

    char dir_tmp[PATH_MAX];
    strcpy(dir_tmp, dir);
    if(create_directory_recursive(dir_tmp) != 0) {
        std::string notification_body = std::string("Failed to start replay. Failed to create ") + dir_tmp;
        show_notification(app, "GPU Screen Recorder", notification_body.c_str(), G_NOTIFICATION_PRIORITY_URGENT);
        return true;
    }

    bool follow_focused = false;
    std::string window_str = record_area_selection_menu_get_active_id();
    if(window_str == "window") {
        if(select_window_userdata.selected_window == None) {
            fprintf(stderr, "No window selected!\n");
            return true;
        }
        window_str = std::to_string(select_window_userdata.selected_window);
        if(!validate_window(app, select_window_userdata.selected_window))
            return true;
    } else if(window_str == "focused") {
        follow_focused = true;
    }
    const std::string fps_str = std::to_string(fps);
    const std::string replay_time_str = std::to_string(replay_time);
    const bool change_video_resolution = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(change_video_resolution_button));

    const gchar* container_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(replay_container));
    const gchar* color_range_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(color_range_input_menu));
    const gchar* quality_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(quality_input_menu));
    const gchar* audio_codec_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(audio_codec_input_menu));
    const gchar* framerate_mode_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(framerate_mode_input_menu));
    const bool exclude_metadata = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(exclude_metadata_button));
    const bool record_cursor = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(record_cursor_button));
    const bool restore_portal_session = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_portal_session_button));
    const std::string video_bitrate_str = std::to_string(gtk_spin_button_get_value_as_int(video_bitrate_entry));

    const int record_width = follow_focused ? gtk_spin_button_get_value_as_int(area_width_entry) : gtk_spin_button_get_value_as_int(video_width_entry);
    const int record_height = follow_focused ? gtk_spin_button_get_value_as_int(area_height_entry) : gtk_spin_button_get_value_as_int(video_height_entry);

    const char *encoder = "gpu";
    std::string video_codec_input_str = video_codec_selection_menu_get_active_id();
    if(video_codec_input_str == "h264_software") {
        video_codec_input_str = "h264";
        encoder = "cpu";
    } else if(video_codec_input_str == "auto") {
        if(!get_first_usable_hardware_video_codec_name()) {
            video_codec_input_str = "h264";
            encoder = "cpu";
        }
    }

    change_container_if_codec_not_supported(video_codec_input_str, &container_str);

    char area[64];
    snprintf(area, sizeof(area), "%dx%d", record_width, record_height);

    std::vector<const char*> args = {
        "gpu-screen-recorder", "-w", window_str.c_str(), "-c", container_str, "-k", video_codec_input_str.c_str(), "-ac", audio_codec_input_str, "-f", fps_str.c_str(), "-cursor", record_cursor ? "yes" : "no", "-restore-portal-session", restore_portal_session ? "yes" : "no", "-cr", color_range_input_str, "-r", replay_time_str.c_str(), "-encoder", encoder, "-o", dir
    };

    if(exclude_metadata) {
        args.push_back("-exclude-metadata");
        args.push_back("yes");
    }

    add_quality_command_line_args(args, quality_input_str, video_bitrate_str.c_str());

    if(strcmp(framerate_mode_input_str, "auto") != 0)
        args.insert(args.end(), { "-fm", framerate_mode_input_str });

    std::string merge_audio_tracks;
    const std::vector<std::string> audio_tracks = create_audio_tracks_real_names(merge_audio_tracks);
    add_audio_command_line_args(args, audio_tracks, merge_audio_tracks);

    if(follow_focused || change_video_resolution)
        args.insert(args.end(), { "-s", area });

    args.push_back(NULL);
    debug_print_args(args.data());

    if(gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(hide_window_when_recording_menu_item))) {
        hide_window();
    }

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if(pid == -1) {
        perror("failed to fork");
        show_notification(app, "GPU Screen Recorder", "Failed to start replay (failed to fork)", G_NOTIFICATION_PRIORITY_URGENT);
        return true;
    } else if(pid == 0) { /* child process */
        if(prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
            perror("prctl(PR_SET_PDEATHSIG, SIGTERM) failed");
            _exit(3);
        }

        if(getppid() != parent_pid)
            _exit(3);

        execvp(args[0], (char* const*)args.data());
        perror("failed to launch gpu-screen-recorder");
        _exit(127);
    } else { /* parent process */
        gpu_screen_recorder_process = pid;
    }

    replaying = true;

    gtk_button_set_label(button, "Stop replay");

    gtk_widget_set_sensitive(GTK_WIDGET(replay_back_button), false);
    gtk_widget_set_sensitive(GTK_WIDGET(replay_save_button), true);
    gtk_widget_set_opacity(GTK_WIDGET(replay_bottom_panel_grid), 1.0);

    gtk_menu_item_set_label(GTK_MENU_ITEM(start_stop_replay_menu_item), "Stop replay");
    gtk_widget_set_sensitive(save_replay_menu_item, true);
    app_indicator_set_icon_full(app_indicator, get_tray_recording_icon_name(), "Recording");

    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_started_notification_button)))
        show_notification(app, "GPU Screen Recorder", "Started replay", G_NOTIFICATION_PRIORITY_NORMAL);

    record_start_time_sec = clock_get_monotonic_seconds();
    return true;
}

static gboolean on_replay_save_button_click(GtkButton*, gpointer userdata) {
    if(gpu_screen_recorder_process == -1)
        return true;

    GtkApplication *app = (GtkApplication*)userdata;
    kill(gpu_screen_recorder_process, SIGUSR1);
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_saved_notification_button)))
        show_notification(app, "GPU Screen Recorder", "Saved replay", G_NOTIFICATION_PRIORITY_NORMAL);
    return true;
}

static gboolean on_pause_unpause_button_click(GtkButton*, gpointer) {
    if(!recording)
        return true;

    if(gpu_screen_recorder_process == -1)
        return true;

    kill(gpu_screen_recorder_process, SIGUSR2);
    paused = !paused;
    if(paused) {
        gtk_button_set_label(pause_recording_button, "Unpause recording");
        gtk_image_set_from_icon_name(GTK_IMAGE(recording_record_icon), "media-playback-pause", GTK_ICON_SIZE_SMALL_TOOLBAR);
        gtk_menu_item_set_label(GTK_MENU_ITEM(pause_recording_menu_item), "Unpause recording");
        app_indicator_set_icon_full(app_indicator, get_tray_paused_icon_name(), "Paused");
        pause_start_sec = clock_get_monotonic_seconds();
    } else {
        gtk_button_set_label(pause_recording_button, "Pause recording");
        gtk_image_set_from_icon_name(GTK_IMAGE(recording_record_icon), "media-record", GTK_ICON_SIZE_SMALL_TOOLBAR);
        gtk_menu_item_set_label(GTK_MENU_ITEM(pause_recording_menu_item), "Pause recording");
        app_indicator_set_icon_full(app_indicator, get_tray_recording_icon_name(), "Recording");
        paused_time_offset_sec += (clock_get_monotonic_seconds() - pause_start_sec);
    }
    return true;
}

static void on_panel_recording_finished(bool saved);

static gboolean on_start_recording_button_click(GtkButton *button, gpointer userdata) {
    GtkApplication *app = (GtkApplication*)userdata;
    const gchar *dir = gtk_button_get_label(record_file_chooser_button);

    int exit_status = prev_exit_status;
    prev_exit_status = -1;
    if(recording) {
        bool already_dead = true;
        bool exit_success = kill_gpu_screen_recorder_get_result(&already_dead);

        gtk_button_set_label(button, "Start recording");
        recording = false;
        gtk_widget_set_sensitive(GTK_WIDGET(record_back_button), true);

        paused = false;
        gtk_widget_set_sensitive(GTK_WIDGET(pause_recording_button), false);
        gtk_button_set_label(pause_recording_button, "Pause recording");

        gtk_widget_set_opacity(GTK_WIDGET(recording_bottom_panel_grid), 0.5);
        gtk_image_set_from_icon_name(GTK_IMAGE(recording_record_icon), "media-record", GTK_ICON_SIZE_SMALL_TOOLBAR);
        gtk_label_set_text(GTK_LABEL(recording_record_time_label), "00:00:00");

        gtk_menu_item_set_label(GTK_MENU_ITEM(start_stop_recording_menu_item), "Start recording");
        gtk_menu_item_set_label(GTK_MENU_ITEM(pause_recording_menu_item), "Pause recording");
        gtk_widget_set_sensitive(pause_recording_menu_item, false);
        app_indicator_set_icon_full(app_indicator, get_tray_idle_icon_name(), "Idle");

        if(exit_status == 10) {
            show_notification(app, "GPU Screen Recorder", "You need to have pkexec installed and have a polkit agent running to record your monitor", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_status == 50) {
            show_notification(app, "GPU Screen Recorder", "Desktop portal capture failed. Either you canceled the desktop portal or your Wayland compositor doesn't support desktop portal capture or it's incorrectly setup on your system", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_status == 60) {
            // Canceled by the user
        } else if(!exit_success || (already_dead && exit_status != 0)) {
            show_notification(app, "GPU Screen Recorder", "Failed to save video. Either your graphics card doesn't support GPU Screen Recorder with the settings you used or you don't have enough disk space to record a video. Start this GPU Screen Recorder GUI application from the terminal to see more information when this failure happens", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_success) {
            if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_saved_notification_button))) {
                const std::string notification_body = std::string("The recording was saved to ") + record_file_current_filename;
                show_notification(app, "GPU Screen Recorder", notification_body.c_str(), G_NOTIFICATION_PRIORITY_NORMAL);
            }
        }

        if(panel_recording.active) {
            const bool saved = exit_status != 10 && exit_status != 50 && exit_status != 60 && exit_success && !(already_dead && exit_status != 0);
            on_panel_recording_finished(saved);
        }
        return true;
    }

    save_configs();

    const int fps = gtk_spin_button_get_value_as_int(fps_entry);

    bool follow_focused = false;
    std::string window_str = record_area_selection_menu_get_active_id();
    if(panel_recording.active) {
        window_str = panel_recording.window_arg;
    } else if(window_str == "window") {
        if(select_window_userdata.selected_window == None) {
            fprintf(stderr, "No window selected!\n");
            return true;
        }
        window_str = std::to_string(select_window_userdata.selected_window);
        if(!validate_window(app, select_window_userdata.selected_window))
            return true;
    } else if(window_str == "focused") {
        follow_focused = true;
    }

    const std::string fps_str = std::to_string(fps);
    const bool change_video_resolution = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(change_video_resolution_button));

    const gchar* container_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(record_container));
    const gchar* container_name = gtk_combo_box_text_get_active_text(record_container);
    const gchar* color_range_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(color_range_input_menu));
    const gchar* quality_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(quality_input_menu));
    const gchar* audio_codec_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(audio_codec_input_menu));
    const gchar* framerate_mode_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(framerate_mode_input_menu));
    const bool exclude_metadata = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(exclude_metadata_button));
    const bool record_cursor = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(record_cursor_button));
    const bool restore_portal_session = panel_recording.active ? panel_recording.restore_portal_session : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_portal_session_button));
    const std::string video_bitrate_str = std::to_string(gtk_spin_button_get_value_as_int(video_bitrate_entry));

    const int record_width = follow_focused ? gtk_spin_button_get_value_as_int(area_width_entry) : gtk_spin_button_get_value_as_int(video_width_entry);
    const int record_height = follow_focused ? gtk_spin_button_get_value_as_int(area_height_entry) : gtk_spin_button_get_value_as_int(video_height_entry);

    const char *encoder = "gpu";
    std::string video_codec_input_str = video_codec_selection_menu_get_active_id();
    if(video_codec_input_str == "h264_software") {
        video_codec_input_str = "h264";
        encoder = "cpu";
    } else if(video_codec_input_str == "auto") {
        if(!get_first_usable_hardware_video_codec_name()) {
            video_codec_input_str = "h264";
            encoder = "cpu";
        }
    }

    change_container_if_codec_not_supported(video_codec_input_str, &container_str);

    char dir_tmp[PATH_MAX];
    strcpy(dir_tmp, dir);
    if(create_directory_recursive(dir_tmp) != 0) {
        std::string notification_body = std::string("Failed to start recording. Failed to create ") + dir_tmp;
        show_notification(app, "GPU Screen Recorder", notification_body.c_str(), G_NOTIFICATION_PRIORITY_URGENT);
        return true;
    }

    record_file_current_filename = std::string(dir_tmp) + "/Video_" + get_date_str() + "." + container_name;

    char area[64];
    snprintf(area, sizeof(area), "%dx%d", record_width, record_height);

    std::vector<const char*> args = {
        "gpu-screen-recorder", "-w", window_str.c_str(), "-c", container_str, "-k", video_codec_input_str.c_str(), "-ac", audio_codec_input_str, "-f", fps_str.c_str(), "-cursor", record_cursor ? "yes" : "no", "-restore-portal-session", restore_portal_session ? "yes" : "no", "-cr", color_range_input_str, "-encoder", encoder, "-o", record_file_current_filename.c_str()
    };

    if(exclude_metadata) {
        args.push_back("-exclude-metadata");
        args.push_back("yes");
    }

    add_quality_command_line_args(args, quality_input_str, video_bitrate_str.c_str());

    if(strcmp(framerate_mode_input_str, "auto") != 0)
        args.insert(args.end(), { "-fm", framerate_mode_input_str });

    std::string merge_audio_tracks;
    const std::vector<std::string> audio_tracks = create_audio_tracks_real_names(merge_audio_tracks);
    add_audio_command_line_args(args, audio_tracks, merge_audio_tracks);

    if(follow_focused || change_video_resolution)
        args.insert(args.end(), { "-s", area });

    if(panel_recording.active && !panel_recording.region_arg.empty())
        args.insert(args.end(), { "-region", panel_recording.region_arg.c_str() });

    args.push_back(NULL);
    debug_print_args(args.data());

    if(gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(hide_window_when_recording_menu_item))) {
        hide_window();
    }

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if(pid == -1) {
        perror("failed to fork");
        show_notification(app, "GPU Screen Recorder", "Failed to start recording (failed to fork)", G_NOTIFICATION_PRIORITY_URGENT);
        return true;
    } else if(pid == 0) { /* child process */
        if(prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
            perror("prctl(PR_SET_PDEATHSIG, SIGTERM) failed");
            _exit(3);
        }

        if(getppid() != parent_pid)
            _exit(3);

        execvp(args[0], (char* const*)args.data());
        perror("failed to launch gpu-screen-recorder");
        _exit(127);
    } else { /* parent process */
        gpu_screen_recorder_process = pid;
    }

    recording = true;

    gtk_button_set_label(button, "Stop recording");
    gtk_widget_set_sensitive(GTK_WIDGET(record_back_button), false);
    gtk_widget_set_sensitive(GTK_WIDGET(pause_recording_button), true);
    gtk_widget_set_opacity(GTK_WIDGET(recording_bottom_panel_grid), 1.0);

    gtk_menu_item_set_label(GTK_MENU_ITEM(start_stop_recording_menu_item), "Stop recording");
    gtk_widget_set_sensitive(pause_recording_menu_item, true);
    app_indicator_set_icon_full(app_indicator, get_tray_recording_icon_name(), "Recording");

    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_started_notification_button)))
        show_notification(app, "GPU Screen Recorder", "Started recording", G_NOTIFICATION_PRIORITY_NORMAL);

    record_start_time_sec = clock_get_monotonic_seconds();
    paused_time_offset_sec = 0.0;
    return true;
}

static gboolean on_start_streaming_button_click(GtkButton *button, gpointer userdata) {
    GtkApplication *app = (GtkApplication*)userdata;

    int exit_status = prev_exit_status;
    prev_exit_status = -1;
    if(streaming) {
        bool already_dead = true;
        bool exit_success = kill_gpu_screen_recorder_get_result(&already_dead);

        gtk_button_set_label(button, "Start streaming");
        streaming = false;
        gtk_widget_set_sensitive(GTK_WIDGET(stream_back_button), true);

        gtk_widget_set_opacity(GTK_WIDGET(streaming_bottom_panel_grid), 0.5);
        gtk_label_set_text(GTK_LABEL(streaming_record_time_label), "00:00:00");

        gtk_menu_item_set_label(GTK_MENU_ITEM(start_stop_streaming_menu_item), "Start streaming");
        app_indicator_set_icon_full(app_indicator, get_tray_idle_icon_name(), "Idle");

        if(exit_status == 10) {
            show_notification(app, "GPU Screen Recorder", "You need to have pkexec installed and have a polkit agent running to record your monitor", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_status == 50) {
            show_notification(app, "GPU Screen Recorder", "Desktop portal capture failed. Either you canceled the desktop portal or your Wayland compositor doesn't support desktop portal capture or it's incorrectly setup on your system", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_status == 60) {
            // Canceled by the user
        } else if(!exit_success || (already_dead && exit_status != 0)) {
            show_notification(app, "GPU Screen Recorder", "Failed to stream video. There is either an error in your streaming config or your graphics card doesn't support GPU Screen Recorder with the settings you used", G_NOTIFICATION_PRIORITY_URGENT);
        } else if(exit_success) {
            if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_stopped_notification_button)))
                show_notification(app, "GPU Screen Recorder", "Stopped streaming", G_NOTIFICATION_PRIORITY_NORMAL);
        }

        return true;
    }

    save_configs();

    const int fps = gtk_spin_button_get_value_as_int(fps_entry);

    bool follow_focused = false;
    std::string window_str = record_area_selection_menu_get_active_id();
    if(window_str == "window") {
        if(select_window_userdata.selected_window == None) {
            fprintf(stderr, "No window selected!\n");
            return true;
        }
        window_str = std::to_string(select_window_userdata.selected_window);
        if(!validate_window(app, select_window_userdata.selected_window))
            return true;
    } else if(window_str == "focused") {
        follow_focused = true;
    }
    const std::string fps_str = std::to_string(fps);
    const bool change_video_resolution = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(change_video_resolution_button));

    std::string stream_url;
    const gchar *container_str = "flv";
    const gchar *stream_service = gtk_combo_box_get_active_id(GTK_COMBO_BOX(stream_service_input_menu));
    if(strcmp(stream_service, "twitch") == 0) {
        stream_url = "rtmp://live.twitch.tv/app/";
        stream_url += gtk_entry_get_text(twitch_stream_id_entry);
    } else if(strcmp(stream_service, "youtube") == 0) {
        stream_url = "rtmp://a.rtmp.youtube.com/live2/";
        stream_url += gtk_entry_get_text(youtube_stream_id_entry);
    } else if(strcmp(stream_service, "custom") == 0) {
        stream_url = gtk_entry_get_text(custom_stream_url_entry);
        container_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(custom_stream_container));
        if(stream_url.size() >= 7 && strncmp(stream_url.c_str(), "rtmp://", 7) == 0)
        {}
        else if(stream_url.size() >= 8 && strncmp(stream_url.c_str(), "rtmps://", 8) == 0)
        {}
        else if(stream_url.size() >= 7 && strncmp(stream_url.c_str(), "rtsp://", 7) == 0)
        {}
        else if(stream_url.size() >= 6 && strncmp(stream_url.c_str(), "srt://", 6) == 0)
        {}
        else if(stream_url.size() >= 7 && strncmp(stream_url.c_str(), "http://", 7) == 0)
        {}
        else if(stream_url.size() >= 8 && strncmp(stream_url.c_str(), "https://", 8) == 0)
        {}
        else if(stream_url.size() >= 6 && strncmp(stream_url.c_str(), "tcp://", 6) == 0)
        {}
        else if(stream_url.size() >= 6 && strncmp(stream_url.c_str(), "udp://", 6) == 0)
        {}
        else
            stream_url = "rtmp://" + stream_url;
    }

    const gchar* quality_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(quality_input_menu));
    const gchar* color_range_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(color_range_input_menu));
    const gchar* audio_codec_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(audio_codec_input_menu));
    const gchar* framerate_mode_input_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(framerate_mode_input_menu));
    const bool exclude_metadata = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(exclude_metadata_button));
    const bool record_cursor = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(record_cursor_button));
    const bool restore_portal_session = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_portal_session_button));
    const std::string video_bitrate_str = std::to_string(gtk_spin_button_get_value_as_int(video_bitrate_entry));

    const int record_width = follow_focused ? gtk_spin_button_get_value_as_int(area_width_entry) : gtk_spin_button_get_value_as_int(video_width_entry);
    const int record_height = follow_focused ? gtk_spin_button_get_value_as_int(area_height_entry) : gtk_spin_button_get_value_as_int(video_height_entry);

    const char *encoder = "gpu";
    std::string video_codec_input_str = video_codec_selection_menu_get_active_id();
    if(video_codec_input_str == "h264_software") {
        video_codec_input_str = "h264";
        encoder = "cpu";
    } else if(video_codec_input_str == "auto") {
        if(!get_first_usable_hardware_video_codec_name()) {
            video_codec_input_str = "h264";
            encoder = "cpu";
        }
    }

    change_container_if_codec_not_supported(video_codec_input_str, &container_str);

    char area[64];
    snprintf(area, sizeof(area), "%dx%d", record_width, record_height);

    std::vector<const char*> args = {
        "gpu-screen-recorder", "-w", window_str.c_str(), "-c", container_str, "-k", video_codec_input_str.c_str(), "-ac", audio_codec_input_str, "-f", fps_str.c_str(), "-cursor", record_cursor ? "yes" : "no", "-restore-portal-session", restore_portal_session ? "yes" : "no", "-cr", color_range_input_str, "-encoder", encoder, "-o", stream_url.c_str()
    };

    if(exclude_metadata) {
        args.push_back("-exclude-metadata");
        args.push_back("yes");
    }

    add_quality_command_line_args(args, quality_input_str, video_bitrate_str.c_str());

    if(strcmp(framerate_mode_input_str, "auto") != 0)
        args.insert(args.end(), { "-fm", framerate_mode_input_str });

    std::string merge_audio_tracks;
    const std::vector<std::string> audio_tracks = create_audio_tracks_real_names(merge_audio_tracks);
    add_audio_command_line_args(args, audio_tracks, merge_audio_tracks);

    if(follow_focused || change_video_resolution)
        args.insert(args.end(), { "-s", area });

    args.push_back(NULL);
    debug_print_args(args.data());

    if(gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(hide_window_when_recording_menu_item))) {
        hide_window();
    }

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if(pid == -1) {
        perror("failed to fork");
        show_notification(app, "GPU Screen Recorder", "Failed to start streaming (failed to fork)", G_NOTIFICATION_PRIORITY_URGENT);
        return true;
    } else if(pid == 0) { /* child process */
        if(prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
            perror("prctl(PR_SET_PDEATHSIG, SIGTERM) failed");
            _exit(3);
        }

        if(getppid() != parent_pid)
            _exit(3);

        execvp(args[0], (char* const*)args.data());
        perror("failed to launch gpu-screen-recorder");
        _exit(127);
    } else { /* parent process */
        gpu_screen_recorder_process = pid;
    }

    streaming = true;

    gtk_button_set_label(button, "Stop streaming");

    gtk_widget_set_sensitive(GTK_WIDGET(stream_back_button), false);
    gtk_widget_set_opacity(GTK_WIDGET(streaming_bottom_panel_grid), 1.0);

    gtk_menu_item_set_label(GTK_MENU_ITEM(start_stop_streaming_menu_item), "Stop streaming");
    app_indicator_set_icon_full(app_indicator, get_tray_recording_icon_name(), "Recording");

    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_recording_started_notification_button)))
        show_notification(app, "GPU Screen Recorder", "Started streaming", G_NOTIFICATION_PRIORITY_NORMAL);

    record_start_time_sec = clock_get_monotonic_seconds();
    return true;
}

static void start_stop_streaming_menu_item_systray_callback(GtkMenuItem*, gpointer userdata) {
    on_start_streaming_button_click(start_streaming_button, userdata);
}

static void start_stop_recording_systray_callback(GtkMenuItem*, gpointer userdata) {
    on_start_recording_button_click(start_recording_button, userdata);
}

static void pause_recording_systray_callback(GtkMenuItem*, gpointer userdata) {
    on_pause_unpause_button_click(pause_recording_button, userdata);
}

static void start_stop_replay_systray_callback(GtkMenuItem*, gpointer userdata) {
    on_start_replay_button_click(start_replay_button, userdata);
}

static void save_replay_systray_callback(GtkMenuItem*, gpointer userdata) {
    on_replay_save_button_click(replay_save_button, userdata);
}

static void gtk_widget_set_margin(GtkWidget *widget, int top, int bottom, int left, int right) {
    gtk_widget_set_margin_top(widget, top);
    gtk_widget_set_margin_bottom(widget, bottom);
    gtk_widget_set_margin_start(widget, left);
    gtk_widget_set_margin_end(widget, right);
}

static void record_area_item_change_callback(GtkComboBox *widget, gpointer userdata) {
    (void)widget;
    (void)userdata;
    const std::string selected_window_area = record_area_selection_menu_get_active_id();
    gtk_widget_set_visible(GTK_WIDGET(select_window_button), strcmp(selected_window_area.c_str(), "window") == 0);
    gtk_widget_set_visible(GTK_WIDGET(area_size_grid), strcmp(selected_window_area.c_str(), "focused") == 0);
    gtk_widget_set_visible(GTK_WIDGET(restore_portal_session_button), strcmp(selected_window_area.c_str(), "portal") == 0);
    gtk_widget_set_visible(GTK_WIDGET(change_video_resolution_button), strcmp(selected_window_area.c_str(), "focused") != 0);
    on_change_video_resolution_button_click(GTK_BUTTON(change_video_resolution_button), nullptr);
    enable_stream_record_button_if_info_filled();
}

static void view_combo_box_change_callback(GtkComboBox *widget, gpointer userdata) {
    (void)userdata;
    const gchar *selected_view = gtk_combo_box_get_active_id(widget);
    const bool advanced_view = strcmp(selected_view, "advanced") == 0;
    gtk_widget_set_visible(GTK_WIDGET(color_range_grid), advanced_view);
    gtk_widget_set_visible(GTK_WIDGET(video_codec_grid), advanced_view);
    gtk_widget_set_visible(GTK_WIDGET(audio_codec_grid), advanced_view);
    gtk_widget_set_visible(GTK_WIDGET(framerate_mode_grid), advanced_view);
    gtk_widget_set_visible(GTK_WIDGET(notifications_frame), advanced_view);
    gtk_widget_set_visible(GTK_WIDGET(general_frame), advanced_view);
    gtk_widget_set_visible(GTK_WIDGET(split_audio_button), advanced_view);
}

static void quality_combo_box_change_callback(GtkComboBox *widget, gpointer userdata) {
    (void)userdata;
    const gchar *selected_view = gtk_combo_box_get_active_id(widget);
    const bool custom_selected = strcmp(selected_view, "custom") == 0;
    gtk_widget_set_visible(GTK_WIDGET(video_bitrate_grid), custom_selected);
}

static void stream_service_item_change_callback(GtkComboBox *widget, gpointer userdata) {
    (void)userdata;

    GtkEntry *stream_id_entries[3] = { youtube_stream_id_entry, twitch_stream_id_entry, custom_stream_url_entry };
    for(int i = 0; i < 3; ++i) {
        gtk_widget_set_visible(GTK_WIDGET(stream_id_entries[i]), false);
    }

    const gchar *selected_stream_service = gtk_combo_box_get_active_id(widget);
    if(strcmp(selected_stream_service, "youtube") == 0) {
        gtk_label_set_text(stream_key_label, "Stream key: ");
        gtk_widget_set_visible(GTK_WIDGET(youtube_stream_id_entry), true);
        gtk_widget_set_visible(GTK_WIDGET(custom_stream_container_grid), false);
    } else if(strcmp(selected_stream_service, "twitch") == 0) {
        gtk_label_set_text(stream_key_label, "Stream key: ");
        gtk_widget_set_visible(GTK_WIDGET(twitch_stream_id_entry), true);
        gtk_widget_set_visible(GTK_WIDGET(custom_stream_container_grid), false);
    } else if(strcmp(selected_stream_service, "custom") == 0) {
        gtk_label_set_text(stream_key_label, "Url: ");
        gtk_widget_set_visible(GTK_WIDGET(custom_stream_url_entry), true);
        gtk_widget_set_visible(GTK_WIDGET(custom_stream_container_grid), true);
    }
}

static bool is_nv_fbc_installed() {
    void *lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
    if(lib)
        dlclose(lib);
    return lib != nullptr;
}

static bool is_nvenc_installed() {
    void *lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
    if(lib)
        dlclose(lib);
    return lib != nullptr;
}

static bool is_cuda_installed() {
    void *lib = dlopen("libcuda.so.1", RTLD_LAZY);
    if(!lib)
        lib = dlopen("libcuda.so", RTLD_LAZY);

    if(lib)
        dlclose(lib);

    return lib != nullptr;
}

static bool is_hotkey_already_bound_to_another_action_on_same_page(const Hotkey *_current_hotkey, const Hotkey new_hotkey, GtkWidget *page) {
    for(int i = 0; i < num_hotkeys; ++i) {
        if(hotkeys[i] != _current_hotkey && hotkeys[i]->page == page && hotkeys[i]->keysym == new_hotkey.keysym && hotkeys[i]->modkey_mask == new_hotkey.modkey_mask)
            return true;
    }
    return false;
}

static GdkFilterReturn hotkey_filter_callback(GdkXEvent *xevent, GdkEvent*, gpointer userdata) {
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        return GDK_FILTER_CONTINUE;

    if(hotkey_mode == HotkeyMode::NoAction)
        return GDK_FILTER_CONTINUE;

    PageNavigationUserdata *_page_navigation_userdata = (PageNavigationUserdata*)userdata;
    XEvent *ev = (XEvent*)xevent;
    if(ev->type != KeyPress && ev->type != KeyRelease)
        return GDK_FILTER_CONTINUE;

    Display *display = gdk_x11_get_default_xdisplay();
    KeySym key_sym = XLookupKeysym(&ev->xkey, 0);

    if(hotkey_mode == HotkeyMode::Record && ev->type == KeyRelease) {
        const GtkWidget *visible_page = gtk_stack_get_visible_child(_page_navigation_userdata->stack);
        for(int i = 0; i < num_hotkeys; ++i) {
            if(visible_page != hotkeys[i]->page)
                continue;

            if(key_sym == hotkeys[i]->keysym && key_state_without_locks(ev->xkey.state) == key_mod_mask_to_x11_mask(hotkeys[i]->modkey_mask))
                hotkeys[i]->trigger_handler(hotkeys[i]->associated_button, _page_navigation_userdata->app);
        }
        return GDK_FILTER_CONTINUE;
    }

    if(hotkey_mode != HotkeyMode::NewHotkey)
        return GDK_FILTER_CONTINUE;

    if(ev->type == KeyPress && key_sym == XK_Escape) {
        if(pressed_hotkey.modkey_mask == None && pressed_hotkey.keysym == None) {
            ungrab_keyboard(display);
            current_hotkey = nullptr;
            hotkey_mode = HotkeyMode::Record;
        }
        return GDK_FILTER_CONTINUE;
    }

    if(ev->type == KeyPress && key_sym == XK_BackSpace) {
        if(pressed_hotkey.modkey_mask == None && pressed_hotkey.keysym == None) {
            ungrab_keyboard(display);
            grab_ungrab_hotkey_combo(gdk_x11_get_default_xdisplay(), *current_hotkey, false);
            gtk_entry_set_text(GTK_ENTRY(current_hotkey->hotkey_entry), "");
            current_hotkey->grab_success = true;
            current_hotkey->keysym = None;
            current_hotkey->modkey_mask = 0;
            current_hotkey = nullptr;
            hotkey_mode = HotkeyMode::Record;
            save_configs();
        }
        return GDK_FILTER_CONTINUE;
    }

    if(ev->type == KeyPress) {
        // Ignore already pressed key
        if(key_is_modifier(key_sym)) {
            if(pressed_hotkey.modkey_mask & modkey_to_mask(key_sym))
                return GDK_FILTER_CONTINUE;
            pressed_hotkey.modkey_mask |= modkey_to_mask(key_sym);
        } else {
            if(key_sym == pressed_hotkey.keysym)
                return GDK_FILTER_CONTINUE;
            pressed_hotkey.keysym = key_sym;
        }

        latest_hotkey = pressed_hotkey;
        set_hotkey_text_from_hotkey_data(GTK_ENTRY(current_hotkey->hotkey_entry), latest_hotkey);
    }

    if(ev->type == KeyRelease) {
        if(key_is_modifier(key_sym)) {
            pressed_hotkey.modkey_mask &= ~modkey_to_mask(key_sym);
        } else if(key_sym == pressed_hotkey.keysym) {
            pressed_hotkey.keysym = None;
        }

        if(pressed_hotkey.modkey_mask == None && pressed_hotkey.keysym == None && latest_hotkey.keysym == None) {
            set_hotkey_text_from_hotkey_data(GTK_ENTRY(current_hotkey->hotkey_entry), *current_hotkey);
            return GDK_FILTER_CONTINUE;
        }

        if(pressed_hotkey.modkey_mask == None && pressed_hotkey.keysym == None) {
            ungrab_keyboard(display);

            if(is_hotkey_already_bound_to_another_action_on_same_page(current_hotkey, latest_hotkey, gtk_stack_get_visible_child(_page_navigation_userdata->stack))) {
                const std::string hotkey_text = gtk_entry_get_text(GTK_ENTRY(current_hotkey->hotkey_entry));
                const std::string error_text = "Hotkey " + hotkey_text + " can't be used because it's used for something else. Please choose another hotkey.";
                set_hotkey_text_from_hotkey_data(GTK_ENTRY(current_hotkey->hotkey_entry), *current_hotkey);
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", error_text.c_str());
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);

                current_hotkey = nullptr;
                hotkey_mode = HotkeyMode::Record;
                return GDK_FILTER_CONTINUE;
            }

            const Hotkey prev_current_hotkey = *current_hotkey;
            current_hotkey->keysym = latest_hotkey.keysym;
            current_hotkey->modkey_mask = latest_hotkey.modkey_mask;

            const std::string hotkey_text = gtk_entry_get_text(GTK_ENTRY(current_hotkey->hotkey_entry));
            if(replace_grabbed_keys_depending_on_active_page()) {
                save_configs();
                current_hotkey = nullptr;
                hotkey_mode = HotkeyMode::Record;
                return GDK_FILTER_CONTINUE;
            } else {
                const std::string error_text = "Hotkey " + hotkey_text + " can't be used because it's used by another program. Please choose another hotkey.";

                current_hotkey->keysym = prev_current_hotkey.keysym;
                current_hotkey->modkey_mask = prev_current_hotkey.modkey_mask;
                set_hotkey_text_from_hotkey_data(GTK_ENTRY(current_hotkey->hotkey_entry), *current_hotkey);

                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", error_text.c_str());
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);

                return GDK_FILTER_CONTINUE;
            }
        }
    }

    return GDK_FILTER_CONTINUE;
}

static gboolean on_hotkey_entry_click(GtkWidget *button, gpointer) {
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        return true;

    pressed_hotkey.hotkey_entry = nullptr;
    pressed_hotkey.hotkey_active_label = nullptr;
    pressed_hotkey.keysym = None;
    pressed_hotkey.modkey_mask = 0;
    latest_hotkey = pressed_hotkey;

    current_hotkey = nullptr;
    for(int i = 0; i < num_hotkeys; ++i) {
        if(button == hotkeys[i]->hotkey_entry) {
            current_hotkey = hotkeys[i];
            break;
        }
    }

    if(!current_hotkey)
        return true;

    hotkey_mode = HotkeyMode::NewHotkey;

    gtk_grab_add(current_hotkey->hotkey_entry);
    gtk_widget_set_visible(current_hotkey->hotkey_active_label, true);

    Display *display = gdk_x11_get_default_xdisplay();
    XErrorHandler prev_error_handler = XSetErrorHandler(xerror_dummy);
    XGrabKeyboard(display, DefaultRootWindow(display), False, GrabModeAsync, GrabModeAsync, CurrentTime);
    XSync(display, False);
    XSetErrorHandler(prev_error_handler);
    return true;
}

static void parse_system_info_line(GsrInfo *_gsr_info, const std::string &line) {
    const size_t space_index = line.find('|');
    if(space_index == std::string::npos)
        return;

    const StringView attribute_name = {line.c_str(), space_index};
    const StringView attribute_value = {line.c_str() + space_index + 1, line.size() - (space_index + 1)};
    if(attribute_name == "display_server") {
        if(attribute_value == "x11")
            _gsr_info->system_info.display_server = DisplayServer::X11;
        else if(attribute_value == "wayland")
            _gsr_info->system_info.display_server = DisplayServer::WAYLAND;
    } else if(attribute_name == "is_steam_deck") {
        _gsr_info->system_info.is_steam_deck = attribute_value == "yes";
    } else if(attribute_name == "supports_app_audio") {
        _gsr_info->system_info.supports_app_audio = attribute_value == "yes";
    }
}

static void parse_gpu_info_line(GsrInfo *_gsr_info, const std::string &line) {
    const size_t space_index = line.find('|');
    if(space_index == std::string::npos)
        return;

    const StringView attribute_name = {line.c_str(), space_index};
    const StringView attribute_value = {line.c_str() + space_index + 1, line.size() - (space_index + 1)};
    if(attribute_name == "vendor") {
        if(attribute_value == "amd")
            _gsr_info->gpu_info.vendor = GpuVendor::AMD;
        else if(attribute_value == "intel")
            _gsr_info->gpu_info.vendor = GpuVendor::INTEL;
        else if(attribute_value == "nvidia")
            _gsr_info->gpu_info.vendor = GpuVendor::NVIDIA;
        else if(attribute_value == "broadcom")
            _gsr_info->gpu_info.vendor = GpuVendor::BROADCOM;
        else if(attribute_value == "apple")
            _gsr_info->gpu_info.vendor = GpuVendor::APPLE;
    }
}

static void parse_video_codecs_line(GsrInfo *_gsr_info, const std::string &line) {
    if(line == "h264")
        _gsr_info->supported_video_codecs.h264 = true;
    else if(line == "h264_software")
        _gsr_info->supported_video_codecs.h264_software = true;
    else if(line == "hevc")
        _gsr_info->supported_video_codecs.hevc = true;
    else if(line == "hevc_hdr")
        _gsr_info->supported_video_codecs.hevc_hdr = true;
    else if(line == "hevc_10bit")
        _gsr_info->supported_video_codecs.hevc_10bit = true;
    else if(line == "av1")
        _gsr_info->supported_video_codecs.av1 = true;
    else if(line == "av1_hdr")
        _gsr_info->supported_video_codecs.av1_hdr = true;
    else if(line == "av1_10bit")
        _gsr_info->supported_video_codecs.av1_10bit = true;
    else if(line == "vp8")
        _gsr_info->supported_video_codecs.vp8 = true;
    else if(line == "vp9")
        _gsr_info->supported_video_codecs.vp9 = true;
}

static GsrMonitor capture_option_line_to_monitor(const std::string &line) {
    size_t space_index = line.find('|');
    if(space_index == std::string::npos)
        return { line, {0, 0} };

    vec2i size = {0, 0};
    if(sscanf(line.c_str() + space_index + 1, "%dx%d", &size.x, &size.y) != 2)
        size = {0, 0};

    return { line.substr(0, space_index), size };
}

static void parse_capture_options_line(GsrInfo *_gsr_info, const std::string &line) {
    if(line == "window")
        _gsr_info->supported_capture_options.window = true;
    else if(line == "focused")
        _gsr_info->supported_capture_options.focused = true;
    else if(line == "portal")
        _gsr_info->supported_capture_options.portal = true;
    else if(line == "region") // Only used by the capture panel
        _gsr_info->supported_capture_options.region = true;
    else if(!line.empty() && line[0] != '/')
        _gsr_info->supported_capture_options.monitors.push_back(capture_option_line_to_monitor(line));
}

enum class GsrInfoSection {
    UNKNOWN,
    SYSTEM_INFO,
    GPU_INFO,
    VIDEO_CODECS,
    CAPTURE_OPTIONS
};

static GsrInfoExitStatus get_gpu_screen_recorder_info(GsrInfo *_gsr_info) {
    *_gsr_info = GsrInfo{};

    FILE *f = popen("gpu-screen-recorder --info", "r");
    if(!f) {
        fprintf(stderr, "error: 'gpu-screen-recorder --info' failed\n");
        return GsrInfoExitStatus::FAILED_TO_RUN_COMMAND;
    }

    char output[8192];
    ssize_t bytes_read = fread(output, 1, sizeof(output) - 1, f);
    if(bytes_read < 0 || ferror(f)) {
        fprintf(stderr, "error: failed to read 'gpu-screen-recorder --info' output\n");
        pclose(f);
        return GsrInfoExitStatus::FAILED_TO_RUN_COMMAND;
    }
    output[bytes_read] = '\0';

    GsrInfoSection section = GsrInfoSection::UNKNOWN;
    string_split_char(output, '\n', [&](StringView line) {
        const std::string line_str(line.str, line.size);

        if(starts_with(line_str, "section=")) {
            const char *section_name = line_str.c_str() + 8;
            if(strcmp(section_name, "system_info") == 0)
                section = GsrInfoSection::SYSTEM_INFO;
            else if(strcmp(section_name, "gpu_info") == 0)
                section = GsrInfoSection::GPU_INFO;
            else if(strcmp(section_name, "video_codecs") == 0)
                section = GsrInfoSection::VIDEO_CODECS;
            else if(strcmp(section_name, "capture_options") == 0)
                section = GsrInfoSection::CAPTURE_OPTIONS;
            else
                section = GsrInfoSection::UNKNOWN;
            return true;
        }

        switch(section) {
            case GsrInfoSection::UNKNOWN: {
                break;
            }
            case GsrInfoSection::SYSTEM_INFO: {
                parse_system_info_line(_gsr_info, line_str);
                break;
            }
            case GsrInfoSection::GPU_INFO: {
                parse_gpu_info_line(_gsr_info, line_str);
                break;
            }
            case GsrInfoSection::VIDEO_CODECS: {
                parse_video_codecs_line(_gsr_info, line_str);
                break;
            }
            case GsrInfoSection::CAPTURE_OPTIONS: {
                parse_capture_options_line(_gsr_info, line_str);
                break;
            }
        }

        return true;
    });

    int status = pclose(f);
    if(WIFEXITED(status)) {
        switch(WEXITSTATUS(status)) {
            case 0:  return GsrInfoExitStatus::OK;
            case 22: return GsrInfoExitStatus::OPENGL_FAILED;
            case 23: return GsrInfoExitStatus::NO_DRM_CARD;
            default: return GsrInfoExitStatus::FAILED_TO_RUN_COMMAND;
        }
    }

    return GsrInfoExitStatus::FAILED_TO_RUN_COMMAND;
}

static void record_area_set_sensitive(GtkCellLayout *cell_layout, GtkCellRenderer *cell, GtkTreeModel *tree_model, GtkTreeIter *iter, gpointer data) {
    (void)cell_layout;
    (void)data;

    gchar *id;
    gtk_tree_model_get(tree_model, iter, 1, &id, -1);
    g_object_set(cell, "sensitive", is_video_capture_option_enabled(id), NULL);
    g_free(id);
}

static void video_codec_set_sensitive(GtkCellLayout *cell_layout, GtkCellRenderer *cell, GtkTreeModel *tree_model, GtkTreeIter *iter, gpointer data) {
    (void)cell_layout;
    (void)data;

    gchar *id;
    gtk_tree_model_get(tree_model, iter, 1, &id, -1);
    g_object_set(cell, "sensitive", is_video_codec_enabled(id), NULL);
    g_free(id);
}

static void launch_gsr_ui(bool launched_by_daemon) {
    const char *args[] = { "gsr-ui", launched_by_daemon ? "launch-daemon" : "launch-show", nullptr };
    execvp(args[0], (char* const*)args);
    // TODO: This is incorrect because window wont be defined here if this is called from startup.
    // This is fine for not because this is only called inside the flatpak where gsr-ui is always available.
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "gsr-ui (gpu-screen-recorder-ui) isn't installed. Please install it first.");
    set_dialog_selectable(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static bool kms_server_proxy_setup_gsr_ui(const char *msg) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", msg);
    const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    switch(response) {
        case GTK_RESPONSE_YES:
            break;
        case GTK_RESPONSE_NO:
        default: {
            config.main_config.use_new_ui = false;
            save_config(config);
            return false;
        }
    }

    const int exit_code = system("flatpak-spawn --host -- /var/lib/flatpak/app/com.dec05eba.gpu_screen_recorder/current/active/files/bin/kms-server-proxy setup-gsr-ui");
    if(exit_code != 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to setup the new UI. You either cancelled the installation or you don't have pkexec installed and a polkit agent running.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        config.main_config.use_new_ui = false;
        // save_config instead of save_configs because this can run at startup (startup_new_ui), before the widgets that save_configs reads are created
        save_config(config);
        return false;
    }

    config.main_config.use_new_ui = true;
    config.main_config.installed_gsr_global_hotkeys_version = GSR_CURRENT_GLOBAL_HOTKEYS_CODE_VERSION;
    save_config(config);
    return true;
}

static gboolean on_click_switch_to_new_ui(GtkButton*, gpointer) {
    if(!dpy) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "The new UI only works on X11 or through XWayland on Wayland. Native Wayland is not supported because Wayland is missing features required by this software.\n"
            "Install X11 on your system to use the new UI.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return true;
    }

    if(!is_pkexec_installed()) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "pkexec needs to be installed to switch to the new UI");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return true;
    }

    if(!flatpak_is_installed_as_system()) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "GPU Screen Recorder needs to be installed system-wide to use the new UI. You can run this command to install GPU Screen recorder system-wide:\n"
            "flatpak install --system com.dec05eba.gpu_screen_recorder\n");
        set_dialog_selectable(dialog);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return true;
    }

    GtkWidget *dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "You are about to try out the new UI, which is a ShadowPlay-like fullscreen UI. It runs in the background and you have to show/hide it by pressing Left Alt+Z.\n"
        "This new UI is still experimental and you may experience issues depending on your system, especially on Wayland since Wayland doesn't support this software properly.\n"
        "You can switch back to the old UI at any time by opening the UI and clicking on the settings button and clicking on the \"Go back to the old UI\" button.\n"
        "\n"
        "This new UI comes with new features, such as being able to automatically launch it on system startup by enabling it in settings, and hotkey support on any Wayland compositor.\n"
        "\n"
        "If you are using keyboard remapping software such as keyd then make sure it ignores \"gsr-ui virtual keyboard\" (dec0:5eba device id), or it will prevent you from using your keyboard.\n"
        "You can alternatively go into GPU Screen Recorder settings on the right in the UI and change hotkeys to only grab virtual devices or to not grab devices.\n"
        "You can go back to the old UI by pressing (left) ctrl+shift+alt+esc if this happens.\n"
        "\n"
        "If you are using an NVIDIA GPU then you may experience issue with recording/replay if a suspend happens while recording/using replay. This is an NVIDIA driver issue and it also happens in the old UI.\n"
        "See this for a workaround: <a href=\"https://wiki.archlinux.org/title/NVIDIA/Tips_and_tricks#Preserve_video_memory_after_suspend\">Arch Wiki - Preserve video memory after suspend</a>.\n"
        "\n"
        "Are you sure you want to switch to the new UI?");
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    switch(response) {
        case GTK_RESPONSE_YES:
            break;
        case GTK_RESPONSE_NO:
        default:
            return true;
    }

    const bool kms_server_setup_finished = kms_server_proxy_setup_gsr_ui(
        "The new UI needs root privileges to finish setup to make global hotkeys and recording work on any system. The new UI will also be added to system startup.\n"
        "\n"
        "Are you sure you want to continue?");
    if(!kms_server_setup_finished)
        return true;

    const bool startup_installed_successfully = (system("gsr-ui install-startup") == 0);
    if(!startup_installed_successfully) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Failed to add GPU Screen Recorder to system startup. If you want the new UI to start on system startup then you need\n"
            "to either install and configure \"dex\" and go into GPU Screen Recorder settings (the icon on the right)\n"
            "and enable system startup or add this command to system startup:\n"
            "flatpak run com.dec05eba.gpu_screen_recorder gsr-ui");
        set_dialog_selectable(dialog);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    launch_gsr_ui(false);
    g_application_quit(G_APPLICATION(select_window_userdata.app));
    return true;
}

static GtkWidget* create_common_settings_page(GtkStack *stack, GtkApplication *app) {
    GtkGrid *main_grid = GTK_GRID(gtk_grid_new());
    gtk_stack_add_named(stack, GTK_WIDGET(main_grid), "common-settings");
    gtk_widget_set_vexpand(GTK_WIDGET(main_grid), true);
    gtk_widget_set_hexpand(GTK_WIDGET(main_grid), true);
    gtk_grid_set_row_spacing(main_grid, 10);
    gtk_grid_set_column_spacing(main_grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(main_grid), 10, 10, 10, 10);

    int main_grid_row = 0;
    int grid_row = 0;
    int record_area_row = 0;
    int audio_input_area_row = 0;
    int video_input_area_row = 0;
    int notifications_area_row = 0;
    int general_area_row = 0;

    GtkGrid *simple_advanced_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_column_spacing(simple_advanced_grid, 10);
    gtk_grid_attach(main_grid, GTK_WIDGET(simple_advanced_grid), 0, main_grid_row++, flatpak ? 3 : 2, 1);

    gtk_grid_attach(simple_advanced_grid, gtk_label_new("View: "), 0, 0, 1, 1);
    view_combo_box = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(view_combo_box, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_combo_box_text_append(view_combo_box, "simple", "Simple");
    gtk_combo_box_text_append(view_combo_box, "advanced", "Advanced");
    gtk_widget_set_hexpand(GTK_WIDGET(view_combo_box), true);
    gtk_grid_attach(simple_advanced_grid, GTK_WIDGET(view_combo_box), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(view_combo_box), 0);
    g_signal_connect(view_combo_box, "changed", G_CALLBACK(view_combo_box_change_callback), view_combo_box);

    if(flatpak) {
        GtkButton *switch_to_new_ui_button = GTK_BUTTON(gtk_button_new_with_label("Try out the new UI"));
        gtk_grid_attach(simple_advanced_grid, GTK_WIDGET(switch_to_new_ui_button), 2, 0, 1, 1);
        g_signal_connect(switch_to_new_ui_button, "clicked", G_CALLBACK(on_click_switch_to_new_ui), nullptr);
    }

    GtkScrolledWindow *scrolled_window = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_scrolled_window_set_min_content_width(scrolled_window, 100);
    gtk_scrolled_window_set_min_content_height(scrolled_window, 100);
    gtk_scrolled_window_set_max_content_width(scrolled_window, 1200);
    gtk_scrolled_window_set_max_content_height(scrolled_window, 700);
    gtk_scrolled_window_set_propagate_natural_width(scrolled_window, true);
    gtk_scrolled_window_set_propagate_natural_height(scrolled_window, true);
    gtk_grid_attach(main_grid, GTK_WIDGET(scrolled_window), 0, main_grid_row++, 2, 1);

    GtkGrid *scrolled_window_grid = GTK_GRID(gtk_grid_new());
    gtk_container_add(GTK_CONTAINER(scrolled_window), GTK_WIDGET(scrolled_window_grid));
    gtk_widget_set_vexpand(GTK_WIDGET(scrolled_window_grid), true);
    gtk_widget_set_hexpand(GTK_WIDGET(scrolled_window_grid), true);
    gtk_grid_set_row_spacing(scrolled_window_grid, 0);
    gtk_grid_set_column_spacing(scrolled_window_grid, 0);
    gtk_widget_set_margin(GTK_WIDGET(scrolled_window_grid), 0, 0, 0, 0);

    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    gtk_container_add(GTK_CONTAINER(scrolled_window_grid), GTK_WIDGET(grid));
    gtk_widget_set_halign(GTK_WIDGET(grid), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(grid), GTK_ALIGN_START);
    gtk_widget_set_vexpand(GTK_WIDGET(grid), true);
    gtk_widget_set_hexpand(GTK_WIDGET(grid), true);
    gtk_grid_set_row_spacing(grid, 10);
    gtk_grid_set_column_spacing(grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(grid), 10, 10, 25, 25);

    GtkFrame *capture_target_frame = GTK_FRAME(gtk_frame_new("Capture target"));
    gtk_grid_attach(grid, GTK_WIDGET(capture_target_frame), 0, grid_row++, 2, 1);

    GtkGrid *record_area_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_vexpand(GTK_WIDGET(record_area_grid), false);
    gtk_widget_set_hexpand(GTK_WIDGET(record_area_grid), true);
    gtk_grid_set_row_spacing(record_area_grid, 10);
    gtk_grid_set_column_spacing(record_area_grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(record_area_grid), 10, 10, 10, 10);
    gtk_container_add(GTK_CONTAINER(capture_target_frame), GTK_WIDGET(record_area_grid));

    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkTreeIter iter;
    record_area_selection_model = GTK_TREE_MODEL(store);

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, "Window (Not available on Wayland)", -1);
        gtk_list_store_set(store, &iter, 1, "window", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, "Follow focused window (Not available on Wayland)", -1);
        gtk_list_store_set(store, &iter, 1, "focused", -1);
    } else {
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, "Window", -1);
        gtk_list_store_set(store, &iter, 1, "window", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, "Follow focused window", -1);
        gtk_list_store_set(store, &iter, 1, "focused", -1);
    }

    const bool allow_screen_capture = is_monitor_capture_drm() || nvfbc_installed;
    if(allow_screen_capture) {
        for(const auto &monitor : gsr_info.supported_capture_options.monitors) {
            std::string label = "Monitor ";
            label += monitor.name;
            label += " (";
            label += std::to_string(monitor.size.x);
            label += "x";
            label += std::to_string(monitor.size.y);
            if(flatpak && is_monitor_capture_drm()) {
                label += ", requires root access";
            }
            label += ")";

            // Leak on purpose, what are you gonna do? stab me?
            char *id = (char*)malloc(monitor.name.size() + 1);
            memcpy(id, monitor.name.c_str(), monitor.name.size());
            id[monitor.name.size()] = '\0';

            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, label.c_str(), -1);
            gtk_list_store_set(store, &iter, 1, id, -1);
        }

        if(gsr_info.supported_capture_options.monitors.empty() && gsr_info.system_info.display_server == DisplayServer::WAYLAND && !gsr_info.supported_capture_options.portal) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "No monitors to record found. Make sure GPU Screen Recorder is running on the same GPU device that is displaying graphics on the screen.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            g_application_quit(G_APPLICATION(select_window_userdata.app));
            return GTK_WIDGET(main_grid); // TODO: ???
        }
    }

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_capture_options.portal ? "Desktop portal" : "Desktop portal (Not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "portal", -1);
    } else {
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, "Desktop portal (Not available on X11)", -1);
        gtk_list_store_set(store, &iter, 1, "portal", -1);
    }

    record_area_selection_menu = GTK_COMBO_BOX(gtk_combo_box_new_with_model(record_area_selection_model));
    g_signal_connect(record_area_selection_menu, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(record_area_selection_menu), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(record_area_selection_menu), renderer, "text", 0, NULL);
    gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(record_area_selection_menu), renderer, record_area_set_sensitive, NULL, NULL);

    if(allow_screen_capture && !gsr_info.supported_capture_options.portal && gsr_info.supported_capture_options.monitors.empty())
        gtk_combo_box_set_active(record_area_selection_menu, 0);
    else if(allow_screen_capture || gsr_info.supported_capture_options.portal)
        gtk_combo_box_set_active(record_area_selection_menu, 2);
    else
        gtk_combo_box_set_active(record_area_selection_menu, 0);

    gtk_widget_set_hexpand(GTK_WIDGET(record_area_selection_menu), true);
    gtk_grid_attach(record_area_grid, GTK_WIDGET(record_area_selection_menu), 0, record_area_row++, 3, 1);

    g_signal_connect(record_area_selection_menu, "changed", G_CALLBACK(record_area_item_change_callback), NULL);

    select_window_button = GTK_BUTTON(gtk_button_new_with_label("Select window..."));
    gtk_widget_set_hexpand(GTK_WIDGET(select_window_button), true);
    g_signal_connect(select_window_button, "clicked", G_CALLBACK(on_select_window_button_click), app);
    gtk_grid_attach(record_area_grid, GTK_WIDGET(select_window_button), 0, record_area_row++, 3, 1);

    change_video_resolution_button = gtk_check_button_new_with_label("Change video resolution");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(change_video_resolution_button), false);
    gtk_widget_set_halign(change_video_resolution_button, GTK_ALIGN_START);
    g_signal_connect(change_video_resolution_button, "clicked", G_CALLBACK(on_change_video_resolution_button_click), app);
    gtk_grid_attach(record_area_grid, change_video_resolution_button, 0, record_area_row++, 3, 1);

    {
        area_size_grid = GTK_GRID(gtk_grid_new());
        gtk_grid_set_row_spacing(area_size_grid, 10);
        gtk_grid_attach(record_area_grid, GTK_WIDGET(area_size_grid), 0, record_area_row++, 3, 1);

        GtkLabel *video_resolution_label = GTK_LABEL(gtk_label_new("Video resolution limit: "));
        gtk_label_set_xalign(video_resolution_label, 0.0f);
        gtk_grid_attach(area_size_grid, GTK_WIDGET(video_resolution_label), 0, 0, 3, 1);

        area_width_entry = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5.0, 10000.0, 1.0));
        g_signal_connect(area_width_entry, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
        gtk_spin_button_set_value(area_width_entry, 1920.0);
        gtk_widget_set_hexpand(GTK_WIDGET(area_width_entry), true);
        gtk_grid_attach(area_size_grid, GTK_WIDGET(area_width_entry), 0, 1, 1, 1);

        gtk_grid_attach(area_size_grid, gtk_label_new("x"), 1, 1, 1, 1);

        area_height_entry = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5.0, 10000.0, 1.0));
        g_signal_connect(area_height_entry, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
        gtk_spin_button_set_value(area_height_entry, 1080.0);
        gtk_widget_set_hexpand(GTK_WIDGET(area_height_entry), true);
        gtk_grid_attach(area_size_grid, GTK_WIDGET(area_height_entry), 2, 1, 1, 1);
    }

    {
        video_resolution_grid = GTK_GRID(gtk_grid_new());
        gtk_grid_set_row_spacing(video_resolution_grid, 10);
        gtk_grid_attach(record_area_grid, GTK_WIDGET(video_resolution_grid), 0, record_area_row++, 3, 1);

        GtkLabel *video_resolution_label = GTK_LABEL(gtk_label_new("Video resolution: "));
        gtk_label_set_xalign(video_resolution_label, 0.0f);
        gtk_grid_attach(video_resolution_grid, GTK_WIDGET(video_resolution_label), 0, 0, 3, 1);

        video_width_entry = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5.0, 10000.0, 1.0));
        g_signal_connect(video_width_entry, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
        gtk_spin_button_set_value(video_width_entry, 1920.0);
        gtk_widget_set_hexpand(GTK_WIDGET(video_width_entry), true);
        gtk_grid_attach(video_resolution_grid, GTK_WIDGET(video_width_entry), 0, 1, 1, 1);

        gtk_grid_attach(video_resolution_grid, gtk_label_new("x"), 1, 1, 1, 1);

        video_height_entry = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5.0, 10000.0, 1.0));
        g_signal_connect(video_height_entry, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
        gtk_spin_button_set_value(video_height_entry, 1080.0);
        gtk_widget_set_hexpand(GTK_WIDGET(video_height_entry), true);
        gtk_grid_attach(video_resolution_grid, GTK_WIDGET(video_height_entry), 2, 1, 1, 1);
    }

    restore_portal_session_button = gtk_check_button_new_with_label("Restore portal session");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restore_portal_session_button), true);
    gtk_widget_set_halign(restore_portal_session_button, GTK_ALIGN_START);
    gtk_grid_attach(record_area_grid, restore_portal_session_button, 0, record_area_row++, 3, 1);

    GtkFrame *audio_input_frame = GTK_FRAME(gtk_frame_new("Audio"));
    gtk_grid_attach(grid, GTK_WIDGET(audio_input_frame), 0, grid_row++, 2, 1);

    GtkGrid *audio_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_vexpand(GTK_WIDGET(audio_grid), false);
    gtk_widget_set_hexpand(GTK_WIDGET(audio_grid), true);
    gtk_grid_set_row_spacing(audio_grid, 10);
    gtk_grid_set_column_spacing(audio_grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(audio_grid), 10, 10, 10, 10);
    gtk_container_add(GTK_CONTAINER(audio_input_frame), GTK_WIDGET(audio_grid));

    {
        int audio_devices_row = 0;

        audio_devices_grid = GTK_GRID(gtk_grid_new());
        gtk_grid_set_row_spacing(audio_devices_grid, 10);
        gtk_grid_set_column_spacing(audio_devices_grid, 10);
        gtk_widget_set_margin(GTK_WIDGET(audio_devices_grid), 0, 0, 0, 0);
        gtk_grid_attach(audio_grid, GTK_WIDGET(audio_devices_grid), 0, audio_input_area_row++, 2, 1);

        GtkGrid *add_audio_grid = GTK_GRID(gtk_grid_new());
        gtk_grid_set_row_spacing(add_audio_grid, 10);
        gtk_grid_set_column_spacing(add_audio_grid, 10);
        gtk_grid_attach(audio_devices_grid, GTK_WIDGET(add_audio_grid), 0, audio_devices_row++, 2, 1);

        GtkWidget *add_audio_device_button = gtk_button_new_with_label("Add audio device");
        gtk_grid_attach(add_audio_grid, add_audio_device_button, 0, 0, 1, 1);
        g_signal_connect(add_audio_device_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
            audio_inputs = get_audio_devices();
            GtkWidget *row = create_audio_device_combo_box_row("");
            gtk_widget_show_all(row);
            gtk_box_pack_start(audio_devices_items_box, row, false, false, 0);
            return true;
        }), nullptr);

        add_application_audio_button = gtk_button_new_with_label("Add application audio");
        gtk_grid_attach(add_audio_grid, add_application_audio_button, 1, 0, 1, 1);
        g_signal_connect(add_application_audio_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
            application_audio = get_application_audio();
            GtkWidget *row = create_application_audio_combo_box_row("");
            gtk_widget_show_all(row);
            gtk_box_pack_start(audio_devices_items_box, row, false, false, 0);
            return true;
        }), nullptr);

        add_custom_application_audio_button = gtk_button_new_with_label("Add custom application audio");
        gtk_grid_attach(add_audio_grid, add_custom_application_audio_button, 3, 0, 1, 1);
        g_signal_connect(add_custom_application_audio_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
            GtkWidget *row = create_application_audio_custom_row("");
            gtk_widget_show_all(row);
            gtk_box_pack_start(audio_devices_items_box, row, false, false, 0);
            return true;
        }), nullptr);

        audio_devices_items_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 10));
        gtk_grid_attach(audio_devices_grid, GTK_WIDGET(audio_devices_items_box), 0, audio_devices_row++, 2, 1);
    }

    split_audio_button = gtk_check_button_new_with_label("Split each device/app audio into separate audio tracks");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(split_audio_button), false);
    gtk_widget_set_halign(split_audio_button, GTK_ALIGN_START);
    gtk_grid_attach(audio_grid, split_audio_button, 0, audio_input_area_row++, 2, 1);

    record_app_audio_inverted_button = gtk_check_button_new_with_label("Record audio from all applications except the selected ones");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(record_app_audio_inverted_button), false);
    gtk_widget_set_halign(record_app_audio_inverted_button, GTK_ALIGN_START);
    gtk_grid_attach(audio_grid, record_app_audio_inverted_button, 0, audio_input_area_row++, 2, 1);

    audio_codec_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(audio_grid, GTK_WIDGET(audio_codec_grid), 0, audio_input_area_row++, 2, 1);
    gtk_grid_attach(audio_codec_grid, gtk_label_new("Audio codec: "), 0, 0, 1, 1);
    audio_codec_input_menu = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(audio_codec_input_menu, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_combo_box_text_append(audio_codec_input_menu, "opus", "Opus (Recommended)");
    gtk_combo_box_text_append(audio_codec_input_menu, "aac", "AAC");
    gtk_widget_set_hexpand(GTK_WIDGET(audio_codec_input_menu), true);
    gtk_grid_attach(audio_codec_grid, GTK_WIDGET(audio_codec_input_menu), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(audio_codec_input_menu), 0);

    GtkFrame *video_input_frame = GTK_FRAME(gtk_frame_new("Video"));
    gtk_grid_attach(grid, GTK_WIDGET(video_input_frame), 0, grid_row++, 2, 1);

    GtkGrid *video_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_vexpand(GTK_WIDGET(video_grid), false);
    gtk_widget_set_hexpand(GTK_WIDGET(video_grid), true);
    gtk_grid_set_row_spacing(video_grid, 10);
    gtk_grid_set_column_spacing(video_grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(video_grid), 10, 10, 10, 10);
    gtk_container_add(GTK_CONTAINER(video_input_frame), GTK_WIDGET(video_grid));

    GtkGrid *video_quality_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(video_grid, GTK_WIDGET(video_quality_grid), 0, video_input_area_row++, 2, 1);
    gtk_grid_attach(video_quality_grid, gtk_label_new("Video quality: "), 0, 0, 1, 1);
    quality_input_menu = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(quality_input_menu, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_combo_box_text_append(quality_input_menu, "custom", "Constant bitrate (Recommended for live streaming and replay)");
    gtk_combo_box_text_append(quality_input_menu, "medium", "Medium");
    gtk_combo_box_text_append(quality_input_menu, "high", "High");
    gtk_combo_box_text_append(quality_input_menu, "very_high", "Very High (Recommended for recording)");
    gtk_combo_box_text_append(quality_input_menu, "ultra", "Ultra");
    gtk_widget_set_hexpand(GTK_WIDGET(quality_input_menu), true);
    gtk_grid_attach(video_quality_grid, GTK_WIDGET(quality_input_menu), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(quality_input_menu), 0);
    g_signal_connect(quality_input_menu, "changed", G_CALLBACK(quality_combo_box_change_callback), quality_input_menu);

    video_bitrate_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(video_grid, GTK_WIDGET(video_bitrate_grid), 0, video_input_area_row++, 2, 1);
    gtk_grid_attach(video_bitrate_grid, gtk_label_new("Video bitrate (kbps): "), 0, 0, 1, 1);
    video_bitrate_entry = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1.0, 500000.0, 1.0));
    g_signal_connect(video_bitrate_entry, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_spin_button_set_value(video_bitrate_entry, 15000.0);
    gtk_widget_set_hexpand(GTK_WIDGET(video_bitrate_entry), true);
    gtk_grid_attach(video_bitrate_grid, GTK_WIDGET(video_bitrate_entry), 1, 0, 1, 1);

    video_codec_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(video_grid, GTK_WIDGET(video_codec_grid), 0, video_input_area_row++, 2, 1);
    gtk_grid_attach(video_codec_grid, gtk_label_new("Video codec: "), 0, 0, 1, 1);

    {
        store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        video_codec_selection_model = GTK_TREE_MODEL(store);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, "Auto (Recommended)", -1);
        gtk_list_store_set(store, &iter, 1, "auto", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.h264 ? "H264 (Largest file size, best software compatibility)" : "H264 (Not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "h264", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.hevc ? "HEVC" : "HEVC (Not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "hevc", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.hevc ? "HEVC (10 bit, reduces banding)" : "HEVC (10 bit, not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "hevc_10bit", -1);

        if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.hevc ? "HEVC (HDR)" : "HEVC (HDR, not available on your system)", -1);
            gtk_list_store_set(store, &iter, 1, "hevc_hdr", -1);
        } else {
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, "HEVC (HDR, not available on X11)", -1);
            gtk_list_store_set(store, &iter, 1, "hevc_hdr", -1);
        }

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.av1 ? "AV1 (Smallest file size, worst software compatibility)" : "AV1 (Not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "av1", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.av1 ? "AV1 (10 bit, reduces banding)" : "AV1 (10 bit, not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "av1_10bit", -1);

        if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.av1 ? "AV1 (HDR)" : "AV1 (HDR, not available on your system)", -1);
            gtk_list_store_set(store, &iter, 1, "av1_hdr", -1);
        } else {
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, "AV1 (HDR, not available on X11)", -1);
            gtk_list_store_set(store, &iter, 1, "av1_hdr", -1);
        }

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.vp8 ? "VP8" : "VP8 (Not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "vp8", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.vp9 ? "VP9" : "VP9 (Not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "vp9", -1);

        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, gsr_info.supported_video_codecs.h264_software ? "H264 Software Encoder (Slow, not recommeded)" : "H264 Software Encoder (Not available on your system)", -1);
        gtk_list_store_set(store, &iter, 1, "h264_software", -1);

        video_codec_selection_menu = GTK_COMBO_BOX(gtk_combo_box_new_with_model(video_codec_selection_model));
        g_signal_connect(video_codec_selection_menu, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);

        renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(video_codec_selection_menu), renderer, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(video_codec_selection_menu), renderer, "text", 0, NULL);
        gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(video_codec_selection_menu), renderer, video_codec_set_sensitive, NULL, NULL);

        gtk_combo_box_set_active(video_codec_selection_menu, 0);

        gtk_widget_set_hexpand(GTK_WIDGET(video_codec_selection_menu), true);
        gtk_grid_attach(video_codec_grid, GTK_WIDGET(video_codec_selection_menu), 1, 0, 1, 1);
    }

    color_range_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(video_grid, GTK_WIDGET(color_range_grid), 0, video_input_area_row++, 2, 1);
    gtk_grid_attach(color_range_grid, gtk_label_new("Color range: "), 0, 0, 1, 1);
    color_range_input_menu = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(color_range_input_menu, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_combo_box_text_append(color_range_input_menu, "limited", "Limited");
    gtk_combo_box_text_append(color_range_input_menu, "full", "Full");
    gtk_widget_set_hexpand(GTK_WIDGET(color_range_input_menu), true);
    gtk_grid_attach(color_range_grid, GTK_WIDGET(color_range_input_menu), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(color_range_input_menu), 0);

    GtkGrid *fps_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(video_grid, GTK_WIDGET(fps_grid), 0, video_input_area_row++, 2, 1);
    gtk_grid_attach(fps_grid, gtk_label_new("Frame rate: "), 0, 0, 1, 1);
    fps_entry = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1.0, 500.0, 1.0));
    g_signal_connect(fps_entry, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_spin_button_set_value(fps_entry, 60.0);
    gtk_widget_set_hexpand(GTK_WIDGET(fps_entry), true);
    gtk_grid_attach(fps_grid, GTK_WIDGET(fps_entry), 1, 0, 1, 1);

    framerate_mode_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(video_grid, GTK_WIDGET(framerate_mode_grid), 0, video_input_area_row++, 2, 1);
    gtk_grid_attach(framerate_mode_grid, gtk_label_new("Frame rate mode: "), 0, 0, 1, 1);
    framerate_mode_input_menu = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(framerate_mode_input_menu, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_combo_box_text_append(framerate_mode_input_menu, "auto", "Auto (Recommended)");
    gtk_combo_box_text_append(framerate_mode_input_menu, "cfr", "Constant");
    gtk_combo_box_text_append(framerate_mode_input_menu, "vfr", "Variable");
    gtk_combo_box_text_append(framerate_mode_input_menu, "content", "Sync to content (Only X11 or desktop portal capture)");
    gtk_widget_set_hexpand(GTK_WIDGET(framerate_mode_input_menu), true);
    gtk_grid_attach(framerate_mode_grid, GTK_WIDGET(framerate_mode_input_menu), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(framerate_mode_input_menu), 0);

    record_cursor_button = gtk_check_button_new_with_label("Record cursor");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(record_cursor_button), true);
    gtk_widget_set_halign(record_cursor_button, GTK_ALIGN_START);
    gtk_grid_attach(video_grid, record_cursor_button, 0, video_input_area_row++, 2, 1);

    notifications_frame = GTK_FRAME(gtk_frame_new("Notifications"));
    gtk_grid_attach(grid, GTK_WIDGET(notifications_frame), 0, grid_row++, 2, 1);

    GtkGrid *notifications_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_vexpand(GTK_WIDGET(notifications_grid), false);
    gtk_widget_set_hexpand(GTK_WIDGET(notifications_grid), true);
    gtk_grid_set_row_spacing(notifications_grid, 10);
    gtk_grid_set_column_spacing(notifications_grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(notifications_grid), 10, 10, 10, 10);
    gtk_container_add(GTK_CONTAINER(notifications_frame), GTK_WIDGET(notifications_grid));

    show_recording_started_notification_button = gtk_check_button_new_with_label("Show recording/streaming/replay started notification");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_recording_started_notification_button), false);
    gtk_widget_set_halign(show_recording_started_notification_button, GTK_ALIGN_START);
    gtk_grid_attach(notifications_grid, show_recording_started_notification_button, 0, notifications_area_row++, 2, 1);

    show_recording_stopped_notification_button = gtk_check_button_new_with_label("Show streaming/replay stopped notification");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_recording_stopped_notification_button), false);
    gtk_widget_set_halign(show_recording_stopped_notification_button, GTK_ALIGN_START);
    gtk_grid_attach(notifications_grid, show_recording_stopped_notification_button, 0, notifications_area_row++, 2, 1);

    show_recording_saved_notification_button = gtk_check_button_new_with_label("Show video saved notification");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_recording_saved_notification_button), true);
    gtk_widget_set_halign(show_recording_saved_notification_button, GTK_ALIGN_START);
    gtk_grid_attach(notifications_grid, show_recording_saved_notification_button, 0, notifications_area_row++, 2, 1);

    general_frame = GTK_FRAME(gtk_frame_new("General"));
    gtk_grid_attach(grid, GTK_WIDGET(general_frame), 0, grid_row++, 2, 1);

    GtkGrid *general_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_vexpand(GTK_WIDGET(general_grid), false);
    gtk_widget_set_hexpand(GTK_WIDGET(general_grid), true);
    gtk_grid_set_row_spacing(general_grid, 10);
    gtk_grid_set_column_spacing(general_grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(general_grid), 10, 10, 10, 10);
    gtk_container_add(GTK_CONTAINER(general_frame), GTK_WIDGET(general_grid));

    exclude_metadata_button = gtk_check_button_new_with_label("Exclude personal metadata from the video (such as audio track title)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(exclude_metadata_button), false);
    gtk_widget_set_halign(exclude_metadata_button, GTK_ALIGN_START);
    gtk_grid_attach(general_grid, exclude_metadata_button, 0, general_area_row++, 2, 1);

    GtkGrid *start_button_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(main_grid, GTK_WIDGET(start_button_grid), 0, main_grid_row++, 2, 1);
    gtk_grid_set_column_spacing(start_button_grid, 10);

    stream_button = GTK_BUTTON(gtk_button_new_with_label("Stream"));
    GtkWidget *go_next_stream = gtk_image_new_from_icon_name("go-next", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(stream_button, go_next_stream);
    gtk_button_set_always_show_image(stream_button, true);
    gtk_button_set_image_position(stream_button, GTK_POS_RIGHT);
    gtk_widget_set_hexpand(GTK_WIDGET(stream_button), true);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(stream_button), 0, 0, 1, 1);

    record_button = GTK_BUTTON(gtk_button_new_with_label("Record"));
    GtkWidget *go_next_record = gtk_image_new_from_icon_name("go-next", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(record_button, go_next_record);
    gtk_button_set_always_show_image(record_button, true);
    gtk_button_set_image_position(record_button, GTK_POS_RIGHT);
    gtk_widget_set_hexpand(GTK_WIDGET(record_button), true);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(record_button), 1, 0, 1, 1);

    replay_button = GTK_BUTTON(gtk_button_new_with_label("Replay"));
    GtkWidget *go_next_replay = gtk_image_new_from_icon_name("go-next", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(replay_button, go_next_replay);
    gtk_button_set_always_show_image(replay_button, true);
    gtk_button_set_image_position(replay_button, GTK_POS_RIGHT);
    gtk_widget_set_hexpand(GTK_WIDGET(replay_button), true);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(replay_button), 2, 0, 1, 1);

    gtk_widget_set_sensitive(GTK_WIDGET(replay_button), false);
    gtk_widget_set_sensitive(GTK_WIDGET(record_button), false);
    gtk_widget_set_sensitive(GTK_WIDGET(stream_button), false);

    return GTK_WIDGET(main_grid);
}

static void deactivated_callback(const char *description, void *userdata) {
    (void)userdata;
    const GtkWidget *visible_page = gtk_stack_get_visible_child(page_navigation_userdata.stack);
    for(int i = 0; i < num_hotkeys; ++i) {
        if(visible_page != hotkeys[i]->page)
            continue;

        if(strcmp(description, hotkeys[i]->shortcut_id) == 0)
            hotkeys[i]->trigger_handler(hotkeys[i]->associated_button, page_navigation_userdata.app);
    }
}

static void add_wayland_global_hotkeys_ui(GtkGrid *grid, int &row, int width) {
    GtkGrid *aa_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_halign(GTK_WIDGET(aa_grid), GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, GTK_WIDGET(aa_grid), 0, row++, width, 1);
    gtk_grid_set_column_spacing(aa_grid, 10);

    if(wayland_compositor == WaylandCompositor::KDE_OLD) {
        gtk_grid_attach(aa_grid, gtk_label_new("Hotkeys are managed externally on KDE Plasma Wayland. Click here to change hotkeys:"), 0, 0, 1, 1);

        GtkButton *register_hotkeys_button = GTK_BUTTON(gtk_button_new_with_label("Change hotkeys"));
        gtk_widget_set_hexpand(GTK_WIDGET(register_hotkeys_button), true);
        //gtk_widget_set_halign(GTK_WIDGET(register_hotkeys_button), GTK_ALIGN_START);
        g_signal_connect(register_hotkeys_button, "clicked", G_CALLBACK(on_register_hotkeys_button_clicked), nullptr);
        gtk_grid_attach(aa_grid, GTK_WIDGET(register_hotkeys_button), 1, 0, 1, 1);
    } else {
        gtk_grid_attach(aa_grid, gtk_label_new("Hotkeys are managed externally on Wayland. Go into your system application/hotkey settings to change hotkeys."), 0, 0, 1, 1);
    }

    row++;
    gtk_grid_attach(grid, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row, width, 1);
    row++;
}

static void create_replay_hotkey_items(GtkGrid *parent_grid, int row, int num_columns) {
    replay_hotkeys_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_halign(GTK_WIDGET(replay_hotkeys_grid), GTK_ALIGN_START);
    gtk_grid_set_row_spacing(replay_hotkeys_grid, 10);
    gtk_grid_set_column_spacing(replay_hotkeys_grid, 10);
    gtk_grid_attach(parent_grid, GTK_WIDGET(replay_hotkeys_grid), 0, row, num_columns, 1);
    int hotkeys_row = 0;

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        add_wayland_global_hotkeys_ui(replay_hotkeys_grid, hotkeys_row, num_columns);

    {
        gtk_grid_attach(replay_hotkeys_grid, gtk_label_new("Press"), 0, hotkeys_row, 1, 1);

        replay_start_stop_hotkey_button = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(replay_start_stop_hotkey_button), gsr_info.system_info.display_server == DisplayServer::WAYLAND ? "" : "Alt + 1");
        g_signal_connect(replay_start_stop_hotkey_button, "button-press-event", G_CALLBACK(on_hotkey_entry_click), replay_start_stop_hotkey_button);
        gtk_grid_attach(replay_hotkeys_grid, replay_start_stop_hotkey_button, 1, hotkeys_row, 1, 1);

        gtk_grid_attach(replay_hotkeys_grid, gtk_label_new("to start/stop the replay and"), 2, hotkeys_row, 1, 1);

        replay_save_hotkey_button = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(replay_save_hotkey_button), gsr_info.system_info.display_server == DisplayServer::WAYLAND ? "" : "Alt + 2");
        g_signal_connect(replay_save_hotkey_button, "button-press-event", G_CALLBACK(on_hotkey_entry_click), replay_save_hotkey_button);
        gtk_grid_attach(replay_hotkeys_grid, replay_save_hotkey_button, 3, hotkeys_row, 1, 1);

        GtkWidget *save_replay_label = gtk_label_new("to save the replay");
        gtk_widget_set_halign(save_replay_label, GTK_ALIGN_START);
        gtk_widget_set_hexpand(save_replay_label, true);
        gtk_grid_attach(replay_hotkeys_grid, save_replay_label, 4, hotkeys_row, 1, 1);

        ++hotkeys_row;
    }
}

static GtkWidget* create_replay_page(GtkApplication *app, GtkStack *stack) {
    int row = 0;
    const int num_columns = 5;

    std::string video_filepath = get_videos_dir();

    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    gtk_stack_add_named(stack, GTK_WIDGET(grid), "replay");
    gtk_widget_set_vexpand(GTK_WIDGET(grid), true);
    gtk_widget_set_hexpand(GTK_WIDGET(grid), true);
    gtk_grid_set_row_spacing(grid, 10);
    gtk_grid_set_column_spacing(grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(grid), 10, 10, 10, 10);

    GtkWidget *hotkey_active_label = nullptr;
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        replay_hotkeys_not_supported_label = gtk_label_new("Your Wayland compositor doesn't support global hotkeys. Use X11 or KDE Plasma on Wayland if you want to use hotkeys.");
        gtk_grid_attach(grid, replay_hotkeys_not_supported_label, 0, row++, num_columns, 1);
    } else {
        hotkey_active_label = gtk_label_new("Press a key combination to set a new hotkey, backspace to remove the hotkey or esc to cancel.");
        gtk_grid_attach(grid, hotkey_active_label, 0, row++, num_columns, 1);
    }

    create_replay_hotkey_items(grid, row, num_columns);
    ++row;

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        gtk_widget_set_visible(GTK_WIDGET(replay_hotkeys_grid), false);
        gtk_widget_set_visible(GTK_WIDGET(replay_hotkeys_not_supported_label), false);
    }

    gtk_grid_attach(grid, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, num_columns, 1);

    GtkWidget *save_icon = gtk_image_new_from_icon_name("document-save", GTK_ICON_SIZE_BUTTON);

    GtkGrid *file_chooser_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(file_chooser_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(file_chooser_grid, 10);
    GtkWidget *file_chooser_label = gtk_label_new("Where do you want to save the replays?");
    gtk_grid_attach(file_chooser_grid, file_chooser_label, 0, 0, 1, 1);
    replay_file_chooser_button = GTK_BUTTON(gtk_button_new_with_label(video_filepath.c_str()));
    gtk_button_set_image(replay_file_chooser_button, save_icon);
    gtk_button_set_always_show_image(replay_file_chooser_button, true);
    gtk_button_set_image_position(replay_file_chooser_button, GTK_POS_RIGHT);
    gtk_widget_set_hexpand(GTK_WIDGET(replay_file_chooser_button), true);
    g_signal_connect(replay_file_chooser_button, "clicked", G_CALLBACK(on_replay_file_chooser_button_click), nullptr);
    gtk_grid_attach(file_chooser_grid, GTK_WIDGET(replay_file_chooser_button), 1, 0, 1, 1);

    GtkGrid *container_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(container_grid), 0, row++, num_columns, 1);
    gtk_grid_attach(container_grid, gtk_label_new("Container: "), 0, 0, 1, 1);
    replay_container = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(replay_container, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    for(auto &supported_container : supported_containers) {
        gtk_combo_box_text_append(replay_container, supported_container.container_name, supported_container.file_extension);
    }
    if(gsr_info.supported_video_codecs.vp8 || gsr_info.supported_video_codecs.vp9) {
        gtk_combo_box_text_append(replay_container, "webm", "webm");
    }
    gtk_widget_set_hexpand(GTK_WIDGET(replay_container), true);
    gtk_grid_attach(container_grid, GTK_WIDGET(replay_container), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(replay_container), 0);

    GtkGrid *replay_time_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(replay_time_grid), 0, row++, num_columns, 1);
    gtk_grid_attach(replay_time_grid, gtk_label_new("Replay time in seconds: "), 0, 0, 1, 1);
    replay_time_entry = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5.0, 1200.0, 1.0));
    g_signal_connect(replay_time_entry, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_spin_button_set_value(replay_time_entry, 30.0);
    gtk_widget_set_hexpand(GTK_WIDGET(replay_time_entry), true);
    gtk_grid_attach(replay_time_grid, GTK_WIDGET(replay_time_entry), 1, 0, 1, 1);

    GtkGrid *start_button_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(start_button_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(start_button_grid, 10);

    replay_back_button = GTK_BUTTON(gtk_button_new_with_label("Back"));
    GtkWidget *go_previous = gtk_image_new_from_icon_name("go-previous", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(replay_back_button, go_previous);
    gtk_button_set_always_show_image(replay_back_button, true);
    gtk_button_set_image_position(replay_back_button, GTK_POS_LEFT);
    gtk_widget_set_hexpand(GTK_WIDGET(replay_back_button), true);

    gtk_grid_attach(start_button_grid, GTK_WIDGET(replay_back_button), 0, 0, 1, 1);
    start_replay_button = GTK_BUTTON(gtk_button_new_with_label("Start replay"));
    gtk_widget_set_hexpand(GTK_WIDGET(start_replay_button), true);
    g_signal_connect(start_replay_button, "clicked", G_CALLBACK(on_start_replay_button_click), app);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(start_replay_button), 1, 0, 1, 1);

    replay_save_button = GTK_BUTTON(gtk_button_new_with_label("Save replay"));
    gtk_widget_set_hexpand(GTK_WIDGET(replay_save_button), true);
    g_signal_connect(replay_save_button, "clicked", G_CALLBACK(on_replay_save_button_click), app);
    gtk_widget_set_sensitive(GTK_WIDGET(replay_save_button), false);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(replay_save_button), 2, 0, 1, 1);

    gtk_grid_attach(grid, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, num_columns, 1);

    replay_bottom_panel_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(replay_bottom_panel_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(replay_bottom_panel_grid, 5);
    gtk_widget_set_opacity(GTK_WIDGET(replay_bottom_panel_grid), 0.5);
    gtk_widget_set_halign(GTK_WIDGET(replay_bottom_panel_grid), GTK_ALIGN_END);

    GtkWidget *record_icon = gtk_image_new_from_icon_name("media-record", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_valign(record_icon, GTK_ALIGN_CENTER);
    gtk_grid_attach(replay_bottom_panel_grid, record_icon, 0, 0, 1, 1);

    replay_record_time_label = gtk_label_new("00:00:00");
    gtk_widget_set_valign(replay_record_time_label, GTK_ALIGN_CENTER);
    gtk_grid_attach(replay_bottom_panel_grid, replay_record_time_label, 1, 0, 1, 1);

    replay_start_stop_hotkey.modkey_mask = modkey_to_mask(XK_Alt_L);
    replay_start_stop_hotkey.keysym = XK_1;
    replay_start_stop_hotkey.hotkey_entry = replay_start_stop_hotkey_button;
    replay_start_stop_hotkey.hotkey_active_label = hotkey_active_label;
    replay_start_stop_hotkey.config = &config.replay_config.start_stop_recording_hotkey;
    replay_start_stop_hotkey.page = GTK_WIDGET(grid);
    replay_start_stop_hotkey.trigger_handler = on_start_replay_button_click;
    replay_start_stop_hotkey.associated_button = start_replay_button;
    replay_start_stop_hotkey.shortcut_id = SHORTCUT_ID_START_STOP_RECORDING;

    replay_save_hotkey.modkey_mask = modkey_to_mask(XK_Alt_L);
    replay_save_hotkey.keysym = XK_2;
    replay_save_hotkey.hotkey_entry = replay_save_hotkey_button;
    replay_save_hotkey.hotkey_active_label = hotkey_active_label;
    replay_save_hotkey.config = &config.replay_config.save_recording_hotkey;
    replay_save_hotkey.page = GTK_WIDGET(grid);
    replay_save_hotkey.trigger_handler = on_replay_save_button_click;
    replay_save_hotkey.associated_button = replay_save_button;
    replay_save_hotkey.shortcut_id = SHORTCUT_ID_SAVE_REPLAY;

    return GTK_WIDGET(grid);
}

static void create_recording_hotkey_items(GtkGrid *parent_grid, int row, int num_columns) {
    recording_hotkeys_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_halign(GTK_WIDGET(recording_hotkeys_grid), GTK_ALIGN_START);
    gtk_grid_set_row_spacing(recording_hotkeys_grid, 10);
    gtk_grid_set_column_spacing(recording_hotkeys_grid, 10);
    gtk_grid_attach(parent_grid, GTK_WIDGET(recording_hotkeys_grid), 0, row, num_columns, 1);
    int hotkeys_row = 0;

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        add_wayland_global_hotkeys_ui(recording_hotkeys_grid, hotkeys_row, num_columns);

    {
        gtk_grid_attach(recording_hotkeys_grid, gtk_label_new("Press"), 0, hotkeys_row, 1, 1);

        record_start_stop_hotkey_button = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(record_start_stop_hotkey_button), gsr_info.system_info.display_server == DisplayServer::WAYLAND ? "" : "Alt + 1");
        g_signal_connect(record_start_stop_hotkey_button, "button-press-event", G_CALLBACK(on_hotkey_entry_click), record_start_stop_hotkey_button);
        gtk_grid_attach(recording_hotkeys_grid, record_start_stop_hotkey_button, 1, hotkeys_row, 1, 1);

        gtk_grid_attach(recording_hotkeys_grid, gtk_label_new("to start/stop recording and"), 2, hotkeys_row, 1, 1);

        pause_unpause_hotkey_button = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(pause_unpause_hotkey_button), gsr_info.system_info.display_server == DisplayServer::WAYLAND ? "" : "Alt + 2");
        g_signal_connect(pause_unpause_hotkey_button, "button-press-event", G_CALLBACK(on_hotkey_entry_click), pause_unpause_hotkey_button);
        gtk_grid_attach(recording_hotkeys_grid, pause_unpause_hotkey_button, 3, hotkeys_row, 1, 1);

        GtkWidget *pause_unpause_recording_label = gtk_label_new("to pause/unpause recording");
        gtk_widget_set_halign(pause_unpause_recording_label, GTK_ALIGN_START);
        gtk_widget_set_hexpand(pause_unpause_recording_label, true);
        gtk_grid_attach(recording_hotkeys_grid, pause_unpause_recording_label, 4, hotkeys_row, 1, 1);

        ++hotkeys_row;
    }
}

static GtkWidget* create_recording_page(GtkApplication *app, GtkStack *stack) {
    int row = 0;
    const int num_columns = 5;

    std::string video_filepath = get_videos_dir();

    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    gtk_stack_add_named(stack, GTK_WIDGET(grid), "recording");
    gtk_widget_set_vexpand(GTK_WIDGET(grid), true);
    gtk_widget_set_hexpand(GTK_WIDGET(grid), true);
    gtk_grid_set_row_spacing(grid, 10);
    gtk_grid_set_column_spacing(grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(grid), 10, 10, 10, 10);

    GtkWidget *hotkey_active_label = nullptr;
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        recording_hotkeys_not_supported_label = gtk_label_new("Your Wayland compositor doesn't support global hotkeys. Use X11 or KDE Plasma on Wayland if you want to use hotkeys.");
        gtk_grid_attach(grid, recording_hotkeys_not_supported_label, 0, row++, num_columns, 1);
    } else {
        hotkey_active_label = gtk_label_new("Press a key combination to set a new hotkey, backspace to remove the hotkey or esc to cancel.");
        gtk_grid_attach(grid, hotkey_active_label, 0, row++, num_columns, 1);
    }

    create_recording_hotkey_items(grid, row, num_columns);
    ++row;

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        gtk_widget_set_visible(GTK_WIDGET(recording_hotkeys_grid), false);
        gtk_widget_set_visible(GTK_WIDGET(recording_hotkeys_not_supported_label), false);
    }

    gtk_grid_attach(grid, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, num_columns, 1);

    GtkWidget *save_icon = gtk_image_new_from_icon_name("document-save", GTK_ICON_SIZE_BUTTON);

    GtkGrid *file_chooser_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(file_chooser_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(file_chooser_grid, 10);
    GtkWidget *file_chooser_label = gtk_label_new("Where do you want to save the video?");
    gtk_grid_attach(file_chooser_grid, GTK_WIDGET(file_chooser_label), 0, 0, 1, 1);
    record_file_chooser_button = GTK_BUTTON(gtk_button_new_with_label(video_filepath.c_str()));
    gtk_button_set_image(record_file_chooser_button, save_icon);
    gtk_button_set_always_show_image(record_file_chooser_button, true);
    gtk_button_set_image_position(record_file_chooser_button, GTK_POS_RIGHT);
    gtk_widget_set_hexpand(GTK_WIDGET(record_file_chooser_button), true);
    g_signal_connect(record_file_chooser_button, "clicked", G_CALLBACK(on_record_file_choose_button_click), nullptr);
    gtk_grid_attach(file_chooser_grid, GTK_WIDGET(record_file_chooser_button), 1, 0, 1, 1);

    GtkGrid *container_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(container_grid), 0, row++, num_columns, 1);
    gtk_grid_attach(container_grid, gtk_label_new("Container: "), 0, 0, 1, 1);
    record_container = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(record_container, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    for(auto &supported_container : supported_containers) {
        gtk_combo_box_text_append(record_container, supported_container.container_name, supported_container.file_extension);
    }
    if(gsr_info.supported_video_codecs.vp8 || gsr_info.supported_video_codecs.vp9) {
        gtk_combo_box_text_append(record_container, "webm", "webm");
    }
    gtk_widget_set_hexpand(GTK_WIDGET(record_container), true);
    gtk_grid_attach(container_grid, GTK_WIDGET(record_container), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(record_container), 0);

    GtkGrid *start_button_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(start_button_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(start_button_grid, 10);

    record_back_button = GTK_BUTTON(gtk_button_new_with_label("Back"));
    GtkWidget *go_previous = gtk_image_new_from_icon_name("go-previous", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(record_back_button, go_previous);
    gtk_button_set_always_show_image(record_back_button, true);
    gtk_button_set_image_position(record_back_button, GTK_POS_LEFT);
    gtk_widget_set_hexpand(GTK_WIDGET(record_back_button), true);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(record_back_button), 0, 0, 1, 1);

    start_recording_button = GTK_BUTTON(gtk_button_new_with_label("Start recording"));
    gtk_widget_set_hexpand(GTK_WIDGET(start_recording_button), true);
    g_signal_connect(start_recording_button, "clicked", G_CALLBACK(on_start_recording_button_click), app);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(start_recording_button), 1, 0, 1, 1);

    pause_recording_button = GTK_BUTTON(gtk_button_new_with_label("Pause recording"));
    gtk_widget_set_hexpand(GTK_WIDGET(pause_recording_button), true);
    g_signal_connect(pause_recording_button, "clicked", G_CALLBACK(on_pause_unpause_button_click), app);
    gtk_widget_set_sensitive(GTK_WIDGET(pause_recording_button), false);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(pause_recording_button), 2, 0, 1, 1);

    gtk_grid_attach(grid, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, num_columns, 1);

    recording_bottom_panel_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(recording_bottom_panel_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(recording_bottom_panel_grid, 5);
    gtk_widget_set_opacity(GTK_WIDGET(recording_bottom_panel_grid), 0.5);
    gtk_widget_set_halign(GTK_WIDGET(recording_bottom_panel_grid), GTK_ALIGN_END);

    recording_record_icon = gtk_image_new_from_icon_name("media-record", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_valign(recording_record_icon, GTK_ALIGN_CENTER);
    gtk_grid_attach(recording_bottom_panel_grid, recording_record_icon, 0, 0, 1, 1);

    recording_record_time_label = gtk_label_new("00:00:00");
    gtk_widget_set_valign(recording_record_time_label, GTK_ALIGN_CENTER);
    gtk_grid_attach(recording_bottom_panel_grid, recording_record_time_label, 1, 0, 1, 1);

    record_start_stop_hotkey.modkey_mask = modkey_to_mask(XK_Alt_L);
    record_start_stop_hotkey.keysym = XK_1;
    record_start_stop_hotkey.hotkey_entry = record_start_stop_hotkey_button;
    record_start_stop_hotkey.hotkey_active_label = hotkey_active_label;
    record_start_stop_hotkey.config = &config.record_config.start_stop_recording_hotkey;
    record_start_stop_hotkey.page = GTK_WIDGET(grid);
    record_start_stop_hotkey.trigger_handler = on_start_recording_button_click;
    record_start_stop_hotkey.associated_button = start_recording_button;
    record_start_stop_hotkey.shortcut_id = SHORTCUT_ID_START_STOP_RECORDING;

    pause_unpause_hotkey.modkey_mask = modkey_to_mask(XK_Alt_L);
    pause_unpause_hotkey.keysym = XK_2;
    pause_unpause_hotkey.hotkey_entry = pause_unpause_hotkey_button;
    pause_unpause_hotkey.hotkey_active_label = hotkey_active_label;
    pause_unpause_hotkey.config = &config.record_config.pause_unpause_recording_hotkey;
    pause_unpause_hotkey.page = GTK_WIDGET(grid);
    pause_unpause_hotkey.trigger_handler = on_pause_unpause_button_click;
    pause_unpause_hotkey.associated_button = pause_recording_button;
    pause_unpause_hotkey.shortcut_id = SHORTCUT_ID_PAUSE_UNPAUSE_RECORDING;

    return GTK_WIDGET(grid);
}

static void create_streaming_hotkey_items(GtkGrid *parent_grid, int row, int num_columns) {
    streaming_hotkeys_grid = GTK_GRID(gtk_grid_new());
    gtk_widget_set_halign(GTK_WIDGET(streaming_hotkeys_grid), GTK_ALIGN_START);
    gtk_grid_set_row_spacing(streaming_hotkeys_grid, 10);
    gtk_grid_set_column_spacing(streaming_hotkeys_grid, 10);
    gtk_grid_attach(parent_grid, GTK_WIDGET(streaming_hotkeys_grid), 0, row, num_columns, 1);
    int hotkeys_row = 0;

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND)
        add_wayland_global_hotkeys_ui(streaming_hotkeys_grid, hotkeys_row, num_columns);

    {
        gtk_grid_attach(streaming_hotkeys_grid, gtk_label_new("Press"), 0, hotkeys_row, 1, 1);

        streaming_start_stop_hotkey_button = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(streaming_start_stop_hotkey_button), gsr_info.system_info.display_server == DisplayServer::WAYLAND ? "" : "Alt + 1");
        g_signal_connect(streaming_start_stop_hotkey_button, "button-press-event", G_CALLBACK(on_hotkey_entry_click), streaming_start_stop_hotkey_button);
        gtk_grid_attach(streaming_hotkeys_grid, streaming_start_stop_hotkey_button, 1, hotkeys_row, 1, 1);

        GtkWidget *start_stop_streaming_label = gtk_label_new("to start/stop streaming");
        gtk_widget_set_halign(start_stop_streaming_label, GTK_ALIGN_START);
        gtk_widget_set_hexpand(start_stop_streaming_label, true);
        gtk_grid_attach(streaming_hotkeys_grid, start_stop_streaming_label, 2, hotkeys_row, 1, 1);

        ++hotkeys_row;
    }
}

static GtkWidget* create_streaming_page(GtkApplication *app, GtkStack *stack) {
    int row = 0;
    const int num_columns = 3;

    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    gtk_stack_add_named(stack, GTK_WIDGET(grid), "streaming");
    gtk_widget_set_vexpand(GTK_WIDGET(grid), true);
    gtk_widget_set_hexpand(GTK_WIDGET(grid), true);
    gtk_grid_set_row_spacing(grid, 10);
    gtk_grid_set_column_spacing(grid, 10);
    gtk_widget_set_margin(GTK_WIDGET(grid), 10, 10, 10, 10);

    GtkWidget *hotkey_active_label = nullptr;
    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        streaming_hotkeys_not_supported_label = gtk_label_new("Your Wayland compositor doesn't support global hotkeys. Use X11 or KDE Plasma on Wayland if you want to use hotkeys.");
        gtk_grid_attach(grid, streaming_hotkeys_not_supported_label, 0, row++, num_columns, 1);
    } else {
        hotkey_active_label = gtk_label_new("Press a key combination to set a new hotkey, backspace to remove the hotkey or esc to cancel.");
        gtk_grid_attach(grid, hotkey_active_label, 0, row++, num_columns, 1);
    }

    create_streaming_hotkey_items(grid, row, num_columns);
    ++row;

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        gtk_widget_set_visible(GTK_WIDGET(streaming_hotkeys_grid), false);
        gtk_widget_set_visible(GTK_WIDGET(streaming_hotkeys_not_supported_label), false);
    }

    gtk_grid_attach(grid, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, num_columns, 1);

    GtkGrid *stream_service_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(stream_service_grid), 0, row++, num_columns, 1);
    gtk_grid_attach(stream_service_grid, gtk_label_new("Stream service: "), 0, 0, 1, 1);
    stream_service_input_menu = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(stream_service_input_menu, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    gtk_combo_box_text_append(stream_service_input_menu, "twitch", "Twitch");
    gtk_combo_box_text_append(stream_service_input_menu, "youtube", "Youtube");
    gtk_combo_box_text_append(stream_service_input_menu, "custom", "Custom");
    gtk_combo_box_set_active(GTK_COMBO_BOX(stream_service_input_menu), 0);
    gtk_widget_set_hexpand(GTK_WIDGET(stream_service_input_menu), true);
    g_signal_connect(stream_service_input_menu, "changed", G_CALLBACK(stream_service_item_change_callback), NULL);
    gtk_grid_attach(stream_service_grid, GTK_WIDGET(stream_service_input_menu), 1, 0, 1, 1);

    GtkGrid *stream_id_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(stream_id_grid), 0, row++, num_columns, 1);
    stream_key_label = GTK_LABEL(gtk_label_new("Stream key: "));
    gtk_grid_attach(stream_id_grid, GTK_WIDGET(stream_key_label), 0, 0, 1, 1);

    GtkEntry **stream_id_entries[3] = { &youtube_stream_id_entry, &twitch_stream_id_entry, &custom_stream_url_entry };
    for(int i = 0; i < 3; ++i) {
        *stream_id_entries[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_visibility(*stream_id_entries[i], FALSE);
        gtk_entry_set_input_purpose(*stream_id_entries[i], GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_icon_from_icon_name(*stream_id_entries[i], GTK_ENTRY_ICON_SECONDARY, "view-reveal-symbolic");
        gtk_entry_set_icon_activatable(*stream_id_entries[i], GTK_ENTRY_ICON_SECONDARY, true);
        g_signal_connect(*stream_id_entries[i], "icon-press", G_CALLBACK(on_stream_key_icon_click), nullptr);
        gtk_widget_set_hexpand(GTK_WIDGET(*stream_id_entries[i]), true);
        gtk_grid_attach(stream_id_grid, GTK_WIDGET(*stream_id_entries[i]), 1, 0, 1, 1);
        gtk_widget_set_visible(GTK_WIDGET(*stream_id_entries[i]), false);
    }

    custom_stream_container_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(custom_stream_container_grid), 0, row++, num_columns, 1);
    gtk_grid_attach(custom_stream_container_grid, gtk_label_new("Container: "), 0, 0, 1, 1);
    custom_stream_container = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    g_signal_connect(custom_stream_container, "scroll-event", G_CALLBACK(scroll_event_ignore), NULL);
    for(auto &supported_container : supported_containers) {
        gtk_combo_box_text_append(custom_stream_container, supported_container.container_name, supported_container.file_extension);
    }
    if(gsr_info.supported_video_codecs.vp8 || gsr_info.supported_video_codecs.vp9) {
        gtk_combo_box_text_append(custom_stream_container, "webm", "webm");
    }
    gtk_widget_set_hexpand(GTK_WIDGET(custom_stream_container), true);
    gtk_grid_attach(custom_stream_container_grid, GTK_WIDGET(custom_stream_container), 1, 0, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(custom_stream_container), 1);

    GtkGrid *start_button_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(start_button_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(start_button_grid, 10);

    stream_back_button = GTK_BUTTON(gtk_button_new_with_label("Back"));
    GtkWidget *go_previous = gtk_image_new_from_icon_name("go-previous", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(stream_back_button, go_previous);
    gtk_button_set_always_show_image(stream_back_button, true);
    gtk_button_set_image_position(stream_back_button, GTK_POS_LEFT);
    gtk_widget_set_hexpand(GTK_WIDGET(stream_back_button), true);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(stream_back_button), 0, 0, 1, 1);
    start_streaming_button = GTK_BUTTON(gtk_button_new_with_label("Start streaming"));
    gtk_widget_set_hexpand(GTK_WIDGET(start_streaming_button), true);
    g_signal_connect(start_streaming_button, "clicked", G_CALLBACK(on_start_streaming_button_click), app);
    gtk_grid_attach(start_button_grid, GTK_WIDGET(start_streaming_button), 1, 0, 1, 1);

    gtk_grid_attach(grid, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, num_columns, 1);

    streaming_bottom_panel_grid = GTK_GRID(gtk_grid_new());
    gtk_grid_attach(grid, GTK_WIDGET(streaming_bottom_panel_grid), 0, row++, num_columns, 1);
    gtk_grid_set_column_spacing(streaming_bottom_panel_grid, 5);
    gtk_widget_set_opacity(GTK_WIDGET(streaming_bottom_panel_grid), 0.5);
    gtk_widget_set_halign(GTK_WIDGET(streaming_bottom_panel_grid), GTK_ALIGN_END);

    GtkWidget *record_icon = gtk_image_new_from_icon_name("media-record", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_valign(record_icon, GTK_ALIGN_CENTER);
    gtk_grid_attach(streaming_bottom_panel_grid, record_icon, 0, 0, 1, 1);

    streaming_record_time_label = gtk_label_new("00:00:00");
    gtk_widget_set_valign(streaming_record_time_label, GTK_ALIGN_CENTER);
    gtk_grid_attach(streaming_bottom_panel_grid, streaming_record_time_label, 1, 0, 1, 1);

    streaming_start_stop_hotkey.modkey_mask = modkey_to_mask(XK_Alt_L);
    streaming_start_stop_hotkey.keysym = XK_1;
    streaming_start_stop_hotkey.hotkey_entry = streaming_start_stop_hotkey_button;
    streaming_start_stop_hotkey.hotkey_active_label = hotkey_active_label;
    streaming_start_stop_hotkey.config = &config.streaming_config.start_stop_recording_hotkey;
    streaming_start_stop_hotkey.page = GTK_WIDGET(grid);
    streaming_start_stop_hotkey.trigger_handler = on_start_streaming_button_click;
    streaming_start_stop_hotkey.associated_button = start_streaming_button;
    streaming_start_stop_hotkey.shortcut_id = SHORTCUT_ID_START_STOP_RECORDING;

    return GTK_WIDGET(grid);
}

static gboolean on_destroy_window(GtkWidget*, GdkEvent*, gpointer) {
    if(gpu_screen_recorder_process != -1) {
        kill(gpu_screen_recorder_process, SIGINT);
        int status;
        if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
            perror("waitpid failed");
            /* Ignore... */
        }
        gpu_screen_recorder_process = -1;
    }
    save_configs();
    return true;
}

static void handle_child_process_death(gpointer userdata) {
    if(gpu_screen_recorder_process == -1)
        return;

    int status = 0;
    if(waitpid(gpu_screen_recorder_process, &status, WNOHANG) == 0)
        return;

    gpu_screen_recorder_process = -1;
    prev_exit_status = -1;
    if(WIFEXITED(status))
        prev_exit_status = WEXITSTATUS(status);

    if(replaying) {
        on_start_replay_button_click(start_replay_button, userdata);
    } else if(recording) {
        on_start_recording_button_click(start_recording_button, userdata);
    } else if(streaming) {
        on_start_streaming_button_click(start_streaming_button, userdata);
    }
}

static void seconds_to_record_time_format(int seconds, char *buffer, size_t buffer_size) {
    const int hours = seconds / 60 / 60;
    seconds -= (hours * 60 * 60);

    const int mins = seconds / 60;
    seconds -= (mins * 60);

    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, mins, seconds);
}

static void handle_record_timer() {
    if(streaming) {
        const double recording_time_passed_seconds = clock_get_monotonic_seconds() - record_start_time_sec;
        char record_time_str[32];
        seconds_to_record_time_format(recording_time_passed_seconds, record_time_str, sizeof(record_time_str));
        gtk_label_set_text(GTK_LABEL(streaming_record_time_label), record_time_str);
    }

    if(recording && !paused) {
        const double recording_time_passed_seconds = (clock_get_monotonic_seconds() - record_start_time_sec) - paused_time_offset_sec;
        char record_time_str[32];
        seconds_to_record_time_format(recording_time_passed_seconds, record_time_str, sizeof(record_time_str));
        gtk_label_set_text(GTK_LABEL(recording_record_time_label), record_time_str);
        recording_indicator_set_text(record_time_str);
    }

    if(replaying) {
        const double recording_time_passed_seconds = clock_get_monotonic_seconds() - record_start_time_sec;
        char record_time_str[32];
        seconds_to_record_time_format(recording_time_passed_seconds, record_time_str, sizeof(record_time_str));
        gtk_label_set_text(GTK_LABEL(replay_record_time_label), record_time_str);
    }
}

static void handle_notification_timer(GtkApplication *app) {
    if(!showing_notification)
        return;

    const double now = clock_get_monotonic_seconds();
    if(now - notification_start_seconds >= notification_timeout_seconds) {
        g_application_withdraw_notification(&app->parent, "gpu-screen-recorder");
        showing_notification = false;
    }
}

static gboolean timer_timeout_handler(gpointer userdata) {
    GtkApplication *app = (GtkApplication*)userdata;
    handle_notification_timer(app);
    handle_child_process_death(userdata);
    handle_record_timer();
    return G_SOURCE_CONTINUE;
}

static const std::string* get_application_audio_by_name_case_insensitive(const std::vector<std::string> &application_audio, const std::string &name) {
    for(const auto &app_audio : application_audio) {
        if(strcasecmp(app_audio.c_str(), name.c_str()) == 0)
            return &app_audio;
    }
    return nullptr;
}

static void load_config() {
    std::string first_monitor;
    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND && strcmp(config.main_config.record_area_option.c_str(), "window") == 0) {
        //
    } else if(gsr_info.system_info.display_server != DisplayServer::WAYLAND && strcmp(config.main_config.record_area_option.c_str(), "focused") == 0) {
        //
    } else if(config.main_config.record_area_option == "portal" && gsr_info.supported_capture_options.portal && gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        //
    } else {
        bool found_monitor = false;
        for(const auto &monitor : gsr_info.supported_capture_options.monitors) {
            if(first_monitor.empty())
                first_monitor = monitor.name;

            if(config.main_config.record_area_option == monitor.name)
                found_monitor = true;
        }

        if(!found_monitor)
            config.main_config.record_area_option.clear();
    }

    if(config.main_config.record_area_option.empty()) {
        const bool allow_screen_capture = is_monitor_capture_drm() || nvfbc_installed;
        if(allow_screen_capture) {
            config.main_config.record_area_option = first_monitor;
        } else {
            config.main_config.record_area_option = "window";
        }
    }

    gtk_widget_set_visible(GTK_WIDGET(select_window_button), strcmp(config.main_config.record_area_option.c_str(), "window") == 0);
    //gtk_widget_set_visible(GTK_WIDGET(area_size_grid), strcmp(config.main_config.record_area_option.c_str(), "focused") == 0);
    gtk_widget_set_visible(GTK_WIDGET(restore_portal_session_button), strcmp(config.main_config.record_area_option.c_str(), "portal") == 0);

    if(config.main_config.record_area_width == 0)
        config.main_config.record_area_width = 1920;

    if(config.main_config.record_area_height == 0)
        config.main_config.record_area_height = 1080;

    if(config.main_config.video_width == 0)
        config.main_config.video_width = 1920;

    if(config.main_config.video_height == 0)
        config.main_config.video_height = 1080;

    if(config.main_config.fps == 0)
        config.main_config.fps = 60;
    else if(config.main_config.fps < 1)
        config.main_config.fps = 1;
    else if(config.main_config.fps > 5000)
        config.main_config.fps = 5000;

    if(config.main_config.color_range != "limited" && config.main_config.color_range != "full")
        config.main_config.color_range = "limited";

    if(config.main_config.quality != "custom" && config.main_config.quality != "medium" && config.main_config.quality != "high" && config.main_config.quality != "very_high" && config.main_config.quality != "ultra")
        config.main_config.quality = "very_high";

    if(config.main_config.audio_codec != "opus" && config.main_config.audio_codec != "aac")
        config.main_config.audio_codec = "opus";

    if(config.main_config.framerate_mode != "auto" && config.main_config.framerate_mode != "cfr" && config.main_config.framerate_mode != "vfr")
        config.main_config.framerate_mode = "auto";

    if(config.streaming_config.streaming_service != "twitch" && config.streaming_config.streaming_service != "youtube" && config.streaming_config.streaming_service != "custom")
        config.streaming_config.streaming_service = "twitch";

    if(config.record_config.save_directory.empty() || !is_directory(config.record_config.save_directory.c_str()))
        config.record_config.save_directory = get_videos_dir();

    if(config.replay_config.save_directory.empty() || !is_directory(config.replay_config.save_directory.c_str()))
        config.replay_config.save_directory = get_videos_dir();

    if(config.replay_config.replay_time < 5)
        config.replay_config.replay_time = 5;
    else if(config.replay_config.replay_time > 1200)
        config.replay_config.replay_time = 1200;

    record_area_selection_menu_set_active_id(config.main_config.record_area_option.c_str());
    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND) {
        gtk_spin_button_set_value(area_width_entry, config.main_config.record_area_width);
        gtk_spin_button_set_value(area_height_entry, config.main_config.record_area_height);
    }
    gtk_spin_button_set_value(video_width_entry, config.main_config.video_width);
    gtk_spin_button_set_value(video_height_entry, config.main_config.video_height);
    gtk_spin_button_set_value(fps_entry, config.main_config.fps);
    gtk_spin_button_set_value(video_bitrate_entry, config.main_config.video_bitrate);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(split_audio_button), !config.main_config.merge_audio_tracks);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(record_app_audio_inverted_button), config.main_config.record_app_audio_inverted);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(change_video_resolution_button), config.main_config.change_video_resolution);

    for(const std::string &audio_input : config.main_config.audio_input) {
        GtkWidget *row = nullptr;
        if(starts_with(audio_input, "app:")) {
            if(!gsr_info.system_info.supports_app_audio)
                continue;

            std::string audio_input_name = audio_input.substr(4);
            const std::string *app_audio_existing = get_application_audio_by_name_case_insensitive(application_audio, audio_input_name);
            if(app_audio_existing)
                row = create_application_audio_combo_box_row(*app_audio_existing);
            else
                row = create_application_audio_custom_row(std::move(audio_input_name));
        } else if(starts_with(audio_input, "device:")) {
            row = create_audio_device_combo_box_row(audio_input.substr(7));
        } else {
            row = create_audio_device_combo_box_row(audio_input);
        }

        gtk_widget_show_all(row);
        gtk_box_pack_start(audio_devices_items_box, row, false, false, 0);
    }

    if(config_empty && config.main_config.audio_input.empty()) {
        GtkWidget *row = create_audio_device_combo_box_row("Default output");
        gtk_widget_show_all(row);
        gtk_box_pack_start(audio_devices_items_box, row, false, false, 0);
    }

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(color_range_input_menu), config.main_config.color_range.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(quality_input_menu), config.main_config.quality.c_str());
    video_codec_selection_menu_set_active_id("auto");
    video_codec_selection_menu_set_active_id(config.main_config.codec.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(audio_codec_input_menu), config.main_config.audio_codec.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(framerate_mode_input_menu), config.main_config.framerate_mode.c_str());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_recording_started_notification_button), config.main_config.show_recording_started_notifications);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_recording_stopped_notification_button), config.main_config.show_recording_stopped_notifications);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_recording_saved_notification_button), config.main_config.show_recording_saved_notifications);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(exclude_metadata_button), config.main_config.exclude_metadata);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(record_cursor_button), config.main_config.record_cursor);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hide_window_when_recording_menu_item), config.main_config.hide_window_when_recording);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restore_portal_session_button), config.main_config.restore_portal_session);

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(stream_service_input_menu), config.streaming_config.streaming_service.c_str());
    gtk_entry_set_text(youtube_stream_id_entry, config.streaming_config.youtube.stream_key.c_str());
    gtk_entry_set_text(twitch_stream_id_entry, config.streaming_config.twitch.stream_key.c_str());
    gtk_entry_set_text(custom_stream_url_entry, config.streaming_config.custom.url.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(custom_stream_container), config.streaming_config.custom.container.c_str());

    gtk_button_set_label(record_file_chooser_button, config.record_config.save_directory.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(record_container), config.record_config.container.c_str());

    gtk_button_set_label(replay_file_chooser_button, config.replay_config.save_directory.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(replay_container), config.replay_config.container.c_str());
    gtk_spin_button_set_value(replay_time_entry, config.replay_config.replay_time);

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(view_combo_box), config.main_config.advanced_view ? "advanced" : "simple");

    view_combo_box_change_callback(GTK_COMBO_BOX(view_combo_box), view_combo_box);
    quality_combo_box_change_callback(GTK_COMBO_BOX(quality_input_menu), quality_input_menu);
    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND) {
        if(!config_empty) {
            for(int i = 0; i < num_hotkeys; ++i) {
                hotkeys[i]->keysym = hotkeys[i]->config->keysym;
                hotkeys[i]->modkey_mask = hotkeys[i]->config->modifiers;
                set_hotkey_text_from_hotkey_data(GTK_ENTRY(hotkeys[i]->hotkey_entry), *hotkeys[i]);
            }
        }

        gtk_widget_set_visible(record_start_stop_hotkey.hotkey_active_label, false);
        gtk_widget_set_visible(streaming_start_stop_hotkey.hotkey_active_label, false);
        gtk_widget_set_visible(replay_start_stop_hotkey.hotkey_active_label, false);
    }
    record_area_item_change_callback(nullptr, nullptr);
    stream_service_item_change_callback(GTK_COMBO_BOX(stream_service_input_menu), nullptr);

    on_change_video_resolution_button_click(GTK_BUTTON(change_video_resolution_button), nullptr);

    if(!gsr_info.system_info.supports_app_audio) {
        gtk_widget_set_visible(GTK_WIDGET(add_application_audio_button), false);
        gtk_widget_set_visible(GTK_WIDGET(add_custom_application_audio_button), false);
        gtk_widget_set_visible(GTK_WIDGET(record_app_audio_inverted_button), false);
    }

    if(!config.main_config.software_encoding_warning_shown && !get_first_usable_hardware_video_codec_name()) {
        GtkWidget *dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Unable to find a hardware video encoder on your system, using software video encoder instead (slow!). If you know that your system supports H264/HEVC hardware video encoding and "
            "you are using the flatpak version of GPU Screen Recorder then try installing mesa-extra freedesktop runtime by running this command:\n"
            "flatpak install --system org.freedesktop.Platform.GL.default//24.08extra\n"
            "If you have an Intel GPU then you might need to run this command as well:\n"
            "flatpak install --system org.freedesktop.Platform.VAAPI.Intel//24.08\n"
            "and then restart GPU Screen Recorder. If that doesn't work then you may have to install another mesa package for your distro if you are using AMD.\n"
            "If you are using NVIDIA then you might need to run the \"nvidia-smi\" command first before starting GPU Screen Recorder if NVIDIA is incorrectly setup on your distro.\n"
            "If you are using a distro such as manjaro which disables hardware accelerated video encoding then you can also try the <a href=\"https://flathub.org/apps/com.dec05eba.gpu_screen_recorder\">flatpak version of GPU Screen Recorder</a> instead which doesn't have this issue.");
        set_dialog_selectable(dialog);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        config.main_config.software_encoding_warning_shown = true;
        video_codec_selection_menu_set_active_id("h264_software");
        config.main_config.advanced_view = true;
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view_combo_box), "advanced");
    }
}

static void init_shortcuts_callback(bool success, void *userdata) {
    (void)userdata;
    global_shortcuts_initialized = success;
    if(success) {
        gtk_widget_set_visible(GTK_WIDGET(recording_hotkeys_grid), true);
        gtk_widget_set_visible(GTK_WIDGET(replay_hotkeys_grid), true);
        gtk_widget_set_visible(GTK_WIDGET(streaming_hotkeys_grid), true);

        gtk_widget_set_visible(GTK_WIDGET(recording_hotkeys_not_supported_label), false);
        gtk_widget_set_visible(GTK_WIDGET(replay_hotkeys_not_supported_label), false);
        gtk_widget_set_visible(GTK_WIDGET(streaming_hotkeys_not_supported_label), false);

        if(!gsr_global_shortcuts_list_shortcuts(&global_shortcuts, shortcut_changed_callback, NULL)) {
            fprintf(stderr, "gsr error: failed to list shortcuts\n");
        }
        gsr_global_shortcuts_subscribe_activated_signal(&global_shortcuts, deactivated_callback, shortcut_changed_callback, NULL);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(recording_hotkeys_grid), false);
        gtk_widget_set_visible(GTK_WIDGET(replay_hotkeys_grid), false);
        gtk_widget_set_visible(GTK_WIDGET(streaming_hotkeys_grid), false);

        gtk_widget_set_visible(GTK_WIDGET(recording_hotkeys_not_supported_label), true);
        gtk_widget_set_visible(GTK_WIDGET(replay_hotkeys_not_supported_label), true);
        gtk_widget_set_visible(GTK_WIDGET(streaming_hotkeys_not_supported_label), true);
    }
}

static const char* gpu_vendor_to_name(GpuVendor vendor) {
    switch(vendor) {
        case GpuVendor::UNKNOWN:  return "Unknown";
        case GpuVendor::AMD:      return "AMD";
        case GpuVendor::INTEL:    return "Intel";
        case GpuVendor::NVIDIA:   return "NVIDIA";
        case GpuVendor::BROADCOM: return "Broadcom";
        case GpuVendor::APPLE:    return "Apple";
    }
    return "";
}

static bool gsr_startup_validation() {
    if(gsr_info_exit_status == GsrInfoExitStatus::FAILED_TO_RUN_COMMAND) {
        const char *cmd = flatpak ? "flatpak run --command=gpu-screen-recorder com.dec05eba.gpu_screen_recorder -w screen -f 60 -o video.mp4" : "gpu-screen-recorder -w screen -f 60 -o video.mp4";
        GtkWidget *dialog = gtk_message_dialog_new_with_markup(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to run 'gpu-screen-recorder' command. If you are using gpu-screen-recorder flatpak then this is a bug. Otherwise you need to make sure gpu-screen-recorder is installed on your system and working properly (install necessary depedencies depending on your GPU, such as libva-mesa-driver, libva-intel-driver, intel-media-driver and linux-firmware). Run:\n"
            "%s\n"
            "in a terminal to see more information about the issue.", cmd);
        set_dialog_selectable(dialog);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return false;
    }

    if(gsr_info_exit_status == GsrInfoExitStatus::OPENGL_FAILED) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to get OpenGL information. Make sure your GPU drivers are properly installed.\n"
            "If you are using nvidia then make sure to run \"flatpak update\" to make sure that your flatpak nvidia driver version matches your distros nvidia driver version. If this doesn't work then you might need to manually install a flatpak nvidia driver version that matches your distros nvidia driver version.\n"
            "If you are using nvidia and have recently updated your nvidia driver then make sure to reboot your computer first.");
        set_dialog_selectable(dialog);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return false;
    }

    if(gsr_info_exit_status == GsrInfoExitStatus::NO_DRM_CARD) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to find a valid DRM card. If you are running GPU Screen Recorder with prime-run then try running without it.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return false;
    }

    if(gsr_info.system_info.display_server == DisplayServer::UNKNOWN) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Neither X11 nor Wayland is running.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return false;
    }

    if(gsr_info.system_info.display_server == DisplayServer::X11 && !dpy) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to connect to the X11 server");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return false;
    }

    if(gsr_info.gpu_info.vendor == GpuVendor::NVIDIA) {
        if(!is_cuda_installed()) {
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "CUDA is not installed on your system. GPU Screen Recorder requires CUDA to be installed to work with a NVIDIA GPU.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return false;
        }

        if(!is_nvenc_installed()) {
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "NVENC is not installed on your system. GPU Screen Recorder requires NVENC to be installed to work with a NVIDIA GPU.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return false;
        }
    }

    return true;
}

static void show_capture_panel() {
    capture_panel_show(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(record_cursor_button)));
}

static void open_settings() {
    capture_panel_hide();
    show_window();
}

static void close_settings() {
    hide_window();
    if(!recording && !replaying && !streaming)
        show_capture_panel();
}

static gboolean on_settings_window_delete(GtkWidget*, GdkEvent*, gpointer) {
    // Closing the settings goes back to the capture panel instead of quitting
    save_configs();
    close_settings();
    return true;
}

static void quit_application() {
    if(quit_timeout_id) {
        g_source_remove(quit_timeout_id);
        quit_timeout_id = 0;
    }

    capture_panel_hide();
    recording_indicator_hide();
    // Stops gpu screen recorder and saves the config
    on_destroy_window(window, nullptr, nullptr);
    g_application_quit(G_APPLICATION(page_navigation_userdata.app));
}

static gboolean quit_application_timeout_handler(gpointer) {
    quit_timeout_id = 0;
    quit_application();
    return G_SOURCE_REMOVE;
}

// Prefer showing the indicator on another monitor than the one being recorded so that it's not visible in the video
static GdkMonitor* get_recording_indicator_monitor(GdkMonitor *recorded_monitor) {
    GdkDisplay *display = gdk_display_get_default();
    const int num_monitors = gdk_display_get_n_monitors(display);
    for(int i = 0; i < num_monitors; ++i) {
        GdkMonitor *monitor = gdk_display_get_monitor(display, i);
        if(recorded_monitor && monitor != recorded_monitor)
            return monitor;
    }
    return recorded_monitor;
}

static void on_panel_recording_finished(bool saved) {
    panel_recording = PanelRecording{};
    recording_indicator_hide();
    on_streaming_recording_replay_page_back_click(nullptr, &page_navigation_userdata);

    if(!window_hidden)
        return;

    if(saved) {
        // Like the GNOME screenshot UI the application goes away once the recording is done. The delay gives the "recording saved" notification time to be sent
        if(!quit_timeout_id)
            quit_timeout_id = g_timeout_add(1000, quit_application_timeout_handler, nullptr);
    } else {
        // Recording failed or was cancelled (for example in the desktop portal), go back to the panel to try again
        show_capture_panel();
    }
}

static void start_panel_recording(GdkMonitor *indicator_monitor) {
    GtkApplication *app = page_navigation_userdata.app;
    panel_recording.active = true;

    if(show_pkexec_flatpak_error_if_needed()) {
        panel_recording = PanelRecording{};
        show_capture_panel();
        return;
    }
    show_bugged_driver_warning();

    // Same as going to the recording page in the settings, so that the recording hotkeys and systray menu work while recording
    gtk_stack_set_visible_child(page_navigation_userdata.stack, page_navigation_userdata.recording_page);
    app_indicator_set_menu(app_indicator, GTK_MENU(create_systray_menu(app, SystrayPage::RECORDING)));
    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND)
        replace_grabbed_keys_depending_on_active_page();

    on_start_recording_button_click(start_recording_button, app);
    if(!recording) {
        panel_recording = PanelRecording{};
        on_streaming_recording_replay_page_back_click(nullptr, &page_navigation_userdata);
        show_capture_panel();
        return;
    }

    recording_indicator_show(indicator_monitor, []() {
        if(recording)
            on_start_recording_button_click(start_recording_button, page_navigation_userdata.app);
    });
}

// Monitor capture on Wayland and on AMD/Intel needs root access, which the flatpak can only get when it's installed system-wide
static bool can_record_monitor_without_portal() {
    if(!flatpak || !is_monitor_capture_drm())
        return true;
    return is_pkexec_installed() && flatpak_is_installed_as_system();
}

// Returns an empty string if the monitor should be recorded as a region instead
static std::string get_gsr_monitor_name(GdkMonitor *monitor) {
    const std::vector<GsrMonitor> &monitors = gsr_info.supported_capture_options.monitors;
    if(monitors.size() == 1 && gdk_display_get_n_monitors(gdk_display_get_default()) == 1)
        return monitors.front().name;

    if(gsr_info.supported_capture_options.region || monitors.empty())
        return "";

    // Older versions of gpu screen recorder don't support region capture, match by resolution instead
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    const int scale = gdk_monitor_get_scale_factor(monitor);
    for(const GsrMonitor &gsr_monitor : monitors) {
        if(gsr_monitor.size.x == geometry.width * scale && gsr_monitor.size.y == geometry.height * scale)
            return gsr_monitor.name;
    }
    return monitors.front().name;
}

static void on_capture_panel_record(CaptureMode mode, CaptureRect area, GdkMonitor *monitor) {
    panel_recording = PanelRecording{};
    panel_recording.restore_portal_session = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_portal_session_button));

    const bool wayland = gsr_info.system_info.display_server == DisplayServer::WAYLAND;
    char region_str[128];

    switch(mode) {
        case CaptureMode::SELECTION: {
            const CaptureRect region = capture_rect_to_display_server(area, monitor);
            snprintf(region_str, sizeof(region_str), "%dx%d+%d+%d", region.width, region.height, region.x, region.y);
            panel_recording.capture_target = "region";
            panel_recording.window_arg = "region";
            panel_recording.region_arg = region_str;
            break;
        }
        case CaptureMode::SCREEN: {
            const std::string monitor_name = get_gsr_monitor_name(monitor);
            if(wayland && gsr_info.supported_capture_options.portal && !can_record_monitor_without_portal()) {
                panel_recording.capture_target = "portal";
                panel_recording.window_arg = "portal";
            } else if(!monitor_name.empty()) {
                panel_recording.capture_target = monitor_name;
                panel_recording.window_arg = monitor_name;
            } else {
                const CaptureRect region = capture_rect_to_display_server(area, monitor);
                snprintf(region_str, sizeof(region_str), "%dx%d+%d+%d", region.width, region.height, region.x, region.y);
                panel_recording.capture_target = "region";
                panel_recording.window_arg = "region";
                panel_recording.region_arg = region_str;
            }
            break;
        }
        case CaptureMode::WINDOW: {
            if(wayland) {
                // Always let the user pick the window in the desktop portal instead of restoring the previous session
                panel_recording.capture_target = "portal";
                panel_recording.window_arg = "portal";
                panel_recording.restore_portal_session = false;
            } else {
                window_selected_callback = [](Window selected_window) {
                    if(selected_window == None || !validate_window(page_navigation_userdata.app, selected_window)) {
                        show_capture_panel();
                        return;
                    }
                    panel_recording.capture_target = "window";
                    panel_recording.window_arg = std::to_string(selected_window);
                    start_panel_recording(nullptr);
                };
                on_select_window_button_click(select_window_button, nullptr);
                return;
            }
            break;
        }
    }

    start_panel_recording(get_recording_indicator_monitor(monitor));
}

static void init_capture_panel() {
    const bool wayland = gsr_info.system_info.display_server == DisplayServer::WAYLAND;
    const bool allow_screen_capture = is_monitor_capture_drm() || nvfbc_installed;

    CapturePanelOptions options;
    options.selection_available = allow_screen_capture && gsr_info.supported_capture_options.region;
    options.screen_available = (allow_screen_capture && !gsr_info.supported_capture_options.monitors.empty()) || (wayland && gsr_info.supported_capture_options.portal);
    options.window_available = wayland ? gsr_info.supported_capture_options.portal : gsr_info.supported_capture_options.window;
    options.window_hint = wayland ? "Press the record button to choose a window" : "Press the record button, then click on the window to record";

    CapturePanelCallbacks callbacks;
    callbacks.record = on_capture_panel_record;
    callbacks.open_settings = open_settings;
    callbacks.close = quit_application;
    callbacks.show_pointer_changed = [](bool show_pointer) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(record_cursor_button), show_pointer);
        config.main_config.record_cursor = show_pointer;
    };

    CaptureMode mode = CaptureMode::SELECTION;
    capture_mode_from_string(config.main_config.capture_panel_mode.c_str(), &mode);

    CaptureRect selection;
    if(sscanf(config.main_config.capture_panel_selection.c_str(), "%dx%d+%d+%d", &selection.width, &selection.height, &selection.x, &selection.y) != 4)
        selection = CaptureRect{};

    capture_panel_init(options, callbacks, mode, selection);
}

static void on_app_activated_again() {
    if(quit_timeout_id) {
        g_source_remove(quit_timeout_id);
        quit_timeout_id = 0;
    }

    if(recording) {
        // Launching the application again while recording stops the recording, so a single hotkey that launches the application can be used to start and stop recording
        on_start_recording_button_click(start_recording_button, page_navigation_userdata.app);
    } else if(window_selected_callback) {
        // Waiting for the user to click on the window to record
    } else if(replaying || streaming || !window_hidden) {
        open_settings();
    } else if(!capture_panel_is_visible()) {
        show_capture_panel();
    }
}

static void on_settings_action(GSimpleAction*, GVariant*, gpointer) {
    if(app_activated)
        open_settings();
    else
        start_with_settings = true;
}

static void activate(GtkApplication *app, gpointer) {
    if(app_activated) {
        on_app_activated_again();
        return;
    }
    app_activated = true;

    if(!gsr_startup_validation()) {
        g_application_quit(G_APPLICATION(app));
        return;
    }

    page_navigation_userdata.app = app;

    std::string window_title = "GPU Screen Recorder | Running on ";
    window_title += gpu_vendor_to_name(gsr_info.gpu_info.vendor);

    window = gtk_application_window_new(app);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy_window), nullptr);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_settings_window_delete), nullptr);
    gtk_window_set_title(GTK_WINDOW(window), window_title.c_str());
    gtk_window_set_resizable(GTK_WINDOW(window), false);

    GtkIconTheme *icon_theme = gtk_icon_theme_get_default();
#ifdef GSR_ICONS_PATH
    const char *icon_path = GSR_ICONS_PATH;
#else
    const char *icon_path = "/usr/share/icons";
#endif
    gtk_icon_theme_prepend_search_path(icon_theme, icon_path);

    const char *icon_name = "com.dec05eba.gpu_screen_recorder";
    if(!gtk_icon_theme_has_icon(icon_theme, icon_name))
        fprintf(stderr, "Error: failed to find icon %s in %s\n", icon_name, icon_path);

    gtk_window_set_default_icon_name(icon_name);
    gtk_window_set_icon_name(GTK_WINDOW(window), icon_name);

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        if(gdk_wayland_display_query_registry(gdk_display_get_default(), "hyprland_global_shortcuts_manager_v1")) {
            wayland_compositor = WaylandCompositor::HYPRLAND;
        } else if(gdk_wayland_display_query_registry(gdk_display_get_default(), "org_kde_plasma_shell")) {
            if(kwin_get_version() >= make_version(6, 7, 4))
                wayland_compositor = WaylandCompositor::KDE_NEW;
            else
                wayland_compositor = WaylandCompositor::KDE_OLD;
        }
    }

    select_window_userdata.app = app;
    audio_inputs = get_audio_devices();
    application_audio = get_application_audio();

    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND)
        crosshair_cursor = XCreateFontCursor(gdk_x11_get_default_xdisplay(), XC_crosshair);

    GtkStack *stack = GTK_STACK(gtk_stack_new());
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(stack));
    gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_transition_duration(stack, 0);
    gtk_stack_set_homogeneous(stack, false);
    GtkWidget *common_settings_page = create_common_settings_page(stack, app);
    GtkWidget *replay_page = create_replay_page(app, stack);
    GtkWidget *recording_page = create_recording_page(app, stack);
    GtkWidget *streaming_page = create_streaming_page(app, stack);
    gtk_stack_set_visible_child(stack, common_settings_page);

    page_navigation_userdata.stack = stack;
    page_navigation_userdata.common_settings_page = common_settings_page;
    page_navigation_userdata.replay_page = replay_page;
    page_navigation_userdata.recording_page = recording_page;
    page_navigation_userdata.streaming_page = streaming_page;

    g_signal_connect(replay_button, "clicked", G_CALLBACK(on_start_replay_click), &page_navigation_userdata);
    g_signal_connect(replay_back_button, "clicked", G_CALLBACK(on_streaming_recording_replay_page_back_click), &page_navigation_userdata);

    g_signal_connect(record_button, "clicked", G_CALLBACK(on_start_recording_click), &page_navigation_userdata);
    g_signal_connect(record_back_button, "clicked", G_CALLBACK(on_streaming_recording_replay_page_back_click), &page_navigation_userdata);

    g_signal_connect(stream_button, "clicked", G_CALLBACK(on_start_streaming_click), &page_navigation_userdata);
    g_signal_connect(stream_back_button, "clicked", G_CALLBACK(on_streaming_recording_replay_page_back_click), &page_navigation_userdata);

    if(gsr_info.system_info.display_server != DisplayServer::WAYLAND) {
        xim = XOpenIM(gdk_x11_get_default_xdisplay(), NULL, NULL, NULL);
        xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, NULL);

        GdkWindow *root_window = gdk_get_default_root_window();
        gdk_window_add_filter(root_window, hotkey_filter_callback, &page_navigation_userdata);
    }

    setup_systray(app);

    g_timeout_add(500, timer_timeout_handler, app);

    // The settings window starts hidden, the capture panel is shown instead
    gtk_widget_show_all(GTK_WIDGET(stack));
    load_config();

    init_capture_panel();
    if(start_with_settings)
        open_settings();
    else
        show_capture_panel();

    if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
        init_shortcuts_callback(false, nullptr);
        // TODO:
        // Disable global hotkeys on Hyprland for now. It crashes the hyprland desktop portal.
        // When it's re-enabled on Hyprland it will need special handing where it does BindShortcuts immediately on startup
        // instead of having a "register hotkeys" button. This needed because Hyprland doesn't remember registered hotkeys after
        // the desktop portal is restarted (when the computer is restarted for example).

        if(wayland_compositor == WaylandCompositor::HYPRLAND) {
            const char *hotkeys_not_supported_text = "Hotkeys have been disabled on your system because of a Hyprland bug.\nUse X11 or KDE Plasma on Wayland if you want to use hotkeys.";
            gtk_label_set_text(GTK_LABEL(recording_hotkeys_not_supported_label), hotkeys_not_supported_text);
            gtk_label_set_text(GTK_LABEL(replay_hotkeys_not_supported_label), hotkeys_not_supported_text);
            gtk_label_set_text(GTK_LABEL(streaming_hotkeys_not_supported_label), hotkeys_not_supported_text);
        } else {
            if(!gsr_global_shortcuts_init(&global_shortcuts, init_shortcuts_callback, NULL)) {
                fprintf(stderr, "gsr error: failed to initialize global shortcuts\n");
            }
        }
    }
}

static bool is_kms_server_proxy_installed() {
    const int exit_code = system("flatpak-spawn --host -- /var/lib/flatpak/app/com.dec05eba.gpu_screen_recorder/current/active/files/bin/kms-server-proxy is-setup");
    return exit_code == 0;
}

static void gtk_activate_handler_run_and_quit(GtkApplication *app, gpointer userdata) {
    std::function<void()> *handler = (std::function<void()>*)userdata;
    (*handler)();
    g_application_quit(G_APPLICATION(app));
}

static void start_gtk_run_handler(std::function<void()> handler) {
    char app_id[] = "com.dec05eba.gpu_screen_recorder";
    // Gtk sets wayland app id / x11 wm class from the binary name, so we override it here.
    // This is needed for the correct window icon on wayland (app id needs to match the desktop file name).
    char *argv[1] = { app_id };
    GtkApplication *app = gtk_application_new(app_id, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(gtk_activate_handler_run_and_quit), &handler);
    g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
}

static void startup_new_ui(bool launched_by_daemon) {
    if(!dpy) {
        if(launched_by_daemon) {
            fprintf(stderr, "Error: failed to connect to the X11 server, assuming no graphical session has started yet\n");
            exit(1);
        } else {
            start_gtk_run_handler([]() {
                GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                    "Failed to connect to the X11 server while trying to start the new GPU Screen Recorder UI. Please install X11 on your system to use the new UI");
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
            });

            config.main_config.use_new_ui = false;
            save_config(config);
            return;
        }
    }

    start_gtk_run_handler([]() {
        if(!gsr_startup_validation())
            exit(1);
    });

    if(!flatpak_is_installed_as_system()) {
        start_gtk_run_handler([]() {
            GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "GPU Screen Recorder needs to be installed system-wide to use the new UI. You can run this command to install GPU Screen recorder system-wide:\n"
                "flatpak install --system com.dec05eba.gpu_screen_recorder\n");
            set_dialog_selectable(dialog);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        });
        exit(1);
    }

    if(config.main_config.installed_gsr_global_hotkeys_version != GSR_CURRENT_GLOBAL_HOTKEYS_CODE_VERSION) {
        bool finished = false;
        start_gtk_run_handler([&finished]() {
            finished = kms_server_proxy_setup_gsr_ui(
                "An update is available. The new GPU Screen Recorder UI needs root privileges to finish update to make global hotkeys and recording work on any system.\n"
                "You will need to restart the application to apply the update.\n"
                "\n"
                "Are you sure you want to continue?");
        });

        if(!finished)
            return;
    } else if(!is_kms_server_proxy_installed()) {
        bool finished = false;
        start_gtk_run_handler([&finished]() {
            finished = kms_server_proxy_setup_gsr_ui(
                "Required files are missing to launch the new GPU Screen Recorder UI. These files will be installed again.\n"
                "\n"
                "Are you sure you want to continue?");
        });

        if(!finished)
            return;
    }

    if(dpy)
        XCloseDisplay(dpy);

    launch_gsr_ui(launched_by_daemon);
    exit(0);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "C");

    const bool use_old_ui_opt = argc >= 2 && strcmp(argv[1], "use-old-ui") == 0;
    const bool launched_by_daemon_opt = argc >= 2 && strcmp(argv[1], "gsr-ui") == 0;
    const bool open_settings_opt = argc >= 2 && strcmp(argv[1], "settings") == 0;
    argc = 1;

    if(geteuid() == 0) {
        fprintf(stderr, "Error: don't run gpu-screen-recorder-gtk as the root user\n");
        return 1;
    }

    dpy = XOpenDisplay(NULL);

    config_empty = false;
    config = read_config(config_empty);

    if(!dpy && launched_by_daemon_opt && config.main_config.use_new_ui) {
        fprintf(stderr, "Error: failed to connect to the X11 server, assuming no graphical session has started yet\n");
        exit(1);
    }

    gsr_info_exit_status = get_gpu_screen_recorder_info(&gsr_info);
    if(gsr_info_exit_status == GsrInfoExitStatus::OK) {
        if(gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
            setenv("GDK_BACKEND", "wayland", true);
        } else {
            setenv("GDK_BACKEND", "x11", true);
        }
    }

    flatpak = is_inside_flatpak();
    nvfbc_installed = gsr_info.system_info.display_server != DisplayServer::WAYLAND && is_nv_fbc_installed();

    if(use_old_ui_opt) {
        config.main_config.use_new_ui = false;
        save_config(config);
    }

    if(config.main_config.use_new_ui)
        startup_new_ui(launched_by_daemon_opt);

    char app_id[] = "com.dec05eba.gpu_screen_recorder";
    // Gtk sets wayland app id / x11 wm class from the binary name, so we override it here.
    // This is needed for the correct window icon on wayland (app id needs to match the desktop file name).
    argv[0] = app_id;

    // Only one instance runs at a time. Launching the application again shows the capture panel or stops the recording, see on_app_activated_again
    GtkApplication *app = gtk_application_new(app_id, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    GSimpleAction *settings_action = g_simple_action_new("settings", nullptr);
    g_signal_connect(settings_action, "activate", G_CALLBACK(on_settings_action), nullptr);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(settings_action));
    g_object_unref(settings_action);

    if(open_settings_opt) {
        GError *error = nullptr;
        if(g_application_register(G_APPLICATION(app), nullptr, &error) && g_application_get_is_remote(G_APPLICATION(app))) {
            g_action_group_activate_action(G_ACTION_GROUP(app), "settings", nullptr);
            GDBusConnection *session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
            if(session_bus) {
                g_dbus_connection_flush_sync(session_bus, nullptr, nullptr);
                g_object_unref(session_bus);
            }
            g_object_unref(app);
            return 0;
        }
        g_clear_error(&error);
        start_with_settings = true;
    }

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
