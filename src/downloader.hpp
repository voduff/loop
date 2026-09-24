#pragma once
#include <gio/gio.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <functional>
#include <string>
#include <vector>
#include <unistd.h>

// Lives on the GTK main thread. Reads child output asynchronously; no polling,
// worker process, or timer remains after a download finishes.
class Downloader {
    GSubprocess* process = nullptr;
    GDataInputStream* output = nullptr;
    bool eof = false, exited = false, success = false, cancelled = false;
    pid_t group = 0;
    guint killTimer = 0;
    std::string resultPath, errorTail;
    void remember(const std::string& line) {
        errorTail += line + "\n";
        if (errorTail.size() > 3000) errorTail.erase(0, errorTail.size()-3000);
    }
    static double number(const gchar* value) {
        double n = g_ascii_strtod(value, nullptr);
        return std::isfinite(n) ? n : 0;
    }
    void line(const std::string& text) {
        if (text.rfind("LOOP_FILE:", 0) == 0) { resultPath = text.substr(10); return; }
        if (cancelled) return;
        if (text.rfind("LOOP_PROGRESS|", 0) == 0) {
            gchar** parts = g_strsplit(text.c_str(), "|", 0);
            if (g_strv_length(parts) == 6) {
                double done = number(parts[1]);
                double size = number(parts[2]); if (size <= 0) size = number(parts[3]);
                double speed = number(parts[4]), eta = number(parts[5]);
                double fraction = size > 0 ? std::clamp(done/size, 0.0, 1.0) : -1;
                gchar* bytes = g_format_size(static_cast<guint64>(std::max(0.0, done)));
                std::string message = "Downloading · ";
                if (fraction >= 0) message += std::to_string(static_cast<int>(fraction*100)) + "% · ";
                message += bytes; g_free(bytes);
                if (speed > 0) {
                    gchar* rate = g_format_size(static_cast<guint64>(speed));
                    message += " · " + std::string(rate) + "/s"; g_free(rate);
                }
                if (eta > 0) message += " · " + std::to_string(static_cast<int>(eta)) + "s left";
                if (progress) progress(fraction, message);
            }
            g_strfreev(parts); return;
        }
        remember(text);
        if (text.rfind("[ExtractAudio]", 0) == 0) {
            if (progress) progress(1, "Converting to MP3…");
        } else if (text.rfind("[youtube]", 0) == 0) {
            if (progress) progress(-1, "Getting audio from YouTube…");
        }
    }
    void read() {
        g_data_input_stream_read_line_async(output, G_PRIORITY_DEFAULT, nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Downloader*>(data);
                GError* error = nullptr; gsize length = 0;
                gchar* text = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &error);
                if (text) {
                    gchar* valid = g_utf8_make_valid(text, static_cast<gssize>(length));
                    self->line(valid); g_free(valid); g_free(text); self->read();
                } else {
                    if (error) { self->remember(error->message); g_error_free(error); }
                    self->eof = true; self->finish();
                }
            }, this);
    }
    void finish() {
        if (!eof || !exited) return;
        if (killTimer) { g_source_remove(killTimer); killTimer = 0; }
        bool ok = success && !cancelled && !resultPath.empty() && g_file_test(resultPath.c_str(), G_FILE_TEST_IS_REGULAR);
        std::string message = cancelled ? "Download cancelled. Paste a link to try again." : errorTail;
        if (!ok && !cancelled && message.empty()) message = "The download did not produce an MP3. Please try again.";
        std::string path = resultPath;
        bool wasCancelled = cancelled;
        g_clear_object(&output); g_clear_object(&process); group = 0;
        if (completed) completed(ok, wasCancelled, path, message);
    }
public:
    std::function<void(double, const std::string&)> progress;
    std::function<void(bool, bool, const std::string&, const std::string&)> completed;
    bool busy() const { return process != nullptr; }
    bool start(const std::string& url, std::string& error) {
        if (busy()) { error = "A download is already running."; return false; }
        std::string base = std::string(g_get_user_data_dir()) + "/loop";
        std::string executable = base + "/downloader/bin/yt-dlp";
        if (!g_file_test(executable.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
            error = "The YouTube downloader is missing. Reinstall loop to restore it."; return false;
        }
        gchar* ffmpeg = g_find_program_in_path("ffmpeg");
        if (!ffmpeg) { error = "FFmpeg is missing. Install FFmpeg to save YouTube audio as MP3."; return false; }
        std::string destination = base + "/downloads";
        if (g_mkdir_with_parents(destination.c_str(), 0700) != 0) {
            g_free(ffmpeg); error = "Could not create the downloads folder."; return false;
        }
        std::vector<std::string> arguments = {
            executable, "--ignore-config", "--no-plugin-dirs", "--no-playlist", "--no-simulate",
            "--newline", "--no-colors", "--progress", "--progress-delta", "0.5",
            "--socket-timeout", "20", "--retries", "3", "--fragment-retries", "3",
            "--match-filters", "!is_live", "--break-on-reject",
            "--windows-filenames", "--no-overwrites", "--no-mtime",
            "-f", "bestaudio/best", "-x", "--audio-format", "mp3", "--audio-quality", "0",
            "--ffmpeg-location", ffmpeg, "--paths", destination,
            "-o", "%(title).150B [%(id)s].%(ext)s",
            "--progress-template", "download:LOOP_PROGRESS|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|%(progress.speed)s|%(progress.eta)s",
            "--print", "after_move:LOOP_FILE:%(filepath)s"
        };
        g_free(ffmpeg);
        gchar* node = g_find_program_in_path("node");
        if (node) { arguments.push_back("--js-runtimes"); arguments.push_back(std::string("node:")+node); g_free(node); }
        arguments.push_back("--"); arguments.push_back(url);
        std::vector<const gchar*> argv;
        for (const auto& argument : arguments) argv.push_back(argument.c_str());
        argv.push_back(nullptr);
        auto* launcher = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE));
        g_subprocess_launcher_setenv(launcher, "PYTHONUNBUFFERED", "1", TRUE);
        // A separate process group lets Cancel also stop a running FFmpeg conversion.
        g_subprocess_launcher_set_child_setup(launcher, +[](gpointer) { setpgid(0, 0); }, nullptr, nullptr);
        GError* spawnError = nullptr;
        process = g_subprocess_launcher_spawnv(launcher, argv.data(), &spawnError);
        g_object_unref(launcher);
        if (!process) { error = spawnError->message; g_error_free(spawnError); return false; }
        group = static_cast<pid_t>(g_ascii_strtoll(g_subprocess_get_identifier(process), nullptr, 10));
        eof = exited = success = cancelled = false; resultPath.clear(); errorTail.clear();
        output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
        g_data_input_stream_set_newline_type(output, G_DATA_STREAM_NEWLINE_TYPE_ANY);
        read();
        g_subprocess_wait_async(process, nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
            auto* self = static_cast<Downloader*>(data); GError* error = nullptr;
            if (g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error)) self->success = g_subprocess_get_successful(G_SUBPROCESS(source));
            else { self->remember(error->message); g_error_free(error); }
            self->exited = true; self->finish();
        }, this);
        return true;
    }
    void cancel() {
        if (!busy() || cancelled) return;
        cancelled = true;
        if (group > 0) kill(-group, SIGTERM);
        killTimer = g_timeout_add_seconds(2, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Downloader*>(data); self->killTimer = 0;
            if (self->group > 0) kill(-self->group, SIGKILL);
            return G_SOURCE_REMOVE;
        }, this);
    }
};
