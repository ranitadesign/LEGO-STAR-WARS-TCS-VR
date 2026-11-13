/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* saga_vr.dll: OpenXR session, head tracking and eye submission for the saga-pc VR build.
 *
 * The game (D3D9, VC8) renders each eye into its back buffer and hands the pixels here; this
 * module owns a D3D11 device on the runtime's adapter and copies them into OpenXR swapchains.
 * The CPU copy is the first, verifiable path; a shared-texture path can replace SubmitEye later. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "saga_xr.h"

#define EXPORT __declspec(dllexport)

typedef struct Eye {
    XrSwapchain swapchain;
    ID3D11Texture2D **images;
    uint32_t image_count;
    int width, height;
    unsigned char *copy; /* last submitted BGRX pixels, kept for SaveStereo */
    int copy_width, copy_height;
    int submitted;
} Eye;

static struct {
    SAGA_XR_LOG log;
    int mode;
    HMODULE loader;
    PFN_xrGetInstanceProcAddr gipa;
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace local_space, view_space;
    XrSessionState state;
    int running;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    int64_t format;
    Eye eyes[2];
    XrFrameState frame_state;
    int frame_open;
    XrView raw_views[2];
    SAGA_XR_VIEW last_views[2];
    int have_views;
    /* Recentring: the origin is a yaw-only rotation plus a position in the LOCAL space. */
    int recenter_pending;
    float origin_yaw;
    float origin_position[3];
    /* Controller actions (stage 3). */
    XrActionSet action_set;
    XrAction move, look, jump, attack, special, swap, menu, recenter, saber, grab_left, grip_pose;
    XrPath hands[2];
    XrSpace hand_spaces[2];
    int actions_attached;
    SAGA_XR_INPUT input;
    unsigned logged_buttons;
    int has_touch_plus, has_touch_pro;
} X;

EXPORT void __cdecl SagaXrEndFrame(void);

/* OpenXR entry points used here. */
static PFN_xrDestroyInstance pxrDestroyInstance;
static PFN_xrGetSystem pxrGetSystem;
static PFN_xrGetSystemProperties pxrGetSystemProperties;
static PFN_xrGetInstanceProperties pxrGetInstanceProperties;
static PFN_xrResultToString pxrResultToString;
static PFN_xrGetD3D11GraphicsRequirementsKHR pxrGetD3D11GraphicsRequirementsKHR;
static PFN_xrCreateSession pxrCreateSession;
static PFN_xrDestroySession pxrDestroySession;
static PFN_xrBeginSession pxrBeginSession;
static PFN_xrEndSession pxrEndSession;
static PFN_xrRequestExitSession pxrRequestExitSession;
static PFN_xrCreateReferenceSpace pxrCreateReferenceSpace;
static PFN_xrDestroySpace pxrDestroySpace;
static PFN_xrLocateSpace pxrLocateSpace;
static PFN_xrLocateViews pxrLocateViews;
static PFN_xrPollEvent pxrPollEvent;
static PFN_xrWaitFrame pxrWaitFrame;
static PFN_xrBeginFrame pxrBeginFrame;
static PFN_xrEndFrame pxrEndFrame;
static PFN_xrEnumerateViewConfigurationViews pxrEnumerateViewConfigurationViews;
static PFN_xrEnumerateSwapchainFormats pxrEnumerateSwapchainFormats;
static PFN_xrCreateSwapchain pxrCreateSwapchain;
static PFN_xrDestroySwapchain pxrDestroySwapchain;
static PFN_xrEnumerateSwapchainImages pxrEnumerateSwapchainImages;
static PFN_xrAcquireSwapchainImage pxrAcquireSwapchainImage;
static PFN_xrWaitSwapchainImage pxrWaitSwapchainImage;
static PFN_xrReleaseSwapchainImage pxrReleaseSwapchainImage;
static PFN_xrStringToPath pxrStringToPath;
static PFN_xrCreateActionSet pxrCreateActionSet;
static PFN_xrCreateAction pxrCreateAction;
static PFN_xrSuggestInteractionProfileBindings pxrSuggestInteractionProfileBindings;
static PFN_xrAttachSessionActionSets pxrAttachSessionActionSets;
static PFN_xrCreateActionSpace pxrCreateActionSpace;
static PFN_xrSyncActions pxrSyncActions;
static PFN_xrGetActionStateBoolean pxrGetActionStateBoolean;
static PFN_xrGetActionStateVector2f pxrGetActionStateVector2f;
static PFN_xrGetCurrentInteractionProfile pxrGetCurrentInteractionProfile;
static PFN_xrPathToString pxrPathToString;

static void xr_log(const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (X.log)
        X.log(text);
}

static const char *result_name(XrResult result) {
    static char text[XR_MAX_RESULT_STRING_SIZE];
    if (pxrResultToString && X.instance && pxrResultToString(X.instance, result, text) == XR_SUCCESS)
        return text;
    snprintf(text, sizeof(text), "XrResult %d", (int)result);
    return text;
}

static int check(XrResult result, const char *what) {
    if (XR_SUCCEEDED(result))
        return 1;
    xr_log("XR %s failed: %s", what, result_name(result));
    return 0;
}

#define LOAD(name) X.gipa(X.instance, #name, (PFN_xrVoidFunction *)&p##name)

static void load_functions(void) {
    LOAD(xrDestroyInstance);
    LOAD(xrGetSystem);
    LOAD(xrGetSystemProperties);
    LOAD(xrGetInstanceProperties);
    LOAD(xrResultToString);
    LOAD(xrGetD3D11GraphicsRequirementsKHR);
    LOAD(xrCreateSession);
    LOAD(xrDestroySession);
    LOAD(xrBeginSession);
    LOAD(xrEndSession);
    LOAD(xrRequestExitSession);
    LOAD(xrCreateReferenceSpace);
    LOAD(xrDestroySpace);
    LOAD(xrLocateSpace);
    LOAD(xrLocateViews);
    LOAD(xrPollEvent);
    LOAD(xrWaitFrame);
    LOAD(xrBeginFrame);
    LOAD(xrEndFrame);
    LOAD(xrEnumerateViewConfigurationViews);
    LOAD(xrEnumerateSwapchainFormats);
    LOAD(xrCreateSwapchain);
    LOAD(xrDestroySwapchain);
    LOAD(xrEnumerateSwapchainImages);
    LOAD(xrAcquireSwapchainImage);
    LOAD(xrWaitSwapchainImage);
    LOAD(xrReleaseSwapchainImage);
    LOAD(xrStringToPath);
    LOAD(xrCreateActionSet);
    LOAD(xrCreateAction);
    LOAD(xrSuggestInteractionProfileBindings);
    LOAD(xrAttachSessionActionSets);
    LOAD(xrCreateActionSpace);
    LOAD(xrSyncActions);
    LOAD(xrGetActionStateBoolean);
    LOAD(xrGetActionStateVector2f);
    LOAD(xrGetCurrentInteractionProfile);
    LOAD(xrPathToString);
}

/* ---- pose math (OpenXR space: right-handed, metres) ---- */

static void quat_rotate(const float q[4], const float v[3], float out[3]) {
    /* out = q * v * q^-1 */
    float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static void quat_multiply(const float a[4], const float b[4], float out[4]) {
    float r[4];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    memcpy(out, r, sizeof(r));
}

/* Expresses a LOCAL-space pose relative to the recentred origin. */
static void relative_view(const XrView *raw, SAGA_XR_VIEW *out) {
    float half = -0.5f * X.origin_yaw;
    float inverse_yaw[4] = {0.0f, sinf(half), 0.0f, cosf(half)};
    float orientation[4] = {raw->pose.orientation.x, raw->pose.orientation.y, raw->pose.orientation.z,
                            raw->pose.orientation.w};
    float offset[3] = {raw->pose.position.x - X.origin_position[0], raw->pose.position.y - X.origin_position[1],
                       raw->pose.position.z - X.origin_position[2]};
    quat_multiply(inverse_yaw, orientation, out->orientation);
    quat_rotate(inverse_yaw, offset, out->position);
    out->fov[0] = raw->fov.angleLeft;
    out->fov[1] = raw->fov.angleRight;
    out->fov[2] = raw->fov.angleUp;
    out->fov[3] = raw->fov.angleDown;
}

static void set_origin(const XrPosef *head) {
    const XrQuaternionf *q = &head->orientation;
    /* Yaw of the head's forward (-Z) direction projected on the floor plane. */
    float forward[3] = {-2.0f * (q->x * q->z + q->w * q->y), 0.0f, -(1.0f - 2.0f * (q->x * q->x + q->y * q->y))};
    X.origin_yaw = atan2f(-forward[0], -forward[2]);
    X.origin_position[0] = head->position.x;
    X.origin_position[1] = head->position.y;
    X.origin_position[2] = head->position.z;
    xr_log("XR recentred: yaw %.1f deg, position (%.2f, %.2f, %.2f)", X.origin_yaw * 57.2958f, head->position.x,
         head->position.y, head->position.z);
}

static void relative_pose(const XrPosef *raw, float orientation[4], float position[3]) {
    float half = -0.5f * X.origin_yaw;
    float inverse_yaw[4] = {0.0f, sinf(half), 0.0f, cosf(half)};
    float q[4] = {raw->orientation.x, raw->orientation.y, raw->orientation.z, raw->orientation.w};
    float offset[3] = {raw->position.x - X.origin_position[0], raw->position.y - X.origin_position[1],
                       raw->position.z - X.origin_position[2]};
    quat_multiply(inverse_yaw, q, orientation);
    quat_rotate(inverse_yaw, offset, position);
}

/* ---- controller actions (stage 3) ---- */

typedef struct Binding {
    XrAction *action;
    const char *path;
} Binding;

static XrPath path(const char *text) {
    XrPath result = XR_NULL_PATH;
    pxrStringToPath(X.instance, text, &result);
    return result;
}

static int create_action(XrAction *action, const char *name, const char *localized, XrActionType type,
                         int per_hand) {
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
    strcpy(info.actionName, name);
    strcpy(info.localizedActionName, localized);
    info.actionType = type;
    if (per_hand) {
        info.countSubactionPaths = 2;
        info.subactionPaths = X.hands;
    }
    return check(pxrCreateAction(X.action_set, &info, action), name);
}

static int suggest(const char *profile, const Binding *bindings, int count) {
    XrActionSuggestedBinding suggested[16];
    XrInteractionProfileSuggestedBinding info = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    int i;
    for (i = 0; i < count; ++i) {
        suggested[i].action = *bindings[i].action;
        suggested[i].binding = path(bindings[i].path);
    }
    info.interactionProfile = path(profile);
    info.suggestedBindings = suggested;
    info.countSuggestedBindings = (uint32_t)count;
    if (!check(pxrSuggestInteractionProfileBindings(X.instance, &info), profile))
        return 0;
    xr_log("XR bindings suggested for %s (%d)", profile, count);
    return 1;
}

/* LEGO controls on Touch: A jump (cross), right trigger attack (square), B / X / left trigger special
 * and build (circle), Y swap character (triangle), left stick click pause and skip (start; the menu button
 * belongs to the runtime under SteamVR / Steam Link), right stick click recentre,
 * right grip lightsaber on/off; in a pod both grips hold its levers. */
static int create_actions(void) {
    XrActionSetCreateInfo set = {XR_TYPE_ACTION_SET_CREATE_INFO};
    static const Binding touch[] = {
        {&X.move, "/user/hand/left/input/thumbstick"},
        {&X.look, "/user/hand/right/input/thumbstick"},
        {&X.jump, "/user/hand/right/input/a/click"},
        {&X.attack, "/user/hand/right/input/trigger/value"},
        {&X.special, "/user/hand/right/input/b/click"},
        {&X.special, "/user/hand/left/input/x/click"},
        {&X.special, "/user/hand/left/input/trigger/value"},
        {&X.swap, "/user/hand/left/input/y/click"},
        {&X.menu, "/user/hand/left/input/thumbstick/click"},
        {&X.recenter, "/user/hand/right/input/thumbstick/click"},
        {&X.saber, "/user/hand/right/input/squeeze/value"},
        {&X.grab_left, "/user/hand/left/input/squeeze/value"},
        {&X.grip_pose, "/user/hand/left/input/grip/pose"},
        {&X.grip_pose, "/user/hand/right/input/grip/pose"},
    };
    static const Binding simple[] = {
        {&X.jump, "/user/hand/left/input/select/click"},
        {&X.attack, "/user/hand/right/input/select/click"},
        {&X.menu, "/user/hand/left/input/menu/click"},
        {&X.grip_pose, "/user/hand/left/input/grip/pose"},
        {&X.grip_pose, "/user/hand/right/input/grip/pose"},
    };
    X.hands[0] = path("/user/hand/left");
    X.hands[1] = path("/user/hand/right");
    strcpy(set.actionSetName, "gameplay");
    strcpy(set.localizedActionSetName, "Gameplay");
    if (!check(pxrCreateActionSet(X.instance, &set, &X.action_set), "CreateActionSet"))
        return 0;
    if (!create_action(&X.move, "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, 0) ||
        !create_action(&X.look, "look", "Look", XR_ACTION_TYPE_VECTOR2F_INPUT, 0) ||
        !create_action(&X.jump, "jump", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.attack, "attack", "Attack", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.special, "special", "Special and build", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.swap, "swap", "Swap character", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.menu, "menu", "Pause menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.recenter, "recenter", "Recentre view", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.saber, "saber", "Lightsaber on/off", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.grab_left, "grab_left", "Grab with the left hand", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !create_action(&X.grip_pose, "grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT, 1))
        return 0;
    /* Valve Index: no menu button, so the left B opens the pause menu and the left A swaps character. */
    static const Binding index[] = {
        {&X.move, "/user/hand/left/input/thumbstick"},
        {&X.look, "/user/hand/right/input/thumbstick"},
        {&X.jump, "/user/hand/right/input/a/click"},
        {&X.attack, "/user/hand/right/input/trigger/value"},
        {&X.special, "/user/hand/right/input/b/click"},
        {&X.special, "/user/hand/left/input/trigger/value"},
        {&X.swap, "/user/hand/left/input/a/click"},
        {&X.menu, "/user/hand/left/input/b/click"},
        {&X.menu, "/user/hand/left/input/thumbstick/click"},
        {&X.recenter, "/user/hand/right/input/thumbstick/click"},
        {&X.saber, "/user/hand/right/input/squeeze/value"},
        {&X.grab_left, "/user/hand/left/input/squeeze/value"},
        {&X.grip_pose, "/user/hand/left/input/grip/pose"},
        {&X.grip_pose, "/user/hand/right/input/grip/pose"},
    };
    const int touch_count = (int)(sizeof(touch) / sizeof(touch[0]));
    suggest("/interaction_profiles/oculus/touch_controller", touch, touch_count);
    /* Quest 3 / Quest Pro controllers share the Touch component paths used here. */
    if (X.has_touch_plus)
        suggest("/interaction_profiles/meta/touch_controller_plus", touch, touch_count);
    if (X.has_touch_pro)
        suggest("/interaction_profiles/facebook/touch_controller_pro", touch, touch_count);
    suggest("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index) / sizeof(index[0])));
    suggest("/interaction_profiles/khr/simple_controller", simple, (int)(sizeof(simple) / sizeof(simple[0])));
    return 1;
}

static void log_interaction_profiles(void) {
    int hand;
    for (hand = 0; hand < 2; ++hand) {
        XrInteractionProfileState state = {XR_TYPE_INTERACTION_PROFILE_STATE};
        char text[XR_MAX_PATH_LENGTH];
        uint32_t length = 0;
        text[0] = 0;
        if (XR_SUCCEEDED(pxrGetCurrentInteractionProfile(X.session, X.hands[hand], &state)) &&
            state.interactionProfile != XR_NULL_PATH)
            pxrPathToString(X.instance, state.interactionProfile, sizeof(text), &length, text);
        xr_log("XR %s hand interaction profile: %s", hand ? "right" : "left", text[0] ? text : "(none)");
    }
}

static void attach_actions(void) {
    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    XrActionSpaceCreateInfo space = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    int hand;
    if (X.action_set == XR_NULL_HANDLE)
        return;
    attach.countActionSets = 1;
    attach.actionSets = &X.action_set;
    if (!check(pxrAttachSessionActionSets(X.session, &attach), "AttachSessionActionSets"))
        return;
    space.action = X.grip_pose;
    space.poseInActionSpace.orientation.w = 1.0f;
    for (hand = 0; hand < 2; ++hand) {
        space.subactionPath = X.hands[hand];
        check(pxrCreateActionSpace(X.session, &space, &X.hand_spaces[hand]), "CreateActionSpace");
    }
    X.actions_attached = 1;
}

static int read_bool(XrAction action) {
    XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
    XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
    info.action = action;
    return XR_SUCCEEDED(pxrGetActionStateBoolean(X.session, &info, &state)) && state.isActive && state.currentState;
}

static void read_vector(XrAction action, float out[2]) {
    XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
    XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
    info.action = action;
    out[0] = out[1] = 0.0f;
    if (XR_SUCCEEDED(pxrGetActionStateVector2f(X.session, &info, &state)) && state.isActive) {
        out[0] = state.currentState.x;
        out[1] = state.currentState.y;
    }
}

static void sync_input(XrTime time) {
    XrActiveActionSet active = {X.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    int hand;
    memset(&X.input, 0, sizeof(X.input));
    if (!X.actions_attached || X.state != XR_SESSION_STATE_FOCUSED)
        return;
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (!check(pxrSyncActions(X.session, &sync), "SyncActions"))
        return;
    X.input.active = 1;
    read_vector(X.move, X.input.move);
    read_vector(X.look, X.input.look);
    X.input.buttons = (read_bool(X.jump) ? SAGA_XR_BUTTON_JUMP : 0) | (read_bool(X.attack) ? SAGA_XR_BUTTON_ATTACK : 0) |
                      (read_bool(X.special) ? SAGA_XR_BUTTON_SPECIAL : 0) |
                      (read_bool(X.swap) ? SAGA_XR_BUTTON_SWAP : 0) | (read_bool(X.menu) ? SAGA_XR_BUTTON_MENU : 0) |
                      (read_bool(X.recenter) ? SAGA_XR_BUTTON_RECENTER : 0) |
                      (read_bool(X.saber) ? SAGA_XR_BUTTON_SABER : 0) |
                      (read_bool(X.grab_left) ? SAGA_XR_BUTTON_GRAB_LEFT : 0);
    if (X.input.buttons != X.logged_buttons) {
        xr_log("XR buttons 0x%02x (move %.2f, %.2f)", X.input.buttons, X.input.move[0], X.input.move[1]);
        X.logged_buttons = X.input.buttons;
    }
    for (hand = 0; hand < 2; ++hand) {
        XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
        if (X.hand_spaces[hand] != XR_NULL_HANDLE &&
            XR_SUCCEEDED(pxrLocateSpace(X.hand_spaces[hand], X.local_space, time, &location)) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            relative_pose(&location.pose, X.input.hand_orientation[hand], X.input.hand_position[hand]);
            X.input.hand_valid[hand] = 1;
        }
    }
}

EXPORT void __cdecl SagaXrGetInput(SAGA_XR_INPUT *input) {
    *input = X.input;
}

/* ---- setup ---- */

static int create_device(void) {
    XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL, *chosen = NULL;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT i;
    HRESULT hr;
    if (!check(pxrGetD3D11GraphicsRequirementsKHR(X.instance, X.system, &requirements), "GetD3D11GraphicsRequirements"))
        return 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return 0;
    for (i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(adapter, &desc);
        if (memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0) {
            chosen = adapter;
            xr_log("XR adapter: %ls", desc.Description);
            break;
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    hr = D3D11CreateDevice((IDXGIAdapter *)chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                           levels, 3, D3D11_SDK_VERSION, &X.device, NULL, &X.context);
    if (chosen)
        IDXGIAdapter1_Release(chosen);
    if (FAILED(hr)) {
        xr_log("XR D3D11CreateDevice failed: 0x%08lx", (unsigned long)hr);
        return 0;
    }
    return 1;
}

static int create_instance(void) {
    const char *extensions[3] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    uint32_t extension_count = 1, available_count = 0, i;
    XrExtensionProperties available[128];
    XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO};
    XrInstanceProperties properties = {XR_TYPE_INSTANCE_PROPERTIES};
    XrSystemGetInfo system_info = {XR_TYPE_SYSTEM_GET_INFO};
    XrSystemProperties system_properties = {XR_TYPE_SYSTEM_PROPERTIES};
    PFN_xrCreateInstance create;
    PFN_xrEnumerateInstanceExtensionProperties enumerate;
    XrResult result;
    X.loader = LoadLibraryA("openxr_loader.dll");
    if (!X.loader) {
        xr_log("XR: openxr_loader.dll not found next to the game");
        return 0;
    }
    X.gipa = (PFN_xrGetInstanceProcAddr)GetProcAddress(X.loader, "xrGetInstanceProcAddr");
    X.gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create);
    X.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumerate);
    /* Optional controller profiles (Quest 3 Touch Plus, Quest Pro): enable them when offered. */
    for (i = 0; i < 128; ++i) {
        available[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        available[i].next = NULL;
    }
    if (enumerate && XR_SUCCEEDED(enumerate(NULL, 128, &available_count, available))) {
        for (i = 0; i < available_count; ++i) {
            if (strcmp(available[i].extensionName, "XR_META_touch_controller_plus") == 0) {
                extensions[extension_count++] = "XR_META_touch_controller_plus";
                X.has_touch_plus = 1;
            } else if (strcmp(available[i].extensionName, "XR_FB_touch_controller_pro") == 0) {
                extensions[extension_count++] = "XR_FB_touch_controller_pro";
                X.has_touch_pro = 1;
            }
        }
    }
    strcpy(info.applicationInfo.applicationName, "LEGO Star Wars TCS VR");
    info.applicationInfo.applicationVersion = 1;
    strcpy(info.applicationInfo.engineName, "saga-pc");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = extension_count;
    info.enabledExtensionNames = extensions;
    result = create(&info, &X.instance);
    if (XR_FAILED(result)) {
        xr_log("XR xrCreateInstance failed: %d (is the runtime running?)", (int)result);
        X.instance = XR_NULL_HANDLE;
        return 0;
    }
    load_functions();
    if (!create_actions())
        xr_log("XR controller actions unavailable; gamepad and keyboard still work");
    pxrGetInstanceProperties(X.instance, &properties);
    xr_log("XR runtime: %s %u.%u.%u", properties.runtimeName, XR_VERSION_MAJOR(properties.runtimeVersion),
         XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(pxrGetSystem(X.instance, &system_info, &X.system), "GetSystem (headset connected?)"))
        return 0;
    pxrGetSystemProperties(X.instance, X.system, &system_properties);
    xr_log("XR system: %s, position tracking %d", system_properties.systemName,
         (int)system_properties.trackingProperties.positionTracking);
    return 1;
}

static int create_session(void) {
    XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrSessionCreateInfo info = {XR_TYPE_SESSION_CREATE_INFO};
    XrReferenceSpaceCreateInfo space = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    int64_t formats[64];
    uint32_t count = 0, i;
    const int64_t preferred[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                 DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    binding.device = X.device;
    info.next = &binding;
    info.systemId = X.system;
    if (!check(pxrCreateSession(X.instance, &info, &X.session), "CreateSession"))
        return 0;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!check(pxrCreateReferenceSpace(X.session, &space, &X.local_space), "CreateReferenceSpace(LOCAL)"))
        return 0;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!check(pxrCreateReferenceSpace(X.session, &space, &X.view_space), "CreateReferenceSpace(VIEW)"))
        return 0;
    if (check(pxrEnumerateViewConfigurationViews(X.instance, X.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                 &count, views),
              "EnumerateViewConfigurationViews"))
        xr_log("XR recommended eye size %ux%u", views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);
    if (!check(pxrEnumerateSwapchainFormats(X.session, 64, &count, formats), "EnumerateSwapchainFormats"))
        return 0;
    X.format = 0;
    for (i = 0; i < sizeof(preferred) / sizeof(preferred[0]) && !X.format; ++i) {
        uint32_t j;
        for (j = 0; j < count; ++j)
            if (formats[j] == preferred[i])
                X.format = preferred[i];
    }
    if (!X.format) {
        xr_log("XR: no 8-bit RGBA swapchain format offered");
        return 0;
    }
    xr_log("XR swapchain format %d", (int)X.format);
    return 1;
}

/* Writes log\<name> next to the executable into path. */
static void log_path(char *path, const char *name) {
    char *slash;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "log\\");
    strcat(path, name);
}

/* A minidump of the first fault (stacks, globals, referenced memory) for offline analysis. */
static void write_dump(EXCEPTION_POINTERS *exception) {
    typedef BOOL(WINAPI * WRITE_DUMP)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    MINIDUMP_EXCEPTION_INFORMATION info;
    char name[64], path[MAX_PATH];
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    WRITE_DUMP write = dbghelp ? (WRITE_DUMP)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    HANDLE file;
    if (write == NULL)
        return;
    snprintf(name, sizeof(name), "vr_crash_%lu.dmp", (unsigned long)GetCurrentProcessId());
    log_path(path, name);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;
    write(GetCurrentProcess(), GetCurrentProcessId(), file,
          (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                          MiniDumpWithUnloadedModules),
          &info, NULL, NULL);
    CloseHandle(file);
}

/* Crash forensics: the game's fault log only knows addresses. On an access violation, name the module
 * and offset in log\vr_fault.txt next to the executable, with the address space in use, and dump the
 * first one. Observes only (continues the search). */
static LONG CALLBACK fault_observer(EXCEPTION_POINTERS *exception) {
    static volatile LONG reported = 0;
    char path[MAX_PATH], module[MAX_PATH], line[700];
    HMODULE owner = NULL;
    HANDLE file;
    DWORD written;
    MEMORYSTATUSEX memory;
    LONG count;
    void *address = exception->ExceptionRecord->ExceptionAddress;
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        (count = InterlockedIncrement(&reported)) > 4)
        return EXCEPTION_CONTINUE_SEARCH;
    strcpy(module, "(unknown)");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &owner))
        GetModuleFileNameA(owner, module, sizeof(module));
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    snprintf(line, sizeof(line),
             "access violation at %p in %s +0x%lx reading/writing %p (thread %lu, session state %d, frame open %d,"
             " address space used %lu of %lu MB)\r\n",
             address, module, owner ? (unsigned long)((char *)address - (char *)owner) : 0ul,
             exception->ExceptionRecord->NumberParameters >= 2
                 ? (void *)exception->ExceptionRecord->ExceptionInformation[1]
                 : NULL,
             (unsigned long)GetCurrentThreadId(), (int)X.state, X.frame_open,
             (unsigned long)((memory.ullTotalVirtual - memory.ullAvailVirtual) >> 20),
             (unsigned long)(memory.ullTotalVirtual >> 20));
    log_path(path, "vr_fault.txt");
    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
        CloseHandle(file);
    }
    if (count == 1)
        write_dump(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

EXPORT int __cdecl SagaXrInit(int api_version, int simulate, SAGA_XR_LOG log) {
    static int observer_installed = 0;
    memset(&X, 0, sizeof(X));
    X.log = log;
    if (!observer_installed) {
        AddVectoredExceptionHandler(0, fault_observer);
        observer_installed = 1;
    }
    if (api_version != SAGA_XR_API_VERSION) {
        xr_log("saga_vr.dll API %d, game expects %d", SAGA_XR_API_VERSION, api_version);
        return SAGA_XR_MODE_OFF;
    }
    if (simulate) {
        xr_log("XR simulated stereo: no runtime, synthetic 64 mm IPD and 90 x 96 degree eyes");
        X.mode = SAGA_XR_MODE_SIMULATED;
        return X.mode;
    }
    if (!create_instance() || !create_device() || !create_session()) {
        xr_log("XR unavailable; the game stays in mono");
        return SAGA_XR_MODE_OFF;
    }
    X.recenter_pending = 1;
    attach_actions();
    X.mode = SAGA_XR_MODE_HEADSET;
    return X.mode;
}

/* ---- frame loop ---- */

static void poll_events(void) {
    XrEventDataBuffer event;
    for (;;) {
        memset(&event, 0, sizeof(event));
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (pxrPollEvent(X.instance, &event) != XR_SUCCESS)
            return;
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *changed = (XrEventDataSessionStateChanged *)&event;
            X.state = changed->state;
            xr_log("XR session state %d", (int)X.state);
            if (X.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                X.running = check(pxrBeginSession(X.session, &begin), "BeginSession");
            } else if (X.state == XR_SESSION_STATE_FOCUSED) {
                log_interaction_profiles();
            } else if (X.state == XR_SESSION_STATE_STOPPING) {
                X.running = 0;
                check(pxrEndSession(X.session), "EndSession");
            } else if (X.state == XR_SESSION_STATE_EXITING || X.state == XR_SESSION_STATE_LOSS_PENDING) {
                X.running = 0;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            xr_log("XR instance loss pending");
            X.running = 0;
        } else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            log_interaction_profiles();
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            X.recenter_pending = 1;
        }
    }
}

static float simulated_orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
static float simulated_position[3] = {0.0f, 0.0f, 0.0f};

EXPORT void __cdecl SagaXrSetSimulatedHead(const float orientation[4], const float position[3]) {
    memcpy(simulated_orientation, orientation, sizeof(simulated_orientation));
    memcpy(simulated_position, position, sizeof(simulated_position));
}

static void simulated_views(SAGA_XR_VIEW views[2]) {
    int eye;
    for (eye = 0; eye < 2; ++eye) {
        float offset[3] = {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}, rotated[3];
        memset(&views[eye], 0, sizeof(views[eye]));
        memcpy(views[eye].orientation, simulated_orientation, sizeof(simulated_orientation));
        quat_rotate(simulated_orientation, offset, rotated);
        views[eye].position[0] = simulated_position[0] + rotated[0];
        views[eye].position[1] = simulated_position[1] + rotated[1];
        views[eye].position[2] = simulated_position[2] + rotated[2];
        /* Asymmetric like a real headset: wider towards the outer side. */
        views[eye].fov[0] = eye == 0 ? -0.82f : -0.75f;
        views[eye].fov[1] = eye == 0 ? 0.75f : 0.82f;
        views[eye].fov[2] = 0.84f;
        views[eye].fov[3] = -0.84f;
    }
}

EXPORT int __cdecl SagaXrBeginFrame(SAGA_XR_VIEW views[2]) {
    XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
    XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
    XrViewState view_state = {XR_TYPE_VIEW_STATE};
    XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
    uint32_t count = 0;
    X.eyes[0].submitted = X.eyes[1].submitted = 0;
    if (X.mode == SAGA_XR_MODE_SIMULATED) {
        simulated_views(views);
        return 1;
    }
    if (X.mode != SAGA_XR_MODE_HEADSET)
        return 0;
    poll_events();
    if (!X.running)
        return 0;
    X.frame_state.type = XR_TYPE_FRAME_STATE;
    X.frame_state.next = NULL;
    if (!check(pxrWaitFrame(X.session, &wait, &X.frame_state), "WaitFrame"))
        return 0;
    if (!check(pxrBeginFrame(X.session, &begin), "BeginFrame"))
        return 0;
    X.frame_open = 1;
    if (!X.frame_state.shouldRender) {
        SagaXrEndFrame();
        return 0;
    }
    if (X.recenter_pending &&
        XR_SUCCEEDED(pxrLocateSpace(X.view_space, X.local_space, X.frame_state.predictedDisplayTime, &head)) &&
        (head.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        set_origin(&head.pose);
        X.recenter_pending = 0;
    }
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = X.frame_state.predictedDisplayTime;
    locate.space = X.local_space;
    X.raw_views[0].type = X.raw_views[1].type = XR_TYPE_VIEW;
    X.raw_views[0].next = X.raw_views[1].next = NULL;
    if (check(pxrLocateViews(X.session, &locate, &view_state, 2, &count, X.raw_views), "LocateViews") &&
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        relative_view(&X.raw_views[0], &X.last_views[0]);
        relative_view(&X.raw_views[1], &X.last_views[1]);
        X.have_views = 1;
    } else if (!X.have_views) {
        /* Tracking never acquired: nothing sensible to render for the headset yet. */
        SagaXrEndFrame();
        return 0;
    }
    sync_input(X.frame_state.predictedDisplayTime);
    /* Tracking lost after it was valid: keep the last good views (raw_views keep their last values too). */
    views[0] = X.last_views[0];
    views[1] = X.last_views[1];
    return 1;
}

static int create_swapchain(Eye *eye, int width, int height) {
    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    XrSwapchainImageD3D11KHR images[8];
    uint32_t i;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = X.format;
    info.sampleCount = 1;
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!check(pxrCreateSwapchain(X.session, &info, &eye->swapchain), "CreateSwapchain"))
        return 0;
    for (i = 0; i < 8; ++i) {
        images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        images[i].next = NULL;
    }
    if (!check(pxrEnumerateSwapchainImages(eye->swapchain, 8, &eye->image_count,
                                           (XrSwapchainImageBaseHeader *)images),
               "EnumerateSwapchainImages"))
        return 0;
    eye->images = (ID3D11Texture2D **)calloc(eye->image_count, sizeof(*eye->images));
    for (i = 0; i < eye->image_count; ++i)
        eye->images[i] = images[i].texture;
    eye->width = width;
    eye->height = height;
    xr_log("XR swapchain %dx%d with %u images", width, height, eye->image_count);
    return 1;
}

static void keep_copy(Eye *eye, const unsigned char *pixels, int pitch, int width, int height) {
    int y;
    if (eye->copy_width != width || eye->copy_height != height) {
        free(eye->copy);
        eye->copy = (unsigned char *)malloc((size_t)width * height * 4);
        eye->copy_width = width;
        eye->copy_height = height;
    }
    for (y = 0; y < height; ++y)
        memcpy(eye->copy + (size_t)y * width * 4, pixels + (size_t)y * pitch, (size_t)width * 4);
}

EXPORT void __cdecl SagaXrSubmitEye(int eye_index, const void *pixels, int pitch, int width, int height) {
    Eye *eye;
    XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    uint32_t index;
    if (eye_index < 0 || eye_index > 1 || pixels == NULL)
        return;
    eye = &X.eyes[eye_index];
    /* A CPU copy is only needed for simulated captures and the RGBA swizzle; skip it on the BGRA path. */
    if (X.mode != SAGA_XR_MODE_HEADSET || X.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        X.format == DXGI_FORMAT_R8G8B8A8_UNORM)
        keep_copy(eye, (const unsigned char *)pixels, pitch, width, height);
    if (X.mode != SAGA_XR_MODE_HEADSET || !X.frame_open)
        return;
    if (eye->swapchain != XR_NULL_HANDLE && (eye->width != width || eye->height != height)) {
        pxrDestroySwapchain(eye->swapchain);
        eye->swapchain = XR_NULL_HANDLE;
        free(eye->images);
        eye->images = NULL;
    }
    if (eye->swapchain == XR_NULL_HANDLE && !create_swapchain(eye, width, height))
        return;
    if (!check(pxrAcquireSwapchainImage(eye->swapchain, &acquire, &index), "AcquireSwapchainImage"))
        return;
    wait.timeout = XR_INFINITE_DURATION;
    if (check(pxrWaitSwapchainImage(eye->swapchain, &wait), "WaitSwapchainImage")) {
        if (X.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || X.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            /* The game's pixels are BGRX: swap to RGBA in the kept copy's layout. */
            static unsigned char *swapped;
            static size_t swapped_size;
            size_t size = (size_t)width * height * 4, i;
            if (swapped_size < size) {
                free(swapped);
                swapped = (unsigned char *)malloc(size);
                swapped_size = size;
            }
            for (i = 0; i < size; i += 4) {
                swapped[i] = eye->copy[i + 2];
                swapped[i + 1] = eye->copy[i + 1];
                swapped[i + 2] = eye->copy[i];
                swapped[i + 3] = 255;
            }
            ID3D11DeviceContext_UpdateSubresource(X.context, (ID3D11Resource *)eye->images[index], 0, NULL, swapped,
                                                  (UINT)width * 4, 0);
        } else {
            ID3D11DeviceContext_UpdateSubresource(X.context, (ID3D11Resource *)eye->images[index], 0, NULL, pixels,
                                                  (UINT)pitch, 0);
        }
        eye->submitted = 1;
    }
    check(pxrReleaseSwapchainImage(eye->swapchain, &release), "ReleaseSwapchainImage");
}

EXPORT void __cdecl SagaXrEndFrame(void) {
    XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2];
    const XrCompositionLayerBaseHeader *layers[1];
    int eye;
    if (X.mode != SAGA_XR_MODE_HEADSET || !X.frame_open)
        return;
    X.frame_open = 0;
    end.displayTime = X.frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (X.frame_state.shouldRender && X.eyes[0].submitted && X.eyes[1].submitted) {
        for (eye = 0; eye < 2; ++eye) {
            memset(&views[eye], 0, sizeof(views[eye]));
            views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            views[eye].pose = X.raw_views[eye].pose;
            views[eye].fov = X.raw_views[eye].fov;
            views[eye].subImage.swapchain = X.eyes[eye].swapchain;
            views[eye].subImage.imageRect.extent.width = X.eyes[eye].width;
            views[eye].subImage.imageRect.extent.height = X.eyes[eye].height;
        }
        layer.space = X.local_space;
        layer.viewCount = 2;
        layer.views = views;
        layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
        end.layerCount = 1;
        end.layers = layers;
    }
    check(pxrEndFrame(X.session, &end), "EndFrame");
}

EXPORT void __cdecl SagaXrEndFrameScreen(float width, float aspect) {
    XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *layers[1];
    float half = 0.5f * X.origin_yaw, ahead[3] = {0.0f, 0.0f, -2.5f}, rotated[3];
    float yaw[4] = {0.0f, sinf(half), 0.0f, cosf(half)};
    if (X.mode != SAGA_XR_MODE_HEADSET || !X.frame_open)
        return;
    X.frame_open = 0;
    end.displayTime = X.frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (X.frame_state.shouldRender && X.eyes[0].submitted && aspect > 0.0f) {
        /* Facing the player from the recentred origin, at standing eye height relative to it. */
        quat_rotate(yaw, ahead, rotated);
        quad.space = X.local_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = X.eyes[0].swapchain;
        quad.subImage.imageRect.extent.width = X.eyes[0].width;
        quad.subImage.imageRect.extent.height = X.eyes[0].height;
        quad.pose.orientation.x = yaw[0];
        quad.pose.orientation.y = yaw[1];
        quad.pose.orientation.z = yaw[2];
        quad.pose.orientation.w = yaw[3];
        quad.pose.position.x = X.origin_position[0] + rotated[0];
        quad.pose.position.y = X.origin_position[1] + rotated[1];
        quad.pose.position.z = X.origin_position[2] + rotated[2];
        quad.size.width = width;
        quad.size.height = width / aspect;
        layers[0] = (const XrCompositionLayerBaseHeader *)&quad;
        end.layerCount = 1;
        end.layers = layers;
    }
    check(pxrEndFrame(X.session, &end), "EndFrame(screen)");
}

EXPORT void __cdecl SagaXrRecenter(void) {
    X.recenter_pending = 1;
}

EXPORT int __cdecl SagaXrSaveStereo(const char *path) {
    Eye *left = &X.eyes[0], *right = &X.eyes[1];
    int width, height, row_size, x, y;
    unsigned char header[54] = {'B', 'M'};
    unsigned char *row;
    FILE *file;
    if (!left->copy || !right->copy || left->copy_height != right->copy_height)
        return 0;
    width = left->copy_width + right->copy_width;
    height = left->copy_height;
    row_size = (width * 3 + 3) & ~3;
    *(uint32_t *)(header + 2) = 54u + (uint32_t)row_size * height;
    *(uint32_t *)(header + 10) = 54;
    *(uint32_t *)(header + 14) = 40;
    *(int32_t *)(header + 18) = width;
    *(int32_t *)(header + 22) = height;
    *(uint16_t *)(header + 26) = 1;
    *(uint16_t *)(header + 28) = 24;
    file = fopen(path, "wb");
    if (!file)
        return 0;
    fwrite(header, 1, sizeof(header), file);
    row = (unsigned char *)calloc(1, (size_t)row_size);
    for (y = height - 1; y >= 0; --y) {
        for (x = 0; x < width; ++x) {
            Eye *eye = x < left->copy_width ? left : right;
            int ex = x < left->copy_width ? x : x - left->copy_width;
            const unsigned char *pixel = eye->copy + ((size_t)y * eye->copy_width + ex) * 4;
            row[x * 3] = pixel[0];
            row[x * 3 + 1] = pixel[1];
            row[x * 3 + 2] = pixel[2];
        }
        fwrite(row, 1, (size_t)row_size, file);
    }
    free(row);
    fclose(file);
    return 1;
}

EXPORT void __cdecl SagaXrShutdown(void) {
    int eye;
    if (X.mode == SAGA_XR_MODE_HEADSET) {
        if (X.frame_open)
            SagaXrEndFrame();
        if (X.running)
            pxrRequestExitSession(X.session);
        for (eye = 0; eye < 2; ++eye)
            if (X.eyes[eye].swapchain)
                pxrDestroySwapchain(X.eyes[eye].swapchain);
        if (X.view_space)
            pxrDestroySpace(X.view_space);
        if (X.local_space)
            pxrDestroySpace(X.local_space);
        if (X.session)
            pxrDestroySession(X.session);
    }
    if (X.instance)
        pxrDestroyInstance(X.instance);
    if (X.context)
        ID3D11DeviceContext_Release(X.context);
    if (X.device)
        ID3D11Device_Release(X.device);
    for (eye = 0; eye < 2; ++eye)
        free(X.eyes[eye].copy);
    xr_log("XR shut down");
    memset(&X, 0, sizeof(X));
}
