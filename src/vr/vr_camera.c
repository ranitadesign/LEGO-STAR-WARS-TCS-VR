/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* VR fork: world camera adjustment and per-frame control.
 *
 * SagaVrCameraAdjust runs inside NuCameraSetEx on the incoming perspective world camera and applies,
 * in camera-local space, (1) the manual offset of milestone 2 and (2) when stereo is live, the head
 * pose with a field of view wide enough to cull for both eyes. View, projection packet, clip planes
 * and culling all follow. Control goes through the shared memory block "Local\SagaVrControl"
 * (tools/vr_control.c) or, while the game window has focus:
 *   F9 offset on/off | Numpad 4/6 yaw | 8/2 pitch | 7/9 roll | 1/3 left/right | -/+ back/forward
 *   Numpad / and * down/up | Numpad 5 reset offset | F7 lightsaber on/off | F8 first person | F10 recentre head
 *   F11 stereo on/off */
#include "vr/vr_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SAGA_VR_CONTROL_NAME "Local\\SagaVrControl"
#define SAGA_VR_CONTROL_MAGIC 0x32525653u /* "SVR2" */
#define SAGA_VR_FP_NEAR_CLIP 0.03f

typedef HRESULT(__stdcall *SAGA_VR_SAVE_SURFACE)(const char *, i32, void *, const void *, const RECT *);

static SAGA_VR_CONTROL SagaVrLocalControl;
static SAGA_VR_CONTROL *SagaVrControl = NULL;
static i32 SagaVrInitialized = 0;
static u32 SagaVrCallsThisFrame = 0;
static u32 SagaVrAdjustedThisFrame = 0;
static i32 SagaVrKeysWereDown[5] = {0, 0, 0, 0, 0};
/* The engine re-sets global_camera itself; recognise our own output so the adjustment never stacks. */
static NUMTX SagaVrLastBase;
static NUMTX SagaVrLastAdjusted;
static NUCAMERA SagaVrLastBaseCamera;
static i32 SagaVrHasLast = 0;
static f32 SagaVrRig[16];
static u32 SagaVrRigFrame = 0xffffffffu;

static void SagaVrInitialize(void) {
    HANDLE mapping;
    SagaVrInitialized = 1;
    SagaVrControl = &SagaVrLocalControl;
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SAGA_VR_CONTROL),
                                 SAGA_VR_CONTROL_NAME);
    if (mapping != NULL) {
        SAGA_VR_CONTROL *shared =
            (SAGA_VR_CONTROL *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SAGA_VR_CONTROL));
        if (shared != NULL)
            SagaVrControl = shared;
    }
    memset(SagaVrControl, 0, sizeof(*SagaVrControl));
    SagaVrControl->magic = SAGA_VR_CONTROL_MAGIC;
    SagaVrControl->size = sizeof(SAGA_VR_CONTROL);
    SagaVrControl->fp_eye_height = 0.85f;
    SagaVrControl->fp_forward = 0.12f;
    SagaVrControl->fp_smoothing = 0.12f;
    SagaVrControl->saber_lit = 1;
    LOG_INFO("LEGO Star Wars VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR");
    LOG_INFO("VR camera control ready (%s, shared=%d, main thread %lu)", SAGA_VR_CONTROL_NAME,
             SagaVrControl != &SagaVrLocalControl, (unsigned long)GetCurrentThreadId());
    SagaVrStereoInit(SagaVrControl);
}

SAGA_VR_CONTROL *__cdecl SagaVrGetControl(void) {
    if (!SagaVrInitialized)
        SagaVrInitialize();
    return SagaVrControl;
}

void __cdecl SagaVrMul(f32 *out, const f32 *a, const f32 *b) {
    i32 row, column;
    for (row = 0; row < 4; ++row) {
        for (column = 0; column < 4; ++column) {
            out[row * 4 + column] = a[row * 4 + 0] * b[0 * 4 + column] + a[row * 4 + 1] * b[1 * 4 + column] +
                                    a[row * 4 + 2] * b[2 * 4 + column] + a[row * 4 + 3] * b[3 * 4 + column];
        }
    }
}

/* Local offset = roll * pitch * yaw, then translation (D3D row-vector conventions). */
static void SagaVrBuildOffset(f32 *out, const SAGA_VR_CONTROL *control) {
    f32 roll[16], pitch[16], yaw[16], temporary[16];
    f32 c, s;
    memset(roll, 0, sizeof(roll));
    memset(pitch, 0, sizeof(pitch));
    memset(yaw, 0, sizeof(yaw));
    c = (f32)cos(control->roll);
    s = (f32)sin(control->roll);
    roll[0] = c, roll[1] = s, roll[4] = -s, roll[5] = c, roll[10] = 1.0f, roll[15] = 1.0f;
    c = (f32)cos(control->pitch);
    s = (f32)sin(control->pitch);
    pitch[0] = 1.0f, pitch[5] = c, pitch[6] = s, pitch[9] = -s, pitch[10] = c, pitch[15] = 1.0f;
    c = (f32)cos(control->yaw);
    s = (f32)sin(control->yaw);
    yaw[0] = c, yaw[2] = -s, yaw[5] = 1.0f, yaw[8] = s, yaw[10] = c, yaw[15] = 1.0f;
    SagaVrMul(temporary, roll, pitch);
    SagaVrMul(out, temporary, yaw);
    out[12] = control->x;
    out[13] = control->y;
    out[14] = control->z;
}

void __cdecl SagaVrCameraAdjust(NUCAMERA *camera) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    f32 matrix[16], adjusted[16], head[16];
    f32 fov, aspect;
    u32 index;
    i32 changed = 0;
    if (SagaVrHasLast && memcmp(&camera->mtx, &SagaVrLastAdjusted, sizeof(NUMTX)) == 0) {
        camera->mtx = SagaVrLastBase;
        camera->fov = SagaVrLastBaseCamera.fov;
        camera->aspect = SagaVrLastBaseCamera.aspect;
        camera->near_clip = SagaVrLastBaseCamera.near_clip;
    }
    index = SagaVrCallsThisFrame++;
    if (index < SAGA_VR_TRACE_CAMERAS) {
        SAGA_VR_CAMERA_TRACE *trace = &control->cameras[index];
        trace->position[0] = camera->mtx.m30;
        trace->position[1] = camera->mtx.m31;
        trace->position[2] = camera->mtx.m32;
        trace->fov = camera->fov;
        trace->aspect = camera->aspect;
        trace->orthographic = NuCameraOrthographic;
        trace->mode = NuCameraMode;
    }
    /* Only the perspective world camera moves; orthographic HUD and mirrored passes keep theirs. */
    if (NuCameraOrthographic != 0 || NuCameraMode != 0)
        return;
    SagaVrLastBaseCamera = *camera;
    memcpy(matrix, &camera->mtx, sizeof(matrix));
    if (SagaVrFirstPersonCamera(matrix)) {
        /* Hands and lightsaber are a few centimetres from the eyes of a 0.42-unit minifig. */
        camera->near_clip = SAGA_VR_FP_NEAR_CLIP;
        changed = 1;
    }
    if (control->enabled) {
        f32 offset[16];
        SagaVrBuildOffset(offset, control);
        SagaVrMul(adjusted, offset, matrix);
        memcpy(matrix, adjusted, sizeof(matrix));
        changed = 1;
    }
    SagaVrSaberPrepare();
    /* The rig (camera before head tracking) places the controllers in the world. */
    memcpy(SagaVrRig, matrix, sizeof(SagaVrRig));
    SagaVrRigFrame = control->frame;
    if (SagaVrStereoHead(head, &fov, &aspect)) {
        SagaVrMul(adjusted, head, matrix);
        memcpy(matrix, adjusted, sizeof(matrix));
        camera->fov = fov;
        camera->aspect = aspect;
        changed = 1;
    }
    if (!changed)
        return;
    SagaVrLastBase = camera->mtx;
    memcpy(&camera->mtx, matrix, sizeof(matrix));
    SagaVrLastAdjusted = camera->mtx;
    SagaVrHasLast = 1;
    ++SagaVrAdjustedThisFrame;
}

/* The HUD's 3D pieces are placed in front of NuCameraGetMtx(), the head-tracked camera in stereo. While the
 * HUD is drawn that matrix is the rig's (no head) through the HUD panel's mapping, so they stay with the 2D
 * overlay on the fixed panel. Only when it still holds this frame's adjusted world camera; put back after,
 * unless the HUD set its camera again itself (DrawPanel ends with that). */
static NUMTX SagaVrPanelSaved, SagaVrPanelMatrix;
static i32 SagaVrPanelActive = 0;

void __cdecl SagaVrPanelBegin(void) {
    f32 shear[16];
    SagaVrPanelActive = 0;
    if (!SagaVrHasLast || SagaVrRigFrame != SagaVrGetControl()->frame ||
        memcmp(&global_camera.mtx, &SagaVrLastAdjusted, sizeof(NUMTX)) != 0 || !SagaVrHudShear(shear))
        return;
    SagaVrPanelSaved = global_camera.mtx;
    SagaVrMul(&SagaVrPanelMatrix.m00, shear, SagaVrRig);
    global_camera.mtx = SagaVrPanelMatrix;
    SagaVrPanelActive = 1;
}

void __cdecl SagaVrPanelEnd(void) {
    if (SagaVrPanelActive && memcmp(&global_camera.mtx, &SagaVrPanelMatrix, sizeof(NUMTX)) == 0)
        global_camera.mtx = SagaVrPanelSaved;
    SagaVrPanelActive = 0;
}

static i32 SagaVrKeyDown(i32 key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

/* Edge-triggered function key; slot indexes SagaVrKeysWereDown. */
static i32 SagaVrKeyPressed(i32 key, i32 slot) {
    i32 down = SagaVrKeyDown(key);
    i32 pressed = down && !SagaVrKeysWereDown[slot];
    SagaVrKeysWereDown[slot] = down;
    return pressed;
}

static void SagaVrPollKeyboard(SAGA_VR_CONTROL *control) {
    const f32 angle = 0.02f;
    const f32 distance = 0.05f;
    DWORD process = 0;
    HWND foreground = GetForegroundWindow();
    if (foreground == NULL)
        return;
    GetWindowThreadProcessId(foreground, &process);
    if (process != GetCurrentProcessId())
        return;
    if (SagaVrKeyPressed(VK_F7, 4)) {
        control->saber_lit = !control->saber_lit;
        LOG_INFO("VR lightsaber %s", control->saber_lit ? "on" : "off");
    }
    if (SagaVrKeyPressed(VK_F10, 1)) {
        ++control->recenter_request;
        LOG_INFO("VR recentre requested");
    }
    /* The rest are testing aids: a player pressing them by accident lost first person, the stereo view or the
     * head-tracked camera. */
    if (!control->debug_keys)
        return;
    if (SagaVrKeyPressed(VK_F8, 3)) {
        control->first_person = !control->first_person;
        LOG_INFO("VR first person %s", control->first_person ? "enabled" : "disabled");
    }
    if (SagaVrKeyPressed(VK_F9, 0)) {
        control->enabled = !control->enabled;
        LOG_INFO("VR camera offset %s", control->enabled ? "enabled" : "disabled");
    }
    if (SagaVrKeyPressed(VK_F11, 2)) {
        control->stereo_enabled = !control->stereo_enabled;
        LOG_INFO("VR stereo %s", control->stereo_enabled ? "enabled" : "disabled");
    }
    if (SagaVrKeyDown(VK_NUMPAD4))
        control->yaw -= angle;
    if (SagaVrKeyDown(VK_NUMPAD6))
        control->yaw += angle;
    if (SagaVrKeyDown(VK_NUMPAD8))
        control->pitch -= angle;
    if (SagaVrKeyDown(VK_NUMPAD2))
        control->pitch += angle;
    if (SagaVrKeyDown(VK_NUMPAD7))
        control->roll -= angle;
    if (SagaVrKeyDown(VK_NUMPAD9))
        control->roll += angle;
    if (SagaVrKeyDown(VK_NUMPAD1))
        control->x -= distance;
    if (SagaVrKeyDown(VK_NUMPAD3))
        control->x += distance;
    if (SagaVrKeyDown(VK_DIVIDE))
        control->y -= distance;
    if (SagaVrKeyDown(VK_MULTIPLY))
        control->y += distance;
    if (SagaVrKeyDown(VK_SUBTRACT))
        control->z -= distance;
    if (SagaVrKeyDown(VK_ADD))
        control->z += distance;
    if (SagaVrKeyDown(VK_NUMPAD5)) {
        control->yaw = control->pitch = control->roll = 0.0f;
        control->x = control->y = control->z = 0.0f;
    }
}

/* .bmp saves both submitted eyes side by side; anything else saves the back buffer as PNG through
 * d3dx9_35 (IDirect3DDevice9::GetBackBuffer is vtable slot 18). */
static void SagaVrSaveCapture(SAGA_VR_CONTROL *control) {
    typedef HRESULT(__stdcall * GET_BACK_BUFFER)(void *, UINT, UINT, i32, void **);
    typedef ULONG(__stdcall * RELEASE)(void *);
    static SAGA_VR_SAVE_SURFACE save_surface = NULL;
    void *device = PCDeviceBackend.device;
    void *surface = NULL;
    HRESULT result = E_FAIL;
    size_t length;
    control->screenshot_path[sizeof(control->screenshot_path) - 1] = 0;
    length = strlen(control->screenshot_path);
    if (length > 4 && _stricmp(control->screenshot_path + length - 4, ".bmp") == 0) {
        result = SagaVrStereoSave(control->screenshot_path) ? S_OK : E_FAIL;
    } else {
        if (save_surface == NULL) {
            HMODULE d3dx = LoadLibraryA("d3dx9_35.dll");
            if (d3dx != NULL)
                save_surface = (SAGA_VR_SAVE_SURFACE)GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA");
        }
        if (save_surface != NULL && device != NULL) {
            void **methods = *(void ***)device;
            result = ((GET_BACK_BUFFER)methods[18])(device, 0, 0, 0, &surface);
            if (result >= 0) {
                result = save_surface(control->screenshot_path, 3 /* D3DXIFF_PNG */, surface, NULL, NULL);
                ((RELEASE)(*(void ***)surface)[2])(surface);
            }
        }
    }
    control->screenshot_result = result;
    control->screenshot_done = control->screenshot_request;
    LOG_INFO("VR capture %s: 0x%08x", control->screenshot_path, (u32)result);
}

static void SagaVrLogFloor(SAGA_VR_CONTROL *control);
static void SagaVrScanFloor(void);

void __cdecl SagaVrFrameEnd(void) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    SagaVrFirstPersonAfterDraw();
    SagaVrSaberDeflect();
    SagaVrPunchUpdate();
    SagaVrRegisterStatusScreen();
    SagaVrLogFloor(control);
    if (Player[0] != NULL) {
        GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
        control->player_position[0] = player->position.x;
        control->player_position[1] = player->position.y;
        control->player_position[2] = player->position.z;
    }
    if (control->screenshot_request != control->screenshot_done)
        SagaVrSaveCapture(control);
    control->calls_last_frame = SagaVrCallsThisFrame;
    control->adjusted_last_frame = SagaVrAdjustedThisFrame;
    ++control->frame;
    if (control->frame % 600 == 1) {
        MEMORYSTATUSEX memory;
        memory.dwLength = sizeof(memory);
        GlobalMemoryStatusEx(&memory);
        LOG_INFO("VR frame %u: %u camera sets, %u adjusted, %u stereo frames; address space used %u of %u MB",
                 control->frame, SagaVrCallsThisFrame, SagaVrAdjustedThisFrame, control->stereo_frames,
                 (u32)((memory.ullTotalVirtual - memory.ullAvailVirtual) >> 20), (u32)(memory.ullTotalVirtual >> 20));
    }
    if (Player[0] != NULL && WORLD != NULL) {
        /* Telemetry: shots in flight from others and shots the player sent back (blocked / deflected). */
        static f32 last_time[32];
        static u8 was_active[32];
        static i32 enemy_shots = 0, returned_shots = 0;
        GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
        i32 i;
        for (i = 0; i < 32; ++i) {
            BOLT *bolt = &Bolt[i];
            if (bolt->active && (!was_active[i] || bolt->time < last_time[i])) {
                if (bolt->owner == player) {
                    returned_shots += (bolt->flags & 0x10000000u) != 0;
                    LOG_INFO("VR shot: bolt direction (%.2f %.2f %.2f) accel %.2f", bolt->direction.x, bolt->direction.y,
                             bolt->direction.z, bolt->acceleration_y);
                }
                else if (bolt->owner != NULL && bolt->owner != Player[1])
                    ++enemy_shots;
            }
            was_active[i] = bolt->active;
            last_time[i] = bolt->time;
        }
        if (control->frame % 300 == 0 && Obj != NULL) {
            GAMEOBJECT *object = Obj;
            i32 firing = 0;
            for (i = 0; i < HIGHGAMEOBJECT; ++i, ++object)
                firing += object != player && object->script_fire_target == player;
            LOG_INFO("VR AI: player hp %d context %d drawn %d; %d engaging it; shots: %d by others, %d sent back",
                     (i32)player->current_hp, (i32)player->character_context, (i32)player->model_draw_result, firing,
                     enemy_shots, returned_shots);
            enemy_shots = returned_shots = 0;
        }
    }
    if (control->unlock_all && WORLD != NULL && WORLD->current_level != NULL && WORLD->current_level != TITLES_LDATA &&
        Player[0] != NULL && Game_CharacterSave != NULL) {
        /* The game's own unlocks, once play starts: every collectable character (free play and shop) and
         * every area, then the game's autosave. Once per session. */
        static i32 unlocked = 0;
        if (!unlocked) {
            i32 id, added = 0;
            unlocked = 1;
            for (id = 1; id < CHARCOUNT; ++id)
                added += AddToCollection(id);
            Areas_OpenAll(1);
            /* Write it to the loaded slot the way the shop and level ends do: the autosave when it is on,
             * otherwise the game's save menu over the pause menu. Nothing new (already saved): no prompt. */
            if (added > 0 && TriggerAutoSave()) {
                LOG_INFO("VR unlock_all: %d characters added, all areas opened, autosave requested", added);
            } else if (added > 0) {
                LOG_INFO("VR unlock_all: %d characters added, all areas opened, opening the save menu", added);
                PauseGame(0);
                NewMenu(1000, -1, -1);
            } else {
                LOG_INFO("VR unlock_all: everything was already unlocked");
            }
        }
    }
    if (control->frame % 120 == 0 && (StatusPacket.status_flags != 0 || StatusPacket.stage != NULL)) {
        /* Telemetry: end-of-level status screen (a headset session hung on its countdown). */
        LOG_INFO("VR status: flags %u stage %p type %d timer %.2f new_level %p fade %.2f cut %d",
                 (u32)StatusPacket.status_flags, (void *)StatusPacket.stage,
                 StatusPacket.stage != NULL ? StatusPacket.stage->type : -1, GameTimer.time_elapsed,
                 (void *)NewLData, FadeSys.fade, (i32)CUTSTOPGAME);
    }
    if (control->level_request != control->level_done) {
        i32 index = -1;
        f32 tp[3];
        control->level_name[sizeof(control->level_name) - 1] = '\0';
        if (strcmp(control->level_name, "!scan") == 0) {
            SagaVrScanFloor();
        } else if (sscanf(control->level_name, "!tp %f %f %f", &tp[0], &tp[1], &tp[2]) == 3 && Player[0] != NULL) {
            GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
            player->position.x = tp[0], player->position.y = tp[1], player->position.z = tp[2];
            player->velocity.x = player->velocity.y = player->velocity.z = 0.0f;
            LOG_INFO("VR test: teleported to (%.2f, %.2f, %.2f)", tp[0], tp[1], tp[2]);
        } else if (strcmp(control->level_name, "!complete") == 0 && WORLD != NULL) {
            LOG_INFO("VR test: completing the current level");
            CompleteLevel(WORLD);
        } else if (Level_FindByName(control->level_name, &index) != NULL && index >= 0) {
            LOG_INFO("VR test: going to level %s (%d)", control->level_name, index);
            GoToNewLevel(index);
        } else {
            LOG_WARN("VR test: no level named %s", control->level_name);
        }
        control->level_done = control->level_request;
    }
    SagaVrCallsThisFrame = 0;
    SagaVrAdjustedThisFrame = 0;
    SagaVrPollKeyboard(control);
}

const f32 *__cdecl SagaVrViewMatrix(void) {
    return SagaVrHasLast ? &SagaVrLastAdjusted.m00 : NULL;
}

const f32 *__cdecl SagaVrGetRig(void) {
    /* Valid for the frame that set it (the engine sets the camera before drawing characters). */
    return SagaVrRigFrame == SagaVrGetControl()->frame ? SagaVrRig : NULL;
}

i32 __cdecl SagaVrStatusIdle(i32 idle) {
    if (idle && NewLData == NULL && GameTimer.time_elapsed >= 5.0f) {
        LOG_INFO("VR: status screen idle for 5 s, continuing");
        if (StatusPacket.finish_callback != NULL)
            FinishStatusPacket(0);
        else
            NewLData = HUB_LDATA; /* no status screen registered: back to the cantina */
    }
    return idle;
}

/* The decompilation leaves LoadPermData's LSW_registerStatusScreen call stubbed, so the end-of-chapter
 * status screen has no stages or callbacks and hangs. The VR build registers it once at start-up. */
void __cdecl LSW_registerStatusScreen(void);

void __cdecl SagaVrRegisterStatusScreen(void) {
    if (StatusPacket.finish_callback == NULL && StartupGameFlag1 != 0) {
        LSW_registerStatusScreen();
        LOG_INFO("VR: registered the LSW status screen (stubbed in LoadPermData)");
    }
}

/* Telemetry: what is under player 0 and how fast it moves, on every change of surface or platform and
 * twice a second while walking on a platform or a low-traction surface. */
static void SagaVrLogFloor(SAGA_VR_CONTROL *control) {
    static i32 last_surface = -2, last_platform = -2;
    static f32 last_input = -1.0f;
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    i32 surface, platform;
    f32 scale;
    if (player == NULL || WORLD == NULL)
        return;
    surface = (i8)player->jump_animation_state;
    platform = player->terrain_platform_id;
    scale = surface >= 0 && surface < 32 ? TerSurface[surface].movement_scale : 1.0f;
    if (surface != last_surface || platform != last_platform ||
        (player->pad != NULL && player->pad->input_magnitude != last_input) ||
        ((platform != -1 || scale != 1.0f) && player->pad != NULL && player->pad->input_magnitude > 0.0f &&
         control->frame % 30 == 0)) {
        LOG_INFO("VR floor: surface %d scale %.2f flags 0x%x platform %d grounded %d context %d speed %.2f target %.2f "
                 "input %.2f pos (%.2f, %.2f, %.2f)",
                 surface, scale, surface >= 0 && surface < 32 ? TerSurface[surface].flags : 0, platform,
                 (i32)player->field_281, (i32)player->character_context,
                 (f32)sqrt(player->velocity.x * player->velocity.x + player->velocity.z * player->velocity.z),
                 (f32)sqrt(player->target_velocity.x * player->target_velocity.x +
                           player->target_velocity.z * player->target_velocity.z),
                 player->pad != NULL ? player->pad->input_magnitude : 0.0f, player->position.x, player->position.y,
                 player->position.z);
        last_surface = surface;
        last_platform = platform;
        last_input = player->pad != NULL ? player->pad->input_magnitude : 0.0f;
    }
}

/* Test aid (vr_control level !scan): the floor's surface type on a grid around player 0, per platform. */
static void SagaVrScanFloor(void) {
    enum { MAX_ROWS = 96 };
    static struct {
        i32 surface, platform, count;
        f32 x, y, z;
    } rows[MAX_ROWS];
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    i32 used = 0, ix, iz, i;
    if (player == NULL)
        return;
    for (ix = -200; ix <= 200; ++ix) {
        for (iz = -200; iz <= 200; ++iz) {
            NUVEC probe;
            f32 floor;
            i32 surface, platform;
            probe.x = player->position.x + ix * 0.4f;
            probe.y = player->position.y + 12.0f;
            probe.z = player->position.z + iz * 0.4f;
            floor = NewShadow(&probe, 0.0f, 30.0f, -1);
            if (floor == 2000000.0f)
                continue;
            surface = ShadowInfo();
            platform = NewShadowOnPlatform();
            for (i = 0; i < used && (rows[i].surface != surface || rows[i].platform != platform); ++i)
                ;
            if (i == used) {
                if (used == MAX_ROWS)
                    continue;
                ++used;
                rows[i].surface = surface;
                rows[i].platform = platform;
                rows[i].count = 0;
                rows[i].x = rows[i].y = rows[i].z = 0.0f;
            }
            ++rows[i].count;
            rows[i].x += probe.x;
            rows[i].y += floor;
            rows[i].z += probe.z;
        }
    }
    for (i = 0; i < used; ++i)
        LOG_INFO("VR scan: surface %d scale %.2f flags 0x%x platform %d samples %d centre (%.2f, %.2f, %.2f)", rows[i].surface,
                 rows[i].surface >= 0 && rows[i].surface < 32 ? TerSurface[rows[i].surface].movement_scale : 1.0f,
                 rows[i].surface >= 0 && rows[i].surface < 32 ? TerSurface[rows[i].surface].flags : 0, rows[i].platform,
                 rows[i].count, rows[i].x / rows[i].count, rows[i].y / rows[i].count, rows[i].z / rows[i].count);
    LOG_INFO("VR scan: %d rows around (%.2f, %.2f, %.2f)", used, player->position.x, player->position.y,
             player->position.z);
}
