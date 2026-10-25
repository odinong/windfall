#pragma once

union SDL_Event;

namespace wf::ui::menu {

bool is_open();
void toast(const std::string& text);
void toggle();
void open_tab(int tab);

void on_sdl_event(const SDL_Event& e);

void render();

}
