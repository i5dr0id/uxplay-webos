#include "media.h"
#include <StarfishMediaAPIs.h>
#include <SDL_webOS.h>
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

struct Media::Impl { std::mutex mutex; std::string video_info; };
Media::Media() : impl_(new Impl) {}
Media::~Media() { close(); delete impl_; }
void Media::event(int type, int64_t value, const char *message, void *user) {
    Media *m = static_cast<Media *>(user);
    if (type == PF_EVENT_TYPE_FRAMEREADY) { ++m->frames; return; }
    if (type == PF_EVENT_TYPE_INT_NEED_DATA || type == PF_EVENT_TYPE_INT_ENOUGH_DATA || type == PF_EVENT_TYPE_INT_BUFFERLOW || type == 0x2a || type == 0x2d || type == 0x2e || type == 0x31) return;
    fprintf(stderr, "[smp] event=0x%x value=%lld %s\n", type, (long long)value, message ? message : "");
    switch (type) {
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED: m->ready = true; break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING: m->playing = true; break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM: m->ended = true; break;
    case PF_EVENT_TYPE_INT_ERROR:
    case PF_EVENT_TYPE_STR_ERROR: m->failed = true; break;
    case PF_EVENT_TYPE_STR_VIDEO_INFO:
        if (message) {
            std::lock_guard<std::mutex> lock(m->impl_->mutex);
            m->impl_->video_info = message;
            m->video_info_pending_ = true;
        }
        break;
    default: break;
    }
}
bool Media::open(const char *app_id, int width, int height, bool audio, int rate) {
    close();
    ready = playing = ended = failed = false;
    frames = 0;

    fprintf(stderr, "[media] Creating exported window type=VIDEO\n");
    const char *id = SDL_webOSCreateExportedWindow(SDL_WEBOS_EXPORED_WINDOW_TYPE_VIDEO);
    if (!id) {
        fprintf(stderr, "[media] SDL_webOSCreateExportedWindow failed\n");
        return false;
    }
    snprintf(window_id_, sizeof(window_id_), "%s", id);
    fprintf(stderr, "[media] Exported window id=%s\n", window_id_);

    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
        display_w_ = mode.w;
        display_h_ = mode.h;
    } else {
        display_w_ = width;
        display_h_ = height;
    }

    SDL_Rect src = {0, 0, width, height};
    SDL_Rect dst = {0, 0, display_w_, display_h_};
    if (!SDL_webOSSetExportedWindow(window_id_, &src, &dst)) {
        fprintf(stderr, "[media] SDL_webOSSetExportedWindow failed\n");
    }
    fprintf(stderr, "[media] Window geometry set src=%dx%d dst=%dx%d\n", width, height, display_w_, display_h_);

    api_ = new StarfishMediaAPIs(nullptr);
    fprintf(stderr, "[media] StarfishMediaAPIs created\n");
    std::ostringstream p;
    p << "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{\"appId\":\"" << app_id
      << "\",\"windowId\":\"" << window_id_
      << "\",\"queryPosition\":false,\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"H264\"";
    if (audio) p << ",\"audio\":\"PCM\"";
    p << "},\"esInfo\":{\"pauseAtDecodeTime\":true,\"ptsToDecode\":0,\"seperatedPTS\":true,\"videoFpsValue\":30,\"videoFpsScale\":1},\"format\":\"RAW\",\"provider\":\"Chrome\"";
    if (audio) p << ",\"pcmInfo\":{\"channelMode\":\"stereo\",\"format\":\"S16LE\",\"sampleRate\":" << rate / 1000.0
                 << ",\"bitsPerSample\":16,\"layout\":\"interleaved\"}";
    p << "},\"restartStreaming\":false,\"streamQualityInfo\":true,\"streamQualityInfoNonFlushable\":true,\"totalStreamSize\":256,"
         "\"bufferingCtrInfo\":{\"preBufferByte\":0,\"bufferMinLevel\":0,\"bufferMaxLevel\":0,\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
         "\"srcBufferLevelAudio\":{\"minimum\":1024,\"maximum\":131072},\"srcBufferLevelVideo\":{\"minimum\":4096,\"maximum\":4194304}}},"
         "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":" << width << ",\"maxHeight\":" << height << ",\"maxFrameRate\":30}}}]}";
    fprintf(stderr, "[media] Load %s\n", p.str().c_str());
    loaded_ = api_->Load(p.str().c_str(), event, this);
    if (!loaded_) { close(); return false; }
    return true;
}
void Media::tick() {
    if (!api_ || !loaded_ || failed) return;
    if (video_info_pending_.exchange(false)) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // On webOS 5 the exported window handles geometry updates
        SDL_Rect src = {0, 0, display_w_, display_h_};
        SDL_Rect dst = {0, 0, display_w_, display_h_};
        SDL_webOSSetExportedWindow(window_id_, &src, &dst);
    }
    if (ready && !play_requested_) play_requested_ = api_->Play();
}
FeedResult Media::feed(const void *data, size_t size, int64_t pts_ns, bool video) {
    if (!api_ || !loaded_ || failed || !data || !size) return FeedResult::Error;
    char payload[200];
    snprintf(payload, sizeof(payload), "{\"bufferAddr\":\"%p\",\"bufferSize\":%zu,\"pts\":%lld,\"esData\":%d}",
             data, size, (long long)pts_ns, video ? 1 : 2);
    std::string reply = api_->Feed(payload);
    if (reply.find("Ok") != std::string::npos) return FeedResult::Accepted;
    if (reply.find("BufferFull") != std::string::npos || reply.find("Pending") != std::string::npos) return FeedResult::Retry;
    fprintf(stderr, "[media] Feed rejected: %s\n", reply.c_str());
    return FeedResult::Error;
}
void Media::eos() { if (api_ && loaded_) api_->pushEOS(); }
void Media::pause(bool paused) {if(api_&&loaded_) {if(paused)api_->Pause();else api_->Play();}}
void Media::close() {
    if (api_) {
        if (loaded_) { api_->Unload(); fprintf(stderr, "[media] unloaded\n"); }
        delete api_; api_ = nullptr;
    }
    if (window_id_[0]) {
        SDL_webOSDestroyExportedWindow(window_id_);
        window_id_[0] = '\0';
        fprintf(stderr, "[media] exported window released\n");
    }
    loaded_ = play_requested_ = false;
    video_info_pending_ = false;
}
