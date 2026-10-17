#include "utils.h"
#include <cstdio>
#include <cstring>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static std::string log_path;
int64_t now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return int64_t(t.tv_sec)*1000000000 + t.tv_nsec; }
std::string app_directory() {
    char p[4096]; ssize_t n = readlink("/proc/self/exe", p, sizeof(p)-1);
    if (n <= 0) return ".";
    p[n] = 0; std::string s(p); return s.substr(0, s.rfind('/'));
}
void start_log(const char *name) {
    std::string p = app_directory() + "/" + name;
    log_path = p;
    rename(p.c_str(), (p + ".previous").c_str());
    freopen(p.c_str(), "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    int nullfd = open("/dev/null", O_WRONLY);
    if (nullfd >= 0) { dup2(nullfd, STDOUT_FILENO); close(nullfd); }
}
void rotate_log() {
    struct stat s;
    if (!log_path.empty() && fstat(fileno(stderr), &s) == 0 && s.st_size > 1024*1024) {
        rename(log_path.c_str(), (log_path + ".previous").c_str());
        freopen(log_path.c_str(), "w", stderr); setvbuf(stderr, nullptr, _IOLBF, 0);
    }
}
