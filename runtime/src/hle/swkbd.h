#pragma once

namespace wf::hle {

bool swkbd_active();
void swkbd_text(const char* utf8);
void swkbd_backspace();
void swkbd_enter();

}
