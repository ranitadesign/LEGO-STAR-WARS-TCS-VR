/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* VR fork: virtual pad input and virtual keyboard keys.
 *
 * The game samples every input device into g_nupadScannedPads; player 1 reads the device it claimed
 * on the title screen. VR input is merged into that device (or into SAGA_VR_PAD_DEVICE before any is
 * claimed), so controllers work whichever keyboard or pad the player picked. Sources: the control
 * block (automated tests, tools/vr_control.c) and the VR controllers (stage 3): left stick moves,
 * right stick is the pad's right stick, SAGA_XR_BUTTON_* map onto the PS2-layout buttons. The title
 * screen only accepts raw device presses, so on the title level (and before any device is claimed) the
 * jump button presses Enter on the virtual keyboard instead. */
#include "vr/vr_internal.h"

#include <math.h>

#include "../../xr/saga_xr.h"

static u8 SagaVrStickAxis(f32 value, i32 invert) {
    i32 axis = (i32)(128.0f + (invert ? -value : value) * 127.0f);
    return (u8)(axis < 0 ? 0 : axis > 255 ? 255 : axis);
}

#define SAGA_VR_STICK_DEADZONE 0.15f

static i32 SagaVrLastTarget = -2;

/* Title screen and main menus: they only accept a raw key press to claim a device, and confirm with Enter. */
static i32 SagaVrOnTitles(void) {
    return WORLD != NULL && WORLD->current_level == TITLES_LDATA;
}
static i32 SagaVrSnapSide = 0;

#define SAGA_VR_CONTEXT_FORCE 8

void __cdecl SagaVrCutSceneSkip(INSTNUGCUTSCENE *owner); /* cutscene_lifecycle.c */

/* Story cutscenes cannot be skipped on PC (CutScene_IsSkippable is 0 and the pause menu stays shut). In VR
 * the pause button jumps the playing one to its end, and the game takes the normal end path (next
 * cutscene, door or level). Returns 1 when it consumed the press. */
static i32 SagaVrSkipCutscene(i32 menu_down) {
    static i32 was_down = 0, swallow = 0;
    i32 pressed = menu_down && !was_down;
    CUTINFO *cut = (CUTINFO *)CutStopInfo;
    was_down = menu_down;
    /* The press that skipped stays consumed until released: the cutscene ends within a frame or two and
     * the still-held button would otherwise open the pause menu. */
    if (swallow) {
        swallow = menu_down;
        return 1;
    }
    if (CUTSTOPGAME == 0 || cut == NULL || cut->instance == NULL)
        return 0;
    if (pressed) {
        LOG_INFO("VR: skipping cutscene %s", cut->name);
        SagaVrCutSceneSkip((INSTNUGCUTSCENE *)cut->instance);
        swallow = 1;
    }
    return 1;
}

/* Telemetry only: the Force lasts as long as the player holds special, as in the game. */
static void SagaVrForceLog(void) {
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    static i32 active = 0;
    static u32 started_frame = 0;
    i32 now = player != NULL && player->character_context == SAGA_VR_CONTEXT_FORCE;
    if (now && !active) {
        started_frame = SagaVrGetControl()->frame;
        LOG_INFO("VR force: started");
    } else if (!now && active) {
        LOG_INFO("VR force: ended after %u frames", SagaVrGetControl()->frame - started_frame);
    }
    active = now;
}

void __cdecl SagaVrPadInject(i32 device, FRAME_SCANNED_PAD_PC *pad) {
    SAGA_VR_CONTROL *control;
    u32 buttons, controller_buttons = 0;
    f32 move[2], look[2];
    i32 controllers, target;
    target = TextInputDeviceLinksPc[0].device >= 0 ? TextInputDeviceLinksPc[0].device : SAGA_VR_PAD_DEVICE;
    if (device == 0 && target != SagaVrLastTarget) {
        LOG_INFO("VR input target: scanned device %d (player 1 link %d, available %d)", target,
                 TextInputDeviceLinksPc[0].device, TextInputDeviceLinksPc[0].available);
        SagaVrLastTarget = target;
    }
    if (device != target) {
        SagaVrRotateStick(pad);
        return;
    }
    control = SagaVrGetControl();
    controllers = SagaVrControllerInput(move, look, &controller_buttons);
    if (!control->pad_active && !controllers) {
        SagaVrRotateStick(pad);
        return;
    }
    /* Merge: the claimed device keeps its own input unless a VR source is actually pushing. */
    if (!pad->is_valid) {
        pad->is_valid = 1;
        pad->digital_buttons = 0;
        pad->analog_left_x = pad->analog_left_y = pad->analog_right_x = pad->analog_right_y = 128;
    }
    buttons = pad->digital_buttons;
    if (controllers) {
        /* In a pod's cockpit only its levers drive; menus and cutscenes end first person and get the stick back. */
        if ((fabs(move[0]) > SAGA_VR_STICK_DEADZONE || fabs(move[1]) > SAGA_VR_STICK_DEADZONE) &&
            !(SagaVrFirstPersonActive() && SagaVrOnPod((GAMEOBJECT *)Player[0]))) {
            pad->analog_left_x = SagaVrStickAxis(move[0], 0);
            pad->analog_left_y = SagaVrStickAxis(move[1], 1);
        }
        /* A pod's levers, while held: the left one steers, the right one forward is the race's accelerator
         * (jump) or the sprint's stick forward (there jump makes the pod loop). */
        if (SagaVrFirstPersonActive() && SagaVrOnPod((GAMEOBJECT *)Player[0])) {
            f32 steer, throttle;
            i32 held = SagaVrPodLeverInput(&steer, &throttle);
            if (held & 1)
                pad->analog_left_x = SagaVrStickAxis(steer, 0);
            if ((held & 2) && throttle > 0.25f) {
                if (WORLD->area == PODRACE_ADATA)
                    buttons |= 0x40u;
                else
                    pad->analog_left_y = SagaVrStickAxis(throttle, 1);
            }
        }
        if (SagaVrFirstPersonActive() && SagaVrInShip((GAMEOBJECT *)Player[0])) {
            f32 stick_x, stick_y;
            if (SagaVrShipStickInput(&stick_x, &stick_y)) {
                pad->analog_left_x = SagaVrStickAxis(stick_x, 0);
                pad->analog_left_y = SagaVrStickAxis(stick_y, 1);
            }
        }
        if (control->first_person && control->smooth_turn > 0.0f) {
            /* Right stick: smooth turning, speed proportional to the push past the dead zone. */
            f32 frame_time = nuapi.frame_time > 0.0f && nuapi.frame_time < 0.1f ? nuapi.frame_time : 1.0f / 60.0f;
            if (fabs(look[0]) > SAGA_VR_STICK_DEADZONE)
                SagaVrFirstPersonSnapTurn(look[0] * control->smooth_turn * 0.017453293f * frame_time);
        } else if (control->first_person) {
            /* Right stick: 30 degree snap turns of the first-person view, edge-triggered. */
            i32 side = look[0] > 0.7f ? 1 : look[0] < -0.7f ? -1 : 0;
            if (side != 0 && SagaVrSnapSide == 0)
                SagaVrFirstPersonSnapTurn((f32)side * 0.5235988f);
            if (side != 0 || fabs(look[0]) < 0.3f)
                SagaVrSnapSide = side;
        } else if (fabs(look[0]) > SAGA_VR_STICK_DEADZONE || fabs(look[1]) > SAGA_VR_STICK_DEADZONE) {
            pad->analog_right_x = SagaVrStickAxis(look[0], 0);
            pad->analog_right_y = SagaVrStickAxis(look[1], 1);
        }
        /* Right stick up / down in first person: the shoulder buttons (R1 / L1), which cycle the character
         * in free play (GAMEPAD_TOGGLERIGHT / GAMEPAD_TOGGLELEFT); held, the game repeats the toggle. */
        if (control->first_person && FreePlay != 0 && fabs(look[1]) > 0.7f && fabs(look[0]) < 0.5f)
            buttons |= look[1] > 0.0f ? 0x8u : 0x4u;
        /* On the title level the jump button presses Enter instead (see SagaVrKeyboardInject). */
        if (!SagaVrOnTitles())
            buttons |= (controller_buttons & SAGA_XR_BUTTON_JUMP) ? 0x40u : 0u;
        buttons |= (controller_buttons & SAGA_XR_BUTTON_ATTACK) ? 0x80u : 0u;
        buttons |= (controller_buttons & SAGA_XR_BUTTON_SPECIAL) ? 0x20u : 0u;
        buttons |= (controller_buttons & SAGA_XR_BUTTON_SWAP) ? 0x10u : 0u;
        buttons |= (controller_buttons & SAGA_XR_BUTTON_MENU) ? 0x800u : 0u;
        if (SagaVrSkipCutscene((controller_buttons & SAGA_XR_BUTTON_MENU) != 0 ||
                               (control->pad_active && (control->pad_buttons & 0x800u) != 0)))
            buttons &= ~0x800u;
        SagaVrForceLog();
    }
    if (control->pad_active) {
        buttons |= control->pad_buttons;
        if (control->pad_axes[0] != 128 || control->pad_axes[1] != 128) {
            pad->analog_left_x = control->pad_axes[0];
            pad->analog_left_y = control->pad_axes[1];
        }
        if (control->pad_axes[2] != 128 || control->pad_axes[3] != 128) {
            pad->analog_right_x = control->pad_axes[2];
            pad->analog_right_y = control->pad_axes[3];
        }
    }
    pad->digital_buttons = buttons;
    if (buttons & 0x1000)
        pad->analog_left_pad_up = 255;
    if (buttons & 0x2000)
        pad->analog_left_pad_right = 255;
    if (buttons & 0x4000)
        pad->analog_left_pad_down = 255;
    if (buttons & 0x8000)
        pad->analog_left_pad_left = 255;
    SagaVrRotateStick(pad);
}

u8 *__cdecl SagaVrKeyboardInject(u8 *page) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    f32 move[2], look[2];
    u32 buttons = 0;
    i32 i;
    for (i = 0; i < 8; ++i)
        if (control->keys[i] != 0)
            page[control->keys[i]] = 0x80;
    if ((TextInputDeviceLinksPc[0].device < 0 || SagaVrOnTitles()) && SagaVrControllerInput(move, look, &buttons) &&
        (buttons & SAGA_XR_BUTTON_JUMP))
        page[0x1c] = 0x80; /* DIK_RETURN: claim a device / confirm on the title screen */
    return page;
}
