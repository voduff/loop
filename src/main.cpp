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
    bool ended() const { return sound && ma_sound_at_end(sound.get()); }
    void looping(bool enabled) { if (sound) ma_sound_set_looping(sound.get(), enabled ? MA_TRUE : MA_FALSE); }
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

struct Preset {
    std::string name;
    std::vector<std::string> tracks;
    std::string selectedPath;
    bool multiple = true;
    double volume = 0.65;
};
static std::vector<std::string> readPaths(GKeyFile* key, const char* group, const char* field) {
    gsize size = 0; gchar** values = g_key_file_get_string_list(key, group, field, &size, nullptr);
    std::vector<std::string> result;
    for (gsize i = 0; i < size; ++i) result.emplace_back(values[i]);
    g_strfreev(values); return result;
}
static void writePaths(GKeyFile* key, const char* group, const char* field, const std::vector<std::string>& values) {
    std::vector<const gchar*> pointers;
    for (const auto& value : values) pointers.push_back(value.c_str());
    g_key_file_set_string_list(key, group, field, pointers.data(), pointers.size());
}
static std::string readString(GKeyFile* key, const char* group, const char* field) {
    gchar* value = g_key_file_get_string(key, group, field, nullptr);
    std::string result = value ? value : ""; g_free(value); return result;
}
static std::string fold(const std::string& text) {
    gchar* folded = g_utf8_casefold(text.c_str(), -1);
    gchar* normalized = g_utf8_normalize(folded, -1, G_NORMALIZE_ALL_COMPOSE);
    std::string result = normalized ? normalized : folded;
    g_free(normalized); g_free(folded); return result;
}

struct App {
    GtkApplication* application = nullptr;
    GtkWidget *window = nullptr, *list = nullptr, *trackTitle = nullptr, *trackPath = nullptr;
    GtkWidget *playButton = nullptr, *playImage = nullptr, *playText = nullptr, *status = nullptr;
    GtkWidget *seekScale = nullptr, *elapsed = nullptr, *total = nullptr, *volumeScale = nullptr;
    GtkWidget *removeButton = nullptr, *restartButton = nullptr, *empty = nullptr, *count = nullptr;
    GtkWidget *youtubeEntry = nullptr, *downloadButton = nullptr, *cancelButton = nullptr;
    GtkWidget *downloadStatus = nullptr, *downloadProgress = nullptr;
    GtkWidget *searchEntry = nullptr, *modeCombo = nullptr, *presetCombo = nullptr;
    GtkWidget *nextButton = nullptr, *queueCount = nullptr, *presetDelete = nullptr, *libraryNotice = nullptr;
    Player player;
    Downloader downloader;
    std::vector<std::string> paths;
    std::vector<std::string> queue;
    std::vector<Preset> presets;
    std::string searchQuery, activePreset;
    bool multiple = false, refreshingPresets = false;
    gint64 lastDisplayUpdate = 0;
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
            queue = readPaths(key, "loop", "queue");
            multiple = g_key_file_get_boolean(key, "loop", "multiple", nullptr);
            activePreset = readString(key, "loop", "preset");
            gsize groupCount = 0; gchar** groups = g_key_file_get_groups(key, &groupCount);
            for (gsize i = 0; i < groupCount; ++i) {
                if (!g_str_has_prefix(groups[i], "preset-")) continue;
                Preset preset;
                preset.name = readString(key, groups[i], "name");
                preset.tracks = readPaths(key, groups[i], "tracks");
                preset.selectedPath = readString(key, groups[i], "selected");
                preset.multiple = g_key_file_get_boolean(key, groups[i], "multiple", nullptr);
                preset.volume = g_key_file_get_double(key, groups[i], "volume", nullptr);
                if (!std::isfinite(preset.volume)) preset.volume = .65;
                preset.volume = std::clamp(preset.volume, 0.0, 1.0);
                if (!preset.name.empty()) presets.push_back(std::move(preset));
            }
            g_strfreev(groups);
        } else {
            paths.push_back(std::string(g_get_user_data_dir()) + "/loop/what it feels like to be a memory (playlist).mp3");
            selected = 0;
        }
        if (!std::isfinite(volume)) volume = .65;
        volume = std::clamp(volume, 0.0, 1.0);
        selected = paths.empty() ? -1 : std::clamp(selected, 0, static_cast<int>(paths.size())-1);
        if (!g_key_file_has_key(key, "loop", "queue", nullptr) && selected >= 0) queue.push_back(paths[selected]);
        queue.erase(std::remove_if(queue.begin(), queue.end(), [this](const std::string& p) {
            return std::find(paths.begin(), paths.end(), p) == paths.end();
        }), queue.end());
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
        writePaths(key, "loop", "queue", queue);
        g_key_file_set_boolean(key, "loop", "multiple", multiple);
        g_key_file_set_string(key, "loop", "preset", activePreset.c_str());
        for (size_t i = 0; i < presets.size(); ++i) {
            const auto& preset = presets[i]; auto group = "preset-" + std::to_string(i);
            g_key_file_set_string(key, group.c_str(), "name", preset.name.c_str());
            writePaths(key, group.c_str(), "tracks", preset.tracks);
            g_key_file_set_string(key, group.c_str(), "selected", preset.selectedPath.c_str());
            g_key_file_set_boolean(key, group.c_str(), "multiple", preset.multiple);
            g_key_file_set_double(key, group.c_str(), "volume", preset.volume);
        }
        GError* error = nullptr;
        if (!g_key_file_save_to_file(key, configPath.c_str(), &error)) { g_warning("Settings: %s", error->message); g_error_free(error); }
        g_key_file_unref(key);
    }
    void report(const std::string& message) {
        auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    }
    bool queued(const std::string& path) const { return std::find(queue.begin(), queue.end(), path) != queue.end(); }
    void loopPolicy() {
        player.looping(!multiple || (queue.size() == 1 && selected >= 0 && queue.front() == paths[selected]));
    }
    void notice(const std::string& message) {
        gtk_label_set_text(GTK_LABEL(libraryNotice), message.c_str());
        gtk_widget_set_tooltip_text(libraryNotice, message.c_str());
        gtk_widget_set_visible(libraryNotice, !message.empty());
    }
    void refreshPresets() {
        refreshingPresets = true;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(presetCombo));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(presetCombo), "Custom selection");
        int active = 0;
        for (size_t i = 0; i < presets.size(); ++i) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(presetCombo), presets[i].name.c_str());
            if (presets[i].name == activePreset) active = static_cast<int>(i)+1;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(presetCombo), active);
        gtk_widget_set_sensitive(presetDelete, active > 0);
        refreshingPresets = false;
    }
    void custom() { activePreset.clear(); refreshPresets(); }
    void refreshQueue() {
        rebuilding = true;
        auto* rows = gtk_container_get_children(GTK_CONTAINER(list));
        for (auto* item = rows; item; item = item->next) {
            auto* row = GTK_WIDGET(item->data);
            int index = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row));
            auto found = std::find(queue.begin(), queue.end(), paths[index]);
            auto* check = GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(row), "queue-check"));
            auto* order = GTK_LABEL(g_object_get_data(G_OBJECT(row), "queue-order"));
            gtk_toggle_button_set_active(check, found != queue.end());
            std::string text = found == queue.end() ? "MP3" : "#" + std::to_string(found-queue.begin()+1);
            gtk_label_set_text(order, text.c_str());
        }
        g_list_free(rows); rebuilding = false;
        auto description = std::to_string(queue.size()) + " in loop · check tracks in play order";
        gtk_label_set_text(GTK_LABEL(queueCount), description.c_str());
        loopPolicy();
    }
    void filter() {
        searchQuery = fold(gtk_entry_get_text(GTK_ENTRY(searchEntry)));
        gtk_list_box_invalidate_filter(GTK_LIST_BOX(list));
        size_t visible = 0;
        for (const auto& path : paths) if (fold(displayPath(path)).find(searchQuery) != std::string::npos) ++visible;
        gtk_label_set_text(GTK_LABEL(empty), paths.empty() ? "Nothing here yet. Add your first MP3 above." : "No tracks match your search.");
        gtk_widget_set_visible(empty, visible == 0);
        auto description = std::to_string(paths.size()) + (paths.size() == 1 ? " saved track" : " saved tracks");
        if (!searchQuery.empty()) description = std::to_string(visible) + " of " + description;
        gtk_label_set_text(GTK_LABEL(count), description.c_str());
    }
    void playQueue(size_t start, bool resume) {
        player.clear();
        size_t skipped = 0;
        for (size_t offset = 0; offset < queue.size(); ++offset) {
            const auto& path = queue[(start+offset)%queue.size()];
            auto found = std::find(paths.begin(), paths.end(), path);
            if (found == paths.end() || !player.load(path)) { ++skipped; continue; }
            selected = static_cast<int>(found-paths.begin());
            loopPolicy(); player.volume(volume);
            if (resume && !player.play()) {
                notice(player.error); save(); rebuild(); return;
            }
            notice(skipped ? "Skipped " + std::to_string(skipped) + " unavailable track(s)." : "");
            save(); rebuild(); return;
        }
        notice(queue.empty() ? "Check some tracks to build your loop." : "No playable tracks in this loop. Check the file paths.");
        save(); rebuild();
    }
    void next(bool resume) {
        size_t index = 0;
        if (selected >= 0) {
            auto found = std::find(queue.begin(), queue.end(), paths[selected]);
            if (found != queue.end()) index = static_cast<size_t>(found-queue.begin()+1);
        }
        playQueue(index, resume);
    }
    void setMode(bool enabled) {
        multiple = enabled; custom();
        if (multiple && queue.empty() && selected >= 0) queue.push_back(paths[selected]);
        if (multiple && player.playing && selected >= 0 && !queued(paths[selected])) { playQueue(0, true); }
        refreshQueue(); save(); sync();
    }
    void savePreset() {
        if (selected < 0 || (multiple && queue.empty())) { notice("Choose tracks before saving a preset."); return; }
        auto* dialog = gtk_dialog_new_with_buttons("Save preset", GTK_WINDOW(window), GTK_DIALOG_MODAL,
            "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr);
        auto* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), 20); gtk_box_set_spacing(GTK_BOX(content), 12);
        pack(content, label("Save track order, loop mode, and volume.", "muted"));
        auto* entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "e.g. Deep focus");
        gtk_entry_set_text(GTK_ENTRY(entry), activePreset.c_str());
        gtk_entry_set_max_length(GTK_ENTRY(entry), 80); gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        pack(content, entry); pack(content, label("An existing name replaces that preset.", "footer"));
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); gtk_widget_show_all(dialog);
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            gchar* raw = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))); std::string name = g_strstrip(raw); g_free(raw);
            if (!name.empty()) {
                Preset preset{name, queue, paths[selected], multiple, volume};
                auto found = std::find_if(presets.begin(), presets.end(), [&](const Preset& p) { return p.name == name; });
                if (found == presets.end()) presets.push_back(std::move(preset)); else *found = std::move(preset);
                activePreset = name; refreshPresets(); save(); notice("Preset saved · " + name);
            } else notice("Enter a name to save a preset.");
        }
        gtk_widget_destroy(dialog);
    }
    void loadPreset(int index) {
        if (index < 0 || index >= static_cast<int>(presets.size())) return;
        const auto preset = presets[index]; bool resume = player.playing;
        player.clear(); queue.clear();
        for (const auto& path : preset.tracks) if (std::find(paths.begin(), paths.end(), path) != paths.end() && !queued(path)) queue.push_back(path);
        multiple = preset.multiple; volume = preset.volume; activePreset = preset.name;
        auto found = std::find(paths.begin(), paths.end(), preset.selectedPath);
        selected = found == paths.end() ? -1 : static_cast<int>(found-paths.begin());
        refreshingPresets = true;
        gtk_combo_box_set_active(GTK_COMBO_BOX(modeCombo), multiple ? 1 : 0);
        gtk_range_set_value(GTK_RANGE(volumeScale), volume*100);
        refreshingPresets = false;
        refreshPresets();
        notice("");
        gtk_entry_set_text(GTK_ENTRY(searchEntry), "");
        if (multiple) playQueue(0, resume);
        else {
            if (selected < 0) notice("This preset's track is no longer in the library. Choose another track or preset.");
            if (resume && selected >= 0) {
                if (player.load(paths[selected])) { loopPolicy(); player.volume(volume); if (!player.play()) notice(player.error); }
                else notice(player.error);
            }
            rebuild(); save();
        }
    }
    void deletePreset() {
        int index = gtk_combo_box_get_active(GTK_COMBO_BOX(presetCombo))-1;
        if (index < 0 || index >= static_cast<int>(presets.size())) return;
        presets.erase(presets.begin()+index); custom(); save(); notice("Preset removed. Your tracks are still in the library.");
    }
    void sync() {
        bool valid = selected >= 0 && selected < static_cast<int>(paths.size());
        gtk_label_set_text(GTK_LABEL(trackTitle), valid ? title(paths[selected]).c_str() : "Make yourself a loop");
        gtk_label_set_text(GTK_LABEL(trackPath), valid ? displayPath(paths[selected]).c_str() : "Add an MP3 to get started.");
        gtk_widget_set_tooltip_text(trackPath, valid ? displayPath(paths[selected]).c_str() : nullptr);
        gtk_label_set_text(GTK_LABEL(status), player.playing ? (multiple ? "LOOPING SELECTED TRACKS" : "PLAYING ON REPEAT") : (valid ? "READY WHEN YOU ARE" : "YOUR SOUND. ON REPEAT."));
        gtk_label_set_text(GTK_LABEL(playText), player.playing ? "Pause" : "Play");
        gtk_image_set_from_icon_name(GTK_IMAGE(playImage), player.playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_sensitive(playButton, player.playing || (multiple ? !queue.empty() : valid));
        gtk_widget_set_sensitive(nextButton, multiple && !queue.empty());
        gtk_widget_set_sensitive(removeButton, valid);
        gtk_widget_set_sensitive(restartButton, player.ready());
        filter();
        tick(true);
        if (player.playing && !timer) timer = g_timeout_add(250, [](gpointer data) -> gboolean { static_cast<App*>(data)->tick(); return G_SOURCE_CONTINUE; }, this);
        if (!player.playing && timer) { g_source_remove(timer); timer = 0; }
    }
    void tick(bool force = false) {
        if (multiple && player.playing && player.ended()) { next(true); return; }
        if (dragging) return;
        auto now = g_get_monotonic_time();
        if (!force && now-lastDisplayUpdate < G_USEC_PER_SEC) return;
        lastDisplayUpdate = now;
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
        if (selected < 0) { if (multiple) playQueue(0, true); return; }
        if (player.playing) player.pause();
        else {
            if (multiple && (queue.empty() || !queued(paths[selected]))) { playQueue(0, true); return; }
            if (!player.ready() && !player.load(paths[selected])) { report(player.error); return; }
            loopPolicy();
            player.volume(volume);
            if (!player.play()) report(player.error);
        }
        sync();
    }
    void select(int index) {
        if (index == selected || index < 0 || index >= static_cast<int>(paths.size())) return;
        bool resume = player.playing;
        player.clear(); selected = index;
        custom();
        if (multiple && !queued(paths[selected])) queue.push_back(paths[selected]);
        if (resume) {
            if (player.load(paths[selected])) { loopPolicy(); player.volume(volume); if (!player.play()) report(player.error); }
            else report(player.error);
        }
        refreshQueue(); save(); sync();
    }
    void rebuild() {
        rebuilding = true;
        auto* children = gtk_container_get_children(GTK_CONTAINER(list));
        for (auto* p = children; p; p = p->next) gtk_widget_destroy(GTK_WIDGET(p->data));
        g_list_free(children);
        for (const auto& path : paths) {
            auto* row = gtk_list_box_row_new(); auto* line = box(GTK_ORIENTATION_HORIZONTAL, 14);
            gtk_container_set_border_width(GTK_CONTAINER(line), 12);
            auto* check = gtk_check_button_new();
            gtk_widget_set_tooltip_text(check, "Include in multi-track loop (checked order)");
            atk_object_set_name(gtk_widget_get_accessible(check), ("Include " + title(path) + " in loop").c_str());
            g_object_set_data(G_OBJECT(check), "index", GINT_TO_POINTER(static_cast<int>(&path-paths.data())+1));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), queued(path));
            pack(line, check); g_object_set_data(G_OBJECT(row), "queue-check", check);
            g_signal_connect(check, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                auto* a = static_cast<App*>(data); if (a->rebuilding) return;
                int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "index"))-1;
                const auto& path = a->paths[index];
                if (gtk_toggle_button_get_active(button)) { if (!a->queued(path)) a->queue.push_back(path); }
                else a->queue.erase(std::remove(a->queue.begin(), a->queue.end(), path), a->queue.end());
                if (a->multiple && a->queue.empty()) a->player.pause();
                a->custom(); a->refreshQueue(); a->save(); a->sync();
            }), this);
            auto* music = gtk_image_new_from_icon_name("audio-x-generic-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
            cssClass(music, "muted"); pack(line, music);
            auto* texts = box(GTK_ORIENTATION_VERTICAL, 4);
            auto* name = label(title(path).c_str(), "track-name");
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            auto* location = label(displayPath(path).c_str(), "path");
            gtk_label_set_ellipsize(GTK_LABEL(location), PANGO_ELLIPSIZE_MIDDLE);
            gtk_widget_set_tooltip_text(row, displayPath(path).c_str());
            pack(texts, name); pack(texts, location); pack(line, texts, true);
            auto* order = label("MP3", "format"); pack(line, order);
            g_object_set_data(G_OBJECT(row), "queue-order", order);
            g_object_set_data_full(G_OBJECT(row), "search-text", g_strdup(fold(displayPath(path)).c_str()), g_free);
            gtk_container_add(GTK_CONTAINER(row), line); gtk_container_add(GTK_CONTAINER(list), row);
        }
        gtk_widget_show_all(list);
        if (selected >= 0) gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), selected));
        rebuilding = false; refreshQueue(); sync();
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
        const auto removed = paths[selected];
        queue.erase(std::remove(queue.begin(), queue.end(), removed), queue.end());
        for (auto& preset : presets) {
            preset.tracks.erase(std::remove(preset.tracks.begin(), preset.tracks.end(), removed), preset.tracks.end());
            if (preset.selectedPath == removed) preset.selectedPath.clear();
        }
        custom();
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
            .card { background: #fafafa; border: 1px solid #262626; border-radius: 14px; padding: 20px; }
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
            checkbutton check { background: #ffffff; border: 1px solid #aaaaaa; border-radius: 5px; min-width: 17px; min-height: 17px; }
            checkbutton check:checked { background: #191919; color: #ffffff; border-color: #191919; }
            combobox box.linked button { border-radius: 10px; }
        )CSS";
        gtk_css_provider_load_from_data(provider, css, -1, nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        window = gtk_application_window_new(application);
        cssClass(window, "loop-window");
        if (auto* visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window))) gtk_widget_set_visual(window, visual);
        gtk_window_set_title(GTK_WINDOW(window), "loop");
        gtk_window_set_icon_name(GTK_WINDOW(window), "io.local.loop");
        gtk_window_set_default_size(GTK_WINDOW(window), 700, 850);
        auto* header = gtk_header_bar_new(); gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header), "loop"); gtk_window_set_titlebar(GTK_WINDOW(window), header);
        // Paint the full content area; keep padding on a separate inner widget.
        // GtkContainer border-width sits outside that widget's CSS background.
        auto* body = box(GTK_ORIENTATION_VERTICAL, 0);
        cssClass(body, "app-body");
        gtk_container_add(GTK_CONTAINER(window), body);
        auto* root = box(GTK_ORIENTATION_VERTICAL, 18);
        gtk_container_set_border_width(GTK_CONTAINER(root), 24);
        pack(body, root, true);
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
        nextButton = iconButton("media-skip-forward-symbolic", "Next track in loop"); pack(controls, nextButton);
        auto* spacer = box(GTK_ORIENTATION_HORIZONTAL, 0); pack(controls, spacer, true);
        auto* volumeIcon = gtk_image_new_from_icon_name("audio-volume-medium-symbolic", GTK_ICON_SIZE_BUTTON); cssClass(volumeIcon, "muted"); pack(controls, volumeIcon);
        volumeScale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        gtk_scale_set_draw_value(GTK_SCALE(volumeScale), FALSE); gtk_range_set_value(GTK_RANGE(volumeScale), volume*100);
        gtk_widget_set_size_request(volumeScale, 100, -1); gtk_widget_set_tooltip_text(volumeScale, "Volume");
        atk_object_set_name(gtk_widget_get_accessible(volumeScale), "Volume"); pack(controls, volumeScale); pack(card, controls);
        auto* library = box(GTK_ORIENTATION_VERTICAL, 10); pack(root, library, true);
        auto* youtube = box(GTK_ORIENTATION_VERTICAL, 8);
        auto* youtubeExpander = gtk_expander_new(nullptr);
        gtk_expander_set_label_widget(GTK_EXPANDER(youtubeExpander), label("Add from YouTube", "section"));
        gtk_container_add(GTK_CONTAINER(youtubeExpander), youtube);
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
        pack(youtube, downloadStatus); pack(library, youtubeExpander);
        auto* divider = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL); pack(library, divider);
        auto* libraryHeader = box(GTK_ORIENTATION_HORIZONTAL, 8); pack(libraryHeader, label("Your tracks", "section"), true);
        auto* pathButton = gtk_button_new_with_label("Paste path"); auto* browseButton = gtk_button_new_with_label("+ Add MP3");
        pack(libraryHeader, pathButton); pack(libraryHeader, browseButton); pack(library, libraryHeader);
        searchEntry = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), "Search tracks or file paths…");
        atk_object_set_name(gtk_widget_get_accessible(searchEntry), "Search library"); pack(library, searchEntry);
        auto* modeRow = box(GTK_ORIENTATION_HORIZONTAL, 12);
        modeCombo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(modeCombo), "Loop one track");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(modeCombo), "Loop checked tracks");
        gtk_combo_box_set_active(GTK_COMBO_BOX(modeCombo), multiple ? 1 : 0);
        atk_object_set_name(gtk_widget_get_accessible(modeCombo), "Loop mode"); pack(modeRow, modeCombo);
        queueCount = label("", "footer"); gtk_label_set_ellipsize(GTK_LABEL(queueCount), PANGO_ELLIPSIZE_END);
        pack(modeRow, queueCount, true); pack(library, modeRow);
        auto* presetRow = box(GTK_ORIENTATION_HORIZONTAL, 8); pack(presetRow, label("Preset", "muted"));
        presetCombo = gtk_combo_box_text_new();
        atk_object_set_name(gtk_widget_get_accessible(presetCombo), "Saved presets");
        gtk_widget_set_hexpand(presetCombo, TRUE); pack(presetRow, presetCombo, true);
        // Long user-defined names must not force the window beyond the screen.
        auto* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(presetCombo));
        for (auto* cell = cells; cell; cell = cell->next) {
            if (GTK_IS_CELL_RENDERER_TEXT(cell->data)) g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 24, nullptr);
        }
        g_list_free(cells);
        auto* presetSave = gtk_button_new_with_label("Save preset…"); pack(presetRow, presetSave);
        presetDelete = iconButton("edit-delete-symbolic", "Delete this preset (keeps all audio files)"); pack(presetRow, presetDelete); pack(library, presetRow);
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, -1, 115); list = gtk_list_box_new();
        gtk_list_box_set_filter_func(GTK_LIST_BOX(list), +[](GtkListBoxRow* row, gpointer data) -> gboolean {
            auto* a = static_cast<App*>(data);
            const char* text = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "search-text"));
            return a->searchQuery.empty() || (text && std::strstr(text, a->searchQuery.c_str()));
        }, this, nullptr);
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE); gtk_container_add(GTK_CONTAINER(scroll), list); pack(library, scroll, true);
        empty = label("Nothing here yet. Add your first MP3 above.", "muted"); pack(library, empty);
        libraryNotice = label("", "footer");
        gtk_label_set_ellipsize(GTK_LABEL(libraryNotice), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(libraryNotice), 1);
        gtk_widget_set_no_show_all(libraryNotice, TRUE); pack(library, libraryNotice);
        auto* libraryFooter = box(GTK_ORIENTATION_HORIZONTAL, 8); count = label("", "footer"); pack(libraryFooter, count, true);
        removeButton = iconButton("list-remove-symbolic", "Remove selected path from this list (keeps the file)"); pack(libraryFooter, removeButton); pack(library, libraryFooter);
        auto* footer = label("Space to play / pause  ·  Ctrl+F to search  ·  Minimize to keep looping", "footer"); pack(root, footer);
        g_signal_connect(playButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->toggle(); }), this);
        g_signal_connect(restartButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { auto* a = static_cast<App*>(p); a->player.seek(0); a->tick(true); }), this);
        g_signal_connect(nextButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { auto* a = static_cast<App*>(p); a->next(a->player.playing); }), this);
        g_signal_connect(searchEntry, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) { static_cast<App*>(p)->filter(); }), this);
        g_signal_connect(modeCombo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer p) {
            auto* a = static_cast<App*>(p); if (!a->refreshingPresets) a->setMode(gtk_combo_box_get_active(combo) == 1);
        }), this);
        g_signal_connect(presetCombo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer p) {
            auto* a = static_cast<App*>(p); if (a->refreshingPresets) return;
            int index = gtk_combo_box_get_active(combo)-1;
            if (index >= 0) a->loadPreset(index); else { a->activePreset.clear(); gtk_widget_set_sensitive(a->presetDelete, FALSE); a->save(); }
        }), this);
        g_signal_connect(presetSave, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->savePreset(); }), this);
        g_signal_connect(presetDelete, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<App*>(p)->deletePreset(); }), this);
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
            if (!a->refreshingPresets) a->custom();
            if (a->saveTimer) g_source_remove(a->saveTimer);
            a->saveTimer = g_timeout_add(400, +[](gpointer q) -> gboolean { auto* b = static_cast<App*>(q); b->saveTimer = 0; b->save(); return G_SOURCE_REMOVE; }, a);
        }), this);
        g_signal_connect(window, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer p) -> gboolean {
            auto* a = static_cast<App*>(p);
            if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_o) { a->browse(); return TRUE; }
            if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_f) { gtk_widget_grab_focus(a->searchEntry); return TRUE; }
            if (GTK_IS_ENTRY(gtk_window_get_focus(GTK_WINDOW(a->window)))) return FALSE;
            if (GTK_IS_TOGGLE_BUTTON(gtk_window_get_focus(GTK_WINDOW(a->window)))) return FALSE;
            if (event->keyval == GDK_KEY_space && !(event->state & (GDK_CONTROL_MASK|GDK_MOD1_MASK))) { a->toggle(); return TRUE; }
            return FALSE;
        }), this);
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) {
            auto* a = static_cast<App*>(p); a->player.clear(); if (a->timer) { g_source_remove(a->timer); a->timer = 0; } a->save(); a->window = nullptr; a->downloader.cancel();
        }), this);
        refreshPresets(); rebuild(); gtk_widget_show_all(window); sync();
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
