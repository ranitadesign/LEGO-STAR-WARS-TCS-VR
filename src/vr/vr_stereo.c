/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* VR fork, milestones 3-5: OpenXR frames, per-eye replay of the recorded scenes, head tracking.
 *
 * The game records each frame's scenes into display lists; NuRndrSwapScreen replays them. In stereo
 * that replay runs once per eye (SAGA_VR_EYE_LOOP_*), and every perspective camera packet is
 * re-derived for the eye: view = packet_view * head_at_update * inverse(eye_at_render), projection =
 * the eye's asymmetric field of view. The update-time head transform moves the world camera so
 * culling follows the head; the render-time eye pose corrects for the latest tracking. Each eye's
 * back buffer is copied to saga_vr.dll, which owns OpenXR and the D3D11 swapchains.
 *
 * Configuration: saga_vr.ini next to the executable, section [vr]: stereo, simulate, world_scale, fp_world_scale.
 * The environment variable SAGA_VR_SIMULATE=1 forces simulated stereo (no runtime needed). */
#include "vr/vr_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../xr/saga_xr.h"

#define SAGA_VR_MAX_SCENES 16
#define SAGA_VR_CULL_MARGIN 0.15f /* radians added around the eyes' field of view for culling */
#define SAGA_VR_FORCE_CAMERA_ID 0x7ffffff3

typedef struct SAGA_VR_SURFACE_DESC {
    i32 format;
    i32 type;
    u32 usage;
    i32 pool;
    i32 multisample_type;
    u32 multisample_quality;
    u32 width;
    u32 height;
} SAGA_VR_SURFACE_DESC;

typedef struct SAGA_VR_LOCKED_RECT {
    i32 pitch;
    void *bits;
} SAGA_VR_LOCKED_RECT;

static SAGA_VR_CONTROL *SagaVrStereoControl = NULL;
static HMODULE SagaVrXrModule = NULL;
static SAGA_XR_BEGIN_FRAME SagaVrXrBeginFrame = NULL;
static SAGA_XR_SUBMIT_EYE SagaVrXrSubmitEye = NULL;
static SAGA_XR_END_FRAME SagaVrXrEndFrame = NULL;
static SAGA_XR_RECENTER SagaVrXrRecenter = NULL;
static SAGA_XR_SAVE_STEREO SagaVrXrSaveStereo = NULL;
static SAGA_XR_SET_SIMULATED_HEAD SagaVrXrSetSimulatedHead = NULL;
static SAGA_XR_GET_INPUT SagaVrXrGetInput = NULL;
static SAGA_XR_END_FRAME_SCREEN SagaVrXrEndFrameScreen = NULL;
static i32 SagaVrScreenFrame = 0;

/* Story cutscenes (the ones that stop the game) run with their own camera on a flat screen, never in
 * first person or head-tracked. Ambient cutscenes (e.g. the cantina's background animations) also
 * count in ACTIVECUTCOUNT, so only CUTSTOPGAME selects cinema mode. */
/* Pause menu, save/load, shop and other game menus: shown on the same distant screen as cutscenes
 * instead of glued to the face. */
static i32 SagaVrMenuOpen(void) {
    static i32 was_open = -1;
    i32 open = Paused != 0 || (GameMenuLevel >= 0 && GameMenuLevel < 10 && GameMenu[GameMenuLevel].menu != -1);
    if (open != was_open) {
        LOG_INFO("VR menu %s (paused %d, level %d, menu %d)", open ? "open" : "closed", Paused, GameMenuLevel,
                 GameMenuLevel >= 0 && GameMenuLevel < 10 ? (i32)GameMenu[GameMenuLevel].menu : -2);
        was_open = open;
    }
    return open;
}

i32 __cdecl SagaVrCinema(void) {
    return CUTSTOPGAME != 0 || SagaVrMenuOpen();
}
static SAGA_XR_INPUT SagaVrInput;
static u32 SagaVrPreviousButtons = 0;
static LARGE_INTEGER SagaVrSwingUntil;
static SAGA_XR_VIEW SagaVrViews[2];
static i32 SagaVrHaveViews = 0;
static i32 SagaVrCurrentEye = -1;
static i32 SagaVrSwap = 0; /* test aid: SAGA_VR_SWAP_EYES renders the right eye first */
static f32 SagaVrHeadAtUpdate[16];
static i32 SagaVrHeadApplied = 0;
static NURNDRLIBSCENE SagaVrSceneSnapshot[SAGA_VR_MAX_SCENES];
static i32 SagaVrSceneSnapshotCount = 0;
static NUMTX SagaVrEyeViewMatrix;
static NUMTX SagaVrEyeProjectionMatrix;
static void *SagaVrCopySurface = NULL;
static SAGA_VR_SURFACE_DESC SagaVrCopyDesc;
static u32 SagaVrRecenterSeen = 0;

static void __cdecl SagaVrXrLog(const char *message) {
    LOG_INFO("%s", message);
}

static f32 SagaVrIniFloat(const char *ini, const char *key, f32 fallback) {
    char text[32];
    GetPrivateProfileStringA("vr", key, "", text, sizeof(text), ini);
    return text[0] != 0 ? (f32)atof(text) : fallback;
}

/* saga_vr.ini ai_trace: comma-separated parts of AI script names whose state changes are logged ("" = none). */
static char SagaVrAiTrace[256] = "Maul,Jedi,Party,Level";

static i32 SagaVrAiTraced(const char *name) {
    const char *part = SagaVrAiTrace;
    char token[64];
    while (name != NULL && *part != 0) {
        i32 length = 0, i, j;
        while (*part == ',' || *part == ' ')
            ++part;
        while (part[length] != 0 && part[length] != ',' && length < (i32)sizeof(token) - 1)
            token[length] = part[length], ++length;
        token[length] = 0;
        part += length;
        if (length == 0)
            continue;
        for (i = 0; name[i] != 0; ++i) {
            for (j = 0; j < length && name[i + j] != 0 && tolower((u8)name[i + j]) == tolower((u8)token[j]); ++j)
                ;
            if (j == length)
                return 1;
        }
    }
    return 0;
}

void __cdecl SagaVrAiState(AISCRIPTPROCESS_PC *processor, AISTATE_PC *state) {
    const char *script = processor->script != NULL ? processor->script->name : NULL;
    if (processor->state == state || !SagaVrAiTraced(script))
        return;
    LOG_INFO("VR AI %s (%p): %s -> %s", script, (void *)processor,
             processor->state != NULL && processor->state->name != NULL ? processor->state->name : "-",
             state->name != NULL ? state->name : "-");
}

void __cdecl SagaVrStereoInit(SAGA_VR_CONTROL *control) {
    char ini[MAX_PATH];
    char *slash;
    char environment[8];
    SAGA_XR_INIT init;
    i32 simulate;
    SagaVrStereoControl = control;
    GetModuleFileNameA(NULL, ini, sizeof(ini));
    slash = strrchr(ini, '\\');
    strcpy(slash != NULL ? slash + 1 : ini, "saga_vr.ini");
    control->stereo_enabled = (i32)SagaVrIniFloat(ini, "stereo", 1.0f);
    control->world_scale = SagaVrIniFloat(ini, "world_scale", 1.0f);
    control->fp_world_scale = SagaVrIniFloat(ini, "fp_world_scale", 0.25f);
    control->swing_speed = SagaVrIniFloat(ini, "swing_speed", 2.5f);
    control->saber_cooldown = SagaVrIniFloat(ini, "saber_cooldown", 0.5f);
    control->aim_assist = SagaVrIniFloat(ini, "aim_assist", 10.0f);
    control->unlock_all = (i32)SagaVrIniFloat(ini, "unlock_all", 0.0f);
    control->blaster_scale = SagaVrIniFloat(ini, "blaster_scale", 0.7f);
    control->blaster_pitch = SagaVrIniFloat(ini, "blaster_pitch", -40.0f);
    control->saber = (i32)SagaVrIniFloat(ini, "saber", 1.0f);
    control->saber_pitch = SagaVrIniFloat(ini, "saber_pitch", 0.0f);
    control->saber_scale = SagaVrIniFloat(ini, "saber_scale", 1.0f);
    control->debug_keys = (i32)SagaVrIniFloat(ini, "debug_keys", 0.0f);
    control->smooth_turn = SagaVrIniFloat(ini, "smooth_turn", 0.0f);
    {
        char trace[sizeof(SagaVrAiTrace)];
        GetPrivateProfileStringA("vr", "ai_trace", SagaVrAiTrace, trace, sizeof(trace), ini);
        strcpy(SagaVrAiTrace, trace);
    }
    control->hand_rotation[0][0] = SagaVrIniFloat(ini, "left_hand_yaw", 0.0f);
    control->hand_rotation[0][1] = SagaVrIniFloat(ini, "left_hand_pitch", 0.0f);
    control->hand_rotation[0][2] = SagaVrIniFloat(ini, "left_hand_roll", 0.0f);
    control->hand_rotation[1][0] = SagaVrIniFloat(ini, "right_hand_yaw", 0.0f);
    control->hand_rotation[1][1] = SagaVrIniFloat(ini, "right_hand_pitch", 0.0f);
    control->hand_rotation[1][2] = SagaVrIniFloat(ini, "right_hand_roll", 0.0f);
    control->first_person = (i32)SagaVrIniFloat(ini, "first_person", 0.0f);
    simulate = (i32)SagaVrIniFloat(ini, "simulate", 0.0f);
    if (GetEnvironmentVariableA("SAGA_VR_SIMULATE", environment, sizeof(environment)) > 0)
        simulate = atoi(environment);
    /* Test runs load the user's save: SAGA_VR_UNLOCK=0 keeps them from unlocking and prompting to save. */
    if (GetEnvironmentVariableA("SAGA_VR_UNLOCK", environment, sizeof(environment)) > 0)
        control->unlock_all = atoi(environment);
    control->xr_mode = SAGA_XR_MODE_OFF;
    SagaVrXrModule = LoadLibraryA("saga_vr.dll");
    if (SagaVrXrModule == NULL) {
        LOG_INFO("VR: saga_vr.dll not found, mono only");
        return;
    }
    init = (SAGA_XR_INIT)GetProcAddress(SagaVrXrModule, "SagaXrInit");
    SagaVrXrBeginFrame = (SAGA_XR_BEGIN_FRAME)GetProcAddress(SagaVrXrModule, "SagaXrBeginFrame");
    SagaVrXrSubmitEye = (SAGA_XR_SUBMIT_EYE)GetProcAddress(SagaVrXrModule, "SagaXrSubmitEye");
    SagaVrXrEndFrame = (SAGA_XR_END_FRAME)GetProcAddress(SagaVrXrModule, "SagaXrEndFrame");
    SagaVrXrRecenter = (SAGA_XR_RECENTER)GetProcAddress(SagaVrXrModule, "SagaXrRecenter");
    SagaVrXrSaveStereo = (SAGA_XR_SAVE_STEREO)GetProcAddress(SagaVrXrModule, "SagaXrSaveStereo");
    SagaVrXrSetSimulatedHead =
        (SAGA_XR_SET_SIMULATED_HEAD)GetProcAddress(SagaVrXrModule, "SagaXrSetSimulatedHead");
    SagaVrXrGetInput = (SAGA_XR_GET_INPUT)GetProcAddress(SagaVrXrModule, "SagaXrGetInput");
    SagaVrXrEndFrameScreen = (SAGA_XR_END_FRAME_SCREEN)GetProcAddress(SagaVrXrModule, "SagaXrEndFrameScreen");
    if (init == NULL || SagaVrXrBeginFrame == NULL || SagaVrXrSubmitEye == NULL || SagaVrXrEndFrame == NULL ||
        SagaVrXrRecenter == NULL || SagaVrXrSaveStereo == NULL) {
        LOG_ERR("VR: saga_vr.dll is missing exports, mono only");
        return;
    }
    control->xr_mode = init(SAGA_XR_API_VERSION, simulate, SagaVrXrLog);
    LOG_INFO("VR: mode %d, stereo %d, world scale %.3f, aim assist %.1f, unlock_all %d", control->xr_mode,
             control->stereo_enabled, control->world_scale, control->aim_assist, control->unlock_all);
}

/* Device creation runs before the XR session exists, so the per-eye render size comes from the ini.
 * The desktop window keeps its size and shows the back buffer scaled. */
void __cdecl SagaVrBackBufferSize(u32 *width, u32 *height, BOOL windowed) {
    char ini[MAX_PATH];
    char *slash;
    i32 render_width, render_height;
    if (!windowed)
        return;
    GetModuleFileNameA(NULL, ini, sizeof(ini));
    slash = strrchr(ini, '\\');
    strcpy(slash != NULL ? slash + 1 : ini, "saga_vr.ini");
    render_width = (i32)SagaVrIniFloat(ini, "render_width", 1600.0f);
    render_height = (i32)SagaVrIniFloat(ini, "render_height", 1712.0f);
    if (render_width < 320 || render_height < 240 || render_width > 4096 || render_height > 4096)
        return;
    LOG_INFO("VR: back buffer %ux%u -> %dx%d per eye", *width, *height, render_width, render_height);
    *width = (u32)render_width;
    *height = (u32)render_height;
}

/* OpenXR pose (right-handed) to a camera-local row-vector matrix (left-handed, game units). */
static void SagaVrPoseMatrix(f32 *out, const f32 *orientation, const f32 *position, f32 scale) {
    f32 x = -orientation[0], y = -orientation[1], z = orientation[2], w = orientation[3];
    out[0] = 1.0f - 2.0f * (y * y + z * z);
    out[1] = 2.0f * (x * y + z * w);
    out[2] = 2.0f * (x * z - y * w);
    out[3] = 0.0f;
    out[4] = 2.0f * (x * y - z * w);
    out[5] = 1.0f - 2.0f * (x * x + z * z);
    out[6] = 2.0f * (y * z + x * w);
    out[7] = 0.0f;
    out[8] = 2.0f * (x * z + y * w);
    out[9] = 2.0f * (y * z - x * w);
    out[10] = 1.0f - 2.0f * (x * x + y * y);
    out[11] = 0.0f;
    out[12] = position[0] * scale;
    out[13] = position[1] * scale;
    out[14] = -position[2] * scale;
    out[15] = 1.0f;
}

static void SagaVrRigidInverse(f32 *out, const f32 *in) {
    i32 row, column;
    for (row = 0; row < 3; ++row) {
        for (column = 0; column < 3; ++column)
            out[row * 4 + column] = in[column * 4 + row];
        out[row * 4 + 3] = 0.0f;
    }
    for (column = 0; column < 3; ++column)
        out[12 + column] =
            -(in[12] * in[column * 4 + 0] + in[13] * in[column * 4 + 1] + in[14] * in[column * 4 + 2]);
    out[15] = 1.0f;
}

static f32 SagaVrWorldScale(const SAGA_VR_CONTROL *control) {
    /* Ship and speeder levels are third person even with first person on (see SagaVrFirstPersonCamera). */
    if (VehicleArea != 0 && !SagaVrOnPod((GAMEOBJECT *)Player[0]))
        return control->world_scale;
    return control->first_person ? control->fp_world_scale : control->world_scale;
}

i32 __cdecl SagaVrStereoHead(f32 *head, f32 *fov, f32 *aspect) {
    SAGA_VR_CONTROL *control = SagaVrStereoControl;
    f32 centre[3];
    f32 vertical = 0.0f, horizontal = 0.0f;
    i32 eye, side;
    if (control == NULL || control->xr_mode == SAGA_XR_MODE_OFF || !control->stereo_enabled || !SagaVrHaveViews ||
        SagaVrCinema())
        return 0;
    for (side = 0; side < 3; ++side)
        centre[side] = 0.5f * (SagaVrViews[0].position[side] + SagaVrViews[1].position[side]);
    SagaVrPoseMatrix(head, SagaVrViews[0].orientation, centre, SagaVrWorldScale(control));
    for (eye = 0; eye < 2; ++eye) {
        for (side = 0; side < 2; ++side) {
            f32 h = (f32)fabs(SagaVrViews[eye].fov[side]);
            f32 v = (f32)fabs(SagaVrViews[eye].fov[2 + side]);
            if (h > horizontal)
                horizontal = h;
            if (v > vertical)
                vertical = v;
        }
    }
    vertical += SAGA_VR_CULL_MARGIN;
    horizontal += SAGA_VR_CULL_MARGIN;
    if (vertical > 1.45f)
        vertical = 1.45f;
    if (horizontal > 1.45f)
        horizontal = 1.45f;
    *fov = 2.0f * vertical;
    *aspect = (f32)(tan(vertical) / tan(horizontal));
    memcpy(SagaVrHeadAtUpdate, head, sizeof(SagaVrHeadAtUpdate));
    SagaVrHeadApplied = 1;
    return 1;
}

/* Swing to attack: a fast right-hand motion holds the attack button briefly. The original combat
 * then plays its own swing and hit detection; nothing about the hand's path is simulated. */
static void SagaVrDetectSwing(SAGA_VR_CONTROL *control) {
    static LARGE_INTEGER previous_time;
    static f32 previous_position[3];
    static i32 have_previous = 0;
    LARGE_INTEGER now, frequency;
    f32 dx, dy, dz, seconds;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (!SagaVrInput.hand_valid[1]) {
        have_previous = 0;
        control->hand_speed = 0.0f;
        return;
    }
    if (have_previous) {
        seconds = (f32)(now.QuadPart - previous_time.QuadPart) / (f32)frequency.QuadPart;
        if (seconds > 0.001f && seconds < 0.25f) {
            dx = SagaVrInput.hand_position[1][0] - previous_position[0];
            dy = SagaVrInput.hand_position[1][1] - previous_position[1];
            dz = SagaVrInput.hand_position[1][2] - previous_position[2];
            control->hand_speed = (f32)sqrt(dx * dx + dy * dy + dz * dz) / seconds;
            if (control->swing_speed > 0.0f && control->hand_speed > control->swing_speed) {
                if (SagaVrSwingUntil.QuadPart < now.QuadPart)
                    ++control->swings;
                SagaVrSwingUntil.QuadPart = now.QuadPart + frequency.QuadPart * 15 / 100;
            }
        }
    }
    previous_time = now;
    memcpy(previous_position, SagaVrInput.hand_position[1], sizeof(previous_position));
    have_previous = 1;
}

static void SagaVrReadControllers(SAGA_VR_CONTROL *control) {
    i32 hand;
    if (SagaVrXrGetInput == NULL) {
        memset(&SagaVrInput, 0, sizeof(SagaVrInput));
        return;
    }
    SagaVrXrGetInput(&SagaVrInput);
    if (control->xr_mode == SAGA_XR_MODE_SIMULATED && control->simulated_hand_active) {
        /* Test hook: a right hand posed from the control block (yaw about +Y, then pitch about +X). */
        f32 cy = (f32)cos(0.5f * control->simulated_hand[0] * 0.017453293f);
        f32 sy = (f32)sin(0.5f * control->simulated_hand[0] * 0.017453293f);
        f32 cp = (f32)cos(0.5f * control->simulated_hand[1] * 0.017453293f);
        f32 sp = (f32)sin(0.5f * control->simulated_hand[1] * 0.017453293f);
        SagaVrInput.active = 1;
        SagaVrInput.hand_valid[1] = 1;
        SagaVrInput.hand_orientation[1][0] = cy * sp;
        SagaVrInput.hand_orientation[1][1] = sy * cp;
        SagaVrInput.hand_orientation[1][2] = -sy * sp;
        SagaVrInput.hand_orientation[1][3] = cy * cp;
        /* And a roll about the hand's own length (+Z, towards the wrist). */
        if (control->simulated_hand_roll != 0.0f) {
            f32 cr = (f32)cos(0.5f * control->simulated_hand_roll * 0.017453293f);
            f32 sr = (f32)sin(0.5f * control->simulated_hand_roll * 0.017453293f);
            f32 *q = SagaVrInput.hand_orientation[1], x = q[0], y = q[1], z = q[2], w = q[3];
            q[0] = x * cr + y * sr;
            q[1] = y * cr - x * sr;
            q[2] = z * cr + w * sr;
            q[3] = w * cr - z * sr;
        }
        memcpy(SagaVrInput.hand_position[1], &control->simulated_hand[2], sizeof(SagaVrInput.hand_position[1]));
        /* The left hand mirrors the right across the head's x = 0 plane (yaw and x negated). */
        SagaVrInput.hand_valid[0] = 1;
        SagaVrInput.hand_orientation[0][0] = SagaVrInput.hand_orientation[1][0];
        SagaVrInput.hand_orientation[0][1] = -SagaVrInput.hand_orientation[1][1];
        SagaVrInput.hand_orientation[0][2] = -SagaVrInput.hand_orientation[1][2];
        SagaVrInput.hand_orientation[0][3] = SagaVrInput.hand_orientation[1][3];
        SagaVrInput.hand_position[0][0] = -SagaVrInput.hand_position[1][0];
        SagaVrInput.hand_position[0][1] = SagaVrInput.hand_position[1][1];
        SagaVrInput.hand_position[0][2] = SagaVrInput.hand_position[1][2];
        SagaVrInput.buttons |= control->simulated_buttons;
    }
    SagaVrDetectSwing(control);
    control->controllers_active = SagaVrInput.active;
    control->controller_buttons = SagaVrInput.buttons;
    for (hand = 0; hand < 2; ++hand) {
        control->hand_valid[hand] = SagaVrInput.hand_valid[hand];
        memcpy(control->hand_position[hand], SagaVrInput.hand_position[hand], sizeof(control->hand_position[hand]));
    }
    /* Right stick click recentres, right grip toggles the lightsaber; both edge-triggered. */
    if ((SagaVrInput.buttons & SAGA_XR_BUTTON_RECENTER) && !(SagaVrPreviousButtons & SAGA_XR_BUTTON_RECENTER))
        ++control->recenter_request;
    if ((SagaVrInput.buttons & SAGA_XR_BUTTON_SABER) && !(SagaVrPreviousButtons & SAGA_XR_BUTTON_SABER) &&
        !SagaVrOnPod((GAMEOBJECT *)Player[0])) {
        control->saber_lit = !control->saber_lit;
        LOG_INFO("VR lightsaber %s", control->saber_lit ? "on" : "off");
    }
    SagaVrPreviousButtons = SagaVrInput.buttons;
}

i32 __cdecl SagaVrControllerInput(f32 *move, f32 *look, u32 *buttons) {
    LARGE_INTEGER now;
    if (!SagaVrInput.active)
        return 0;
    move[0] = SagaVrInput.move[0];
    move[1] = SagaVrInput.move[1];
    look[0] = SagaVrInput.look[0];
    look[1] = SagaVrInput.look[1];
    *buttons = SagaVrInput.buttons;
    /* With the motion saber the swing itself deals the hits; otherwise it triggers the attack button. */
    QueryPerformanceCounter(&now);
    if (now.QuadPart < SagaVrSwingUntil.QuadPart &&
        !(SagaVrStereoControl != NULL && SagaVrStereoControl->saber && SagaVrFirstPersonActive()))
        *buttons |= SAGA_XR_BUTTON_ATTACK;
    return 1;
}

f32 __cdecl SagaVrHeadYaw(void) {
    f32 head[16];
    static const f32 origin[3] = {0.0f, 0.0f, 0.0f};
    if (!SagaVrHaveViews || SagaVrStereoControl == NULL || SagaVrStereoControl->xr_mode == SAGA_XR_MODE_OFF)
        return 0.0f;
    SagaVrPoseMatrix(head, SagaVrViews[0].orientation, origin, 1.0f);
    return (f32)atan2(head[8], head[10]);
}

i32 __cdecl SagaVrSwinging(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart < SagaVrSwingUntil.QuadPart;
}

i32 __cdecl SagaVrHandTracked(i32 hand, f32 *out) {
    if (hand < 0 || hand > 1 || !SagaVrInput.hand_valid[hand])
        return 0;
    SagaVrPoseMatrix(out, SagaVrInput.hand_orientation[hand], SagaVrInput.hand_position[hand], 1.0f);
    return 1;
}

i32 __cdecl SagaVrHandWorld(i32 hand, f32 *out) {
    const f32 *rig = SagaVrGetRig();
    f32 local[16], pitch[16], tilted[16], c, s;
    if (rig == NULL || hand < 0 || hand > 1 || !SagaVrInput.hand_valid[hand] || SagaVrStereoControl == NULL)
        return 0;
    SagaVrPoseMatrix(local, SagaVrInput.hand_orientation[hand], SagaVrInput.hand_position[hand],
                     SagaVrWorldScale(SagaVrStereoControl));
    /* Optional blade tilt about the grip's x axis (row-vector: applied before the grip rotation). */
    if (SagaVrStereoControl->saber_pitch != 0.0f) {
        c = (f32)cos(SagaVrStereoControl->saber_pitch * 0.017453293f);
        s = (f32)sin(SagaVrStereoControl->saber_pitch * 0.017453293f);
        memset(pitch, 0, sizeof(pitch));
        pitch[0] = 1.0f, pitch[5] = c, pitch[6] = -s, pitch[9] = s, pitch[10] = c, pitch[15] = 1.0f;
        SagaVrMul(tilted, pitch, local);
        memcpy(local, tilted, sizeof(local));
    }
    SagaVrMul(out, local, rig);
    return 1;
}

/* RanitaVR credit plate: after the legal screen, before "A long time ago in a galaxy far, far away....", the headset shows
 * RanitaVR.bmp (next to the executable; 32-bit, uncompressed) on a screen for SAGA_VR_PLATE_SECONDS. */
#define SAGA_VR_PLATE_SECONDS 5.0
static i32 SagaVrPlateState = 0; /* 0 waiting for the legal screen to end, 1 showing, 2 done or unavailable */
static i32 SagaVrPlateFrame = 0;
static LARGE_INTEGER SagaVrPlateStart;
static u8 *SagaVrPlatePixels; /* top-down BGRA */
static i32 SagaVrPlateWidth, SagaVrPlateHeight;

static i32 SagaVrLoadPlate(void) {
    char path[MAX_PATH], *slash;
    u8 header[54];
    FILE *file;
    i32 offset, width, height, bits, compression, y;
    u8 *rows;
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    strcpy(slash != NULL ? slash + 1 : path, "RanitaVR.bmp");
    file = fopen(path, "rb");
    if (file == NULL)
        return 0;
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || header[0] != 'B' || header[1] != 'M') {
        fclose(file);
        return 0;
    }
    offset = *(i32 *)&header[10];
    width = *(i32 *)&header[18];
    height = *(i32 *)&header[22];
    bits = *(u16 *)&header[28];
    compression = *(i32 *)&header[30];
    if (bits != 32 || compression != 0 || width <= 0 || width > 4096 || height == 0 || height > 4096 ||
        height < -4096) {
        fclose(file);
        LOG_ERR("VR plate: %s is not a 32-bit uncompressed BMP", path);
        return 0;
    }
    rows = (u8 *)malloc((size_t)width * (height < 0 ? -height : height) * 4);
    fseek(file, offset, SEEK_SET);
    if (rows == NULL ||
        fread(rows, 1, (size_t)width * (height < 0 ? -height : height) * 4, file) !=
            (size_t)width * (height < 0 ? -height : height) * 4) {
        fclose(file);
        free(rows);
        return 0;
    }
    fclose(file);
    SagaVrPlateWidth = width;
    SagaVrPlateHeight = height < 0 ? -height : height;
    SagaVrPlatePixels = (u8 *)malloc((size_t)width * SagaVrPlateHeight * 4);
    if (SagaVrPlatePixels == NULL) {
        free(rows);
        return 0;
    }
    /* Positive height means bottom-up rows. */
    for (y = 0; y < SagaVrPlateHeight; ++y)
        memcpy(SagaVrPlatePixels + (size_t)y * width * 4,
               rows + (size_t)(height > 0 ? SagaVrPlateHeight - 1 - y : y) * width * 4, (size_t)width * 4);
    free(rows);
    LOG_INFO("VR plate: %s %dx%d", path, width, SagaVrPlateHeight);
    return 1;
}

/* Set by SagaVrIntroGate once the legal screen is over. */
static i32 SagaVrPlateRequested = 0;

/* Whether this frame shows the plate instead of the game. */
static i32 SagaVrPlateActive(void) {
    LARGE_INTEGER now, frequency;
    if (SagaVrPlateState == 0) {
        if (!SagaVrPlateRequested)
            return 0;
        if (!SagaVrLoadPlate()) {
            SagaVrPlateState = 2;
            return 0;
        }
        QueryPerformanceCounter(&SagaVrPlateStart);
        SagaVrPlateState = 1;
        LOG_INFO("VR plate: shown (Developed by RanitaVR)");
    }
    if (SagaVrPlateState != 1)
        return 0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if ((f64)(now.QuadPart - SagaVrPlateStart.QuadPart) / (f64)frequency.QuadPart < SAGA_VR_PLATE_SECONDS)
        return 1;
    SagaVrPlateState = 2;
    free(SagaVrPlatePixels);
    SagaVrPlatePixels = NULL;
    LOG_INFO("VR plate: done");
    return 0;
}

/* Start-up loop, each frame once the legal screen is over: the plate shows first, then the game's
 * "A long time ago in a galaxy far, far away...." may start (returns 1 when it may). */
i32 __cdecl SagaVrIntroGate(void) {
    SagaVrPlateRequested = 1;
    SagaVrPlateActive();
    return SagaVrPlateState == 2;
}

i32 __cdecl SagaVrStereoBegin(void) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    i32 count;
    if (control->xr_mode == SAGA_XR_MODE_OFF)
        return 1;
    if (control->recenter_request != SagaVrRecenterSeen) {
        SagaVrRecenterSeen = control->recenter_request;
        SagaVrXrRecenter();
    }
    if (control->xr_mode == SAGA_XR_MODE_SIMULATED && SagaVrXrSetSimulatedHead != NULL) {
        /* q = yaw about +Y, then pitch about +X (OpenXR axes). */
        f32 cy = (f32)cos(0.5f * control->simulated_head[0]), sy = (f32)sin(0.5f * control->simulated_head[0]);
        f32 cp = (f32)cos(0.5f * control->simulated_head[1]), sp = (f32)sin(0.5f * control->simulated_head[1]);
        f32 orientation[4];
        orientation[0] = cy * sp;
        orientation[1] = sy * cp;
        orientation[2] = -sy * sp;
        orientation[3] = cy * cp;
        SagaVrXrSetSimulatedHead(orientation, &control->simulated_head[2]);
    }
    if (!SagaVrXrBeginFrame(SagaVrViews))
        return 1;
    SagaVrHaveViews = 1;
    SagaVrReadControllers(control);
    control->head_position[0] = 0.5f * (SagaVrViews[0].position[0] + SagaVrViews[1].position[0]);
    control->head_position[1] = 0.5f * (SagaVrViews[0].position[1] + SagaVrViews[1].position[1]);
    control->head_position[2] = 0.5f * (SagaVrViews[0].position[2] + SagaVrViews[1].position[2]);
    if (!control->stereo_enabled) {
        /* Keep the session paced but submit nothing: the headset shows the runtime's idle view. */
        SagaVrXrEndFrame();
        return 1;
    }
    if (SagaVrPlateActive()) {
        /* The credit plate replaces the view; the game keeps running behind it in mono. */
        SagaVrPlateFrame = 1;
        return 1;
    }
    if (SagaVrCinema()) {
        /* Cutscene: render mono with the cutscene's own camera; StereoEnd shows it on a screen. */
        SagaVrScreenFrame = 1;
        return 1;
    }
    count = sceneParametersCount;
    if (count > SAGA_VR_MAX_SCENES)
        count = SAGA_VR_MAX_SCENES;
    memcpy(SagaVrSceneSnapshot, sceneParameters, sizeof(NURNDRLIBSCENE) * (u32)count);
    SagaVrSceneSnapshotCount = count;
    ++control->stereo_frames;
    return 2;
}

void __cdecl SagaVrStereoEye(i32 eye, i32 eyes) {
    if (eyes < 2) {
        SagaVrCurrentEye = -1;
        return;
    }
    {
        static i32 swap = -1;
        if (swap < 0)
            swap = getenv("SAGA_VR_SWAP_EYES") != NULL ? 1 : 0;
        SagaVrSwap = swap;
    }
    SagaVrCurrentEye = eye ^ SagaVrSwap;
    if (eye > 0) {
        /* The replay consumes per-scene state (effects, shadow registration); give each eye the same input.
         * Display-list packets that a handler changes while drawing need the same care (SAGA_VR_FACEON_COPY). */
        memcpy(sceneParameters, SagaVrSceneSnapshot, sizeof(NURNDRLIBSCENE) * (u32)SagaVrSceneSnapshotCount);
        sceneParametersCount = SagaVrSceneSnapshotCount;
    }
    NuDisplayHandlerCameraIdPc = SAGA_VR_FORCE_CAMERA_ID;
}

/* Copies the finished back buffer to system memory and hands it to the XR module.
 * IDirect3DDevice9 slots: GetBackBuffer 18, GetRenderTargetData 32, CreateOffscreenPlainSurface 36;
 * IDirect3DSurface9 slots: Release 2, GetDesc 12, LockRect 13, UnlockRect 14. */
static void SagaVrSubmitBackBuffer(i32 eye) {
    typedef HRESULT(__stdcall * GET_BACK_BUFFER)(void *, UINT, UINT, i32, void **);
    typedef HRESULT(__stdcall * GET_RENDER_TARGET_DATA)(void *, void *, void *);
    typedef HRESULT(__stdcall * CREATE_OFFSCREEN)(void *, UINT, UINT, i32, i32, void **, void *);
    typedef ULONG(__stdcall * RELEASE)(void *);
    typedef HRESULT(__stdcall * GET_DESC)(void *, SAGA_VR_SURFACE_DESC *);
    typedef HRESULT(__stdcall * LOCK_RECT)(void *, SAGA_VR_LOCKED_RECT *, const RECT *, DWORD);
    typedef HRESULT(__stdcall * UNLOCK_RECT)(void *);
    static i32 reported = 0;
    void *device = PCDeviceBackend.device;
    void **methods;
    void *back_buffer = NULL;
    SAGA_VR_SURFACE_DESC desc;
    SAGA_VR_LOCKED_RECT locked;
    HRESULT result;
    if (device == NULL)
        return;
    methods = *(void ***)device;
    if (((GET_BACK_BUFFER)methods[18])(device, 0, 0, 0, &back_buffer) < 0)
        return;
    ((GET_DESC)(*(void ***)back_buffer)[12])(back_buffer, &desc);
    if (SagaVrCopySurface != NULL && (SagaVrCopyDesc.width != desc.width || SagaVrCopyDesc.height != desc.height ||
                                      SagaVrCopyDesc.format != desc.format)) {
        ((RELEASE)(*(void ***)SagaVrCopySurface)[2])(SagaVrCopySurface);
        SagaVrCopySurface = NULL;
    }
    if (SagaVrCopySurface == NULL) {
        result = ((CREATE_OFFSCREEN)methods[36])(device, desc.width, desc.height, desc.format, 2 /* SYSTEMMEM */,
                                                 &SagaVrCopySurface, NULL);
        if (result < 0) {
            LOG_ERR("VR: CreateOffscreenPlainSurface %ux%u format %d failed 0x%08x", desc.width, desc.height,
                    desc.format, (u32)result);
            SagaVrCopySurface = NULL;
        } else {
            SagaVrCopyDesc = desc;
            LOG_INFO("VR: eye copy surface %ux%u format %d", desc.width, desc.height, desc.format);
        }
    }
    if (SagaVrCopySurface != NULL) {
        result = ((GET_RENDER_TARGET_DATA)methods[32])(device, back_buffer, SagaVrCopySurface);
        if (result >= 0 &&
            ((LOCK_RECT)(*(void ***)SagaVrCopySurface)[13])(SagaVrCopySurface, &locked, NULL, 0x10) >= 0) {
            SagaVrXrSubmitEye(eye, locked.bits, locked.pitch, (i32)desc.width, (i32)desc.height);
            ((UNLOCK_RECT)(*(void ***)SagaVrCopySurface)[14])(SagaVrCopySurface);
        } else if (!reported) {
            reported = 1;
            LOG_ERR("VR: GetRenderTargetData failed 0x%08x", (u32)result);
        }
    }
    ((RELEASE)(*(void ***)back_buffer)[2])(back_buffer);
}

void __cdecl SagaVrStereoEyeDone(i32 eye, i32 eyes) {
    if (eyes < 2)
        return;
    SagaVrSubmitBackBuffer(eye ^ SagaVrSwap);
}

/* A flat screen in the headset: 3.2 m wide at 4:3 for cutscenes (the game's camera aspect). */
static void SagaVrShowScreen(f32 width, f32 aspect) {
    SagaVrSubmitBackBuffer(0);
    if (SagaVrXrEndFrameScreen != NULL)
        SagaVrXrEndFrameScreen(width, aspect);
    else
        SagaVrXrEndFrame();
}

void __cdecl SagaVrStereoEnd(i32 eyes) {
    SagaVrCurrentEye = -1;
    if (eyes == 2)
        SagaVrXrEndFrame();
    else if (SagaVrPlateFrame && SagaVrPlatePixels != NULL) {
        SagaVrXrSubmitEye(0, SagaVrPlatePixels, SagaVrPlateWidth * 4, SagaVrPlateWidth, SagaVrPlateHeight);
        if (SagaVrXrEndFrameScreen != NULL)
            SagaVrXrEndFrameScreen(3.0f, (f32)SagaVrPlateWidth / (f32)SagaVrPlateHeight);
        else
            SagaVrXrEndFrame();
    } else if (SagaVrScreenFrame)
        SagaVrShowScreen(3.2f, 4.0f / 3.0f);
    SagaVrPlateFrame = 0;
    SagaVrScreenFrame = 0;
    SagaVrHeadApplied = 0;
}

void __cdecl SagaVrMovieFrame(void) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    if (control->xr_mode != SAGA_XR_MODE_HEADSET || SagaVrXrBeginFrame == NULL)
        return;
    if (!SagaVrXrBeginFrame(SagaVrViews))
        return;
    /* Movies are 16:9 (1280x720). */
    SagaVrShowScreen(3.6f, 16.0f / 9.0f);
}

/* The game draws its overlays (texts, prompts, hearts, studs, portraits) in screen space, which in the headset
 * sticks them to the view and pushes the corners out of sight. In stereo each eye instead sees them on a panel
 * fixed in the room: SAGA_VR_HUD_DISTANCE ahead of the recentred head, facing it, SAGA_VR_HUD_WIDTH wide and
 * with the eye image's proportions (the game lays the overlay out for that image). Two paths draw overlays:
 * 2D primitives already in screen coordinates (SagaVrHudVertices) and 3D models under an orthographic camera
 * (the portraits; SagaVrComputeEye). Primitives covering the whole screen (fades, filters) stay over the view. */
#define SAGA_VR_HUD_DISTANCE 2.0f
#define SAGA_VR_HUD_WIDTH 2.2f
#define SAGA_VR_HUD_MAX_VERTICES 16384

typedef struct SAGA_VR_HUD_VERTEX {
    f32 x, y, z;
    u32 colour;
    f32 u, v;
} SAGA_VR_HUD_VERTEX;

static SAGA_VR_HUD_VERTEX SagaVrHudBuffer[SAGA_VR_HUD_MAX_VERTICES];

/* Test aid (vr_control hide 10000000): the overlay as the game draws it, stuck to the view. */
static i32 SagaVrHudFixed(void) {
    return SagaVrStereoControl == NULL || !(SagaVrStereoControl->vehicle_hide & 0x10000000u);
}

/* The panel seen from an eye: overlay x, y (-1..1) is the point x*axes[0] + y*axes[1] + axes[2] in the eye's
 * space (x right, y up, -z ahead); tangents of the eye's field of view (left, right, up, down). */
static void SagaVrHudPanel(const SAGA_XR_VIEW *eye, f32 axes[3][3], f32 *tangents) {
    f32 room[3][3], t[3];
    f32 qx = -eye->orientation[0], qy = -eye->orientation[1], qz = -eye->orientation[2], qw = eye->orientation[3];
    f32 half_w = 0.5f * SAGA_VR_HUD_WIDTH;
    f32 half_h = half_w * (SagaVrCopyDesc.width > 0 ? (f32)SagaVrCopyDesc.height / (f32)SagaVrCopyDesc.width : 1.07f);
    i32 k;
    memset(room, 0, sizeof(room));
    room[0][0] = half_w;
    room[1][1] = half_h;
    room[2][0] = -eye->position[0], room[2][1] = -eye->position[1];
    room[2][2] = -SAGA_VR_HUD_DISTANCE - eye->position[2];
    /* Rotated by the inverse of the eye's orientation. */
    for (k = 0; k < 3; ++k) {
        const f32 *p = room[k];
        t[0] = 2.0f * (qy * p[2] - qz * p[1]);
        t[1] = 2.0f * (qz * p[0] - qx * p[2]);
        t[2] = 2.0f * (qx * p[1] - qy * p[0]);
        axes[k][0] = p[0] + qw * t[0] + (qy * t[2] - qz * t[1]);
        axes[k][1] = p[1] + qw * t[1] + (qz * t[0] - qx * t[2]);
        axes[k][2] = p[2] + qw * t[2] + (qx * t[1] - qy * t[0]);
    }
    for (k = 0; k < 4; ++k)
        tangents[k] = (f32)tan(eye->fov[k]);
}

/* Clip x, y and w of an eye-space vector (D3D row-vector order, z left alone). */
static void SagaVrHudClip(const f32 *v, const f32 *tangents, f32 *row) {
    row[0] = (2.0f * v[0] + (tangents[1] + tangents[0]) * v[2]) / (tangents[1] - tangents[0]);
    row[1] = (2.0f * v[1] + (tangents[2] + tangents[3]) * v[2]) / (tangents[2] - tangents[3]);
    row[3] = -v[2];
}

/* The HUD's 3D pieces (portraits, hearts, icons) are models placed in front of the game camera's matrix.
 * Placed in front of the camera without the head (the rig) through this matrix instead, they sit on the same
 * fixed panel: a point the head-locked eye saw at overlay x, y lands on the panel there, and the usual depth
 * (2 units ahead) at the panel's distance. Row-vector, game units; 0 outside stereo gameplay. */
i32 __cdecl SagaVrHudShear(f32 *out) {
    f32 tangents[4], half_w, half_h, k;
    i32 i;
    if (!SagaVrHaveViews || !SagaVrHeadApplied || SagaVrStereoControl == NULL || !SagaVrHudFixed() || SagaVrCinema())
        return 0;
    for (i = 0; i < 4; ++i)
        tangents[i] = 0.5f * (f32)(tan(SagaVrViews[0].fov[i]) + tan(SagaVrViews[1].fov[i]));
    half_w = 0.5f * SAGA_VR_HUD_WIDTH;
    half_h = half_w * (SagaVrCopyDesc.width > 0 ? (f32)SagaVrCopyDesc.height / (f32)SagaVrCopyDesc.width : 1.07f);
    k = SAGA_VR_HUD_DISTANCE * SagaVrWorldScale(SagaVrStereoControl) * 0.5f;
    memset(out, 0, sizeof(f32) * 16);
    out[0] = k * half_w / SAGA_VR_HUD_DISTANCE * 2.0f / (tangents[1] - tangents[0]);
    out[8] = -k * half_w / SAGA_VR_HUD_DISTANCE * (tangents[1] + tangents[0]) / (tangents[1] - tangents[0]);
    out[5] = k * half_h / SAGA_VR_HUD_DISTANCE * 2.0f / (tangents[2] - tangents[3]);
    out[9] = -k * half_h / SAGA_VR_HUD_DISTANCE * (tangents[2] + tangents[3]) / (tangents[2] - tangents[3]);
    out[10] = k;
    out[15] = 1.0f;
    return 1;
}

static i32 SagaVrIsOrthographic(const NUMTX *projection) {
    return projection->m23 == 0.0f && projection->m33 == 1.0f && projection->m22 == 1.0f && projection->m32 == 0.0f &&
           projection->m03 == 0.0f && projection->m13 == 0.0f;
}

/* Re-derives a recorded camera for the current eye: perspective world cameras get the eye's pose and field of
 * view, orthographic overlay cameras the HUD panel. */
static i32 SagaVrComputeEye(NUMTX *view, NUMTX *projection) {
    const SAGA_XR_VIEW *eye;
    f32 eye_matrix[16], eye_inverse[16], base[16];
    f32 left, right, up, down, near_clip, far_clip;
    if (SagaVrCurrentEye >= 0 && SagaVrHudFixed() && SagaVrIsOrthographic(projection)) {
        f32 axes[3][3], tangents[4], panel[16];
        SagaVrHudPanel(&SagaVrViews[SagaVrCurrentEye], axes, tangents);
        memset(panel, 0, sizeof(panel));
        SagaVrHudClip(axes[0], tangents, panel + 0);
        SagaVrHudClip(axes[1], tangents, panel + 4);
        /* Depth keeps the overlay's own order: its z times about the w of the panel. */
        panel[10] = SAGA_VR_HUD_DISTANCE;
        SagaVrHudClip(axes[2], tangents, panel + 12);
        SagaVrEyeViewMatrix = *view;
        SagaVrMul(&SagaVrEyeProjectionMatrix.m00, &projection->m00, panel);
        return 1;
    }
    if (SagaVrCurrentEye < 0 || projection->m23 < 0.5f || projection->m22 == 1.0f || projection->m22 == 0.0f)
        return 0;
    eye = &SagaVrViews[SagaVrCurrentEye];
    SagaVrPoseMatrix(eye_matrix, eye->orientation, eye->position, SagaVrWorldScale(SagaVrStereoControl));
    SagaVrRigidInverse(eye_inverse, eye_matrix);
    if (SagaVrHeadApplied) {
        SagaVrMul(base, &view->m00, SagaVrHeadAtUpdate);
        SagaVrMul(&SagaVrEyeViewMatrix.m00, base, eye_inverse);
    } else {
        SagaVrMul(&SagaVrEyeViewMatrix.m00, &view->m00, eye_inverse);
    }
    /* Recover the packet's clip planes from its D3D projection: m22 = f/(f-n), m32 = -n*f/(f-n). */
    near_clip = -projection->m32 / projection->m22;
    far_clip = projection->m22 * near_clip / (projection->m22 - 1.0f);
    left = (f32)tan(eye->fov[0]);
    right = (f32)tan(eye->fov[1]);
    up = (f32)tan(eye->fov[2]);
    down = (f32)tan(eye->fov[3]);
    memset(&SagaVrEyeProjectionMatrix, 0, sizeof(SagaVrEyeProjectionMatrix));
    SagaVrEyeProjectionMatrix.m00 = 2.0f / (right - left);
    SagaVrEyeProjectionMatrix.m11 = 2.0f / (up - down);
    SagaVrEyeProjectionMatrix.m20 = -(right + left) / (right - left);
    SagaVrEyeProjectionMatrix.m21 = -(up + down) / (up - down);
    SagaVrEyeProjectionMatrix.m22 = far_clip / (far_clip - near_clip);
    SagaVrEyeProjectionMatrix.m23 = 1.0f;
    SagaVrEyeProjectionMatrix.m32 = -near_clip * far_clip / (far_clip - near_clip);
    return 1;
}

const void *__cdecl SagaVrHudVertices(const void *vertices, u32 count) {
    const SAGA_VR_HUD_VERTEX *in = (const SAGA_VR_HUD_VERTEX *)vertices;
    f32 min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f, axes[3][3], tangents[4];
    u32 i;
    i32 behind = 0;
    if (SagaVrCurrentEye < 0 || !SagaVrHaveViews || !SagaVrHudFixed() || count == 0 || count > SAGA_VR_HUD_MAX_VERTICES)
        return vertices;
    for (i = 0; i < count; ++i) {
        min_x = in[i].x < min_x ? in[i].x : min_x, max_x = in[i].x > max_x ? in[i].x : max_x;
        min_y = in[i].y < min_y ? in[i].y : min_y, max_y = in[i].y > max_y ? in[i].y : max_y;
    }
    if (max_x - min_x > 1.9f && max_y - min_y > 1.9f)
        return vertices;
    SagaVrHudPanel(&SagaVrViews[SagaVrCurrentEye], axes, tangents);
    for (i = 0; i < count; ++i) {
        f32 local[3], clip[4];
        i32 k;
        for (k = 0; k < 3; ++k)
            local[k] = in[i].x * axes[0][k] + in[i].y * axes[1][k] + axes[2][k];
        SagaVrHudBuffer[i] = in[i];
        if (local[2] > -0.05f) {
            behind = 1;
            break;
        }
        SagaVrHudClip(local, tangents, clip);
        SagaVrHudBuffer[i].x = clip[0] / clip[3];
        SagaVrHudBuffer[i].y = clip[1] / clip[3];
    }
    /* Looking away from the panel: nothing of this primitive is drawn (all its vertices on one point). */
    if (behind)
        for (i = 0; i < count; ++i) {
            SagaVrHudBuffer[i] = in[i];
            SagaVrHudBuffer[i].x = SagaVrHudBuffer[i].y = 2.0f;
        }
    return SagaVrHudBuffer;
}

NUMTX *__cdecl SagaVrEyeView(NUMTX *view, NUMTX *projection) {
    return SagaVrComputeEye(view, projection) ? &SagaVrEyeViewMatrix : view;
}

NUMTX *__cdecl SagaVrEyeProjection(NUMTX *view, NUMTX *projection) {
    return SagaVrComputeEye(view, projection) ? &SagaVrEyeProjectionMatrix : projection;
}

i32 __cdecl SagaVrStereoSave(const char *path) {
    if (SagaVrXrSaveStereo == NULL)
        return 0;
    return SagaVrXrSaveStereo(path);
}

/* Headset sessions crashed in the background level loader on a NULL scene buffer; record why and keep
 * the level loading: retry the managed buffer, then fall back to system memory (slower, still drawable). */
void __cdecl SagaVrSceneBuffer(PC_BUFFER_RESOURCE_PC **resource, u32 size, i32 is_index) {
    MEMORYSTATUSEX memory;
    i32 attempt, pool;
    if (*resource != NULL)
        return;
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    LOG_ERR("VR: scene %s buffer of %u bytes failed 0x%08x (address space used %u of %u MB, thread %lu)",
            is_index ? "index" : "vertex", size, (u32)PCDeviceLastResult,
            (u32)((memory.ullTotalVirtual - memory.ullAvailVirtual) >> 20), (u32)(memory.ullTotalVirtual >> 20),
            (unsigned long)GetCurrentThreadId());
    for (attempt = 0; attempt < 6 && *resource == NULL; ++attempt) {
        pool = attempt < 3 ? 1 /* MANAGED */ : 2 /* SYSTEMMEM */;
        Sleep(20);
        if (is_index)
            PCDeviceCreateIndexBuffer(size, 0, 0x65, pool, resource, NULL);
        else
            PCDeviceCreateVertexBuffer(size, 0, 0, pool, resource);
        if (*resource == NULL)
            LOG_ERR("VR: retry %d (pool %d) failed 0x%08x", attempt + 1, pool, (u32)PCDeviceLastResult);
        else
            LOG_INFO("VR: scene buffer created on retry %d (pool %d)", attempt + 1, pool);
    }
}

/* Copies an AI script as the game loads it (level path + name) to log\ai_scripts next to the executable, once per
 * file: the scripts live inside the game's .DAT files, and a stalled character (Maul's last platform) needs the
 * conditions its state waits for. */
void __cdecl SagaVrAiDump(const char *path, const char *name, const void *data, i32 size) {
    char file_path[MAX_PATH], *slash, *p;
    FILE *file;
    if (data == NULL || size <= 0 || name == NULL)
        return;
    GetModuleFileNameA(NULL, file_path, sizeof(file_path));
    slash = strrchr(file_path, '\\');
    strcpy(slash != NULL ? slash + 1 : file_path, "log\\ai_scripts");
    CreateDirectoryA(file_path, NULL);
    strcat(file_path, "\\");
    p = file_path + strlen(file_path);
    _snprintf(p, sizeof(file_path) - (p - file_path) - 1, "%s_%s", path != NULL ? path : "", name);
    file_path[sizeof(file_path) - 1] = 0;
    for (; *p != 0; ++p)
        if (*p == '\\' || *p == '/' || *p == ':')
            *p = '_';
    if (GetFileAttributesA(file_path) != INVALID_FILE_ATTRIBUTES)
        return;
    file = fopen(file_path, "wb");
    if (file == NULL)
        return;
    fwrite(data, 1, (size_t)size, file);
    fclose(file);
}

/* Trigger sets (two-pad puzzles and the like): logs a set whenever its occupied pads, completion or helper
 * assignments change, with the characters assigned (object index, character id) and the player's position. */
void __cdecl SagaVrTriggerTrace(i32 index, u32 occupied, u32 flags, i32 complete, i32 count, const i8 *indices) {
    static u32 last_occupied[32], last_complete[32];
    static i8 last_indices[32][8];
    static i32 seen[32];
    char text[160];
    i32 i, length;
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    if (index < 0 || index >= 32 || count < 0 || count > 8)
        return;
    if (seen[index] && last_occupied[index] == occupied && last_complete[index] == (u32)complete &&
        memcmp(last_indices[index], indices, (size_t)count) == 0)
        return;
    seen[index] = 1;
    last_occupied[index] = occupied;
    last_complete[index] = (u32)complete;
    memcpy(last_indices[index], indices, (size_t)count);
    length = 0;
    text[0] = 0;
    for (i = 0; i < count && length < (i32)sizeof(text) - 24; ++i)
        length += _snprintf(text + length, sizeof(text) - length - 1, " %d", indices[i] >= 0
                                                                             ? (i32)Obj[indices[i]].id
                                                                             : -1);
    LOG_INFO("VR trigger set %d: %d pads, occupied 0x%x, flags 0x%x, complete %d, helpers (character ids):%s; player at "
             "(%.2f, %.2f, %.2f)",
             index, count, occupied, flags, complete, text, player != NULL ? player->position.x : 0.0f,
             player != NULL ? player->position.y : 0.0f, player != NULL ? player->position.z : 0.0f);
}
