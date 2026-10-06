/*
** Android touch input for Zero Hour Reforged.
** Licensed under GPL-3.0-or-later; see LICENSE.md.
*/

#include "PreRTS.h"

#include "SdlDevice/GameClient/TouchInput.h"
#include "SdlDevice/GameClient/SdlInput.h"
#include "SdlDevice/GameClient/SdlMouse.h"

#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/SelectionInfo.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"
#include "GameLogic/Object.h"
#include "GameClient/CommandXlat.h"
#include "Common/GameCommon.h"
#include "Common/MessageStream.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <string.h>

#if defined(__ANDROID__)

namespace
{
struct Finger
{
    Bool active;
    SDL_FingerID id;
    Int x, y;
    Int startX, startY;
};

Finger g_fingers[2];
Bool g_panning = FALSE;
Bool g_pinching = FALSE;
Real g_pinchDistance = 0.0f;
Int g_pinchMidX = 0;
Int g_pinchMidY = 0;

Bool g_pendingTap = FALSE;
Int g_pendingX = 0;
Int g_pendingY = 0;
UnsignedInt g_pendingAt = 0;

static Int ActiveCount()
{
    return (g_fingers[0].active ? 1 : 0) + (g_fingers[1].active ? 1 : 0);
}

static Finger *Find(SDL_FingerID id)
{
    for (Int i = 0; i < 2; ++i)
        if (g_fingers[i].active && g_fingers[i].id == id)
            return &g_fingers[i];
    return nullptr;
}

static Finger *FreeFinger()
{
    for (Int i = 0; i < 2; ++i)
        if (!g_fingers[i].active)
            return &g_fingers[i];
    return nullptr;
}

static void TouchToGamePixels(Real nx, Real ny, Int &x, Int &y)
{
    SDL_Window *window = SdlInput_gameWindow();
    Int w = 0, h = 0;
    if (window != nullptr)
        SDL_GetWindowSize(window, &w, &h);

    SdlInput_toGamePixels(nx * (Real)w, ny * (Real)h, x, y);
}

static Bool IsUiPoint(Int x, Int y)
{
    // The tactical view is not a GameWindow. Existing ControlBar/radar/shell widgets are.
    return TheWindowManager != nullptr && TheWindowManager->getWindowUnderCursor(x, y) != nullptr;
}

static void UiMouseDown(Int x, Int y, UnsignedInt now)
{
    if (SdlMouse::active() != nullptr)
        SdlMouse::active()->addEvent(SdlMouse::EVENT_BUTTON_DOWN, x, y,
            SdlMouse::BUTTON_LEFT, 1, 0, now);
}

static void UiMouseUp(Int x, Int y, UnsignedInt now)
{
    if (SdlMouse::active() != nullptr)
        SdlMouse::active()->addEvent(SdlMouse::EVENT_BUTTON_UP, x, y,
            SdlMouse::BUTTON_LEFT, 1, 0, now);
}

static Drawable *PickForSelection(const ICoord2D &pixel)
{
    if (TheTacticalView == nullptr)
        return nullptr;
    Drawable *draw = TheTacticalView->pickDrawable(&pixel, FALSE, PICK_TYPE_SELECTABLE);
    if (draw == nullptr)
        return nullptr;
    const Object *obj = draw->getObject();
    if (obj == nullptr || !obj->isLocallyControlled())
        return nullptr;
    return draw;
}

static Drawable *PickForOrder(const ICoord2D &pixel)
{
    if (TheTacticalView == nullptr || TheInGameUI == nullptr)
        return nullptr;
    const Bool forceAttack = TheInGameUI->isInForceAttackMode();
    const PickType pickType = (PickType)getPickTypesForContext(forceAttack);
    return TheTacticalView->pickDrawable(&pixel, forceAttack, pickType);
}

static void SelectOnly(Drawable *draw)
{
    if (draw == nullptr || TheInGameUI == nullptr)
        return;

    TheInGameUI->deselectAllDrawables();
    TheInGameUI->selectDrawable(draw);

    const Object *obj = draw->getObject();
    if (obj != nullptr && TheMessageStream != nullptr)
    {
        GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
        msg->appendBooleanArgument(TRUE);
        msg->appendObjectIDArgument(obj->getID());
    }
}

static void Tap(Int x, Int y)
{
    if (TheTacticalView == nullptr || TheInGameUI == nullptr)
        return;

    ICoord2D pixel = { x, y };
    Drawable *selected = PickForSelection(pixel);
    if (selected != nullptr)
    {
        SelectOnly(selected);
        return;
    }

    Coord3D pos;
    if (!TheTacticalView->screenToTerrain(&pixel, &pos))
        return;

    if (TheInGameUI->areSelectedObjectsControllable() && TheGameClient != nullptr)
    {
        Drawable *target = PickForOrder(pixel);
        TheGameClient->evaluateContextCommand(target, &pos, CommandTranslator::DO_COMMAND);
    }
    else
    {
        TheInGameUI->deselectAllDrawables();
    }
}

static void DoubleTap(Int x, Int y)
{
    if (TheTacticalView == nullptr || TheInGameUI == nullptr)
        return;

    ICoord2D pixel = { x, y };
    Drawable *selected = PickForSelection(pixel);
    if (selected != nullptr && selected->isMassSelectable())
    {
        SelectOnly(selected);
        TheInGameUI->selectMatchingAcrossScreen();
        return;
    }
    Tap(x, y);
}

static Real Distance(const Finger &a, const Finger &b)
{
    const Real dx = (Real)(a.x - b.x);
    const Real dy = (Real)(a.y - b.y);
    return sqrtf(dx * dx + dy * dy);
}

static void UpdatePinch()
{
    if (!g_fingers[0].active || !g_fingers[1].active || TheTacticalView == nullptr)
        return;

    const Real distance = Distance(g_fingers[0], g_fingers[1]);
    const Real delta = distance - g_pinchDistance;

    const Int midX = (g_fingers[0].x + g_fingers[1].x) / 2;
    const Int midY = (g_fingers[0].y + g_fingers[1].y) / 2;

    Coord2D pan;
    pan.x = g_pinchMidX - midX;
    pan.y = g_pinchMidY - midY;
    if (pan.x != 0 || pan.y != 0)
        TheTacticalView->scrollBy(&pan);

    if (fabsf(delta) > 2.0f)
    {
        const Real steps = fabsf(delta) / 80.0f;
        if (delta > 0.0f)
            TheTacticalView->zoomIn(steps);
        else
            TheTacticalView->zoomOut(steps);
    }

    g_pinchDistance = distance;
    g_pinchMidX = midX;
    g_pinchMidY = midY;
}

} // namespace

namespace TouchInput
{

void reset()
{
    memset(g_fingers, 0, sizeof(g_fingers));
    g_panning = FALSE;
    g_pinching = FALSE;
    g_pinchDistance = 0.0f;
    g_pendingTap = FALSE;
}

Bool dispatch(const SDL_Event &event)
{
    if (event.type == SDL_EVENT_FINGER_DOWN)
    {
        Int x, y;
        TouchToGamePixels(event.tfinger.x, event.tfinger.y, x, y);

        if (IsUiPoint(x, y))
        {
            UiMouseDown(x, y, (UnsignedInt)(event.tfinger.timestamp / 1000000u));
            return TRUE;
        }

        Finger *slot = FreeFinger();
        if (slot == nullptr)
            return TRUE;
        slot->active = TRUE;
        slot->id = event.tfinger.fingerID;
        slot->x = slot->startX = x;
        slot->y = slot->startY = y;

        if (ActiveCount() == 2)
        {
            // A second finger changes the gesture class immediately; never let an earlier pending
            // single tap fire after a pinch starts.
            g_pendingTap = FALSE;
            g_pinching = TRUE;
            g_panning = FALSE;
            g_pinchDistance = Distance(g_fingers[0], g_fingers[1]);
            g_pinchMidX = (g_fingers[0].x + g_fingers[1].x) / 2;
            g_pinchMidY = (g_fingers[0].y + g_fingers[1].y) / 2;
        }
        return TRUE;
    }

    if (event.type == SDL_EVENT_FINGER_MOTION)
    {
        Finger *finger = Find(event.tfinger.fingerID);
        if (finger == nullptr)
            return TRUE;

        Int x, y;
        TouchToGamePixels(event.tfinger.x, event.tfinger.y, x, y);

        if (IsUiPoint(x, y))
        {
            finger->x = x;
            finger->y = y;
            return TRUE;
        }

        finger->x = x;
        finger->y = y;

        if (g_pinching)
        {
            UpdatePinch();
            return TRUE;
        }

        const Int dx = finger->x - finger->startX;
        const Int dy = finger->y - finger->startY;
        if (!g_panning && (dx * dx + dy * dy) > (12 * 12))
        {
            g_pendingTap = FALSE;
            g_panning = TRUE;
        }

        if (g_panning && TheTacticalView != nullptr)
        {
            Coord2D delta;
            delta.x = -event.tfinger.dx * TheTacticalView->getWidth();
            delta.y = -event.tfinger.dy * TheTacticalView->getHeight();
            TheTacticalView->scrollBy(&delta);
        }
        return TRUE;
    }

    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED)
    {
        Finger *finger = Find(event.tfinger.fingerID);
        if (finger == nullptr)
            return TRUE;

        Int x, y;
        TouchToGamePixels(event.tfinger.x, event.tfinger.y, x, y);
        const Bool wasPanning = g_panning;
        const Bool wasPinching = g_pinching;

        if (IsUiPoint(x, y))
        {
            if (event.type == SDL_EVENT_FINGER_UP)
                UiMouseUp(x, y, (UnsignedInt)(event.tfinger.timestamp / 1000000u));
        }

        finger->active = FALSE;

        if (ActiveCount() == 0)
        {
            g_panning = FALSE;
            g_pinching = FALSE;
        } else if (ActiveCount() == 1) {
            g_pinching = FALSE;
            g_panning = FALSE;
        }

        if (!wasPanning && !wasPinching && !IsUiPoint(x, y) && event.type == SDL_EVENT_FINGER_UP)
        {
            const UnsignedInt now = (UnsignedInt)(event.tfinger.timestamp / 1000000u);
            if (g_pendingTap && now - g_pendingAt <= 300 && 
                abs(x - g_pendingX) <= 28 && abs(y - g_pendingY) <= 28)
            {
                g_pendingTap = FALSE;
                DoubleTap(x, y);
            }
            else
            {
                g_pendingTap = TRUE;
                g_pendingX = x;
                g_pendingY = y;
                g_pendingAt = now;
            }
        }
        return TRUE;
    }

    return FALSE;
}

void update(UnsignedInt nowMs)
{
    if (g_pendingTap && nowMs >= g_pendingAt && nowMs - g_pendingAt > 300)
    {
        Tap(g_pendingX, g_pendingY);
        g_pendingTap = FALSE;
    }
}

} // namespace TouchInput

#else

namespace TouchInput
{
Bool dispatch(const SDL_Event &) { return FALSE; }
void update(UnsignedInt) {}
void reset() {}
}

#endif
