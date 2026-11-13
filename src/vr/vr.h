/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
#ifndef SAGA_VR_H
#define SAGA_VR_H

/* VR fork hooks. Only built with SAGA_VR=1; Matching and Debug never include this module. */
#ifdef SAGA_VR
#include "saga.h"

#ifdef __cplusplus
extern "C" {
#endif
    /* Called by NuCameraSetEx after it copies the incoming camera: may move global_camera.mtx. */
    void __cdecl SagaVrCameraAdjust(NUCAMERA *camera);
    /* Called once per presented frame. */
    void __cdecl SagaVrFrameEnd(void);
    /* Stereo replay in NuRndrSwapScreen: returns 2 to replay the recorded scenes per eye, 1 for mono. */
    i32 __cdecl SagaVrStereoBegin(void);
    void __cdecl SagaVrStereoEye(i32 eye, i32 eyes);
    void __cdecl SagaVrStereoEyeDone(i32 eye, i32 eyes);
    void __cdecl SagaVrStereoEnd(i32 eyes);
    /* Game systems the main loop leaves stubbed in saga-pc; the VR build calls them (SAGA_VR_RESTORED). */
    void __cdecl Grabber_Update(WORLDINFO *world);
    void __cdecl Grabber_Draw(WORLDINFO *world);
    void __cdecl Grabber_Reset(WORLDINFO *world);
    void __cdecl UpdateCables(void);
    void __cdecl DrawCables(void);
    void __cdecl Detonators_Update(void);
    void __cdecl Detonators_Draw(void);
    /* Per-eye replacement for a recorded camera packet (returns its inputs outside stereo). */
    NUMTX *__cdecl SagaVrEyeView(NUMTX *view, NUMTX *projection);
    NUMTX *__cdecl SagaVrEyeProjection(NUMTX *view, NUMTX *projection);
    /* Per-eye copy of a 2D primitive's vertices placed on the fixed HUD panel (its input outside stereo). */
    const void *__cdecl SagaVrHudVertices(const void *vertices, u32 count);
    /* Virtual pad (scanned device SAGA_VR_PAD_DEVICE) fed by the control block or VR controllers. */
    void __cdecl SagaVrPadInject(i32 device, FRAME_SCANNED_PAD_PC *pad);
    /* Virtual keys (DirectInput scan codes) merged into a polled keyboard page; works unfocused. */
    u8 *__cdecl SagaVrKeyboardInject(u8 *page);
    /* Back buffer size for device creation/reset: each eye is rendered at this size (saga_vr.ini). */
    void __cdecl SagaVrBackBufferSize(u32 *width, u32 *height, BOOL windowed);
    /* Motion-controlled lightsaber (vr_saber.c): moves the saber joint to the right hand after the
     * skeleton is evaluated, keeps the blade lit, and takes hits from the real blade while swinging. */
    void __cdecl SagaVrCharacterJoints(GAMEOBJECT *object, CHARACTER_MODEL_LAYERS_PC *model, NUMTX *world,
                                       NUMTX *joints);
    void __cdecl SagaVrSabreFlags(GAMEOBJECT *object);
    /* Whether the hand-held blade may hurt this target now: one hit per saber_cooldown, not one per frame. */
    i32 __cdecl SagaVrSabreHit(GAMEOBJECT *object, GAMEOBJECT *target);
    i32 __cdecl SagaVrFirstPersonActive(void);
    /* The speed multiplier a pod seeks, given the game's: in its cockpit the pod only moves when told to. */
    f32 __cdecl SagaVrPodSpeed(GAMEOBJECT *object, f32 target);
    i32 __cdecl SagaVrOnPod(GAMEOBJECT *player);
    void __cdecl SagaVrSabrePoints(GAMEOBJECT *object, NUVEC *points, i32 joint_a, i32 joint_b, NUVEC *minimum,
                                   NUVEC *maximum);
    /* DrawWeapons: whether to draw one weapon part (hilt or blade pieces); a switched-off VR saber keeps
     * only its hilt. */
    i32 __cdecl SagaVrDrawWeaponPart(GAMEOBJECT *object, i32 is_hilt);
    /* DrawWeapons: stretches the VR blade along its axis while it ignites or retracts. */
    void __cdecl SagaVrBladeMatrix(GAMEOBJECT *object, NUMTX *blade_matrix);
    /* Movie (Bink) frames bypass NuRndrSwapScreen: show each one on a screen in the headset. */
    void __cdecl SagaVrMovieFrame(void);
    /* Around the HUD drawing: its 3D pieces go on the fixed HUD panel instead of in front of the head. */
    void __cdecl SagaVrPanelBegin(void);
    void __cdecl SagaVrPanelEnd(void);
    /* Around the character's main render: the body's bounds no longer contain the hand-held saber. */
    void __cdecl SagaVrDrawBegin(GAMEOBJECT *object);
    void __cdecl SagaVrDrawEnd(GAMEOBJECT *object);
    /* Scene geometry upload: a vertex/index buffer the device refused (NULL) is retried, then created in
     * system memory, instead of crashing the loader. */
    void __cdecl SagaVrSceneBuffer(PC_BUFFER_RESOURCE_PC **resource, u32 size, i32 is_index);
    /* Force target choice: in first person, the candidate closest to where the player looks wins (the game
     * takes the nearest one in front of the body). Returns the score compared in place of the distance. */
    f32 __cdecl SagaVrForceScore(GAMEOBJECT *object, NUVEC *position, f32 distance);
    /* Bolt_Shoot: in first person the player's shots leave the right hand along the controller, with an
     * optional aim assist towards the enemy nearest that line (saga_vr.ini aim_assist, degrees). */
    void __cdecl SagaVrBoltAim(GAMEOBJECT *object, NUVEC *shot_position, NUMTX *matrix);
    /* UpdateGameObjects: the projected screen position of a character. Player 0 in first person sits at the
     * eye, so its projection jumps to the screen edges; scripts that need the player on screen (the Force
     * cancels at |x| > 0.85, drop-in, targeting) get the centre instead, as in third person. */
    void __cdecl SagaVrScreenPosition(GAMEOBJECT *object, NUVEC *screen_position);
    /* UpdateStatusScreen returns early while no status stage is pending, which also skips its own "after
     * 5 seconds, leave" check (non-matching decompilation): the end-of-chapter screen hung on its
     * countdown. In the VR build the idle screen leaves after 5 seconds as that check intends. */
    i32 __cdecl SagaVrStatusIdle(i32 idle);
    void __cdecl SagaVrAiState(AISCRIPTPROCESS_PC *processor, AISTATE_PC *state);
    i32 __cdecl SagaVrIntroGate(void);
    void __cdecl SagaVrAiDump(const char *path, const char *name, const void *data, i32 size);
    void __cdecl SagaVrTriggerTrace(i32 index, u32 occupied, u32 flags, i32 complete, i32 count, const i8 *indices);
#ifdef __cplusplus
}
#endif

#define SAGA_VR_CAMERA_ADJUST(camera) SagaVrCameraAdjust(camera)
#define SAGA_VR_FRAME_END() SagaVrFrameEnd()
/* The desktop window is the VR mirror; exclusive fullscreen would lose the device on focus loss. */
#define SAGA_VR_WINDOWED(native) 1
/* The window is only the VR mirror: leave the Windows key working and the mouse free (the original took the
 * mouse exclusively and blocked the Windows key, so a player in the headset could not get back to the desktop). */
#define SAGA_VR_INPUT_COOPERATIVE(flags) ((flags) == 5 ? 6 : (flags) & ~0x10)
#define SAGA_VR_EYE_LOOP_BEGIN                                                                                         \
    {                                                                                                                  \
        i32 saga_vr_eyes = SagaVrStereoBegin();                                                                        \
        i32 saga_vr_eye;                                                                                               \
        for (saga_vr_eye = 0; saga_vr_eye < saga_vr_eyes; ++saga_vr_eye) {                                             \
            SagaVrStereoEye(saga_vr_eye, saga_vr_eyes);
#define SAGA_VR_EYE_LOOP_END                                                                                           \
    SagaVrStereoEyeDone(saga_vr_eye, saga_vr_eyes);                                                                    \
    }                                                                                                                  \
    SagaVrStereoEnd(saga_vr_eyes);                                                                                     \
    }
#define SAGA_VR_EYE_VIEW(view, projection) SagaVrEyeView(view, projection)
#define SAGA_VR_EYE_PROJECTION(view, projection) SagaVrEyeProjection(view, projection)
#define SAGA_VR_PAD_DEVICE 9
#define SAGA_VR_PAD_INJECT(device, pad) SagaVrPadInject(device, pad)
#define SAGA_VR_KEYBOARD(page) SagaVrKeyboardInject(page)
#define SAGA_VR_BACKBUFFER_SIZE(width, height, windowed) SagaVrBackBufferSize(width, height, windowed)
#define SAGA_VR_CHARACTER_JOINTS(object, model, world, joints) SagaVrCharacterJoints(object, model, world, joints)
#define SAGA_VR_SABRE_FLAGS(object) SagaVrSabreFlags(object)
#define SAGA_VR_SABRE_HIT(object, target, hit)                                                                         \
    if (SagaVrSabreHit(object, target))                                                                                \
    hit
#define SAGA_VR_SABRE_POINTS(object, points, joint_a, joint_b, minimum, maximum)                                      \
    SagaVrSabrePoints(object, points, joint_a, joint_b, minimum, maximum)
#define SAGA_VR_DRAW_WEAPON_PART(object, is_hilt) SagaVrDrawWeaponPart(object, is_hilt)
#define SAGA_VR_BLADE_MATRIX(object, blade_matrix) SagaVrBladeMatrix(object, blade_matrix)
#define SAGA_VR_MOVIE_FRAME() SagaVrMovieFrame()
#define SAGA_VR_PANEL_BEGIN() SagaVrPanelBegin()
#define SAGA_VR_PANEL_END() SagaVrPanelEnd()
#define SAGA_VR_DRAW_BEGIN(object) SagaVrDrawBegin(object)
#define SAGA_VR_DRAW_END(object) SagaVrDrawEnd(object)
#define SAGA_VR_SCENE_BUFFER(resource, size, is_index) SagaVrSceneBuffer(resource, size, is_index)
#define SAGA_VR_FORCE_SCORE(object, position, distance) SagaVrForceScore(object, position, distance)
#define SAGA_VR_BOLT_AIM(object, shot_position, matrix) SagaVrBoltAim(object, shot_position, matrix)
/* Enemy attack scripts skip a player that is respawn-protected. spawn_protection_timer is set to 2.5 s on
 * respawn and counts down, but the decompiled test is inverted (it skips every player except during
 * the first second after a respawn), so enemies never attack. The VR build uses the intended test. */
#define SAGA_VR_PLAYER_SHIELDED(timer) ((timer) > 1.5f)
#define SAGA_VR_SCREEN_POSITION(object, screen_position) SagaVrScreenPosition(object, screen_position)
#define SAGA_VR_STATUS_IDLE(idle) SagaVrStatusIdle(idle)
/* 2D overlays (texts, prompts, HUD) on a panel fixed in front of the player instead of stuck to the view. */
#define SAGA_VR_HUD_VERTICES(vertices, count) SagaVrHudVertices(vertices, count)
/* CurTerr and CurrentTerrain are one variable in the original game (same address). The source keeps them
 * apart and only assigns CurrentTerrain, so reading CurTerr crashes on Vader_C's rocks before the final fight. */
#define SAGA_VR_CUR_TERR(terrain) CurrentTerrain
/* A pod leans into its turns through an animation overlay; from its cockpit the lean would swing the whole
 * cockpit around the player's head. */
#define SAGA_VR_POD_LEAN(index, weight) ((index) == 0 && SagaVrFirstPersonActive() ? 0.0f : (weight))
#define SAGA_VR_POD_SPEED(object, target) SagaVrPodSpeed(object, target)
/* A pod starting more than 5 units above the floor finds none, and the hover height added to that "no floor"
 * value (2000000) puts it in the sky: it spends the start countdown falling. No floor keeps its start position. */
#define SAGA_VR_POD_HOVER_Y(object, y) (SagaVrOnPod(object) && (y) > 1000000.0f ? 2000000.0f : (y))
/* Face-on (billboard) packets: the handler zeroes the position, sets opacity 1 and transposes the matrices in
 * place, so a second eye's replay drew light cones, glows, stud sparkles and saber colour wrongly or not at all.
 * In stereo the handler works on a copy. */
#define SAGA_VR_FACEON_COPY(packet, type)                                                                              \
    type saga_vr_packet_copy = *(packet);                                                                              \
    (packet) = &saga_vr_packet_copy;
/* Grabbers (cranes, claws), tow cables (Hoth's snowspeeders) and thermal detonators are decompiled but their
 * main-loop calls are still stubs in saga-pc, which left those puzzles unplayable. */
#define SAGA_VR_RESTORED(call) (call)
/* A destroyed turret blows up at its model's draw position, which can be missing (no animated object or no draw
 * matrix): the explosion then read a NULL position and crashed (tester, Episode II). Fall back to the turret's own
 * position, and never blow up at NULL. */
#define SAGA_VR_TURRET_DRAW_POS(object, fallback, original)                                                            \
    ((object) != NULL && NuSpecialGetDrawPos(&(object)->special) != NULL ? NuSpecialGetDrawPos(&(object)->special)      \
                                                                         : (fallback))
#define SAGA_VR_RETURN_IF_NULL(pointer)                                                                                \
    if ((pointer) == NULL)                                                                                             \
        return;
/* AI script loading: logs each condition/action a level script names that saga-pc does not implement (the script
 * then skips it, which can leave characters waiting forever). Diagnostic only. */
#define SAGA_VR_AI_MISSING(kind, name)                                                                                 \
    if ((name) != NULL && (name)[0] != '{' && (name)[0] != '/')                                                        \
    LOG_WARN("VR AI missing %s '%s' (script %s)", kind, (name) != NULL ? (name) : "?",                              \
             aiscript_load_script != NULL && aiscript_load_script->name != NULL ? aiscript_load_script->name : "?");
/* AIScriptSetState: logs state changes of the scripts named by saga_vr.ini ai_trace (diagnostic for AI that stalls). */
#define SAGA_VR_AI_STATE(processor, state) SagaVrAiState(processor, state)
/* Start-up: "A long time ago..." waits until the RanitaVR plate has been shown after the legal screen. */
#define SAGA_VR_INTRO_GATE() SagaVrIntroGate()
/* AIScriptProcessStateConditions (non-matching in saga-pc): retail keeps the condition value across the list, so
 * "PreviousResult" (a condition without an eval function) reads the previous condition's value; upstream saga-pc
 * has the same fix. Reset per condition, it was always 0 and Maul stalled after his third platform. */
#define SAGA_VR_CONDITION_VALUE_INIT(value) (value) = 0.0f
#define SAGA_VR_CONDITION_VALUE_RESET(value) ((void)0)
/* AI script loading: copies each level script to logi_scripts next to the executable (diagnostic). */
#define SAGA_VR_AI_DUMP(path, name, data, size) SagaVrAiDump(path, name, data, size)
/* AITriggerSetSysProcess: logs each change of a trigger set (pads, levers the party helps with): occupied pads,
 * completion and which character is sent to which pad (diagnostic). */
#define SAGA_VR_TRIGGER_TRACE(index, occupied, flags, complete, count, indices)                                        \
    SagaVrTriggerTrace(index, occupied, flags, complete, count, indices)
/* ZapCode (Jawa, Watto, Ugnaught, R2): the zapper only reaches a droid 0.6 units away, nearly touching. In first
 * person, with the target hard to judge, player 0 reaches 2 units. Squared distance. */
#define SAGA_VR_ZAP_RANGE(object, original)                                                                            \
    ((object) == (GAMEOBJECT *)Player[0] && SagaVrFirstPersonActive() ? 4.0f : (original))
#else
#define SAGA_VR_AI_STATE(processor, state) ((void)0)
#define SAGA_VR_INTRO_GATE() 1
#define SAGA_VR_CONDITION_VALUE_INIT(value) ((void)0)
#define SAGA_VR_CONDITION_VALUE_RESET(value) (value) = 0.0f
#define SAGA_VR_AI_DUMP(path, name, data, size) ((void)0)
#define SAGA_VR_TRIGGER_TRACE(index, occupied, flags, complete, count, indices) ((void)0)
#define SAGA_VR_ZAP_RANGE(object, original) (original)
#define SAGA_VR_AI_MISSING(kind, name)
#define SAGA_VR_TURRET_DRAW_POS(object, fallback, original) (original)
#define SAGA_VR_RETURN_IF_NULL(pointer)
#define SAGA_VR_FACEON_COPY(packet, type)
#define SAGA_VR_RESTORED(call) ((void)0)
#define SAGA_VR_CAMERA_ADJUST(camera) ((void)0)
#define SAGA_VR_FRAME_END() ((void)0)
#define SAGA_VR_WINDOWED(native) (native)
#define SAGA_VR_INPUT_COOPERATIVE(flags) (flags)
#define SAGA_VR_EYE_LOOP_BEGIN
#define SAGA_VR_EYE_LOOP_END
#define SAGA_VR_EYE_VIEW(view, projection) (view)
#define SAGA_VR_EYE_PROJECTION(view, projection) (projection)
#define SAGA_VR_PAD_INJECT(device, pad) ((void)0)
#define SAGA_VR_KEYBOARD(page) (page)
#define SAGA_VR_BACKBUFFER_SIZE(width, height, windowed) ((void)0)
#define SAGA_VR_CHARACTER_JOINTS(object, model, world, joints) ((void)0)
#define SAGA_VR_SABRE_FLAGS(object) ((void)0)
#define SAGA_VR_SABRE_HIT(object, target, hit) hit
#define SAGA_VR_SABRE_POINTS(object, points, joint_a, joint_b, minimum, maximum) ((void)0)
#define SAGA_VR_DRAW_WEAPON_PART(object, is_hilt) 1
#define SAGA_VR_BLADE_MATRIX(object, blade_matrix) ((void)0)
#define SAGA_VR_MOVIE_FRAME() ((void)0)
#define SAGA_VR_PANEL_BEGIN() ((void)0)
#define SAGA_VR_PANEL_END() ((void)0)
#define SAGA_VR_DRAW_BEGIN(object) ((void)0)
#define SAGA_VR_DRAW_END(object) ((void)0)
#define SAGA_VR_SCENE_BUFFER(resource, size, is_index) ((void)0)
#define SAGA_VR_FORCE_SCORE(object, position, distance) (distance)
#define SAGA_VR_BOLT_AIM(object, shot_position, matrix) ((void)0)
#define SAGA_VR_PLAYER_SHIELDED(timer) (!((timer) > 1.5f))
#define SAGA_VR_SCREEN_POSITION(object, screen_position) ((void)0)
#define SAGA_VR_STATUS_IDLE(idle) (idle)
#define SAGA_VR_HUD_VERTICES(vertices, count) (vertices)
#define SAGA_VR_CUR_TERR(terrain) (terrain)
#define SAGA_VR_POD_LEAN(index, weight) (weight)
#define SAGA_VR_POD_SPEED(object, target) (target)
#define SAGA_VR_POD_HOVER_Y(object, y) (y)
#endif

#endif
