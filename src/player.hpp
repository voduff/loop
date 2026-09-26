#pragma once
#include "miniaudio.h"
#include <memory>
#include <string>

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
