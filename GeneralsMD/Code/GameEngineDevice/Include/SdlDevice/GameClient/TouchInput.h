/*
** Android touch input for Zero Hour Reforged.
** Battlefield gestures are resolved directly through View/GameClient/InGameUI so the
** simulation receives the same game messages as a real mouse command. UI-only touches
** temporarily reuse SdlMouse because GameWindow owns the existing widget routing.
*/
#pragma once

#include "Lib/BaseType.h"

union SDL_Event;

namespace TouchInput
{
    Bool dispatch(const SDL_Event &event);
    void update(UnsignedInt nowMs);
    void reset();
}
