#pragma once
#include "utils.h"
#include <SDL.h>
#include <cstdint>
#include <string>
struct _TTF_Font;
enum class Action { None, Back, Exit, Left, Right };
class Shell {
public:
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool open();
    bool pump();
    Action action();
    void screen(const std::string& status, const std::string& pin, int audio_delay_ms=100);
    void transparent();
    ~Shell();
private:
    _TTF_Font *font_=nullptr, *large_=nullptr;
    void text(const std::string&,int y,_TTF_Font*,SDL_Color);
};
