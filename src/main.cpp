#include <gtk/gtk.h>
#include "miniaudio.h"
#include "downloader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

class Player {
    ma_engine engine{};
    std::unique_ptr<ma_sound> sound;
    bool initialized = false;
    bool offline = false;
public:
    bool playing = false;
    std::string error;
    ~Player() { clear(); if (initialized) ma_engine_uninit(&engine); }
    bool init(bool test = false) {
        if (initialized) return true;
        offline = test;
        auto cfg = ma_engine_config_init();
        cfg.noAutoStart = MA_TRUE;
        cfg.periodSizeInMilliseconds = 50;
        if (test) { cfg.noDevice = MA_TRUE; cfg.channels = 2; cfg.sampleRate = 48000; }
        auto result = ma_engine_init(&cfg, &engine);
        initialized = result == MA_SUCCESS;
        if (!initialized) error = std::string("Could not open your audio output: ") + ma_result_description(result);
        return initialized;
    }
    void pause() {
        if (!playing) return;
        if (!offline) ma_engine_stop(&engine);
        ma_sound_stop(sound.get());
        playing = false;
    }
    void clear() { pause(); if (sound) { ma_sound_uninit(sound.get()); sound.reset(); } }
    bool load(const std::string& path) {
        if (!init(offline)) return false;
        auto next = std::make_unique<ma_sound>();
        auto result = ma_sound_init_from_file(&engine, path.c_str(),
            MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, next.get());
        if (result != MA_SUCCESS) {
            error = std::string("Could not read this audio file: ") + ma_result_description(result);
            return false;
        }
        clear(); sound = std::move(next);
        ma_sound_set_looping(sound.get(), MA_TRUE);
        return true;
    }
    bool play() {
        if (!sound) return false;
        auto result = ma_sound_start(sound.get());
        if (result == MA_SUCCESS && !offline) result = ma_engine_start(&engine);
        if (result != MA_SUCCESS) {
            ma_sound_stop(sound.get());
            error = std::string("Could not start playback: ") + ma_result_description(result);
            return false;
        }
        playing = true; return true;
    }
    bool ready() const { return sound != nullptr; }
    void volume(double value) { if (initialized) ma_engine_set_volume(&engine, static_cast<float>(value)); }
    float duration() const { float n = 0; if (sound) ma_sound_get_length_in_seconds(sound.get(), &n); return n; }
    float position() const { float n = 0; if (sound) ma_sound_get_cursor_in_seconds(sound.get(), &n); return n; }
    void seek(double seconds) { if (sound) ma_sound_seek_to_second(sound.get(), static_cast<float>(seconds)); }
    ma_result render(float* out, ma_uint64 frames) { return ma_engine_read_pcm_frames(&engine, out, frames, nullptr); }
};

static std::string basename(const std::string& path) {
    gchar* base = g_path_get_basename(path.c_str());
    gchar* display = g_filename_display_name(base);
    std::string result = display; g_free(base); g_free(display); return result;
}
static std::string title(const std::string& path) {
    auto result = basename(path); auto dot = result.rfind('.');
    if (dot != std::string::npos) result.resize(dot);
    return result;
}
static std::string displayPath(const std::string& path) {
    gchar* display = g_filename_display_name(path.c_str());
    std::string result = display; g_free(display); return result;
}
static std::string timeText(double seconds) {
    int n = static_cast<int>(std::max(0.0, seconds)); char text[32];
    if (n >= 3600) std::snprintf(text, sizeof(text), "%d:%02d:%02d", n/3600, n/60%60, n%60);
    else std::snprintf(text, sizeof(text), "%d:%02d", n/60, n%60);
    return text;
}
static void cssClass(GtkWidget* widget, const char* name) { gtk_style_context_add_class(gtk_widget_get_style_context(widget), name); }
static GtkWidget* label(const char* text, const char* style = nullptr) {
    auto* w = gtk_label_new(text); gtk_label_set_xalign(GTK_LABEL(w), 0);
    if (style) cssClass(w, style);
    return w;
}
static GtkWidget* box(GtkOrientation orientation, int spacing) { return gtk_box_new(orientation, spacing); }
static void pack(GtkWidget* parent, GtkWidget* child, bool expand = false) {
    gtk_box_pack_start(GTK_BOX(parent), child, expand, expand, 0);
}
static GtkWidget* iconButton(const char* icon, const char* tip) {
    auto* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tip); atk_object_set_name(gtk_widget_get_accessible(b), tip); return b;
}

struct App {
    GtkApplication* application = nullptr;
    GtkWidget *window = nullptr, *list = nullptr, *trackTitle = nullptr, *trackPath = nullptr;
    GtkWidget *playButton = nullptr, *playImage = nullptr, *playText = nullptr, *status = nullptr;
    GtkWidget *seekScale = nullptr, *elapsed = nullptr, *total = nullptr, *volumeScale = nullptr;
    GtkWidget *removeButton = nullptr, *restartButton = nullptr, *empty = nullptr, *count = nullptr;
    GtkWidget *youtubeEntry = nullptr, *downloadButton = nullptr, *cancelButton = nullptr;
    GtkWidget *downloadStatus = nullptr, *downloadProgress = nullptr;
    Player player;
    Downloader downloader;
    std::vector<std::string> paths;
    int selected = -1;
    double volume = 0.65;
    bool updating = false, rebuilding = false, dragging = false;
    guint timer = 0, saveTimer = 0;
    std::string configPath;

    App() {
        configPath = std::string(g_get_user_config_dir()) + "/loop/settings.ini";
        auto* key = g_key_file_new();
        if (g_key_file_load_from_file(key, configPath.c_str(), G_KEY_FILE_NONE, nullptr)) {
            gsize n = 0; gchar** saved = g_key_file_get_string_list(key, "loop", "paths", &n, nullptr);
            for (gsize i=0; i<n; ++i) paths.emplace_back(saved[i]);
            g_strfreev(saved);
            selected = g_key_file_get_integer(key, "loop", "selected", nullptr);
            if (g_key_file_has_key(key, "loop", "volume", nullptr)) volume = g_key_file_get_double(key, "loop", "volume", nullptr);
        } else {
            paths.push_back(std::string(g_get_user_data_dir()) + "/loop/what it feels like to be a memory (playlist).mp3");
            selected = 0;
        }
        if (!std::isfinite(volume)) volume = .65;
        volume = std::clamp(volume, 0.0, 1.0);
        selected = paths.empty() ? -1 : std::clamp(selected, 0, static_cast<int>(paths.size())-1);
        g_key_file_unref(key);
    }
    ~App() { if (timer) g_source_remove(timer); if (saveTimer) g_source_remove(saveTimer); }
    void save() {
        auto dir = std::string(g_get_user_config_dir()) + "/loop";
        if (g_mkdir_with_parents(dir.c_str(), 0700) != 0) { g_warning("Could not create settings folder"); return; }
        auto* key = g_key_file_new(); std::vector<const gchar*> pointers;
        for (const auto& p : paths) pointers.push_back(p.c_str());
        g_key_file_set_string_list(key, "loop", "paths", pointers.data(), pointers.size());
        g_key_file_set_integer(key, "loop", "selected", selected);
        g_key_file_set_double(key, "loop", "volume", volume);
        GError* error = nullptr;
        if (!g_key_file_save_to_file(key, configPath.c_str(), &error)) { g_warning("Settings: %s", error->message); g_error_free(error); }
        g_key_file_unref(key);
    }
    void report(const std::string& message) {
        auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    }
    void sync() {
        bool valid = selected >= 0 && selected < static_cast<int>(paths.size());
        gtk_label_set_text(GTK_LABEL(trackTitle), valid ? title(paths[selected]).c_str() : "Make yourself a loop");
        gtk_label_set_text(GTK_LABEL(trackPath), valid ? displayPath(paths[selected]).c_str() : "Add an MP3 to get started.");
        gtk_widget_set_tooltip_text(trackPath, valid ? displayPath(paths[selected]).c_str() : nullptr);
        gtk_label_set_text(GTK_LABEL(status), player.playing ? "PLAYING ON REPEAT" : (valid ? "READY WHEN YOU ARE" : "YOUR SOUND. ON REPEAT."));
        gtk_label_set_text(GTK_LABEL(playText), player.playing ? "Pause" : "Play");
        gtk_image_set_from_icon_name(GTK_IMAGE(playImage), player.playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_sensitive(playButton, valid);
        gtk_widget_set_sensitive(removeButton, valid);
        gtk_widget_set_sensitive(restartButton, player.ready());
        gtk_widget_set_visible(empty, paths.empty());
        auto size = std::to_string(paths.size()) + (paths.size() == 1 ? " saved track" : " saved tracks");
        gtk_label_set_text(GTK_LABEL(count), size.c_str());
        tick();
        if (player.playing && !timer) timer = g_timeout_add_seconds(1, [](gpointer data) -> gboolean { static_cast<App*>(data)->tick(); return G_SOURCE_CONTINUE; }, this);
        if (!player.playing && timer) { g_source_remove(timer); timer = 0; }
    }
    void tick() {
        if (dragging) return;
        double duration = player.duration(), position = player.position();
        updating = true;
        gtk_range_set_range(GTK_RANGE(seekScale), 0, std::max(1.0, duration));
        gtk_range_set_value(GTK_RANGE(seekScale), position);
        gtk_widget_set_sensitive(seekScale, duration > 0);
        gtk_label_set_text(GTK_LABEL(elapsed), timeText(position).c_str());
        gtk_label_set_text(GTK_LABEL(total), duration > 0 ? timeText(duration).c_str() : "—:——");
        updating = false;
    }
    void toggle() {
        if (selected < 0) return;
        if (player.playing) player.pause();
        else {
            if (!player.ready() && !player.load(paths[selected])) { report(player.error); return; }
            player.volume(volume);
            if (!player.play()) report(player.error);
        }
        sync();
    }
    void select(int index) {
        if (index == selected || index < 0 || index >= static_cast<int>(paths.size())) return;
        bool resume = player.playing;
        player.clear(); selected = index;
        if (resume) {
            if (player.load(paths[selected])) { player.volume(volume); if (!player.play()) report(player.error); }
            else report(player.error);
        }
        save(); sync();
    }
    void rebuild() {
        rebuilding = true;
        auto* children = gtk_container_get_children(GTK_CONTAINER(list));
        for (auto* p = children; p; p = p->next) gtk_widget_destroy(GTK_WIDGET(p->data));
        g_list_free(children);
        for (const auto& path : paths) {
            auto* row = gtk_list_box_row_new(); auto* line = box(GTK_ORIENTATION_HORIZONTAL, 14);
            gtk_container_set_border_width(GTK_CONTAINER(line), 12);
            auto* music = gtk_image_new_from_icon_name("audio-x-generic-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
            cssClass(music, "muted"); pack(line, music);
            auto* texts = box(GTK_ORIENTATION_VERTICAL, 4);
            auto* name = label(title(path).c_str(), "track-name");
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            auto* location = label(displayPath(path).c_str(), "path");
            gtk_label_set_ellipsize(GTK_LABEL(location), PANGO_ELLIPSIZE_MIDDLE);
            gtk_widget_set_tooltip_text(row, displayPath(path).c_str());
            pack(texts, name); pack(texts, location); pack(line, texts, true);
            pack(line, label("MP3", "format"));
            gtk_container_add(GTK_CONTAINER(row), line); gtk_container_add(GTK_CONTAINER(list), row);
        }
        gtk_widget_show_all(list);
        if (selected >= 0) gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), selected));
        rebuilding = false; sync();
    }
    bool addPath(std::string path, bool choose = true) {
        if (path.size() > 1 && path.front() == '~' && path[1] == '/') path = std::string(g_get_home_dir()) + path.substr(1);
        if (g_str_has_prefix(path.c_str(), "file://")) {
            gchar* decoded = g_filename_from_uri(path.c_str(), nullptr, nullptr);
            if (!decoded) { report("This file URL is not valid."); return false; }
            path = decoded; g_free(decoded);
        }
        gchar* canonical = g_canonicalize_filename(path.c_str(), nullptr); path = canonical; g_free(canonical);
        if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) { report("That file could not be found. Check the path and try again."); return false; }
        // Validate with the same decoder used for playback, without opening an audio device.
        ma_decoder decoder{}; auto cfg = ma_decoder_config_init_default();
        if (ma_decoder_init_file(path.c_str(), &cfg, &decoder) != MA_SUCCESS) { report("This file could not be decoded. Please choose a valid MP3."); return false; }
        ma_decoder_uninit(&decoder);
        auto found = std::find(paths.begin(), paths.end(), path);
        int index = static_cast<int>(found-paths.begin());
        if (found == paths.end()) paths.push_back(path);
        if (choose || selected < 0) select(index);
        rebuild(); save(); return true;
    }
    void setDownloadStatus(const std::string& message, double fraction) {
        if (!window) return;
        gtk_label_set_text(GTK_LABEL(downloadStatus), message.c_str());
        gtk_widget_set_tooltip_text(downloadStatus, message.c_str());
        gtk_widget_set_visible(downloadProgress, true);
        if (fraction >= 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(downloadProgress), std::clamp(fraction, 0.0, 1.0));
        else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(downloadProgress));
    }
    void download() {
        if (downloader.busy()) return;
        gchar* text = g_strdup(gtk_entry_get_text(GTK_ENTRY(youtubeEntry)));
        std::string input = g_strstrip(text); g_free(text);
        if (input.find("://") == std::string::npos) input = "https://" + input;
        GUri* uri = g_uri_parse(input.c_str(), G_URI_FLAGS_NONE, nullptr);
        std::string id;
        if (uri) {
            const char* host = g_uri_get_host(uri);
            const char* scheme = g_uri_get_scheme(uri);
            const char* path = g_uri_get_path(uri);
            bool http = scheme && (!g_ascii_strcasecmp(scheme, "https") || !g_ascii_strcasecmp(scheme, "http"));
            gchar* lower = host ? g_ascii_strdown(host, -1) : g_strdup("");
            bool youtube = !std::strcmp(lower, "youtube.com") || g_str_has_suffix(lower, ".youtube.com");
            if (http && path) {
                if (!std::strcmp(lower, "youtu.be") || !std::strcmp(lower, "www.youtu.be")) id = path[0] == '/' ? path+1 : path;
                else if (youtube) {
                    const char* query = g_uri_get_query(uri);
                    if (!std::strcmp(path, "/watch") && query) {
                        GHashTable* params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, nullptr);
                        if (params) {
                            const char* value = static_cast<const char*>(g_hash_table_lookup(params, "v"));
                            if (value) id = value;
                            g_hash_table_unref(params);
                        }
                    } else {
                        for (const char* prefix : {"/shorts/", "/live/", "/embed/"}) {
                            if (g_str_has_prefix(path, prefix)) id = path+std::strlen(prefix);
                        }
                    }
                }
            }
            g_free(lower); g_uri_unref(uri);
        }
        auto slash = id.find('/'); if (slash != std::string::npos) id.resize(slash);
        if (id.size() != 11 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return g_ascii_isalnum(c) || c == '_' || c == '-'; })) {
            gtk_label_set_text(GTK_LABEL(downloadStatus), "Paste a YouTube video link, such as youtube.com/watch?v=… or youtu.be/…");
            gtk_widget_grab_focus(youtubeEntry); return;
        }
        std::string error;
        if (!downloader.start("https://www.youtube.com/watch?v="+id, error)) {
            gtk_label_set_text(GTK_LABEL(downloadStatus), error.c_str()); return;
        }
        // Keep callback owners alive if the window closes while a child is exiting.
        g_application_hold(G_APPLICATION(application));
        gtk_widget_set_sensitive(youtubeEntry, FALSE);
        gtk_widget_set_sensitive(downloadButton, FALSE);
        gtk_widget_set_sensitive(cancelButton, TRUE);
        gtk_widget_set_visible(cancelButton, TRUE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(downloadProgress), 0);
        setDownloadStatus("Connecting to YouTube…", -1);
    }
    void browse() {
        auto* dialog = gtk_file_chooser_native_new("Add MP3 files", GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_OPEN, "Add", "Cancel");
        gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
        auto* filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "MP3 audio");
        gtk_file_filter_add_mime_type(filter, "audio/mpeg");
        gtk_file_filter_add_pattern(filter, "*.[mM][pP]3"); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            auto* files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
            for (auto* p = files; p; p = p->next) addPath(static_cast<char*>(p->data));
            g_slist_free_full(files, g_free);
        }
        g_object_unref(dialog);
    }
    void pastePath() {
        auto* dialog = gtk_dialog_new_with_buttons("Add a file path", GTK_WINDOW(window), GTK_DIALOG_MODAL,
            "Cancel", GTK_RESPONSE_CANCEL, "Add track", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 540, -1);
        auto* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(content), 20);
        gtk_box_set_spacing(GTK_BOX(content), 12);
        pack(content, label("Paste the path to an MP3 on your computer.", "muted"));
        auto* entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "~/Music/my track.mp3");
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE); pack(content, entry);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); gtk_widget_show_all(dialog);
        while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            std::string path = gtk_entry_get_text(GTK_ENTRY(entry));
            if (path.size() >= 2 && ((path.front() == '"' && path.back() == '"') || (path.front() == '\'' && path.back() == '\''))) path = path.substr(1, path.size()-2);
            if (!path.empty() && addPath(path)) break;
        }
        gtk_widget_destroy(dialog);
    }
    void remove() {
        if (selected < 0) return;
        player.clear(); paths.erase(paths.begin()+selected);
        selected = paths.empty() ? -1 : std::min(selected, static_cast<int>(paths.size())-1);
        rebuild(); save();
    }
    void build() {
        auto* provider = gtk_css_provider_new();
        const char* css = R"CSS(
            window { background: #ffffff; color: #191919; font-family: "Noto Sans", sans-serif; border-radius: 16px; }
            window.loop-window { background: transparent; }
            window.loop-window decoration { border-radius: 16px; }
            .app-body { background: #ffffff; border-radius: 0 0 16px 16px; }
            headerbar { background: #ffffff; color: #191919; border: 0; border-bottom: 1px solid #eeeeee; border-radius: 16px 16px 0 0; box-shadow: none; }
            headerbar .title { font-weight: 700; }
            headerbar button { background: transparent; border-color: transparent; padding: 4px; }
            headerbar button:hover { background: #eeeeee; }
            label { color: inherit; }
            .brand { font-size: 25px; font-weight: 700; letter-spacing: -0.7px; }
            .muted, .path { color: #737373; }
            .path { font-size: 11px; }
            .eyebrow { color: #737373; font-size: 10px; font-weight: 600; letter-spacing: 1.2px; }
            .badge { color: #555555; padding: 6px 0; font-size: 12px; }
            .card { background: #fafafa; border: 1px solid #262626; border-radius: 14px; padding: 24px; }
            .hero { font-size: 23px; font-weight: 700; letter-spacing: -0.5px; }
            .infinity { color: #333333; font-size: 24px; font-weight: 400; }
            .section { font-size: 16px; font-weight: 700; }
            .track-name { font-size: 13px; font-weight: 600; }
            .format { font-size: 9px; color: #666666; background: #eeeeee; border-radius: 7px; padding: 4px 6px; }
            .time { font-family: monospace; font-size: 11px; color: #777777; }
            button { background: #ffffff; color: #191919; border: 1px solid #dddddd; border-radius: 10px; padding: 8px 12px; box-shadow: none; text-shadow: none; }
            button:hover { background: #f2f2f2; border-color: #b3b3b3; }
            button:active { background: #e7e7e7; }
            button:disabled { opacity: 0.4; }
            button.primary { background: #191919; color: #ffffff; border-color: #191919; padding: 12px 24px; font-weight: 600; }
            button.primary:hover { background: #383838; }
            button:focus { outline-color: #888888; outline-width: 1px; outline-style: dashed; outline-offset: 2px; }
            scale { padding: 10px 0; }
            scale trough { background: #dddddd; border: 0; border-radius: 3px; min-height: 3px; }
            scale highlight { background: #262626; border: 0; border-radius: 3px; }
            scale slider { background: #262626; border: 0; box-shadow: none; min-width: 11px; min-height: 11px; }
            list, scrolledwindow { background: transparent; }
            row { background: #ffffff; color: #191919; border-radius: 12px; border: 1px solid #dddddd; margin: 3px 0; }
            row:hover { background: #f7f7f7; }
            row:selected { background: #f7f7f7; border-color: #333333; }
            entry { background: #ffffff; color: #191919; border: 1px solid #cccccc; border-radius: 10px; padding: 8px; }
            .footer { color: #777777; font-size: 11px; }
            progressbar trough { background: #eeeeee; border: 0; min-height: 4px; border-radius: 3px; }
            progressbar progress { background: #262626; border: 0; min-height: 4px; border-radius: 3px; }
        )CSS";
        gtk_css_provider_load_from_data(provider, css, -1, nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        window = gtk_application_window_new(application);
        cssClass(window, "loop-window");
        if (auto* visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window))) gtk_widget_set_visual(window, visual);
        gtk_window_set_title(GTK_WINDOW(window), "loop");
        gtk_window_set_icon_name(GTK_WINDOW(window), "io.local.loop");
        gtk_window_set_default_size(GTK_WINDOW(window), 660, 790);
        auto* header = gtk_header_bar_new(); gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header), "loop"); gtk_window_set_titlebar(GTK_WINDOW(window), header);
        auto* root = box(GTK_ORIENTATION_VERTICAL, 26); gtk_container_set_border_width(GTK_CONTAINER(root), 30);
        cssClass(root, "app-body");
        gtk_container_add(GTK_CONTAINER(window), root);
        auto* brandRow = box(GTK_ORIENTATION_HORIZONTAL, 12); auto* brandText = box(GTK_ORIENTATION_VERTICAL, 3);
        pack(brandText, label("loop", "brand")); pack(brandText, label("Your sound. On repeat.", "muted")); pack(brandRow, brandText, true);
        auto* badge = label("Repeat always on", "badge"); gtk_widget_set_valign(badge, GTK_ALIGN_CENTER); pack(brandRow, badge); pack(root, brandRow);
        auto* card = box(GTK_ORIENTATION_VERTICAL, 12); cssClass(card, "card"); pack(root, card);
        auto* cardHeader = box(GTK_ORIENTATION_HORIZONTAL, 12); status = label("READY WHEN YOU ARE", "eyebrow"); pack(cardHeader, status, true);
        pack(cardHeader, label("∞", "infinity")); pack(card, cardHeader);
        trackTitle = label("", "hero"); gtk_label_set_ellipsize(GTK_LABEL(trackTitle), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(trackTitle), 36); pack(card, trackTitle);
        trackPath = label("", "path"); gtk_label_set_ellipsize(GTK_LABEL(trackPath), PANGO_ELLIPSIZE_MIDDLE); pack(card, trackPath);
        auto* progress = box(GTK_ORIENTATION_VERTICAL, 0);
        seekScale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1); gtk_scale_set_draw_value(GTK_SCALE(seekScale), FALSE);
        atk_object_set_name(gtk_widget_get_accessible(seekScale), "Playback position"); pack(progress, seekScale);
        auto* times = box(GTK_ORIENTATION_HORIZONTAL, 8); elapsed = label("0:00", "time"); total = label("—:——", "time");
        pack(times, elapsed, true); pack(times, total); pack(progress, times); pack(card, progress);
        auto* controls = box(GTK_ORIENTATION_HORIZONTAL, 10);
        restartButton = iconButton("media-skip-backward-symbolic", "Restart track"); pack(controls, restartButton);
        playButton = gtk_button_new(); cssClass(playButton, "primary"); auto* playContents = box(GTK_ORIENTATION_HORIZONTAL, 8);
        playImage = gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
        playText = label("Play"); pack(playContents, playImage); pack(playContents, playText); gtk_container_add(GTK_CONTAINER(playButton), playContents); pack(controls, playButton);
        auto* spacer = box(GTK_ORIENTATION_HORIZONTAL, 0); pack(controls, spacer, true);
        auto* volumeIcon = gtk_image_new_from_icon_name("audio-volume-medium-symbolic", GTK_ICON_SIZE_BUTTON); cssClass(volumeIcon, "muted"); pack(controls, volumeIcon);
        volumeScale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        gtk_scale_set_draw_value(GTK_SCALE(volumeScale), FALSE); gtk_range_set_value(GTK_RANGE(volumeScale), volume*100);
        gtk_widget_set_size_request(volumeScale, 100, -1); gtk_widget_set_tooltip_text(volumeScale, "Volume");
        atk_object_set_name(gtk_widget_get_accessible(volumeScale), "Volume"); pack(controls, volumeScale); pack(card, controls);
        auto* library = box(GTK_ORIENTATION_VERTICAL, 10); pack(root, library, true);
        auto* youtube = box(GTK_ORIENTATION_VERTICAL, 8);
        pack(youtube, label("Add from YouTube", "section"));
        auto* urlRow = box(GTK_ORIENTATION_HORIZONTAL, 8);
        youtubeEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(youtubeEntry), "Paste a YouTube video link");
        gtk_entry_set_input_purpose(GTK_ENTRY(youtubeEntry), GTK_INPUT_PURPOSE_URL);
        atk_object_set_name(gtk_widget_get_accessible(youtubeEntry), "YouTube video link");
        pack(urlRow, youtubeEntry, true);
        downloadButton = gtk_button_new_with_label("Download"); cssClass(downloadButton, "primary");
        pack(urlRow, downloadButton);
        cancelButton = gtk_button_new_with_label("Cancel"); pack(urlRow, cancelButton);
        gtk_widget_set_no_show_all(cancelButton, TRUE);
        pack(youtube, urlRow);
        downloadProgress = gtk_progress_bar_new();
        gtk_widget_set_no_show_all(downloadProgress, TRUE);
        atk_object_set_name(gtk_widget_get_accessible(downloadProgress), "YouTube download progress");
        pack(youtube, downloadProgress);
        downloadStatus = label("Downloads an MP3 and adds it to your tracks.", "footer");
        gtk_label_set_ellipsize(GTK_LABEL(downloadStatus), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(downloadStatus), 1);
        pack(youtube, downloadStatus); pack(library, youtube);
        auto* divider = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL); pack(library, divider);
        auto* libraryHeader = box(GTK_ORIENTATION_HORIZONTAL, 8); pack(libraryHeader, label("Your tracks", "section"), true);
        auto* pathButton = gtk_button_new_with_label("Paste path"); auto* browseButton = gtk_button_new_with_label("+ Add MP3");
        pack(libraryHeader, pathButton); pack(libraryHeader, browseButton); pack(library, libraryHeader);
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, -1, 115); list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE); gtk_container_add(GTK_CONTAINER(scroll), list); pack(library, scroll, true);
        empty = label("Nothing here yet. Add your first MP3 above.", "muted"); pack(library, empty);
        auto* libraryFooter = box(GTK_ORIENTATION_HORIZONTAL, 8); count = label("", "footer"); pack(libraryFooter, count, true);
        removeButton = iconButton("list-remove-symbolic", "Remove selected path from this list (keeps the file)"); pack(libraryFooter, removeButton); pack(library, libraryFooter);
        auto* footer = label("Space to play / pause  ·  Minimize to keep looping", "footer"); pack(root, footer);
        g_signal_connect(playButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->toggle(); }), this);
        g_signal_connect(restartButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { auto* a = static_cast<App*>(p); a->player.seek(0); a->tick(); }), this);
        g_signal_connect(browseButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->browse(); }), this);
        g_signal_connect(pathButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->pastePath(); }), this);
        g_signal_connect(downloadButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->download(); }), this);
        g_signal_connect(youtubeEntry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer p) { static_cast<App*>(p)->download(); }), this);
        g_signal_connect(cancelButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            auto* a = static_cast<App*>(p); a->downloader.cancel();
            gtk_widget_set_sensitive(a->cancelButton, FALSE); a->setDownloadStatus("Cancelling…", -1);
        }), this);
        downloader.progress = [this](double fraction, const std::string& message) { setDownloadStatus(message, fraction); };
        downloader.completed = [this](bool ok, bool cancelled, const std::string& path, const std::string& message) {
            if (window) {
                gtk_widget_set_sensitive(youtubeEntry, TRUE);
                gtk_widget_set_sensitive(downloadButton, TRUE);
                gtk_widget_hide(cancelButton);
                if (ok) {
                    bool added = addPath(path, false);
                    if (window) {
                        setDownloadStatus(added ? "Added to your tracks · " + title(path) : "Downloaded, but the audio file could not be added.", added ? 1 : 0);
                        if (added) gtk_entry_set_text(GTK_ENTRY(youtubeEntry), "");
                    }
                } else {
                    std::string summary = "Download failed. Try again or use another link.";
                    auto pos = message.rfind("ERROR:");
                    if (pos != std::string::npos) summary = message.substr(pos+6);
                    if (cancelled) summary = message;
                    setDownloadStatus(summary, 0);
                    gtk_widget_set_tooltip_text(downloadStatus, message.c_str());
                }
            }
            g_application_release(G_APPLICATION(application));
        };
        g_signal_connect(removeButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->remove(); }), this);
        g_signal_connect(list, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer p) {
            auto* a = static_cast<App*>(p); if (!a->rebuilding && row) a->select(gtk_list_box_row_get_index(row));
        }), this);
        g_signal_connect(seekScale, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton*, gpointer p) -> gboolean { static_cast<App*>(p)->dragging = true; return FALSE; }), this);
        g_signal_connect(seekScale, "button-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton*, gpointer p) -> gboolean {
            auto* a = static_cast<App*>(p); a->dragging = false; a->player.seek(gtk_range_get_value(GTK_RANGE(a->seekScale))); return FALSE;
        }), this);
        g_signal_connect(seekScale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer p) {
            auto* a = static_cast<App*>(p); if (a->updating) return;
            auto n = gtk_range_get_value(range); gtk_label_set_text(GTK_LABEL(a->elapsed), timeText(n).c_str());
            if (!a->dragging) a->player.seek(n);
        }), this);
        g_signal_connect(volumeScale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer p) {
            auto* a = static_cast<App*>(p); a->volume = gtk_range_get_value(range)/100; a->player.volume(a->volume);
            if (a->saveTimer) g_source_remove(a->saveTimer);
            a->saveTimer = g_timeout_add(400, +[](gpointer q) -> gboolean { auto* b = static_cast<App*>(q); b->saveTimer = 0; b->save(); return G_SOURCE_REMOVE; }, a);
        }), this);
        g_signal_connect(window, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer p) -> gboolean {
            auto* a = static_cast<App*>(p);
            if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_o) { a->browse(); return TRUE; }
            if (GTK_IS_ENTRY(gtk_window_get_focus(GTK_WINDOW(a->window)))) return FALSE;
            if (event->keyval == GDK_KEY_space && !(event->state & (GDK_CONTROL_MASK|GDK_MOD1_MASK))) { a->toggle(); return TRUE; }
            return FALSE;
        }), this);
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) {
            auto* a = static_cast<App*>(p); a->player.clear(); if (a->timer) { g_source_remove(a->timer); a->timer = 0; } a->save(); a->window = nullptr; a->downloader.cancel();
        }), this);
        rebuild(); gtk_widget_show_all(window); sync();
    }
};

static int audioTest(const char* path) {
    Player p;
    auto check = [](bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } };
    check(p.init(true), "offline engine initialization");
    check(p.load(path), "streamed MP3 loading");
    const double duration = p.duration();
    check(duration > 0 && duration < 10, "short fixture length");
    check(p.play(), "start");
    std::vector<float> samples(480*2); int wraps = 0; double previous = 0; double energy = 0;
    for (int i=0; i<static_cast<int>((duration*4+1)*100); ++i) {
        check(p.render(samples.data(), 480) == MA_SUCCESS, "render");
        double position = p.position(); if (position+0.05 < previous) ++wraps; previous = position;
        for (float value : samples) { check(std::isfinite(value), "finite decoded samples"); energy += value*value; }
        g_usleep(1000); // Give the streaming worker time to refill pages in accelerated playback.
    }
    check(wraps >= 3 && energy > 1, "repeated audible loops");
    p.pause(); p.render(samples.data(), 480);
    check(std::all_of(samples.begin(), samples.end(), [](float value) { return std::abs(value) < 0.00001f; }), "pause is silent");
    p.seek(duration/2); check(p.play(), "resume after seek");
    g_usleep(20000); p.render(samples.data(), 480);
    check(std::abs(p.position()-duration/2) < 0.1, "seek position");
    check(!p.load("/nonexistent/loop-test.mp3") && p.ready(), "invalid load preserves existing stream");
    p.pause();
    std::printf("PASS: streamed MP3, %d wraps, audio output, pause, seek, resume, invalid path\n", wraps);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--test-audio") == 0) return audioTest(argv[2]);
    App app;
    app.application = gtk_application_new("io.local.loop", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app.application, "activate", G_CALLBACK(+[](GtkApplication*, gpointer p) {
        auto* a = static_cast<App*>(p); if (a->window) gtk_window_present(GTK_WINDOW(a->window)); else a->build();
    }), &app);
    g_signal_connect(app.application, "open", G_CALLBACK(+[](GApplication*, GFile** files, gint n, gchar*, gpointer p) {
        auto* a = static_cast<App*>(p); if (!a->window) a->build();
        for (int i=0; i<n; ++i) { gchar* path = g_file_get_path(files[i]); if (path) { a->addPath(path); g_free(path); } }
        gtk_window_present(GTK_WINDOW(a->window));
    }), &app);
    int result = g_application_run(G_APPLICATION(app.application), argc, argv);
    g_object_unref(app.application); return result;
}
