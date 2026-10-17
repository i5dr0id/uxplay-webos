#include "receiver.h"
#include "bridge.h"
#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/select.h>

static volatile sig_atomic_t quit = 0;
static void on_signal(int) { quit = 1; }

static bool send_pin(int fd, Receiver &receiver) {
    std::string status = receiver.status();
    std::string pin = receiver.pin();
    std::string payload = status + '\0' + pin;
    BridgeHeader h = {};
    h.type = MSG_PIN;
    h.size = payload.size();
    return bridge_write_all(fd, &h, sizeof(h)) && bridge_write_all(fd, payload.data(), payload.size());
}

int main() {
    umask(0077);
    start_log("daemon.log");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    Receiver receiver;
    if (!receiver.start(app_directory() + "/state")) {
        fprintf(stderr, "[daemon] receiver failed\n");
        return 1;
    }
    fprintf(stderr, "[daemon] listening\n");

    while (!quit) {
        while (!quit && !receiver.mirror && !receiver.pin_active) {
            usleep(100000);
            if (receiver.reset.exchange(false)) receiver.disconnect();
        }
        if (quit) break;

        bool for_pin = receiver.pin_active && !receiver.mirror;
        fprintf(stderr, "[daemon] %s, launching app\n", for_pin ? "PIN requested" : "mirroring started");

        unlink(BRIDGE_SOCK);
        int srv = socket(AF_UNIX, SOCK_STREAM, 0);
        if (srv < 0) { receiver.disconnect(); continue; }
        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, BRIDGE_SOCK, sizeof(addr.sun_path) - 1);
        if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(srv, 1) < 0) {
            close(srv); unlink(BRIDGE_SOCK); receiver.disconnect(); continue;
        }
        chmod(BRIDGE_SOCK, 0666);

        system("luna-send -n 1 luna://com.webos.applicationManager/launch "
               "'{\"id\":\"com.github.danlflh.uxplaywebos\"}' >/dev/null 2>&1 &");

        fd_set fds; FD_ZERO(&fds); FD_SET(srv, &fds);
        struct timeval tv = {10, 0};
        if (select(srv + 1, &fds, nullptr, nullptr, &tv) <= 0) {
            fprintf(stderr, "[daemon] app did not connect\n");
            close(srv); unlink(BRIDGE_SOCK); receiver.disconnect(); continue;
        }
        int client = accept(srv, nullptr, nullptr);
        close(srv);
        if (client < 0) { unlink(BRIDGE_SOCK); receiver.disconnect(); continue; }

        fprintf(stderr, "[daemon] app connected, bridging\n");

        if (receiver.pin_active && !receiver.mirror) {
            send_pin(client, receiver);
        }

        bool sent_start = false;
        int64_t last_state = 0, last_pin = now_ns();
        Packet pkt;
        bool active = true;

        while (!quit && active) {
            int64_t now = now_ns();

            if (receiver.reset.exchange(false) ||
                (receiver.activity && now - receiver.activity > 20000000000LL) ||
                (!receiver.mirror && !receiver.pin_active)) {
                BridgeHeader h = {}; h.type = MSG_END;
                bridge_write_all(client, &h, sizeof(h));
                active = false; break;
            }

            if (receiver.pin_active && !receiver.mirror && now - last_pin > 500000000) {
                if (!send_pin(client, receiver)) { active = false; break; }
                last_pin = now;
            }

            if (receiver.mirror && !sent_start) {
                BridgeHeader h = {};
                h.type = MSG_START;
                h.size = 8;
                int32_t dims[2] = {receiver.width.load(), receiver.height.load()};
                if (!bridge_write_all(client, &h, sizeof(h)) || !bridge_write_all(client, dims, 8)) {
                    active = false; break;
                }
                sent_start = true;
                fprintf(stderr, "[daemon] mirroring started\n");
            }

            if (sent_start && now - last_state > 100000000) {
                BridgeHeader h = {};
                h.type = MSG_STATE;
                h.flags = (receiver.paused ? 1 : 0) |
                          (receiver.audio_flush.exchange(false) ? 2 : 0) |
                          (receiver.has_audio ? 8 : 0);
                float vol = receiver.volume;
                h.size = 4;
                h.timestamp = receiver.activity;
                if (!bridge_write_all(client, &h, sizeof(h)) || !bridge_write_all(client, &vol, 4)) {
                    active = false; break;
                }
                last_state = now;
            }

            if (sent_start && receiver.pop(pkt)) {
                BridgeHeader h = {};
                h.type = MSG_PACKET;
                h.flags = pkt.video ? 1 : 0;
                h.codec = pkt.codec;
                h.size = pkt.data.size();
                h.timestamp = pkt.timestamp;
                if (!bridge_write_all(client, &h, sizeof(h)) ||
                    !bridge_write_all(client, pkt.data.data(), pkt.data.size())) {
                    active = false; break;
                }
            } else {
                usleep(1000);
            }
        }

        close(client);
        unlink(BRIDGE_SOCK);
        receiver.disconnect();
        fprintf(stderr, "[daemon] session ended\n");
    }

    receiver.stop();
    unlink(BRIDGE_SOCK);
    fprintf(stderr, "[daemon] stopped\n");
    return 0;
}
