/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
#ifndef SAGA_VR_INTERNAL_H
#define SAGA_VR_INTERNAL_H

/* Shared by vr_camera.c and vr_stereo.c only. */
#include "vr/vr.h"

#define SAGA_VR_TRACE_CAMERAS 8

typedef struct SAGA_VR_CAMERA_TRACE {
    f32 position[3];
    f32 fov;
    f32 aspect;
    i32 orthographic;
    i32 mode;
} SAGA_VR_CAMERA_TRACE;

/* Shared memory "Local\SagaVrControl"; tools/vr_control.c mirrors this layout. */
typedef struct SAGA_VR_CONTROL {
    u32 magic;
    u32 size;
    i32 enabled;
    f32 yaw, pitch, roll; /* radians, camera-local */
    f32 x, y, z;          /* camera-local translation in world units */
    /* Telemetry written by the game. */
    u32 frame;
    u32 calls_last_frame;
    u32 adjusted_last_frame;
    SAGA_VR_CAMERA_TRACE cameras[SAGA_VR_TRACE_CAMERAS];
    /* Capture: the tool writes a path (.png back buffer, .bmp both eyes) and bumps the request. */
    u32 screenshot_request;
    u32 screenshot_done;
    i32 screenshot_result;
    char screenshot_path[260];
    /* Stereo. */
    i32 xr_mode;        /* SAGA_XR_MODE_* */
    i32 stereo_enabled; /* F11 */
    f32 world_scale;    /* game units per metre of head motion (third person) */
    f32 fp_world_scale; /* the same in first person (a minifig is about 0.42 units tall) */
    u32 stereo_frames;
    f32 head_position[3]; /* metres, relative to the recentred origin */
    u32 recenter_request; /* bumped by the tool or F10 */
    /* Simulated mode: head yaw/pitch (radians, OpenXR: +yaw turns left) and position (metres). */
    f32 simulated_head[5];
    /* Virtual pad: PS2-layout buttons (0x40 cross/jump, 0x80 square/attack, 0x20 circle, 0x10 triangle,
     * 0x800 start, 0x1000/0x2000/0x4000/0x8000 up/right/down/left) and sticks (128 = centre). */
    i32 pad_active;
    u32 pad_buttons;
    u8 pad_axes[4]; /* left x, left y, right x, right y */
    u8 keys[8];     /* DirectInput scan codes held down (0 = unused slot) */
    /* First person (F8): eye height as a fraction of the character height, forward offset past the head
     * (world units) and smoothing time constant (seconds). */
    i32 first_person;
    f32 fp_eye_height;
    f32 fp_forward;
    f32 fp_smoothing;
    /* Telemetry: player 0 position, facing (degrees), scaled height; stick rotation applied (degrees). */
    f32 player_position[3];
    f32 player_facing;
    f32 player_height;
    f32 stick_rotation;
    /* VR controllers (stage 3): whether actions are live, their buttons (SAGA_XR_BUTTON_*) and hands. */
    i32 controllers_active;
    u32 controller_buttons;
    i32 hand_valid[2];
    f32 hand_position[2][3]; /* metres, relative to the recentred origin */
    /* Swing to attack: right hand speed (m/s) that counts as a lightsaber swing; 0 disables. */
    f32 swing_speed;
    f32 hand_speed; /* telemetry: current right hand speed */
    u32 swings;     /* telemetry: swings detected */
    /* Motion lightsaber (first person + right controller): 1 = the blade follows the hand. saber_pitch
     * tilts the blade relative to the controller's grip axis (degrees, + tips it up). */
    i32 saber;
    f32 saber_pitch;
    i32 saber_driving; /* telemetry: last frame the saber joint was moved */
    f32 saber_scale;   /* size of the hand-held saber relative to the model's own */
    i32 saber_lit; /* blade on (right grip / F7 toggles) */
    /* Simulated mode: right hand yaw/pitch (degrees, OpenXR axes) and position (metres); active flag. */
    i32 simulated_hand_active;
    f32 simulated_hand[5];
    /* Bare-hand orientation correction in the controller's frame, degrees: [hand][yaw, pitch, roll]. */
    f32 hand_rotation[2][3];
    /* Test aid: go to the level named here (as in the level list) when the request is bumped. */
    u32 level_request;
    u32 level_done;
    char level_name[64];
    /* Blaster aim assist: cone half-angle in degrees around the right controller (0 = pure hand aim). */
    f32 aim_assist;
    /* Unlock every character and level once per session when play starts (saga_vr.ini unlock_all). */
    i32 unlock_all;
    /* Hand-held blasters: size relative to the model (1 = own) and barrel tilt below the grip axis, degrees. */
    f32 blaster_scale;
    f32 blaster_pitch;
    /* Seconds before the hand-held blade can hurt the same target again; 0 = every frame it touches. */
    f32 saber_cooldown;
    /* Test aid: player vehicle joints to collapse, one bit per joint. */
    u32 vehicle_hide;
    /* Test aid: SAGA_XR_BUTTON_* added to the input while a simulated hand is active. */
    u32 simulated_buttons;
    f32 simulated_hand_roll; /* degrees */
    /* saga_vr.ini debug_keys: F8, F9, F11 and the numpad camera offset (testing only; F7 and F10 always work). */
    i32 debug_keys;
    /* saga_vr.ini smooth_turn: right-stick turning speed in degrees per second; 0 = 30 degree snap turns. */
    f32 smooth_turn;
} SAGA_VR_CONTROL;

/* First-person camera before head tracking, set each frame by SagaVrCameraAdjust; NULL when absent. */
const f32 *__cdecl SagaVrGetRig(void);
/* Latest adjusted world camera (head included; row 2 forward, row 3 eye); NULL before the first one. */
const f32 *__cdecl SagaVrViewMatrix(void);
/* World matrix (game units, row-vector) of a controller grip (0 left, 1 right); 0 when unknown. */
i32 __cdecl SagaVrHandWorld(i32 hand, f32 *out);
/* The same hand in tracking space, unaffected by the view: rows right, up, forward, then position in metres. */
i32 __cdecl SagaVrHandTracked(i32 hand, f32 *out);
i32 __cdecl SagaVrHudShear(f32 *out);
/* Whether the right hand is mid-swing (fast motion within the last 0.15 s). */
i32 __cdecl SagaVrSwinging(void);
/* Whether the motion saber drives player 0 this frame (vr_saber.c). */
i32 __cdecl SagaVrSaberDriving(void);
/* Keeps player 0's model drawn while the motion saber is in use (vr_saber.c). */
void __cdecl SagaVrSaberPrepare(void);
/* Head yaw relative to the rig (radians, same convention as the camera yaw); 0 without tracking. */
f32 __cdecl SagaVrHeadYaw(void);
/* Whether the first-person camera drives player 0 this frame (vr_firstperson.c). */
i32 __cdecl SagaVrFirstPersonActive(void);
/* Game unlock helper (parts.c), declared here because saga.h does not export it. */
void __cdecl Areas_OpenAll(i32 mode);
i32 __cdecl TriggerAutoSave(void);
/* After drawing: characters near a first-person player count as on screen for the AI (vr_firstperson.c). */
void __cdecl SagaVrFirstPersonAfterDraw(void);
/* Motion saber: sends back enemy bolts crossing the lit blade (vr_saber.c). */
void __cdecl SagaVrSaberDeflect(void);
i32 __cdecl SagaVrSlamPress(void);
void __cdecl SagaVrPunchUpdate(void);
/* Registers the stubbed LSW end-of-chapter status screen once (vr_camera.c). */
void __cdecl SagaVrRegisterStatusScreen(void);
/* The character sitting on the vehicle player 0 rides or drives; NULL when not riding (vr_firstperson.c). */
GAMEOBJECT *__cdecl SagaVrRider(GAMEOBJECT *player);
i32 __cdecl SagaVrOnPod(GAMEOBJECT *player);
/* The pod's cockpit joint in its own axes (right, up, forward), from the last draw; 0 if not drawn lately. */
i32 __cdecl SagaVrPodCockpit(f32 *cockpit);
/* Where the cockpit joint belongs for the view's current, smoothed eye height. */
f32 __cdecl SagaVrPodCabinHeight(void);
/* The levers held by the hands: bit 0 left (steer, -1 left..1 right), bit 1 right (throttle, 1 forward). */
i32 __cdecl SagaVrPodLeverInput(f32 *steer, f32 *throttle);
/* Ships (vr_cockpit.c): player 0 is a ship without a rider; a drawn cockpit and a flight stick. */
i32 __cdecl SagaVrInShip(GAMEOBJECT *object);
void __cdecl SagaVrDrawShipCockpit(GAMEOBJECT *object);
const f32 *__cdecl SagaVrShipHeldHand(void);
i32 __cdecl SagaVrShipStickInput(f32 *x, f32 *y);
/* A cutscene is playing: original camera, mono, shown on a screen (vr_stereo.c). */
i32 __cdecl SagaVrCinema(void);

/* Latest VR controller sticks (-1..1, y forward) and buttons; returns 0 when no controllers drive the pad. */
i32 __cdecl SagaVrControllerInput(f32 *move, f32 *look, u32 *buttons);

/* First-person camera for this frame (vr_firstperson.c). Replaces camera_matrix and returns 1 when
 * active; stick_rotation receives the yaw (radians) from the game camera to the first-person view. */
i32 __cdecl SagaVrFirstPersonCamera(f32 *camera_matrix);
/* In first person, turns a scanned pad's left stick into a view turn plus a walk along the view. */
void __cdecl SagaVrRotateStick(FRAME_SCANNED_PAD_PC *pad);
/* Turns the first-person view by a fixed angle (VR snap turn); ignored in third person. */
void __cdecl SagaVrFirstPersonSnapTurn(f32 radians);

SAGA_VR_CONTROL *__cdecl SagaVrGetControl(void);
/* Row-vector product out = a * b on 4x4 f32 arrays; out must not alias the inputs. */
void __cdecl SagaVrMul(f32 *out, const f32 *a, const f32 *b);

void __cdecl SagaVrStereoInit(SAGA_VR_CONTROL *control);
/* Head transform (camera-local, game units) to apply to this frame's world camera, plus a field of
 * view wide enough for culling both eyes. Returns 0 when stereo is not live. */
i32 __cdecl SagaVrStereoHead(f32 *head, f32 *fov, f32 *aspect);
i32 __cdecl SagaVrStereoSave(const char *path);

#endif
