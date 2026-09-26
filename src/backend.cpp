#include "player.hpp"
#include <glib.h>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <unistd.h>
#include <vector>

static std::string quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    return out+'"';
}
static std::string decode(const std::string& value) {
    gsize size = 0; auto* bytes = g_base64_decode(value.c_str(), &size);
    std::string out(reinterpret_cast<char*>(bytes), size); g_free(bytes); return out;
}
static void exportSettings(const char* path) {
    auto* key = g_key_file_new();
    if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, nullptr)) { std::cout << "{}\n"; g_key_file_unref(key); return; }
    gsize count = 0; auto** groups = g_key_file_get_groups(key, &count);
    std::cout << '{';
    for (gsize i=0; i<count; ++i) {
        if (i) std::cout << ',';
        std::cout << quote(groups[i]) << ":{";
        gsize n = 0; auto** keys = g_key_file_get_keys(key, groups[i], &n, nullptr);
        for (gsize j=0; j<n; ++j) {
            if (j) std::cout << ',';
            std::cout << quote(keys[j]) << ':';
            std::string name = keys[j];
            if (name == "paths" || name == "queue" || name == "tracks") {
                gsize size = 0; auto** values = g_key_file_get_string_list(key, groups[i], keys[j], &size, nullptr);
                std::cout << '[';
                for (gsize k=0; k<size; ++k) { if (k) std::cout << ','; std::cout << quote(values[k]); }
                std::cout << ']'; g_strfreev(values);
            } else {
                auto* value = g_key_file_get_string(key, groups[i], keys[j], nullptr);
                std::cout << quote(value ? value : ""); g_free(value);
            }
        }
        std::cout << '}'; g_strfreev(keys);
    }
    std::cout << "}\n"; g_strfreev(groups); g_key_file_unref(key);
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--export-settings") { exportSettings(argv[2]); return 0; }
    Player player;
    unsigned generation = 0;
    auto state = [&](const std::string& id, bool ok, const std::string& event = "state", const std::string& error = "") {
        std::cout << "{\"id\":" << id << ",\"event\":" << quote(event)
            << ",\"ok\":" << (ok ? "true" : "false") << ",\"playing\":" << (player.playing ? "true" : "false")
            << ",\"ready\":" << (player.ready() ? "true" : "false") << ",\"generation\":" << generation << ",\"position\":" << player.position()
            << ",\"duration\":" << player.duration() << ",\"error\":" << quote(error) << "}" << std::endl;
    };
    std::string buffer;
    auto nextUpdate = std::chrono::steady_clock::now();
    while (true) {
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        int result = poll(&descriptor, 1, player.playing ? 200 : -1);
        if (result < 0) { if (errno == EINTR) continue; break; }
        if (descriptor.revents & (POLLIN | POLLHUP)) {
            char input[8192]; ssize_t n = read(STDIN_FILENO, input, sizeof(input));
            if (n <= 0) break;
            buffer.append(input, n);
            if (buffer.size() > 1024*1024) break;
            size_t end;
            while ((end = buffer.find('\n')) != std::string::npos) {
                auto line = buffer.substr(0, end); buffer.erase(0, end+1);
                std::istringstream stream(line); std::string id, command, value;
                std::getline(stream, id, '\t'); std::getline(stream, command, '\t'); std::getline(stream, value);
                if (id.empty() || !std::all_of(id.begin(), id.end(), [](char c){ return c >= '0' && c <= '9'; })) continue;
                bool ok = true; std::string error;
                if (command == "load") { ok = player.load(decode(value)); if (ok) ++generation; }
                else if (command == "play") ok = player.play();
                else if (command == "pause") player.pause();
                else if (command == "clear") { player.clear(); ++generation; }
                else if (command == "loop") player.looping(value == "1");
                else if (command == "seek" || command == "volume") {
                    double v = g_ascii_strtod(value.c_str(), nullptr);
                    if (!std::isfinite(v)) ok = false;
                    else if (command == "volume") player.volume(std::clamp(v, 0.0, 1.0));
                    else player.seek(std::clamp(v, 0.0, static_cast<double>(player.duration())));
                } else if (command == "validate") {
                    ma_decoder decoder{}; auto cfg = ma_decoder_config_init_default();
                    ok = ma_decoder_init_file(decode(value).c_str(), &cfg, &decoder) == MA_SUCCESS;
                    if (ok) ma_decoder_uninit(&decoder); else error = "This file could not be decoded.";
                } else if (command == "quit") { player.clear(); return 0; }
                else if (command != "status") { ok = false; error = "Unknown audio command."; }
                if (!ok && error.empty()) error = player.error.empty() ? "Audio operation failed." : player.error;
                state(id, ok, "state", error);
            }
        }
        if (player.playing && player.ended()) { player.pause(); state("0", true, "ended"); }
        auto now = std::chrono::steady_clock::now();
        if (player.playing && now >= nextUpdate) { state("0", true); nextUpdate = now+std::chrono::seconds(1); }
    }
}
