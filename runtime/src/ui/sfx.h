#pragma once

namespace wf::ui::sfx {

enum Sound { kMove, kSelect, kConfirm, kBack, kTab, kOpen, kClose, kSoundCount };

void play(Sound s);

}
