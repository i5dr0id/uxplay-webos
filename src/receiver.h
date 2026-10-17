#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "raop.h"
struct Packet {
    bool video;
    unsigned codec;
    uint64_t timestamp;
    std::vector<uint8_t> data;
};
struct Receiver {
    ~Receiver();
    bool start(const std::string& directory);
    void stop();
    void disconnect();
    bool pop(Packet& packet, uint64_t latest = UINT64_MAX);
    void enqueue(bool video, unsigned codec, uint64_t time, const uint8_t *data, int size);
    void clear();
    std::string status();
    void show(const std::string& text);
    std::string pin();
    void adjust_audio_delay(int delta_ms);
    std::atomic<int> audio_delay_ms{100};
    std::atomic<bool> reset{false}, audio_flush{false}, paused{false}, has_audio{false}, mirror{false}, pin_active{false};
    std::atomic<int> width{1280},height{720};
    std::atomic<float> volume{1};
    std::atomic<int64_t> activity{0};
    std::atomic<int64_t> last_audio_arrival{0};
    std::atomic<unsigned> overflow{0};
    raop_t *raop = nullptr;
    dnssd_t *dnssd = nullptr;
    std::mutex mutex;
    std::deque<Packet> packets;
    size_t bytes=0;
    std::string directory, message="Waiting for a connection", pin_text, client;
    std::set<std::string> keys;
};
