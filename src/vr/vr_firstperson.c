/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* VR fork, stage 2: optional first-person camera.
 *
 * The view has its own yaw, decoupled from the character: LEGO characters turn to face the way they
 * walk, so a view that followed their facing would spin as soon as the player walks backwards. In
 * first person the left stick's x turns the view (keyboard A/D), its y walks forwards/backwards
 * along the view, and VR adds head tracking plus snap turns from the right stick. The camera sits at
 * player 0's eyes, just in front of the face, with a short near clip so hands and lightsaber stay
 * visible. The game still moves the character relative to its own third-person camera, so the walk
 * input is rotated by the yaw between the two cameras. */
#include "vr/vr_internal.h"

#include <math.h>

#define SAGA_VR_TAU 6.2831853f
#define SAGA_VR_FP_TURN_SPEED 2.6f /* radians per second at full stick */
#define SAGA_VR_POD_EYE 0.14f        /* eye height above the pod's cockpit joint: where the hidden pilot's eyes were */
#define SAGA_VR_POD_EYE_AHEAD -0.02f /* and ahead of it, with both levers within reach in front */
#define SAGA_VR_POD_FALLING 100.0f   /* height above which a pod is still dropping in at the level's start */
#define SAGA_VR_SHIP_EYE 0.45f      /* eye height in a ship, as a fraction of its height above its origin */

static i32 SagaVrFpHave = 0;
static f32 SagaVrFpYaw = 0.0f;
static f32 SagaVrFpHeight = 0.0f;
static u32 SagaVrFpFrame = 0xffffffffu;
static f32 SagaVrStickRotation = 0.0f;
static i32 SagaVrFpActive = 0;
static f32 SagaVrFpTurnInput = 0.0f; /* strongest x push this frame, -1..1 */
static f32 SagaVrFpSnap = 0.0f;      /* pending snap turn, radians */

static f32 SagaVrWrapAngle(f32 angle) {
    while (angle > 3.1415927f)
        angle -= SAGA_VR_TAU;
    while (angle < -3.1415927f)
        angle += SAGA_VR_TAU;
    return angle;
}

/* Yaw of a camera matrix: its forward row (2) projected on the floor, measured from +Z towards +X. */
static f32 SagaVrMatrixYaw(const f32 *matrix) {
    return (f32)atan2(matrix[8], matrix[10]);
}

/* The character sitting on a ridden vehicle: player 0 may be the vehicle (control passes to it) or the
 * rider itself (context 0x3b). NULL when not riding. */
GAMEOBJECT *__cdecl SagaVrRider(GAMEOBJECT *player) {
    if (player == NULL || player->rider == NULL)
        return NULL;
    return player->character_context == 0x3b ? player : player->rider;
}

/* Pod races: player 0 is the pod itself, driven from its cockpit. */
i32 __cdecl SagaVrOnPod(GAMEOBJECT *player) {
    return player != NULL && WORLD != NULL && WORLD->area != NULL &&
           (WORLD->area == PODRACE_ADATA || WORLD->area == PODSPRINT_ADATA);
}

f32 __cdecl SagaVrPodCabinHeight(void) {
    return SagaVrFpHeight - SAGA_VR_POD_EYE;
}

void __cdecl SagaVrFirstPersonSnapTurn(f32 radians) {
    if (SagaVrFpActive && !SagaVrCinema())
        SagaVrFpSnap += radians;
}

i32 __cdecl SagaVrFirstPersonCamera(f32 *camera_matrix) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    GAMEOBJECT *seat;
    f32 facing, eye_height, blend, frame_time, c, s, cockpit[3];
    i32 on_pod, in_ship;
    /* Never on the title level: its "player" is a menu prop and the menu needs the original camera. Nor in the
     * ship and speeder levels: their craft turn fast and on their own, which from inside made them unplayable
     * (the user's call: third person there, with the game's camera). The pod keeps its cockpit. */
    if (!control->first_person || player == NULL || WORLD == NULL || WORLD->current_level == TITLES_LDATA ||
        SagaVrCinema() || (SagaVrOnPod(player) && player->position.y > SAGA_VR_POD_FALLING) ||
        (VehicleArea != 0 && !SagaVrOnPod(player))) {
        SagaVrFpHave = 0;
        SagaVrFpActive = 0;
        SagaVrStickRotation = 0.0f;
        control->stick_rotation = 0.0f;
        return 0;
    }
    /* TT angles: 0x10000 per turn; forward = (sin, 0, cos). */
    facing = (f32)player->facing_angle * (SAGA_VR_TAU / 65536.0f);
    /* Driving: the controlled object is the vehicle and its rider (context 0x3b) sits in the seat; the
     * eyes are the rider's, not the vehicle's. */
    seat = SagaVrRider(player);
    if (seat == NULL)
        seat = player;
    eye_height = seat->position.y + seat->scaled_height * control->fp_eye_height;
    on_pod = SagaVrOnPod(player) && SagaVrPodCockpit(cockpit);
    in_ship = SagaVrInShip(player);
    if (in_ship)
        eye_height = player->position.y + player->scaled_height * SAGA_VR_SHIP_EYE;
    if (on_pod)
        eye_height = player->position.y + cockpit[1] + SAGA_VR_POD_EYE;
    control->player_position[0] = player->position.x;
    control->player_position[1] = player->position.y;
    control->player_position[2] = player->position.z;
    control->player_facing = facing * (360.0f / SAGA_VR_TAU);
    control->player_height = player->scaled_height;
    /* Update once per frame; the engine sets the camera several times per frame. */
    if (control->frame != SagaVrFpFrame) {
        SagaVrFpFrame = control->frame;
        frame_time = nuapi.frame_time;
        if (frame_time <= 0.0f || frame_time > 0.1f)
            frame_time = 1.0f / 60.0f;
        if (!SagaVrFpHave) {
            /* Entering first person: look where the character looks. */
            SagaVrFpYaw = facing;
            SagaVrFpHeight = eye_height;
            SagaVrFpHave = 1;
        } else {
            SagaVrFpYaw += SagaVrFpTurnInput * SAGA_VR_FP_TURN_SPEED * frame_time + SagaVrFpSnap;
            /* A pod or a ship carries the view along its heading. */
            if (on_pod || in_ship)
                SagaVrFpYaw += SagaVrWrapAngle(facing - SagaVrFpYaw) * (f32)(1.0 - exp(-frame_time / 0.2f));
            SagaVrFpYaw = SagaVrWrapAngle(SagaVrFpYaw);
            /* Only height is smoothed: jumps and steps should not jerk the view. */
            blend = control->fp_smoothing > 0.0f ? (f32)(1.0 - exp(-frame_time / control->fp_smoothing)) : 1.0f;
            SagaVrFpHeight += (eye_height - SagaVrFpHeight) * blend;
        }
        SagaVrFpTurnInput = 0.0f;
        SagaVrFpSnap = 0.0f;
        /* The game turns the stick into a world direction with GamePad_InputAngle: the camera's input_yaw,
         * or in some areas (socks flagged 0x40, e.g. Mustafar's lava platforms) a fixed per-area yaw. Its
         * offset from the raw stick angle is that reference; the camera matrix's own yaw is not, and on
         * those platforms the walk went sideways and seemed to slow down. */
        if (player->pad != NULL) {
            u16 reference = (u16)(GamePad_InputAngle(player, player->pad) - player->pad->input_angle);
            SagaVrStickRotation = SagaVrWrapAngle(SagaVrFpYaw - (f32)reference * (SAGA_VR_TAU / 65536.0f));
            if (control->frame % 300 == 0 &&
                fabs(SagaVrWrapAngle((f32)reference * (SAGA_VR_TAU / 65536.0f) - SagaVrMatrixYaw(camera_matrix))) > 0.1f)
                LOG_INFO("VR stick reference %.1f deg differs from the camera yaw %.1f deg (area input yaw)",
                         (f32)reference * (360.0f / 65536.0f), SagaVrMatrixYaw(camera_matrix) * (360.0f / SAGA_VR_TAU));
        } else {
            SagaVrStickRotation = SagaVrWrapAngle(SagaVrFpYaw - SagaVrMatrixYaw(camera_matrix));
        }
        /* Standing still with VR controllers, the body turns to where the player looks: the game aims
         * the Force, attacks and shots along the body's facing, not the camera. Only in the normal state
         * (context -1): turning the body mid-Force makes the game drop the Force when the target falls
         * outside the head's turning range. */
        {
            f32 move[2], look[2];
            u32 buttons;
            if (SagaVrControllerInput(move, look, &buttons) && player->pad != NULL &&
                player->pad->input_magnitude == 0.0f && player->character_context == -1 && player->rider == NULL &&
                VehicleArea == 0) {
                f32 view = SagaVrWrapAngle(SagaVrFpYaw + SagaVrHeadYaw());
                u16 angle = (u16)(i32)((view < 0.0f ? view + SAGA_VR_TAU : view) * (65536.0f / SAGA_VR_TAU));
                player->facing_angle = angle;
                player->movement_facing_angle = angle;
                player->field_0x276 = angle;
            }
        }
        control->stick_rotation = SagaVrStickRotation * (360.0f / SAGA_VR_TAU);
    }
    SagaVrFpActive = 1;
    c = (f32)cos(SagaVrFpYaw);
    s = (f32)sin(SagaVrFpYaw);
    /* Rows: right, up, forward, position (left-handed, +Y up). */
    camera_matrix[0] = c, camera_matrix[1] = 0.0f, camera_matrix[2] = -s, camera_matrix[3] = 0.0f;
    camera_matrix[4] = 0.0f, camera_matrix[5] = 1.0f, camera_matrix[6] = 0.0f, camera_matrix[7] = 0.0f;
    camera_matrix[8] = s, camera_matrix[9] = 0.0f, camera_matrix[10] = c, camera_matrix[11] = 0.0f;
    camera_matrix[13] = SagaVrFpHeight;
    if (on_pod) {
        /* The cockpit in the pod's own axes (right, up, forward). */
        f32 pod_c = (f32)cos(facing), pod_s = (f32)sin(facing);
        cockpit[2] += SAGA_VR_POD_EYE_AHEAD;
        camera_matrix[12] = player->position.x + cockpit[0] * pod_c + cockpit[2] * pod_s;
        camera_matrix[14] = player->position.z - cockpit[0] * pod_s + cockpit[2] * pod_c;
    } else if (in_ship) {
        camera_matrix[12] = player->position.x;
        camera_matrix[14] = player->position.z;
    } else {
        camera_matrix[12] = seat->position.x + s * control->fp_forward;
        camera_matrix[14] = seat->position.z + c * control->fp_forward;
    }
    camera_matrix[15] = 1.0f;
    return 1;
}

i32 __cdecl SagaVrFirstPersonActive(void) {
    return SagaVrFpActive;
}

/* Stick convention: x grows right, y grows down (128 centre). With VR controllers the stick only walks,
 * in any direction relative to where the player looks (body plus head yaw); the view turns with the head
 * and the right stick's snap turns. Without them (keyboard/pad on a monitor) x turns the view and y walks
 * along it. Either way the walk is rotated into the game camera's frame. */
void __cdecl SagaVrRotateStick(FRAME_SCANNED_PAD_PC *pad) {
    f32 x, forward, c, s, rx, rf, rotation, strength, largest, move[2], look[2];
    u32 buttons;
    i32 out_x, out_y;
    /* Menus and cutscenes need the raw stick: the view rotation (stale while the game is paused, as the
     * camera stops updating) turned up/down into diagonals and menus missed moves. */
    if (!SagaVrFpActive || !pad->is_valid || SagaVrCinema())
        return;
    x = (f32)pad->analog_left_x - 128.0f;
    forward = 128.0f - (f32)pad->analog_left_y;
    rotation = SagaVrStickRotation;
    if (SagaVrControllerInput(move, look, &buttons)) {
        /* Vehicles steer relative to themselves, whose heading the view follows: the head looks around freely. */
        if (VehicleArea == 0)
            rotation += SagaVrHeadYaw();
    } else {
        if (fabs(x) > 0.25f * 127.0f && fabs(x / 127.0f) > fabs(SagaVrFpTurnInput))
            SagaVrFpTurnInput = x > 127.0f ? 1.0f : x < -127.0f ? -1.0f : x / 127.0f;
        x = 0.0f;
        if (fabs(forward) < 0.25f * 127.0f)
            forward = 0.0f;
    }
    if (x == 0.0f && forward == 0.0f)
        return;
    c = (f32)cos(rotation);
    s = (f32)sin(rotation);
    rx = x * c + forward * s;
    rf = forward * c - x * s;
    /* The game takes stick strength from the larger axis (MovePlayer: below 0.8 it walks), so a full push
     * rotated near a diagonal, 0.71 on each axis, read as a walk. Keep the push's strength on that axis. */
    strength = (f32)sqrt(x * x + forward * forward);
    if (strength > 127.0f)
        strength = 127.0f;
    largest = (f32)(fabs(rx) > fabs(rf) ? fabs(rx) : fabs(rf));
    if (largest > 0.0f) {
        rx *= strength / largest;
        rf *= strength / largest;
    }
    out_x = (i32)(128.0f + rx);
    out_y = (i32)(128.0f - rf);
    pad->analog_left_x = (u8)(out_x < 0 ? 0 : out_x > 255 ? 255 : out_x);
    pad->analog_left_y = (u8)(out_y < 0 ? 0 : out_y > 255 ? 255 : out_y);
}

/* Force targets in first person: by angle from the gaze (head included), the distance only breaking
 * near-ties, so the object you look at is the one that gets built or moved. */
f32 __cdecl SagaVrForceScore(GAMEOBJECT *object, NUVEC *position, f32 distance) {
    const f32 *view = SagaVrViewMatrix();
    f32 direction[3], forward[3], length, dot;
    i32 c;
    if (object == NULL || object != (GAMEOBJECT *)Player[0] || !SagaVrFpActive || view == NULL || position == NULL)
        return distance;
    direction[0] = position->x - view[12];
    direction[1] = position->y - view[13];
    direction[2] = position->z - view[14];
    for (c = 0; c < 3; ++c)
        forward[c] = view[8 + c];
    length = (f32)sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]) *
             (f32)sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (length < 1e-6f)
        return distance;
    dot = (direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2]) / length;
    return (1.0f - dot) * 100.0f + distance * 0.01f;
}

/* Enemy scripts only attack while the attacker itself was drawn last frame (model_draw_result), i.e. on
 * the game camera's screen. The third-person camera frames everything around the player; the headset
 * view does not, so in first person characters near the player count as on screen for the AI. Runs
 * after drawing; the next draw recomputes the real value. */
#define SAGA_VR_AI_AWARE_RADIUS 8.0f

void __cdecl SagaVrFirstPersonAfterDraw(void) {
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0], *object;
    NUVEC difference;
    i32 i;
    if (!SagaVrFpActive || player == NULL || Obj == NULL)
        return;
    for (i = 0, object = Obj; i < HIGHGAMEOBJECT; ++i, ++object) {
        if (object == player || object->model_draw_result != 0 || object->dead != 0 ||
            object->character_data == NULL)
            continue;
        /* Scale-zero characters are deliberately hidden (not spawned yet): leave them alone. */
        if (object->transform.m00 == 0.0f && object->transform.m11 == 0.0f && object->transform.m22 == 0.0f)
            continue;
        if (NuVecDistSqr(&object->position, &player->position, &difference) <
            SAGA_VR_AI_AWARE_RADIUS * SAGA_VR_AI_AWARE_RADIUS)
            object->model_draw_result = 1;
    }
}

void __cdecl SagaVrScreenPosition(GAMEOBJECT *object, NUVEC *screen_position) {
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    if (object == NULL || player == NULL || !SagaVrFpActive ||
        (object != player && object != SagaVrRider(player)))
        return;
    screen_position->x = 0.0f;
    screen_position->y = 0.0f;
    if (!(screen_position->z > 0.0f))
        screen_position->z = 0.5f;
}
