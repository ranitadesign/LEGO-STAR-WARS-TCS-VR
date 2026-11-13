/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* saga_vr.dll: OpenXR runtime bridge for the VR build of saga-pc.
 * Built with a modern x86 compiler (see xr/build.sh); the VC8 game loads it with LoadLibrary and
 * resolves these __cdecl exports by name, so this header is plain C89 and shared by both sides. */
#ifndef SAGA_XR_H
#define SAGA_XR_H

#define SAGA_XR_API_VERSION 1

/* One eye, in the OpenXR reference space (right-handed, metres) relative to the recentred origin. */
typedef struct SAGA_XR_VIEW {
    float orientation[4]; /* x, y, z, w */
    float position[3];
    float fov[4]; /* angle left, right, up, down (radians; left/down negative) */
} SAGA_XR_VIEW;

enum SAGA_XR_MODE {
    SAGA_XR_MODE_OFF = 0,       /* no runtime/headset: the game renders mono */
    SAGA_XR_MODE_HEADSET = 1,   /* real OpenXR session */
    SAGA_XR_MODE_SIMULATED = 2  /* no runtime needed: synthetic stereo views, images kept for capture */
};

typedef void(__cdecl *SAGA_XR_LOG)(const char *message);

/* Returns an SAGA_XR_MODE. simulate != 0 skips OpenXR entirely. */
typedef int(__cdecl *SAGA_XR_INIT)(int api_version, int simulate, SAGA_XR_LOG log);
/* Polls events and, when the session is running, waits for and begins a frame.
 * Returns 1 when both views should be rendered and submitted, 0 to render mono this frame. */
typedef int(__cdecl *SAGA_XR_BEGIN_FRAME)(SAGA_XR_VIEW views[2]);
/* Copies one eye (32-bit BGRX rows) into that eye's swapchain image. */
typedef void(__cdecl *SAGA_XR_SUBMIT_EYE)(int eye, const void *pixels, int pitch, int width, int height);
/* Ends the frame begun by a BeginFrame that returned 1. */
typedef void(__cdecl *SAGA_XR_END_FRAME)(void);
/* Makes the current head yaw/position the new origin. */
typedef void(__cdecl *SAGA_XR_RECENTER)(void);
/* Writes the last submitted eyes side by side as a 24-bit BMP. Returns 1 on success. */
typedef int(__cdecl *SAGA_XR_SAVE_STEREO)(const char *path);
typedef void(__cdecl *SAGA_XR_SHUTDOWN)(void);
/* Controller input, sampled by SagaXrBeginFrame. Button bits follow SAGA_XR_BUTTON_*. */
#define SAGA_XR_BUTTON_JUMP 0x01
#define SAGA_XR_BUTTON_ATTACK 0x02
#define SAGA_XR_BUTTON_SPECIAL 0x04
#define SAGA_XR_BUTTON_SWAP 0x08
#define SAGA_XR_BUTTON_MENU 0x10
#define SAGA_XR_BUTTON_RECENTER 0x20
#define SAGA_XR_BUTTON_SABER 0x40 /* lightsaber on/off (right grip) */
#define SAGA_XR_BUTTON_GRAB_LEFT 0x80 /* left grip */

typedef struct SAGA_XR_INPUT {
    int active; /* 1 when controller actions are bound and the session has focus */
    unsigned buttons;
    float move[2]; /* left stick: x right, y forward, -1..1 */
    float look[2]; /* right stick */
    int hand_valid[2];
    float hand_orientation[2][4]; /* grip poses, same space as the views */
    float hand_position[2][3];
} SAGA_XR_INPUT;

/* Ends the frame showing eye 0's image as a flat screen (cutscenes, movies): width metres wide, the given
 * aspect (width / height), 2.5 m ahead of the recentred origin. Optional export. */
typedef void(__cdecl *SAGA_XR_END_FRAME_SCREEN)(float width, float aspect);
/* Copies the controller state of the current frame. Optional export. */
typedef void(__cdecl *SAGA_XR_GET_INPUT)(SAGA_XR_INPUT *input);
/* Simulated mode only: head pose (OpenXR space) used for the next synthetic views. Optional export. */
typedef void(__cdecl *SAGA_XR_SET_SIMULATED_HEAD)(const float orientation[4], const float position[3]);

#endif
