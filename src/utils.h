#pragma once
#include <cstdint>
#include <string>
int64_t now_ns();
std::string app_directory();
void start_log(const char *name);
void rotate_log();
