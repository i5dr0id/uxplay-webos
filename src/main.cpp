#include "audio.h"
#include "bridge.h"
#include "h264.h"
#include "media.h"
#include "receiver.h"
#include "shell.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>

static int bridge_connect() {
    struct stat st;
    if (stat(BRIDGE_SOCK, &st) != 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, BRIDGE_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

static int run_bridge(Shell &shell) {
    int sock = bridge_connect();
    if (sock < 0) return -1;
    fprintf(stderr, "[bridge] connected to daemon\n");

    const char *app_id = getenv("APPID");
    if (!app_id) app_id = "com.github.danlflh.uxplaywebos";
    Media media; AudioDecoder decoder;
    bool opened = false, last_paused = false, audio_started = false;
    int width = 1280, height = 720;
    std::vector<uint8_t> configuration;
    uint64_t origin = 0;
    int64_t audio_end = 0, start = 0, last_stats = 0, stall = 0;
    unsigned video_count = 0, audio_count = 0, decode_errors = 0, late_audio = 0, silence_blocks = 0, video_drops = 0;
    std::vector<uint8_t> silence(441*4, 0);
    float volume = 1;
    int64_t last_audio_arrival = 0;

    auto finish = [&](){
        media.close(); decoder.reset(); opened = false;
        origin = 0; start = 0; stall = 0; audio_end = 0; configuration.clear();
        last_paused = false; audio_started = false;
    };

    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    shell.transparent();

    while (true) {
        int64_t now = now_ns();
        Action action = shell.action();
        if (action == Action::Exit || action == Action::Back) break;

        BridgeHeader h;
        bool got_msg = false;
        struct pollfd pfd = {sock, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) & ~O_NONBLOCK);
            if (!bridge_read_all(sock, &h, sizeof(h))) break;

            if (h.type == MSG_END) { fprintf(stderr, "[bridge] session ended\n"); finish(); break; }

            if (h.type == MSG_START) {
                int32_t dims[2];
                if (!bridge_read_all(sock, dims, 8)) break;
                width = dims[0]; height = dims[1];
                fprintf(stderr, "[bridge] start %dx%d\n", width, height);
            } else if (h.type == MSG_STATE) {
                float vol;
                if (h.size >= 4 && !bridge_read_all(sock, &vol, 4)) break;
                bool paused = h.flags & 1;
                if (h.flags & 2) decoder.reset();
                volume = vol;
                if (opened && paused != last_paused) { media.pause(paused); last_paused = paused; }
            } else if (h.type == MSG_PACKET) {
                Packet pkt;
                pkt.video = h.flags & 1;
                pkt.codec = h.codec;
                pkt.timestamp = h.timestamp;
                pkt.data.resize(h.size);
                if (!bridge_read_all(sock, pkt.data.data(), h.size)) break;

                if (!opened) {
                    if (!pkt.video) goto next;
                    auto info = inspect_h264(pkt.data.data(), pkt.data.size());
                    if (!info.valid) goto next;
                    if (!info.configuration.empty()) configuration = info.configuration;
                    if (info.width) { width = info.width; height = info.height; }
                    if (!info.keyframe || configuration.empty()) goto next;
                    if (!media.open(app_id, width, height, true)) {
                        fprintf(stderr, "[bridge] could not open decoder\n"); break;
                    }
                    opened = true; origin = pkt.timestamp; start = now + 50000000; stall = 0;
                    video_count = audio_count = decode_errors = late_audio = silence_blocks = video_drops = 0;
                    fprintf(stderr, "[bridge] decoder opened %dx%d\n", width, height);
                }

                {
                    int64_t pts = pkt.timestamp >= origin ? int64_t(pkt.timestamp - origin) : -int64_t(origin - pkt.timestamp);
                    if (pts < 0) goto next;
                    if (pkt.video && now > start + pts + 100000000) { ++video_drops; goto next; }

                    if (pkt.video && pkt.codec == 0) {
                        auto info = inspect_h264(pkt.data.data(), pkt.data.size());
                        if (!info.valid) goto next;
                        if (!info.configuration.empty()) configuration = info.configuration;
                        if (info.width && (width != info.width || height != info.height)) {
                            media.close(); width = info.width; height = info.height;
                            if (!info.keyframe || !media.open(app_id, width, height, true)) { finish(); break; }
                            origin = pkt.timestamp; start = now; pts = 0; audio_end = 0; audio_started = false; decoder.reset();
                        }
                        pkt.codec = 1;
                    }
                    if (!pkt.video && pkt.codec != 0) {
                        std::vector<uint8_t> pcm;
                        if (!decoder.decode(pkt.codec, pkt.data.data(), pkt.data.size(), pcm)) {
                            if (++decode_errors <= 5) fprintf(stderr, "[audio] decode failure\n");
                            goto next;
                        }
                        pkt.data.swap(pcm); pkt.codec = 0;
                        if (volume < 1) {
                            for (size_t i = 0; i + 1 < pkt.data.size(); i += 2) {
                                int16_t sample = int16_t(unsigned(pkt.data[i]) | (unsigned(pkt.data[i+1]) << 8));
                                int16_t out = int16_t(sample * volume);
                                pkt.data[i] = out & 255; pkt.data[i+1] = (uint16_t(out) >> 8) & 255;
                            }
                        }
                        if (pkt.data.empty()) goto next;
                    }
                    if (!pkt.video) audio_started = true;
                    if (!pkt.video && pts < audio_end) {
                        size_t trim = size_t((audio_end - pts) * 44100 / 1000000000) * 4;
                        if (trim >= pkt.data.size()) { ++late_audio; goto next; }
                        pkt.data.erase(pkt.data.begin(), pkt.data.begin() + trim);
                        pkt.timestamp = origin + audio_end; pts = audio_end;
                    }
                    auto result = media.feed(pkt.data.data(), pkt.data.size(), pts, pkt.video);
                    if (result == FeedResult::Accepted) {
                        if (pkt.video) ++video_count;
                        else { ++audio_count; audio_end = pts + int64_t(pkt.data.size()/4) * 1000000000 / 44100; }
                        stall = 0;
                    } else if (result == FeedResult::Retry) {
                        if (!stall) stall = now;
                        if (now - stall > 3000000000LL) { finish(); break; }
                    }
                }
                next:;
            } else if (h.size > 0) {
                std::vector<uint8_t> skip(h.size);
                bridge_read_all(sock, skip.data(), h.size);
            }
            fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
            got_msg = true;
        }

        if (opened) {
            media.tick();
            if (media.failed) { finish(); break; }
            int fills = 0;
            while (opened && !audio_started && !last_paused && silence_blocks < 100 && now - start - audio_end > 100000000 && fills++ < 10) {
                auto r = media.feed(silence.data(), silence.size(), audio_end, false);
                if (r != FeedResult::Accepted) break;
                audio_end += 10000000; ++silence_blocks;
            }
        }

        if (now - last_stats > 5000000000LL) {
            if (opened) fprintf(stderr, "[stats] video=%u audio=%u frames=%u drops=%u errors=%u late=%u silence=%u\n",
                video_count, audio_count, media.frames.load(), video_drops, decode_errors, late_audio, silence_blocks);
            rotate_log(); last_stats = now;
        }

        if (!got_msg) SDL_Delay(1);
    }

    media.close();
    close(sock);
    fprintf(stderr, "[bridge] done\n");
    return 0;
}

int main() {
    umask(0077); start_log("uxplay.log");
    Shell shell;
    if (!shell.open()) return 1;

    if (run_bridge(shell) >= 0) return 0;

    fprintf(stderr, "[app] standalone mode\n");
    shell.screen("Starting AirPlay receiver", "");
    Receiver receiver;
    if (!receiver.start(app_directory() + "/state")) {
        shell.screen("Could not start. See uxplay.log", "");
        while (shell.pump()) SDL_Delay(50);
        return 1;
    }
    const char *app_id = getenv("APPID");
    if (!app_id) app_id = "com.github.danlflh.uxplaywebos";
    Media media; AudioDecoder decoder;
    bool opened = false, have_pending = false, last_paused = false, audio_started = false;
    int width = 1280, height = 720;
    Packet pending;
    std::vector<uint8_t> configuration;
    uint64_t origin = 0;
    int64_t audio_end = 0;
    std::vector<uint8_t> silence(441*4, 0);
    int64_t start = 0, last_ui = 0, last_stats = 0, stall = 0;
    unsigned video_count = 0, audio_count = 0, decode_errors = 0, late_audio = 0, silence_blocks = 0, video_drops = 0;
    auto finish = [&](){
        media.close(); decoder.reset(); receiver.disconnect(); opened = false; have_pending = false;
        origin = 0; start = 0; stall = 0; audio_end = 0; configuration.clear(); last_ui = 0; last_paused = false; audio_started = false;
    };
    bool running = true;
    while (running) {
        int64_t now = now_ns();
        Action action = shell.action();
        if (action == Action::Exit) break;
        if (action == Action::Back) {
            if (opened || receiver.mirror || !receiver.pin().empty()) finish(); else break;
        }
        if (!opened && (action == Action::Left || action == Action::Right)) {
            receiver.adjust_audio_delay(action == Action::Right ? 25 : -25); last_ui = 0;
        }
        if (receiver.reset.exchange(false)) finish();
        if (receiver.activity && now - receiver.activity > 20000000000LL && (opened || receiver.mirror)) {
            fprintf(stderr, "[session] idle timeout\n");
            finish(); receiver.show("Connection lost. Connect again.");
        }
        if (receiver.audio_flush.exchange(false)) decoder.reset();
        if (opened) {
            media.tick();
            bool paused = receiver.paused;
            if (paused != last_paused) { media.pause(paused); last_paused = paused; }
            if (media.failed) { finish(); receiver.show("Playback error. Connect again."); }
            int fills = 0;
            while (opened && !audio_started && !last_paused && silence_blocks < 100 && now - receiver.last_audio_arrival > 500000000 && now - start - audio_end > 100000000 && fills++ < 10) {
                auto r = media.feed(silence.data(), silence.size(), audio_end, false);
                if (r != FeedResult::Accepted) break;
                audio_end += 10000000; ++silence_blocks;
            }
        }
        if (!opened && now - last_ui > 200000000) {
            shell.screen(receiver.status(), receiver.pin(), receiver.audio_delay_ms); last_ui = now;
        }
        if (now - last_stats > 5000000000LL) {
            if (opened) fprintf(stderr, "[stats] video=%u audio=%u frames=%u drops=%u overflow=%u errors=%u late=%u silence=%u\n",
                video_count, audio_count, media.frames.load(), video_drops, receiver.overflow.load(), decode_errors, late_audio, silence_blocks);
            rotate_log(); last_stats = now;
        }
        if (!have_pending) {
            uint64_t latest = opened ? origin + uint64_t(std::max<int64_t>(0, now - start)) : UINT64_MAX;
            have_pending = receiver.pop(pending, latest);
        }
        if (!have_pending) { SDL_Delay(1); continue; }
        if (!opened) {
            if (!pending.video) { have_pending = false; continue; }
            auto info = inspect_h264(pending.data.data(), pending.data.size());
            if (!info.valid) { have_pending = false; continue; }
            if (!info.configuration.empty()) configuration = info.configuration;
            if (info.width) { width = info.width; height = info.height; }
            if (!info.keyframe || configuration.empty()) { have_pending = false; continue; }
            if (!media.open(app_id, width, height, true)) {
                finish(); receiver.show("Could not open the TV decoder."); continue;
            }
            opened = true; origin = pending.timestamp; start = now + 50000000; stall = 0;
            video_count = audio_count = decode_errors = late_audio = silence_blocks = video_drops = 0;
            shell.transparent();
            fprintf(stderr, "[session] %dx%d audio=%d\n", width, height, int(receiver.has_audio));
        }
        int64_t pts = pending.timestamp >= origin ? int64_t(pending.timestamp - origin) : -int64_t(origin - pending.timestamp);
        if (pts < 0) { have_pending = false; continue; }
        if (pts > now - start + 5000000000LL) { fprintf(stderr, "[session] timestamp discontinuity\n"); finish(); receiver.show("Device clock changed. Connect again."); continue; }
        if (now < start + pts) { SDL_Delay(1); continue; }
        if (pending.video && now > start + pts + 100000000) { ++video_drops; have_pending = false; continue; }
        if (pending.video && pending.codec == 0) {
            auto info = inspect_h264(pending.data.data(), pending.data.size());
            if (!info.valid) { have_pending = false; continue; }
            if (!info.configuration.empty()) configuration = info.configuration;
            if (info.width && (width != info.width || height != info.height)) {
                media.close(); width = info.width; height = info.height;
                if (!info.keyframe || !media.open(app_id, width, height, true)) { finish(); continue; }
                origin = pending.timestamp; start = now; pts = 0; audio_end = 0; audio_started = false; decoder.reset();
                fprintf(stderr, "[session] geometry=%dx%d\n", width, height);
            }
            pending.codec = 1;
        }
        if (!pending.video && pending.codec != 0) {
            std::vector<uint8_t> pcm;
            if (!decoder.decode(pending.codec, pending.data.data(), pending.data.size(), pcm)) {
                if (++decode_errors <= 5) fprintf(stderr, "[audio] decode failure ct=%u\n", pending.codec);
                have_pending = false; continue;
            }
            pending.data.swap(pcm); pending.codec = 0;
            float vol = receiver.volume;
            if (vol < 1) {
                for (size_t i = 0; i + 1 < pending.data.size(); i += 2) {
                    int16_t sample = int16_t(unsigned(pending.data[i]) | (unsigned(pending.data[i+1]) << 8));
                    int16_t out = int16_t(sample * vol); pending.data[i] = out & 255; pending.data[i+1] = (uint16_t(out) >> 8) & 255;
                }
            }
            if (pending.data.empty()) { have_pending = false; continue; }
        }
        if (!pending.video) audio_started = true;
        if (!pending.video && pts < audio_end) {
            size_t trim = size_t((audio_end - pts) * 44100 / 1000000000) * 4;
            if (trim >= pending.data.size()) { ++late_audio; have_pending = false; continue; }
            pending.data.erase(pending.data.begin(), pending.data.begin() + trim);
            pending.timestamp = origin + audio_end; pts = audio_end;
        }
        auto result = media.feed(pending.data.data(), pending.data.size(), pts, pending.video);
        if (result == FeedResult::Accepted) {
            if (pending.video) ++video_count; else { ++audio_count; audio_end = pts + int64_t(pending.data.size()/4) * 1000000000 / 44100; }
            have_pending = false; stall = 0;
        } else if (result == FeedResult::Retry) {
            if (!stall) stall = now;
            if (now - stall > 3000000000LL) { finish(); receiver.show("Playback stopped. Connect again."); }
            SDL_Delay(2);
        } else { finish(); receiver.show("Stream failure. Connect again."); }
    }
    receiver.stop(); media.close();
    fprintf(stderr, "[app] stopped cleanly\n");
    return 0;
}
