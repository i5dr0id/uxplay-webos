#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
class StarfishMediaAPIs;
enum class FeedResult { Accepted, Retry, Error };
class Media {
public:
    Media();
    ~Media();
    bool open(const char *app_id, int width, int height, bool audio, int sample_rate = 44100);
    FeedResult feed(const void *data, size_t size, int64_t pts_ns, bool video);
    void tick();
    void eos();
    void pause(bool paused);
    void close();
    std::atomic<bool> ready{false}, playing{false}, ended{false}, failed{false};
    std::atomic<unsigned> frames{0};
private:
    static void event(int type, int64_t value, const char *message, void *user);
    StarfishMediaAPIs *api_ = nullptr;
    char window_id_[64] = {};
    int display_w_ = 0, display_h_ = 0;
    bool loaded_ = false, play_requested_ = false;
    std::atomic<bool> video_info_pending_{false};
    struct Impl;
    Impl *impl_;
};
