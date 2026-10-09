#pragma once

#include <vector>
#include <string>
#include <string.h>
#include <functional>
#include <map>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <stdint.h>
#include <pwd.h>
#include <sys/stat.h>

struct ConfigHotkey {
    int64_t keysym = 0;
    uint32_t modifiers = 0;
};

struct MainConfig {
    std::string record_area_option;
    int32_t record_area_width = 0;
    int32_t record_area_height = 0;
    int32_t video_width = 0;
    int32_t video_height = 0;
    int32_t fps = 60;
    int32_t video_bitrate = 15000;
    bool merge_audio_tracks = true;
    bool record_app_audio_inverted = false;
    bool change_video_resolution = false;
    std::vector<std::string> audio_input;
    std::string color_range;
    std::string quality;
    std::string codec; // Video codec
    std::string audio_codec;
    std::string framerate_mode;
    bool advanced_view = false;
    bool show_recording_started_notifications = false;
    bool show_recording_stopped_notifications = false;
    bool show_recording_saved_notifications = true;
    bool record_cursor = true;
    bool hide_window_when_recording = false;
    bool software_encoding_warning_shown = false;
    bool hevc_amd_bug_warning_shown = false;
    bool av1_amd_bug_warning_shown = false;
    bool restore_portal_session = true;
    bool use_new_ui = false;
    bool exclude_metadata = false;
    int32_t installed_gsr_global_hotkeys_version = 0;
    std::string capture_panel_mode;
    std::string capture_panel_selection; // WxH+X+Y, in gdk coordinates
};

struct YoutubeStreamConfig {
    std::string stream_key;
};

struct TwitchStreamConfig {
    std::string stream_key;
};

struct CustomStreamConfig {
    std::string url;
    std::string container;
};

struct StreamingConfig {
    std::string streaming_service;
    YoutubeStreamConfig youtube;
    TwitchStreamConfig twitch;
    CustomStreamConfig custom;
    ConfigHotkey start_stop_recording_hotkey;
};

struct RecordConfig {
    std::string save_directory;
    std::string container;
    ConfigHotkey start_stop_recording_hotkey;
    ConfigHotkey pause_unpause_recording_hotkey;
};

struct ReplayConfig {
    std::string save_directory;
    std::string container;
    int32_t replay_time = 30;
    ConfigHotkey start_stop_recording_hotkey;
    ConfigHotkey save_recording_hotkey;
};

struct Config {
    MainConfig main_config;
    StreamingConfig streaming_config;
    RecordConfig record_config;
    ReplayConfig replay_config;
};

typedef enum {
    CONFIG_TYPE_BOOL,
    CONFIG_TYPE_STRING,         // std::string
    CONFIG_TYPE_I32,
    CONFIG_TYPE_HOTKEY,         // ConfigHotkey
    CONFIG_TYPE_STRING_ARRAY,   // std::vector<std::string>
} ConfigValueType;

struct ConfigValue {
    ConfigValueType type;
    void *data;
};

static std::string get_home_dir() {
    const char *home_dir = getenv("HOME");
    if(!home_dir) {
        passwd *pw = getpwuid(getuid());
        home_dir = pw->pw_dir;
    }

    if(!home_dir) {
        fprintf(stderr, "Error: Failed to get home directory of user, using /tmp directory\n");
        home_dir = "/tmp";
    }

    return home_dir;
}

static std::string get_config_dir() {
    std::string config_dir;
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if(xdg_config_home) {
        config_dir = xdg_config_home;
    } else {
        config_dir = get_home_dir() + "/.config";
    }
    config_dir += "/gpu-screen-recorder";
    return config_dir;
}

// Whoever designed xdg-user-dirs is retarded. Why are some XDG variables environment variables
// while others are in this pseudo shell config file ~/.config/user-dirs.dirs
static std::map<std::string, std::string> get_xdg_variables() {
    std::string user_dirs_filepath;
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if(xdg_config_home) {
        user_dirs_filepath = xdg_config_home;
    } else {
        user_dirs_filepath = get_home_dir() + "/.config";
    }

    user_dirs_filepath += "/user-dirs.dirs";

    std::map<std::string, std::string> result;
    FILE *f = fopen(user_dirs_filepath.c_str(), "rb");
    if(!f)
        return result;

    char line[PATH_MAX];
    while(fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        if(len < 2)
            continue;

        if(line[0] == '#')
            continue;

        if(line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        if(line[len - 1] != '"')
            continue;

        line[len - 1] = '\0';
        len--;

        const char *sep = strchr(line, '=');
        if(!sep)
            continue;

        if(sep[1] != '\"')
            continue;

        std::string value(sep + 2);
        if(strncmp(value.c_str(), "$HOME/", 6) == 0)
            value = get_home_dir() + value.substr(5);

        std::string key(line, sep - line);
        result[std::move(key)] = std::move(value);
    }

    fclose(f);
    return result;
}

static std::string get_videos_dir() {
    auto xdg_vars = get_xdg_variables();
    std::string xdg_videos_dir = xdg_vars["XDG_VIDEOS_DIR"];
    if(xdg_videos_dir.empty())
        xdg_videos_dir = get_home_dir() + "/Videos";
    return xdg_videos_dir;
}

static int create_directory_recursive(char *path) {
    int path_len = strlen(path);
    char *p = path;
    char *end = path + path_len;
    for(;;) {
        char *slash_p = strchr(p, '/');

        // Skips first '/', we don't want to try and create the root directory
        if(slash_p == path) {
            ++p;
            continue;
        }

        if(!slash_p)
            slash_p = end;

        char prev_char = *slash_p;
        *slash_p = '\0';
        int err = mkdir(path, S_IRWXU);
        *slash_p = prev_char;

        if(err == -1 && errno != EEXIST)
            return err;

        if(slash_p == end)
            break;
        else
            p = slash_p + 1;
    }
    return 0;
}

static bool file_get_content(const char *filepath, std::string &file_content) {
    file_content.clear();
    bool success = false;

    FILE *file = fopen(filepath, "rb");
    if(!file)
        return success;

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    if(file_size != -1) {
        file_content.resize(file_size);
        fseek(file, 0, SEEK_SET);
        if((long)fread(&file_content[0], 1, file_size, file) == file_size)
            success = true;
    }

    fclose(file);
    return success;
}

struct StringView {
    const char *str = nullptr;
    size_t size = 0;

    bool operator == (const char *other) const {
        int len = strlen(other);
        return (size_t)len == size && memcmp(str, other, size) == 0;
    }

    size_t find(char c) const {
        const void *p = memchr(str, c, size);
        if(!p)
            return std::string::npos;
        return (const char*)p - str;
    }
};

using StringSplitCallback = std::function<bool(StringView line)>;

static void string_split_char(const std::string &str, char delimiter, StringSplitCallback callback_func) {
    size_t index = 0;
    while(index < str.size()) {
        size_t new_index = str.find(delimiter, index);
        if(new_index == std::string::npos)
            new_index = str.size();

        if(!callback_func({str.data() + index, new_index - index}))
            break;

        index = new_index + 1;
    }
}

static bool config_split_key_value(const StringView str, StringView &key, StringView &value) {
    key.str = nullptr;
    key.size = 0;

    value.str = nullptr;
    value.size = 0;

    size_t index = str.find(' ');
    if(index == std::string::npos)
        return std::string::npos;

    key.str = str.str;
    key.size = index;

    value.str = str.str + index + 1;
    value.size = str.size - (index + 1);

    return true;
}

static std::map<std::string, ConfigValue> get_config_options(Config &config) {
    return {
        {"main.record_area_option", {CONFIG_TYPE_STRING, &config.main_config.record_area_option}},
        {"main.record_area_width", {CONFIG_TYPE_I32, &config.main_config.record_area_width}},
        {"main.record_area_height", {CONFIG_TYPE_I32, &config.main_config.record_area_height}},
        {"main.video_width", {CONFIG_TYPE_I32, &config.main_config.video_width}},
        {"main.video_height", {CONFIG_TYPE_I32, &config.main_config.video_height}},
        {"main.fps", {CONFIG_TYPE_I32, &config.main_config.fps}},
        {"main.video_bitrate", {CONFIG_TYPE_I32, &config.main_config.video_bitrate}},
        {"main.merge_audio_tracks", {CONFIG_TYPE_BOOL, &config.main_config.merge_audio_tracks}},
        {"main.record_app_audio_inverted", {CONFIG_TYPE_BOOL, &config.main_config.record_app_audio_inverted}},
        {"main.change_video_resolution", {CONFIG_TYPE_BOOL, &config.main_config.change_video_resolution}},
        {"main.audio_input", {CONFIG_TYPE_STRING_ARRAY, &config.main_config.audio_input}},
        {"main.color_range", {CONFIG_TYPE_STRING, &config.main_config.color_range}},
        {"main.quality", {CONFIG_TYPE_STRING, &config.main_config.quality}},
        {"main.codec", {CONFIG_TYPE_STRING, &config.main_config.codec}},
        {"main.audio_codec", {CONFIG_TYPE_STRING, &config.main_config.audio_codec}},
        {"main.framerate_mode", {CONFIG_TYPE_STRING, &config.main_config.framerate_mode}},
        {"main.advanced_view", {CONFIG_TYPE_BOOL, &config.main_config.advanced_view}},
        {"main.show_recording_started_notifications", {CONFIG_TYPE_BOOL, &config.main_config.show_recording_started_notifications}},
        {"main.show_recording_stopped_notifications", {CONFIG_TYPE_BOOL, &config.main_config.show_recording_stopped_notifications}},
        {"main.show_recording_saved_notifications", {CONFIG_TYPE_BOOL, &config.main_config.show_recording_saved_notifications}},
        {"main.record_cursor", {CONFIG_TYPE_BOOL, &config.main_config.record_cursor}},
        {"main.hide_window_when_recording", {CONFIG_TYPE_BOOL, &config.main_config.hide_window_when_recording}},
        {"main.software_encoding_warning_shown", {CONFIG_TYPE_BOOL, &config.main_config.software_encoding_warning_shown}},
        {"main.hevc_amd_bug_warning_shown", {CONFIG_TYPE_BOOL, &config.main_config.hevc_amd_bug_warning_shown}},
        {"main.av1_amd_bug_warning_shown", {CONFIG_TYPE_BOOL, &config.main_config.av1_amd_bug_warning_shown}},
        {"main.restore_portal_session", {CONFIG_TYPE_BOOL, &config.main_config.restore_portal_session}},
        {"main.use_new_ui", {CONFIG_TYPE_BOOL, &config.main_config.use_new_ui}},
        {"main.exclude_metadata", {CONFIG_TYPE_BOOL, &config.main_config.exclude_metadata}},
        {"main.installed_gsr_global_hotkeys_version", {CONFIG_TYPE_I32, &config.main_config.installed_gsr_global_hotkeys_version}},
        {"main.capture_panel_mode", {CONFIG_TYPE_STRING, &config.main_config.capture_panel_mode}},
        {"main.capture_panel_selection", {CONFIG_TYPE_STRING, &config.main_config.capture_panel_selection}},

        {"streaming.service", {CONFIG_TYPE_STRING, &config.streaming_config.streaming_service}},
        {"streaming.youtube.key", {CONFIG_TYPE_STRING, &config.streaming_config.youtube.stream_key}},
        {"streaming.twitch.key", {CONFIG_TYPE_STRING, &config.streaming_config.twitch.stream_key}},
        {"streaming.custom.url", {CONFIG_TYPE_STRING, &config.streaming_config.custom.url}},
        {"streaming.custom.container", {CONFIG_TYPE_STRING, &config.streaming_config.custom.container}},
        {"streaming.start_stop_recording_hotkey", {CONFIG_TYPE_HOTKEY, &config.streaming_config.start_stop_recording_hotkey}},

        {"record.save_directory", {CONFIG_TYPE_STRING, &config.record_config.save_directory}},
        {"record.container", {CONFIG_TYPE_STRING, &config.record_config.container}},
        {"record.start_stop_recording_hotkey", {CONFIG_TYPE_HOTKEY, &config.record_config.start_stop_recording_hotkey}},
        {"record.pause_unpause_recording_hotkey", {CONFIG_TYPE_HOTKEY, &config.record_config.pause_unpause_recording_hotkey}},

        {"replay.save_directory", {CONFIG_TYPE_STRING, &config.replay_config.save_directory}},
        {"replay.container", {CONFIG_TYPE_STRING, &config.replay_config.container}},
        {"replay.time", {CONFIG_TYPE_I32, &config.replay_config.replay_time}},
        {"replay.start_stop_recording_hotkey", {CONFIG_TYPE_HOTKEY, &config.replay_config.start_stop_recording_hotkey}},
        {"replay.save_recording_hotkey", {CONFIG_TYPE_HOTKEY, &config.replay_config.save_recording_hotkey}}
    };
}

#define FORMAT_I32 "%" PRIi32
#define FORMAT_I64 "%" PRIi64
#define FORMAT_U32 "%" PRIu32

static Config read_config(bool &config_empty) {
    Config config;

    const std::string config_path = get_config_dir() + "/config";
    std::string file_content;
    if(!file_get_content(config_path.c_str(), file_content)) {
        fprintf(stderr, "Warning: Failed to read config file: %s\n", config_path.c_str());
        config_empty = true;
        return config;
    }

    auto config_options = get_config_options(config);

    string_split_char(file_content, '\n', [&](StringView line) {
        StringView key, sv_val;
        if(!config_split_key_value(line, key, sv_val)) {
            fprintf(stderr, "Warning: Invalid config option format: %.*s\n", (int)line.size, line.str);
            return true;
        }

        if(key.size == 0 || sv_val.size == 0)
            return true;

        auto it = config_options.find(std::string(key.str, key.size));
        if(it == config_options.end())
            return true;

        switch(it->second.type) {
            case CONFIG_TYPE_BOOL: {
                *(bool*)it->second.data = sv_val == "true";
                break;
            }
            case CONFIG_TYPE_STRING: {
                ((std::string*)it->second.data)->assign(sv_val.str, sv_val.size);
                break;
            }
            case CONFIG_TYPE_I32: {
                std::string value_str(sv_val.str, sv_val.size);
                int32_t *value = (int32_t*)it->second.data;
                if(sscanf(value_str.c_str(), FORMAT_I32, value) != 1) {
                    fprintf(stderr, "Warning: Invalid config option value for %.*s\n", (int)key.size, key.str);
                    *value = 0;
                }
                break;
            }
            case CONFIG_TYPE_HOTKEY: {
                std::string value_str(sv_val.str, sv_val.size);
                ConfigHotkey *config_hotkey = (ConfigHotkey*)it->second.data;
                if(sscanf(value_str.c_str(), FORMAT_I64 " " FORMAT_U32, &config_hotkey->keysym, &config_hotkey->modifiers) != 2) {
                    fprintf(stderr, "Warning: Invalid config option value for %.*s\n", (int)key.size, key.str);
                    config_hotkey->keysym = 0;
                    config_hotkey->modifiers = 0;
                }
                break;
            }
            case CONFIG_TYPE_STRING_ARRAY: {
                std::string array_value(sv_val.str, sv_val.size);
                ((std::vector<std::string>*)it->second.data)->push_back(std::move(array_value));
                break;
            }
        }

        return true;
    });

    return config;
}

static void save_config(Config &config) {
    const std::string config_path = get_config_dir() + "/config";

    char dir_tmp[PATH_MAX];
    strcpy(dir_tmp, config_path.c_str());
    char *dir = dirname(dir_tmp);

    if(create_directory_recursive(dir) != 0) {
        fprintf(stderr, "Warning: Failed to create config directory: %s\n", dir);
        return;
    }

    FILE *file = fopen(config_path.c_str(), "wb");
    if(!file) {
        fprintf(stderr, "Warning: Failed to create config file: %s\n", config_path.c_str());
        return;
    }

    const auto config_options = get_config_options(config);
    for(auto it : config_options) {
        switch(it.second.type) {
            case CONFIG_TYPE_BOOL: {
                fprintf(file, "%s %s\n", it.first.c_str(), *(bool*)it.second.data ? "true" : "false");
                break;
            }
            case CONFIG_TYPE_STRING: {
                fprintf(file, "%s %s\n", it.first.c_str(), ((std::string*)it.second.data)->c_str());
                break;
            }
            case CONFIG_TYPE_I32: {
                fprintf(file, "%s " FORMAT_I32 "\n", it.first.c_str(), *(int32_t*)it.second.data);
                break;
            }
            case CONFIG_TYPE_HOTKEY: {
                const ConfigHotkey *config_hotkey = (const ConfigHotkey*)it.second.data;
                fprintf(file, "%s " FORMAT_I64 " " FORMAT_U32 "\n", it.first.c_str(), config_hotkey->keysym, config_hotkey->modifiers);
                break;
            }
            case CONFIG_TYPE_STRING_ARRAY: {
                for(const std::string &value : *(std::vector<std::string>*)it.second.data) {
                    fprintf(file, "%s %s\n", it.first.c_str(), value.c_str());
                }
                break;
            }
        }
    }

    fclose(file);
}
