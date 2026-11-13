/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* VR fork, stage 3: motion-controlled lightsaber.
 *
 * In first person with a tracked right controller, player 0's saber follows the hand 1:1:
 *  - SagaVrCharacterJoints runs after the skeleton is evaluated and before locators are stored, and
 *    re-poses the joint carrying the blade (and its descendants) so the hilt sits in the grip and the
 *    blade points along the controller. Mesh, streaks and blade locators all follow from that.
 *  - SagaVrSabreFlags drops the streak "aura" and, while the hand is swinging, arms the hit with a
 *    damage value like the game's own combo frames (deflection and breakables follow from it).
 *  - The rest of the player's body is collapsed in first person so it never blocks the view.
 *  - The right grip (or F7) switches the blade on and off.
 *  - SagaVrSabrePoints undoes the original attack's fixed arc in front of the character so hits come
 *    from where the real blade is. Damage, deflection and breakables stay the game's own code. */
#include "vr/vr_internal.h"
#include "../../xr/saga_xr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static u32 SagaVrSaberFrame = 0xffffffffu;
static i32 SagaVrSaberFrameDriving = 0;
static i32 SagaVrSaberLastReason = -1;

/* Logs why the motion saber is not driving, once per change of reason. */
static void SagaVrSaberReason(i32 reason, const char *text, i32 a, i32 b) {
    if (reason != SagaVrSaberLastReason) {
        LOG_INFO("VR saber: %s (%d, %d)", text, a, b);
        SagaVrSaberLastReason = reason;
    }
}

static GAMEOBJECT *SagaVrSaberPlayer(void) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    if (!control->saber || !control->first_person || player == NULL || !control->hand_valid[1] || WORLD == NULL ||
        WORLD->current_level == TITLES_LDATA)
        return NULL;
    if (player->character_data == NULL || player->character_data->game_character == NULL ||
        player->character_data->game_character->streak_joints[0][0] < 0 ||
        player->character_data->game_character->streak_joints[0][1] < 0)
        return NULL;
    return player;
}

/* Ignition: the blade grows out of the hilt (and retracts) over SAGA_VR_BLADE_SECONDS, like the
 * game's own draw/holster, with its SaberOn/SaberOff sounds. 0 = bare hilt, 1 = full blade. */
#define SAGA_VR_BLADE_SECONDS 0.25f
static f32 SagaVrBladeScale = 1.0f;
static i32 SagaVrBladeTarget = 1;
static u32 SagaVrBladeFrame = 0xffffffffu;

static void SagaVrUpdateBlade(GAMEOBJECT *player) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    f32 step;
    if (control->frame == SagaVrBladeFrame)
        return;
    SagaVrBladeFrame = control->frame;
    if (player == NULL) {
        /* Not holding the VR saber: be ready lit or unlit without a transition when it comes back. */
        SagaVrBladeTarget = control->saber_lit;
        SagaVrBladeScale = control->saber_lit ? 1.0f : 0.0f;
        return;
    }
    if (control->saber_lit != SagaVrBladeTarget) {
        SagaVrBladeTarget = control->saber_lit;
        PlaySfx(SagaVrBladeTarget ? "SaberOn" : "SaberOff", &player->collision_position);
    }
    step = (nuapi.frame_time > 0.0f && nuapi.frame_time < 0.1f ? nuapi.frame_time : 1.0f / 60.0f) /
           SAGA_VR_BLADE_SECONDS;
    if (SagaVrBladeTarget)
        SagaVrBladeScale = SagaVrBladeScale + step > 1.0f ? 1.0f : SagaVrBladeScale + step;
    else
        SagaVrBladeScale = SagaVrBladeScale - step < 0.0f ? 0.0f : SagaVrBladeScale - step;
}

/* Called with every world camera set, before characters are drawn. In first person the player's own
 * bounds sit behind the camera and the whole model would be culled (no skeleton, no saber); the
 * "always draw" flag 0x200 skips that test while the VR hands (saber or bare) are in use. */
void __cdecl SagaVrSaberPrepare(void) {
    static GAMEOBJECT *forced = NULL;
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
    /* Any first-person player model must still "draw": the AI only engages a player whose
     * model_draw_result is non-zero, and turrets check it too. */
    if (player == NULL || !control->first_person || WORLD == NULL || WORLD->current_level == TITLES_LDATA ||
        SagaVrCinema())
        player = NULL;
    SagaVrUpdateBlade(SagaVrSaberPlayer());
    if (forced != NULL && forced != player) {
        forced->flags_1f8 &= ~0x200u;
        forced = NULL;
    }
    if (player != NULL && (player->flags_1f8 & 0x200u) == 0) {
        player->flags_1f8 |= 0x200u;
        forced = player;
    }
}

i32 __cdecl SagaVrSaberDriving(void) {
    return SagaVrSaberFrameDriving && SagaVrSaberFrame + 1 >= SagaVrGetControl()->frame;
}

static f32 SagaVrLength(const f32 *v) {
    return (f32)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void SagaVrNormalize(f32 *v) {
    f32 length = SagaVrLength(v);
    if (length > 1e-6f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

static void SagaVrCross(f32 *out, const f32 *a, const f32 *b) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Orthonormal basis rows (axis, up, side) with axis exact and up made perpendicular to it. */
static void SagaVrBasis(f32 *rows, const f32 *axis, const f32 *up_hint) {
    f32 dot;
    memcpy(rows, axis, sizeof(f32) * 3);
    SagaVrNormalize(rows);
    memcpy(rows + 3, up_hint, sizeof(f32) * 3);
    dot = rows[0] * rows[3] + rows[1] * rows[4] + rows[2] * rows[5];
    rows[3] -= rows[0] * dot, rows[4] -= rows[1] * dot, rows[5] -= rows[2] * dot;
    SagaVrNormalize(rows + 3);
    SagaVrCross(rows + 6, rows, rows + 3);
}

/* Re-poses the joint carrying two locators (a = grip end, b = far end) and its subtree so the a->b axis
 * runs along the right grip's forward row with a at the grip; marks the moved subtree in descendant[].
 * The move starts at the joint's parent (the wrist) so the minifig hand stays wrapped around the item.
 * size scales the joint (1 = the model's own). Used for the saber blade and blaster barrels: a weapon keeps
 * its locator order (grip, muzzle) and its locator's up for the roll, and pitch_degrees tilts the axis
 * about the grip's side axis (a blaster points below the controller handle). */
static i32 SagaVrPoseAlongGrip(NUHGOBJ_EVAL_PC *hierarchy, NUMTX *world, NUMTX *joints, u8 *descendant,
                               NUHGOBJ_POI_PC *poi_a, NUHGOBJ_POI_PC *poi_b, const f32 *hand_world, f32 size,
                               i32 weapon, f32 pitch_degrees) {
    f32 world_inverse[16], hand_model[16], new_joint[16], joint_inverse[16], delta[16], moved[16];
    f32 base[3], tip[3], blade_local[3], up_local[3], local_rows[9], model_rows[9], rotation[9], scale;
    f32 forward_model[3], up_model[3];
    NUMTX inverse;
    i32 joint = poi_a->joint_index, k, r, c;
    /* Blade locators in the joint's own space; the one nearer the joint origin is the hilt end. */
    memcpy(base, &poi_a->matrix.m30, sizeof(base));
    memcpy(tip, &poi_b->matrix.m30, sizeof(tip));
    if (!weapon && SagaVrLength(tip) < SagaVrLength(base)) {
        f32 swap[3];
        memcpy(swap, base, sizeof(swap));
        memcpy(base, tip, sizeof(base));
        memcpy(tip, swap, sizeof(tip));
    }
    blade_local[0] = tip[0] - base[0], blade_local[1] = tip[1] - base[1], blade_local[2] = tip[2] - base[2];
    if (SagaVrLength(blade_local) < 1e-5f)
        return 0;
    /* Any up hint not parallel to the blade: blade roll is irrelevant for a cylinder. */
    up_local[0] = 0.0f, up_local[1] = 1.0f, up_local[2] = 0.0f;
    if (weapon)
        memcpy(up_local, &poi_a->matrix.m10, sizeof(up_local));
    if (fabs(up_local[0] * blade_local[0] + up_local[1] * blade_local[1] + up_local[2] * blade_local[2]) >
        0.9f * SagaVrLength(blade_local) * SagaVrLength(up_local))
        up_local[0] = 1.0f, up_local[1] = 0.0f, up_local[2] = 0.0f;
    SagaVrBasis(local_rows, blade_local, up_local);

    /* The grip pose in the character's model space; the blade runs along the grip's forward row. */
    NuMtxInv(&inverse, world);
    memcpy(world_inverse, &inverse, sizeof(world_inverse));
    SagaVrMul(hand_model, hand_world, world_inverse);
    memcpy(forward_model, hand_model + 8, sizeof(forward_model));
    memcpy(up_model, hand_model + 4, sizeof(up_model));
    if (pitch_degrees != 0.0f) {
        f32 cp = (f32)cos(pitch_degrees * 0.017453293f), sp = (f32)sin(pitch_degrees * 0.017453293f), f;
        for (c = 0; c < 3; ++c) {
            f = forward_model[c];
            forward_model[c] = f * cp + up_model[c] * sp;
            up_model[c] = up_model[c] * cp - f * sp;
        }
    }
    SagaVrBasis(model_rows, forward_model, up_model);

    /* rotation = local_rows^T * model_rows maps the blade basis onto the grip basis. */
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c)
            rotation[r * 3 + c] = local_rows[0 * 3 + r] * model_rows[0 * 3 + c] +
                                  local_rows[1 * 3 + r] * model_rows[1 * 3 + c] +
                                  local_rows[2 * 3 + r] * model_rows[2 * 3 + c];
    /* Keep the joint's own scale (characters are scaled through their joints). */
    scale = SagaVrLength(&joints[joint].m00) * (size > 0.0f ? size : 1.0f);
    memset(new_joint, 0, sizeof(new_joint));
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c)
            new_joint[r * 4 + c] = rotation[r * 3 + c] * scale;
    /* Hilt at the grip: grip = base * new_joint, so translation = grip - base * rotation. */
    for (c = 0; c < 3; ++c)
        new_joint[12 + c] = hand_model[12 + c] - (base[0] * new_joint[0 * 4 + c] + base[1] * new_joint[1 * 4 + c] +
                                                  base[2] * new_joint[2 * 4 + c]);
    new_joint[15] = 1.0f;

    /* Move the joint and everything below it by the same rigid delta. */
    NuMtxInv(&inverse, &joints[joint]);
    memcpy(joint_inverse, &inverse, sizeof(joint_inverse));
    SagaVrMul(delta, joint_inverse, new_joint);
    /* The visible hand is skinned to the blade joint's parent (the wrist): move from there so the
     * minifig hand stays wrapped around the hilt. */
    {
        u8 wrist_parent = hierarchy->joints[joint].parent_index;
        if (wrist_parent != 0xff)
            joint = wrist_parent;
    }
    /* descendant[] accumulates (the caller clears it once; both hands may hold a blade), so this subtree
     * is tracked on its own. */
    {
        u8 subtree[256];
        memset(subtree, 0, sizeof(subtree));
        for (k = joint; k < hierarchy->joint_count && k < 256; ++k) {
            u8 parent = hierarchy->joints[k].parent_index;
            if (k != joint && (parent == 0xff || !subtree[parent]))
                continue;
            subtree[k] = 1;
            descendant[k] = 1;
            SagaVrMul(moved, &joints[k].m00, delta);
            memcpy(&joints[k], moved, sizeof(moved));
        }
    }
    return 1;
}

/* Re-poses the blade joint and its subtree at the right grip; marks that subtree in descendant[]. */
static i32 SagaVrPoseSaber(GAMEOBJECT *object, CHARACTER_MODEL_LAYERS_PC *model, NUMTX *world, NUMTX *joints,
                           u8 *descendant) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    PLAYERCHARACTERCONFIG_PC *data;
    NUHGOBJ_EVAL_PC *hierarchy;
    NUHGOBJ_POI_PC *poi_a, *poi_b;
    f32 hand_world[16];
    i32 joint;
    if (world == NULL)
        return 0;
    if (object != SagaVrSaberPlayer()) {
        GAMEOBJECT *player = (GAMEOBJECT *)Player[0];
        SagaVrSaberReason(1, "inactive: saber/first_person/hand or no blade joints", control->hand_valid[1],
                          player->character_data != NULL && player->character_data->game_character != NULL
                              ? player->character_data->game_character->streak_joints[0][0]
                              : -2);
        return 0;
    }
    data = object->character_data->game_character;
    hierarchy = model->hierarchy;
    if (hierarchy == NULL || model->points_of_interest[data->streak_joints[0][0]] == NULL ||
        model->points_of_interest[data->streak_joints[0][1]] == NULL) {
        SagaVrSaberReason(2, "blade locators missing on this model", data->streak_joints[0][0],
                          data->streak_joints[0][1]);
        return 0;
    }
    poi_a = &hierarchy->points[hierarchy->point_index_map[data->streak_joints[0][0]]];
    poi_b = &hierarchy->points[hierarchy->point_index_map[data->streak_joints[0][1]]];
    joint = poi_a->joint_index;
    if (poi_b->joint_index != joint || joint >= hierarchy->joint_count) {
        SagaVrSaberReason(3, "blade locators on different joints", poi_a->joint_index, poi_b->joint_index);
        return 0;
    }
    if (!SagaVrHandWorld(1, hand_world)) {
        SagaVrSaberReason(4, "no hand pose or camera rig this frame", control->hand_valid[1],
                          SagaVrGetRig() != NULL);
        return 0;
    }
    SagaVrSaberReason(0, "driving the blade joint", joint, hierarchy->joint_count);
    if (!SagaVrPoseAlongGrip(hierarchy, world, joints, descendant, poi_a, poi_b, hand_world, control->saber_scale, 0,
                             0.0f))
        return 0;
    /* Keep the saber drawn: the game skips holstered blades (weapon_scale below 1). */
    object->weapon_scale = 1.0f;
    SagaVrSaberFrame = control->frame;
    SagaVrSaberFrameDriving = 1;
    control->saber_driving = (i32)control->frame;
    return 1;
}

/* Frame of the last blaster pose: its locators then follow the hand and give the barrel's direction. */
static u32 SagaVrBlasterFrame;
/* The barrel tilt of the last posed blaster (degrees about the grip's side axis); shots leave along it too. */
static f32 SagaVrBlasterPitchUsed = -40.0f;

typedef struct SAGA_VR_HANDS {
    NUHGOBJ_EVAL_PC *hierarchy;
    i32 tip[2];
    i32 wrist[2];
    f32 rest[2][9]; /* wrist rest orientation in model space, orthonormal rows (see geometric) */
    i32 geometric;  /* rest is relative to the forearm frame instead of the grip axes */
} SAGA_VR_HANDS;

/* Hands of the current skeleton (SagaVrFindHands). */
static SAGA_VR_HANDS SagaVrHands;

/* A blaster (or any held weapon with a muzzle) in the right hand: the grip locator sits in the hand and
 * grip->muzzle points along the controller. On pistols the grip locator sits in the handle, below the barrel, so
 * that line runs above the barrel and blaster_pitch tilts it back down. When the grip locator is the weapon's
 * origin (Watto's zapper, bowcasters, a Geonosian's blaster without a grip locator), the line is the barrel
 * itself and needs no tilt: with it those weapons pointed up and shot diagonally. The game holsters blasters
 * between shots; in first person the weapon stays out. */
static i32 SagaVrPoseBlaster(GAMEOBJECT *object, CHARACTER_MODEL_LAYERS_PC *model, NUMTX *world, NUMTX *joints,
                             u8 *descendant) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    PLAYERCHARACTERCONFIG_PC *data;
    NUHGOBJ_EVAL_PC *hierarchy = model->hierarchy;
    NUHGOBJ_POI_PC *grip, *muzzle;
    f32 hand_world[16], difference[3];
    NUHGOBJ_POI_PC origin_grip;
    i32 c;
    if (world == NULL || object->character_data == NULL || object->character_data->game_character == NULL)
        return 0;
    data = object->character_data->game_character;
    if (data->weapon_shoot_joints[0] < 0 || model->points_of_interest[data->weapon_shoot_joints[0]] == NULL)
        return 0;
    muzzle = &hierarchy->points[hierarchy->point_index_map[data->weapon_shoot_joints[0]]];
    if (data->weapon_joints[0] >= 0 && model->points_of_interest[data->weapon_joints[0]] != NULL) {
        grip = &hierarchy->points[hierarchy->point_index_map[data->weapon_joints[0]]];
    } else {
        /* No grip locator (a Geonosian's blaster): hold the weapon joint by its origin, if it hangs off the right
         * arm (a muzzle on the head or body, as on droids and creatures, stays where it is). */
        i32 joint = muzzle->joint_index, steps = 0, wrist = SagaVrHands.wrist[1];
        while (joint >= 0 && joint < hierarchy->joint_count && joint != wrist && steps++ < 64)
            joint = hierarchy->joints[joint].parent_index == 0xff ? -1 : hierarchy->joints[joint].parent_index;
        if (wrist < 0 || joint != wrist)
            return 0;
        origin_grip = *muzzle;
        origin_grip.matrix.m30 = origin_grip.matrix.m31 = origin_grip.matrix.m32 = 0.0f;
        grip = &origin_grip;
    }
    if (grip->joint_index != muzzle->joint_index || grip->joint_index >= hierarchy->joint_count)
        return 0;
    for (c = 0; c < 3; ++c)
        difference[c] = (&muzzle->matrix.m30)[c] - (&grip->matrix.m30)[c];
    if (SagaVrLength(difference) < 1e-4f || !SagaVrHandWorld(1, hand_world))
        return 0;
    {
        f32 pitch = grip == &origin_grip || SagaVrLength(&grip->matrix.m30) < 0.01f ? 0.0f : control->blaster_pitch;
        if (!SagaVrPoseAlongGrip(hierarchy, world, joints, descendant, grip, muzzle, hand_world,
                                 control->blaster_scale, 1, pitch))
            return 0;
        SagaVrBlasterPitchUsed = pitch;
    }
    /* Out for the game too: otherwise a press only draws a weapon that already looks drawn, and never fires. */
    if ((object->flags_e20 & 0x10000) == 0)
        SetWeaponOut(object);
    object->weapon_scale = 1.0f;
    SagaVrBlasterFrame = control->frame;
    return 1;
}

/* Player 0 in first person: pose the saber at the hand (when lit) and hide the rest of the body by
 * collapsing every other joint's axes to zero (it keeps its position, so effects and locators that
 * read positions still work). Looking down then shows the floor, not the inside of the minifig. */
static i32 SagaVrJointDepth(NUHGOBJ_EVAL_PC *hierarchy, i32 joint) {
    i32 depth = 0;
    while (joint >= 0 && joint < hierarchy->joint_count && hierarchy->joints[joint].parent_index != 0xff && depth < 64) {
        joint = hierarchy->joints[joint].parent_index;
        ++depth;
    }
    return depth;
}

/* Hands of any character, found once per skeleton from its rest pose (the bind chain): the two leaf
 * joints at the same depth whose positions mirror across x = 0 are the arm tips (Qui-Gon: 14 and 20),
 * preferring the deepest pair; the hand mesh hangs off each tip's parent, the wrist. The right hand is
 * on the -x side (or the one carrying the saber's blade locators). Index 0 = left, 1 = right. */
static void SagaVrFindHands(NUHGOBJ_EVAL_PC *hierarchy, i32 saber_joint) {
    static f32 rest[256][16];
    u8 has_child[256];
    i32 count, k, a, b, h, r, c, best_depth = -1, tips[2] = {-1, -1}, depth, depth_a, depth_b, tip_depth[2] = {0, 0};
    f32 best_score = 1e30f, score, separation, *pa, *pb, low = 1e30f, high = -1e30f;
    if (SagaVrHands.hierarchy == hierarchy)
        return;
    SagaVrHands.hierarchy = hierarchy;
    SagaVrHands.tip[0] = SagaVrHands.tip[1] = SagaVrHands.wrist[0] = SagaVrHands.wrist[1] = -1;
    count = hierarchy->joint_count < 256 ? hierarchy->joint_count : 256;
    memset(has_child, 0, sizeof(has_child));
    for (k = 0; k < count; ++k) {
        u8 parent = hierarchy->joints[k].parent_index;
        if (parent != 0xff && parent < count) {
            has_child[parent] = 1;
            SagaVrMul(rest[k], &hierarchy->bind_matrices[k].m00, rest[parent]);
        } else {
            memcpy(rest[k], &hierarchy->bind_matrices[k].m00, sizeof(rest[k]));
        }
    }
    /* Legs mirror too (C-3PO's feet are the deepest pair): arm tips sit above the bottom third. */
    for (k = 0; k < count; ++k) {
        low = rest[k][13] < low ? rest[k][13] : low;
        high = rest[k][13] > high ? rest[k][13] : high;
    }
    for (a = 0; a < count; ++a) {
        if (has_child[a] || SagaVrJointDepth(hierarchy, a) < 3)
            continue;
        for (b = a + 1; b < count; ++b) {
            /* Arms may differ by one joint: some skeletons (TC-14) give only the weapon hand an end joint. */
            depth_a = SagaVrJointDepth(hierarchy, a);
            depth_b = SagaVrJointDepth(hierarchy, b);
            if (has_child[b] || depth_b < depth_a - 1 || depth_b > depth_a + 1)
                continue;
            pa = &rest[a][12];
            pb = &rest[b][12];
            separation = (f32)fabs(pa[0] - pb[0]);
            score = (f32)(fabs(pa[0] + pb[0]) + fabs(pa[1] - pb[1]) + fabs(pa[2] - pb[2]));
            if (separation < 0.04f || score > 0.5f * separation ||
                0.5f * (pa[1] + pb[1]) < low + 0.3f * (high - low))
                continue;
            depth = depth_a < depth_b ? depth_a : depth_b;
            if (depth > best_depth || (depth == best_depth && score < best_score)) {
                best_depth = depth;
                best_score = score;
                tips[0] = rest[a][12] > rest[b][12] ? a : b; /* left: +x */
                tips[1] = tips[0] == a ? b : a;
                tip_depth[0] = tips[0] == a ? depth_a : depth_b;
                tip_depth[1] = tips[1] == a ? depth_a : depth_b;
            }
        }
    }
    if (tips[0] >= 0 && saber_joint >= 0 && saber_joint == tips[0]) {
        /* The saber is held in the right hand whatever side the model put it on. */
        tips[0] = tips[1];
        tips[1] = saber_joint;
        h = tip_depth[0];
        tip_depth[0] = tip_depth[1];
        tip_depth[1] = h;
    }
    for (h = 0; h < 2; ++h) {
        SagaVrHands.tip[h] = tips[h];
        if (tips[h] < 0)
            continue;
        /* The hand mesh hangs off the tip's parent, except on an arm one joint shorter than the other,
         * whose tip already is the hand. */
        if (tip_depth[h] < tip_depth[1 - h])
            k = tips[h];
        else
            k = hierarchy->joints[tips[h]].parent_index != 0xff ? hierarchy->joints[tips[h]].parent_index : tips[h];
        SagaVrHands.wrist[h] = k;
        /* Symmetric arms (Qui-Gon and most minifigs): the wrist keeps its own rest orientation relative
         * to the controller, as tuned in the headset (the left one turned 180 degrees). Arms one joint
         * apart (TC-14) orient their wrists differently and twisted that way, so they are placed by
         * geometry: the rest forearm (elbow -> wrist) maps to the controller's forward axis and the
         * minifig's front (-z) to its up, stored as wrist_rest * forearm_basis^T. */
        SagaVrHands.geometric = tip_depth[0] != tip_depth[1];
        if (!SagaVrHands.geometric) {
            for (r = 0; r < 3; ++r) {
                memcpy(&SagaVrHands.rest[h][r * 3], &rest[k][r * 4], sizeof(f32) * 3);
                SagaVrNormalize(&SagaVrHands.rest[h][r * 3]);
            }
            continue;
        }
        {
            f32 wrist_rows[9], forearm[3], frame[9];
            static const f32 front[3] = {0.0f, 0.0f, -1.0f};
            i32 parent = hierarchy->joints[k].parent_index != 0xff ? hierarchy->joints[k].parent_index : k;
            i32 i;
            for (r = 0; r < 3; ++r) {
                memcpy(&wrist_rows[r * 3], &rest[k][r * 4], sizeof(f32) * 3);
                SagaVrNormalize(&wrist_rows[r * 3]);
                forearm[r] = rest[k][12 + r] - rest[parent][12 + r];
            }
            if (SagaVrLength(forearm) < 1e-5f)
                forearm[0] = 0.0f, forearm[1] = -1.0f, forearm[2] = 0.0f;
            SagaVrBasis(frame, forearm, front);
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) {
                    f32 sum = 0.0f;
                    for (i = 0; i < 3; ++i)
                        sum += wrist_rows[r * 3 + i] * frame[c * 3 + i];
                    SagaVrHands.rest[h][r * 3 + c] = sum;
                }
        }
    }
    LOG_INFO("VR hands: left tip %d wrist %d, right tip %d wrist %d (depth %d)", SagaVrHands.tip[0],
             SagaVrHands.wrist[0], SagaVrHands.tip[1], SagaVrHands.wrist[1], best_depth);
    {
        /* Skeleton summary for tuning the search: joint:parent:leaf:x,y,z (rest pose). */
        char line[2048];
        i32 used = 0;
        for (k = 0; k < count && used < (i32)sizeof(line) - 48; ++k)
            used += _snprintf(line + used, sizeof(line) - used, " %d:%d:%d:%.2f,%.2f,%.2f", k,
                             hierarchy->joints[k].parent_index == 0xff ? -1 : hierarchy->joints[k].parent_index,
                             !has_child[k], rest[k][12], rest[k][13], rest[k][14]);
        LOG_INFO("VR skeleton (%d joints):%s", count, line);
    }
}

/* Applies the rigid move taking joints[joint] to new_joint onto joint and its whole subtree. */
static void SagaVrMoveSubtree(NUHGOBJ_EVAL_PC *hierarchy, NUMTX *joints, i32 joint, const f32 *new_joint, u8 *keep) {
    f32 joint_inverse[16], delta[16], moved[16];
    u8 subtree[256];
    NUMTX inverse;
    i32 k;
    NuMtxInv(&inverse, &joints[joint]);
    memcpy(joint_inverse, &inverse, sizeof(joint_inverse));
    SagaVrMul(delta, joint_inverse, new_joint);
    memset(subtree, 0, sizeof(subtree));
    for (k = joint; k < hierarchy->joint_count && k < 256; ++k) {
        u8 parent = hierarchy->joints[k].parent_index;
        if (k != joint && (parent == 0xff || !subtree[parent]))
            continue;
        subtree[k] = 1;
        keep[k] = 1;
        SagaVrMul(moved, &joints[k].m00, delta);
        memcpy(&joints[k], moved, sizeof(moved));
    }
}

/* Where a hand is held instead of its controller's pose: a pod's lever, while gripped. */
static const f32 *SagaVrHeldHand[2];
/* Size of the posed hand relative to the model: a pod's cockpit is smaller than a walking minifig. */
static f32 SagaVrHandSize = 1.0f;

/* A hand without a saber: the wrist keeps its rest-pose orientation relative to the controller (a
 * controller held level and pointing ahead gives the rest pose) and sits at the grip. */
static i32 SagaVrPoseRestHand(NUHGOBJ_EVAL_PC *hierarchy, NUMTX *world, NUMTX *joints, i32 hand, u8 *keep) {
    f32 hand_world[16], world_inverse[16], hand_model[16], grip[9], new_joint[16], scale, palm[3];
    NUMTX inverse;
    i32 wrist = SagaVrHands.wrist[hand], r, c;
    if (wrist < 0 || world == NULL)
        return 0;
    if (SagaVrHeldHand[hand] != NULL)
        memcpy(hand_world, SagaVrHeldHand[hand], sizeof(hand_world));
    else if (!SagaVrHandWorld(hand, hand_world))
        return 0;
    NuMtxInv(&inverse, world);
    memcpy(world_inverse, &inverse, sizeof(world_inverse));
    SagaVrMul(hand_model, hand_world, world_inverse);
    for (r = 0; r < 3; ++r) {
        memcpy(&grip[r * 3], &hand_model[r * 4], sizeof(f32) * 3);
        SagaVrNormalize(&grip[r * 3]);
    }
    /* Per-hand correction (saga_vr.ini *_hand_yaw/pitch/roll) applied in the controller's own frame:
     * grip = roll * pitch * yaw * grip (row vectors). */
    {
        const f32 *angles = SagaVrGetControl()->hand_rotation[hand];
        /* The wrist-relative placement needs the left hand turned around (tuned in the headset). */
        f32 yaw_degrees = angles[0] + (hand == 0 && !SagaVrHands.geometric ? 180.0f : 0.0f);
        f32 cy = (f32)cos(yaw_degrees * 0.017453293f), sy = (f32)sin(yaw_degrees * 0.017453293f);
        f32 cp = (f32)cos(angles[1] * 0.017453293f), sp = (f32)sin(angles[1] * 0.017453293f);
        f32 cr = (f32)cos(angles[2] * 0.017453293f), sr = (f32)sin(angles[2] * 0.017453293f);
        f32 yaw[9], pitch[9], roll[9], rp[9], correction[9], corrected[9];
        i32 i, j;
        memset(yaw, 0, sizeof(yaw));
        memset(pitch, 0, sizeof(pitch));
        memset(roll, 0, sizeof(roll));
        yaw[0] = cy, yaw[2] = -sy, yaw[4] = 1.0f, yaw[6] = sy, yaw[8] = cy;
        pitch[0] = 1.0f, pitch[4] = cp, pitch[5] = sp, pitch[7] = -sp, pitch[8] = cp;
        roll[0] = cr, roll[1] = sr, roll[3] = -sr, roll[4] = cr, roll[8] = 1.0f;
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j)
                rp[i * 3 + j] = roll[i * 3 + 0] * pitch[0 * 3 + j] + roll[i * 3 + 1] * pitch[1 * 3 + j] +
                                roll[i * 3 + 2] * pitch[2 * 3 + j];
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j)
                correction[i * 3 + j] = rp[i * 3 + 0] * yaw[0 * 3 + j] + rp[i * 3 + 1] * yaw[1 * 3 + j] +
                                        rp[i * 3 + 2] * yaw[2 * 3 + j];
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j)
                corrected[i * 3 + j] = correction[i * 3 + 0] * grip[0 * 3 + j] + correction[i * 3 + 1] * grip[1 * 3 + j] +
                                       correction[i * 3 + 2] * grip[2 * 3 + j];
        memcpy(grip, corrected, sizeof(grip));
    }
    /* Geometric placement: same row order as the rest forearm frame, (forward, up, side). */
    if (SagaVrHands.geometric) {
        f32 frame[9];
        SagaVrBasis(frame, &grip[6], &grip[3]);
        memcpy(grip, frame, sizeof(grip));
    }
    scale = SagaVrLength(&joints[wrist].m00);
    /* The tip of the hand in the wrist's own axes, while the joints are still in their rest pose. */
    if (SagaVrHeldHand[hand] != NULL && SagaVrHands.tip[hand] >= 0) {
        for (r = 0; r < 3; ++r) {
            const f32 *row = &joints[wrist].m00 + r * 4;
            palm[r] = 0.0f;
            for (c = 0; c < 3; ++c)
                palm[r] += ((&joints[SagaVrHands.tip[hand]].m30)[c] - (&joints[wrist].m30)[c]) * row[c] / (scale * scale);
        }
    } else {
        palm[0] = palm[1] = palm[2] = 0.0f;
    }
    scale *= SagaVrHandSize;
    memset(new_joint, 0, sizeof(new_joint));
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c)
            new_joint[r * 4 + c] = (SagaVrHands.rest[hand][r * 3 + 0] * grip[0 * 3 + c] +
                                    SagaVrHands.rest[hand][r * 3 + 1] * grip[1 * 3 + c] +
                                    SagaVrHands.rest[hand][r * 3 + 2] * grip[2 * 3 + c]) *
                                   scale;
    /* Free, the wrist sits at the grip; holding, the palm closes around the held point. */
    for (c = 0; c < 3; ++c)
        new_joint[12 + c] = hand_model[12 + c] - palm[0] * new_joint[0 + c] - palm[1] * new_joint[4 + c] -
                            palm[2] * new_joint[8 + c];
    new_joint[15] = 1.0f;
    SagaVrMoveSubtree(hierarchy, joints, wrist, new_joint, keep);
    return 1;
}

static u32 SagaVrHandsFrame = 0xffffffffu;

i32 __cdecl SagaVrHandsDriving(void) {
    return SagaVrHandsFrame + 1 >= SagaVrGetControl()->frame && SagaVrHandsFrame != 0xffffffffu;
}

/* The parts of a hierarchy's mesh, split by material, as NuDisplayListRndrSpecial walks them. */
typedef struct SAGA_VR_RENDER_PART {
    char *name;
    NUSPECIALHANDLE **rigid;
    NUSPECIALHANDLE *smooth;
    NUSPECIALHANDLE **alternate_rigid;
    NUSPECIALHANDLE *alternate_smooth;
} SAGA_VR_RENDER_PART;

/* Every joint but the kept ones collapses. Arm joints (between a posed wrist and the shoulder fork) collapse
 * onto that wrist. */
static void SagaVrCollapseBody(NUHGOBJ_EVAL_PC *hierarchy, NUMTX *joints, i32 count, const u8 *keep, const u8 *posed) {
    i32 arm[256];
    i32 k, h, joint;
    for (k = 0; k < count; ++k)
        arm[k] = -1;
    for (h = 0; h < 2; ++h) {
        i32 wrist = SagaVrHands.wrist[h], other = SagaVrHands.wrist[1 - h];
        if (!posed[h] || wrist < 0)
            continue;
        for (joint = hierarchy->joints[wrist].parent_index; joint != 0xff && joint < count;
             joint = hierarchy->joints[joint].parent_index) {
            i32 walk = other, shared = 0;
            while (walk >= 0 && walk < count) {
                if (walk == joint) {
                    shared = 1;
                    break;
                }
                walk = hierarchy->joints[walk].parent_index == 0xff ? -1 : hierarchy->joints[walk].parent_index;
            }
            if (shared)
                break;
            arm[joint] = wrist;
        }
    }
    for (k = 0; k < count; ++k) {
        if (keep[k])
            continue;
        memset(&joints[k].m00, 0, sizeof(f32) * 12);
        if (arm[k] >= 0)
            memcpy(&joints[k].m30, &joints[arm[k]].m30, sizeof(f32) * 3);
    }
}

#define SAGA_VR_POD_JOINTS 26 /* the skeleton of Anakin's pods, in the race (ids 257, 258) and the sprint (259) */

static f32 SagaVrPodCockpitOffset[3];
static u32 SagaVrPodCockpitFrame = 0xffffffffu;
static NUMTX SagaVrPodWorld;

i32 __cdecl SagaVrPodCockpit(f32 *cockpit) {
    if (SagaVrPodCockpitFrame == 0xffffffffu || SagaVrPodCockpitFrame + 2 < SagaVrGetControl()->frame)
        return 0;
    memcpy(cockpit, SagaVrPodCockpitOffset, sizeof(SagaVrPodCockpitOffset));
    return 1;
}

/* Joint 1 of a pod is its cockpit. Kept relative to the pod: the camera is set before the next draw, and a
 * world position would trail a fast pod by a frame. */
static void SagaVrPodSaveCockpit(GAMEOBJECT *object, NUMTX *world, NUMTX *joints, i32 joint_count) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    f32 placed[16], dx, dz, angle, c, s, scale, cockpit_y;
    const f32 *rig = SagaVrGetRig();
    if (world == NULL || joint_count < 2 || !SagaVrOnPod(object))
        return;
    angle = (f32)object->facing_angle * (6.2831855f / 65536.0f);
    c = (f32)cos(angle);
    s = (f32)sin(angle);
    scale = SagaVrLength(&world->m00);
    cockpit_y = joints[1].m31 * scale;
    /* The model pitches and rolls with the ground about its origin, far ahead of the cockpit, so the cockpit
     * swung up and down around the level view and the view could end up below it. It stays level instead, at
     * the view's smoothed height: the cockpit is fixed to the head and the bumps only move the world. Model
     * space is turned half a turn from the pod: -x its right, -z its forward. */
    if (SagaVrFirstPersonActive() && joint_count == SAGA_VR_POD_JOINTS && !(control->vehicle_hide & 0x80000000u)) {
        world->m00 = -scale * c, world->m01 = 0.0f, world->m02 = scale * s;
        world->m10 = 0.0f, world->m11 = scale, world->m12 = 0.0f;
        world->m20 = -scale * s, world->m21 = 0.0f, world->m22 = -scale * c;
        world->m31 = SagaVrPodCabinHeight() - cockpit_y;
    }
    SagaVrPodWorld = *world;
    SagaVrMul(placed, &joints[1].m00, &world->m00);
    dx = placed[12] - object->position.x;
    dz = placed[14] - object->position.z;
    SagaVrPodCockpitOffset[0] = dx * c - dz * s;
    SagaVrPodCockpitOffset[1] = control->vehicle_hide & 0x80000000u ? placed[13] - object->position.y : cockpit_y;
    SagaVrPodCockpitOffset[2] = dx * s + dz * c;
    SagaVrPodCockpitFrame = control->frame;
    /* Telemetry: how far the cockpit moves relative to the eyes over a few seconds. */
    if (rig != NULL && SagaVrFirstPersonActive()) {
        static f32 lowest = 1e9f, highest = -1e9f;
        f32 above = rig[13] - placed[13];
        lowest = above < lowest ? above : lowest;
        highest = above > highest ? above : highest;
        if (control->frame % 150 == 0) {
            LOG_INFO("VR pod cabin: eyes %.3f to %.3f above the cockpit", lowest, highest);
            lowest = 1e9f, highest = -1e9f;
        }
    }
}

/* Anakin's pod has its levers on joints 2 (left) and 3 (right). A grip near one holds it; the hand's move
 * from there, across for the left and along for the right, tilts the lever and is the input. Measured in
 * the hand's own tracking space, so the pod turning under it does not count as a move. */
#define SAGA_VR_LEVER_REACH 0.15f  /* world units from the hand to the lever top */
#define SAGA_VR_POD_PILOT_LAYER 7  /* 'TT7_Annakin' */
#define SAGA_VR_POD_HAND_SIZE 0.5f /* the cockpit's half scale */
#define SAGA_VR_POD_HANDS_BELOW 0.55f /* a minifig's height and a bit, world units */
#define SAGA_VR_LEVER_TOP 0.16f    /* model units from the joint to the lever's ball; the pod's model is at half scale */
#define SAGA_VR_LEVER_THROW 0.12f  /* metres of hand travel for a full tilt */
#define SAGA_VR_LEVER_WRIST_THROW 0.60f /* radians of wrist turn for a full tilt, about 35 degrees */
#define SAGA_VR_LEVER_WRIST_DEAD 0.05f  /* radians ignored, so a steady grip stays centred */
#define SAGA_VR_LEVER_TILT 22.0f   /* degrees at a full tilt */

static f32 SagaVrLever[2];
static f32 SagaVrLeverGrab[2];
static i32 SagaVrLeverHeld[2];
static i32 SagaVrLeverMissed;
static i32 SagaVrDrawingPodHands;
static void SagaVrPodHeldPoses(GAMEOBJECT *object, NUMTX *world, NUMTX *joints);
static f32 SagaVrLeverGrabWrist[2];

static f32 SagaVrWrapRadians(f32 angle) {
    while (angle > 3.1415927f)
        angle -= 6.2831855f;
    while (angle < -3.1415927f)
        angle += 6.2831855f;
    return angle;
}

/* How far a hand is turned to its right, in radians and in the hand's tracking space: angles[0] about the
 * vertical, angles[1] tilted about its own length. Both read from the hand's sideways axis, which stays
 * meaningful however the controller is pitched. */
static void SagaVrHandSideAngles(i32 hand, f32 *angles) {
    f32 pose[16];
    angles[0] = angles[1] = 0.0f;
    if (!SagaVrHandTracked(hand, pose))
        return;
    angles[0] = (f32)atan2(-pose[2], pose[0]);
    angles[1] = (f32)asin(pose[1] > 1.0f ? 1.0f : pose[1] < -1.0f ? -1.0f : pose[1]) * -1.0f;
}
static u32 SagaVrLeverFrame = 0xffffffffu;

i32 __cdecl SagaVrPodLeverInput(f32 *steer, f32 *throttle) {
    if (SagaVrLeverFrame == 0xffffffffu || SagaVrLeverFrame + 2 < SagaVrGetControl()->frame)
        return 0;
    *steer = SagaVrLever[0];
    *throttle = SagaVrLever[1];
    return (SagaVrLeverHeld[0] ? 1 : 0) | (SagaVrLeverHeld[1] ? 2 : 0);
}

/* The share of its speed the pod seeks: the right lever pushed forward. It stops otherwise. Scripted stretches
 * (fades, in-game cuts) keep the game's speed so they cannot stall. */
f32 __cdecl SagaVrPodSpeed(GAMEOBJECT *object, f32 target) {
    f32 steer, throttle = 0.0f, move[2], look[2];
    u32 buttons;
    if (object != (GAMEOBJECT *)Player[0] || !SagaVrFirstPersonActive() || FadeSys.fade != 0.0f || MiniCutCam != 0 ||
        !SagaVrControllerInput(move, look, &buttons))
        return target;
    if (!(SagaVrPodLeverInput(&steer, &throttle) & 2))
        throttle = 0.0f;
    if (throttle > 0.1f)
        return target * (throttle > 1.0f ? 1.0f : throttle);
    /* In the race exactly 0 means "not under control": the pod keeps its last speed and ignores the steering. */
    return WORLD->area == PODRACE_ADATA ? 0.01f : 0.0f;
}

static void SagaVrPodLevers(GAMEOBJECT *object, NUMTX *world, NUMTX *joints, i32 joint_count) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    f32 frame_time, blend, c, s, r[3];
    i32 h, i;
    {
        static i32 logged_id = -1;
        if (logged_id != (i32)object->id) {
            logged_id = (i32)object->id;
            LOG_INFO("VR vehicle: object id %d, %d joints, on pod %d", logged_id, joint_count, SagaVrOnPod(object));
        }
    }
    if (world == NULL || joint_count != SAGA_VR_POD_JOINTS || !SagaVrOnPod(object))
        return;
    if (SagaVrLeverFrame != control->frame) {
        SagaVrLeverFrame = control->frame;
        frame_time = nuapi.frame_time > 0.0f && nuapi.frame_time < 0.1f ? nuapi.frame_time : 1.0f / 60.0f;
        blend = (f32)(1.0 - exp(-frame_time / 0.05f));
        for (h = 0; h < 2; ++h) {
            u32 bit = h == 0 ? SAGA_XR_BUTTON_GRAB_LEFT : SAGA_XR_BUTTON_SABER;
            i32 grip = control->hand_valid[h] && (control->controller_buttons & bit) != 0;
            f32 along = h == 0 ? control->hand_position[h][0] : -control->hand_position[h][2], target = 0.0f;
            if (grip && !SagaVrLeverHeld[h]) {
                f32 hand[16], placed[16];
                if (SagaVrHandWorld(h, hand)) {
                    SagaVrMul(placed, &joints[2 + h].m00, &world->m00);
                    r[0] = hand[12] - placed[12];
                    r[1] = hand[13] - (placed[13] + SAGA_VR_LEVER_TOP * 0.5f);
                    r[2] = hand[14] - placed[14];
                    if (SagaVrLength(r) < SAGA_VR_LEVER_REACH) {
                        SagaVrLeverHeld[h] = 1;
                        SagaVrLeverGrab[h] = along;
                        SagaVrHandSideAngles(h, SagaVrLeverGrabWrist);
                        LOG_INFO("VR pod lever %d held", h);
                    } else if (!(SagaVrLeverMissed & (1 << h))) {
                        SagaVrLeverMissed |= 1 << h;
                        LOG_INFO("VR pod lever %d out of reach: %.2f units (hand %.2f %.2f %.2f m)", h, SagaVrLength(r),
                                 control->hand_position[h][0], control->hand_position[h][1],
                                 control->hand_position[h][2]);
                    }
                }
            } else if (!grip && SagaVrLeverHeld[h]) {
                SagaVrLeverHeld[h] = 0;
                LOG_INFO("VR pod lever %d released", h);
            }
            if (!grip)
                SagaVrLeverMissed &= ~(1 << h);
            if (SagaVrLeverHeld[h]) {
                target = (along - SagaVrLeverGrab[h]) / SAGA_VR_LEVER_THROW;
                /* The left lever also follows the wrist: turning or tilting the hand sideways. */
                if (h == 0) {
                    f32 angles[2], wrist;
                    SagaVrHandSideAngles(h, angles);
                    wrist = SagaVrWrapRadians(angles[0] - SagaVrLeverGrabWrist[0]) +
                            SagaVrWrapRadians(angles[1] - SagaVrLeverGrabWrist[1]);
                    if (fabs(wrist) > SAGA_VR_LEVER_WRIST_DEAD)
                        target += (wrist > 0.0f ? wrist - SAGA_VR_LEVER_WRIST_DEAD : wrist + SAGA_VR_LEVER_WRIST_DEAD) /
                                  SAGA_VR_LEVER_WRIST_THROW;
                }
                target = target > 1.0f ? 1.0f : target < -1.0f ? -1.0f : target;
            }
            SagaVrLever[h] += (target - SagaVrLever[h]) * blend;
        }
        if ((SagaVrLeverHeld[0] || SagaVrLeverHeld[1]) && control->frame % 30 == 0)
            LOG_INFO("VR pod levers: steer %.2f throttle %.2f, speed multiplier %.2f", SagaVrLever[0], SagaVrLever[1],
                     object->current_speed_mul);
    }
    /* Tilt about the joint itself. In model space +y is up, -x the pod's right and -z its forward. */
    c = (f32)cos(SagaVrLever[0] * SAGA_VR_LEVER_TILT * 0.017453293f);
    s = (f32)sin(SagaVrLever[0] * SAGA_VR_LEVER_TILT * 0.017453293f);
    for (i = 0; i < 3; ++i) {
        f32 *row = &joints[2].m00 + i * 4, x = row[0], y = row[1];
        row[0] = x * c - y * s;
        row[1] = x * s + y * c;
    }
    c = (f32)cos(-SagaVrLever[1] * SAGA_VR_LEVER_TILT * 0.017453293f);
    s = (f32)sin(-SagaVrLever[1] * SAGA_VR_LEVER_TILT * 0.017453293f);
    for (i = 0; i < 3; ++i) {
        f32 *row = &joints[3].m00 + i * 4, y = row[1], z = row[2];
        row[1] = y * c - z * s;
        row[2] = y * s + z * c;
    }
    SagaVrPodHeldPoses(object, world, joints);
}

/* Telemetry, once per vehicle model: its layers and where each joint sits in the vehicle's own axes (right,
 * up, forward), to find a cockpit, a pilot layer and controls to drive. */
static void SagaVrLogVehicle(GAMEOBJECT *object, CHARACTER_MODEL_LAYERS_PC *model, NUMTX *world, NUMTX *joints) {
    static NUHGOBJ_EVAL_PC *logged[32];
    static i32 logged_count;
    NUHGOBJ_EVAL_PC *hierarchy = model->hierarchy;
    SAGA_VR_RENDER_PART *parts = (SAGA_VR_RENDER_PART *)hierarchy->render_parts;
    f32 angle = (f32)object->facing_angle * (6.2831855f / 65536.0f), c = (f32)cos(angle), s = (f32)sin(angle);
    char line[1400];
    i32 i, p, j, used;
    if (world == NULL)
        return;
    for (i = 0; i < logged_count && logged[i] != hierarchy; ++i)
        ;
    if (i < logged_count || logged_count >= 32)
        return;
    logged[logged_count++] = hierarchy;
    LOG_INFO("VR vehicle model: object id %d, %d joints, %d layers, height %.2f, world scale %.2f", (i32)object->id,
             hierarchy->joint_count, hierarchy->render_count, object->scaled_height, SagaVrLength(&world->m00));
    for (p = 0; p < hierarchy->render_count; ++p) {
        used = 0;
        for (j = 0; parts[p].rigid != NULL && j < hierarchy->joint_count && used < 300; ++j)
            if (parts[p].rigid[j] != NULL)
                used += _snprintf(line + used, sizeof(line) - used, " %d", j);
        line[used] = '\0';
        LOG_INFO("VR vehicle layer %d '%s': smooth %d, rigid on joints%s", p, parts[p].name ? parts[p].name : "",
                 parts[p].smooth != NULL, used ? line : " none");
    }
    used = 0;
    for (j = 0; j < hierarchy->joint_count && j < 64 && used < (i32)sizeof(line) - 48; ++j) {
        f32 placed[16], dx, dz;
        SagaVrMul(placed, &joints[j].m00, &world->m00);
        dx = placed[12] - object->position.x;
        dz = placed[14] - object->position.z;
        used += _snprintf(line + used, sizeof(line) - used, " %d<%d:%.2f,%.2f,%.2f", j,
                          hierarchy->joints[j].parent_index == 0xff ? -1 : hierarchy->joints[j].parent_index,
                          dx * c - dz * s, placed[13] - object->position.y, dx * s + dz * c);
    }
    LOG_INFO("VR vehicle joints (right, up, forward):%s", line);
}

/* The pilot is a layer of the pod's model: it stays out of the way while the player sits in its place. */
static void SagaVrPodPilot(GAMEOBJECT *object, CHARACTER_MODEL_LAYERS_PC *model) {
    static SAGA_VR_RENDER_PART *part;
    static NUSPECIALHANDLE **rigid, **alternate;
    i32 hide = object != NULL && object == (GAMEOBJECT *)Player[0] && SagaVrFirstPersonActive() && SagaVrOnPod(object);
    SAGA_VR_RENDER_PART *candidate;
    if (model == NULL || model->hierarchy == NULL || model->hierarchy->joint_count != SAGA_VR_POD_JOINTS ||
        model->hierarchy->render_count <= SAGA_VR_POD_PILOT_LAYER)
        return;
    candidate = (SAGA_VR_RENDER_PART *)model->hierarchy->render_parts + SAGA_VR_POD_PILOT_LAYER;
    if (candidate != part) {
        if (candidate->name == NULL || strstr(candidate->name, "Annakin") == NULL)
            return;
        /* Another pod's model: the one left behind gets its pilot back. */
        if (part != NULL) {
            part->rigid = rigid;
            part->alternate_rigid = alternate;
        }
        part = candidate;
        rigid = part->rigid;
        alternate = part->alternate_rigid;
    }
    part->rigid = hide ? NULL : rigid;
    part->alternate_rigid = hide ? NULL : alternate;
}

/* A gripped lever holds the hand at its ball, facing along the pod with its up along the lever. */
static void SagaVrPodHeldPoses(GAMEOBJECT *object, NUMTX *world, NUMTX *joints) {
    static f32 held[2][16];
    f32 angle = (f32)object->facing_angle * (6.2831855f / 65536.0f);
    f32 forward[3], base[16], top[16], placed[16], up[3], right[3];
    i32 h, c;
    forward[0] = (f32)sin(angle), forward[1] = 0.0f, forward[2] = (f32)cos(angle);
    for (h = 0; h < 2; ++h) {
        f32 tilt = SagaVrLever[h] * SAGA_VR_LEVER_TILT * 0.017453293f;
        if (!SagaVrLeverHeld[h]) {
            SagaVrHeldHand[h] = NULL;
            continue;
        }
        memcpy(top, &joints[2 + h].m00, sizeof(top));
        /* The lever's axis in model space (+y at rest), tilted as in SagaVrPodLevers. */
        if (h == 0) {
            top[12] -= (f32)sin(tilt) * SAGA_VR_LEVER_TOP;
            top[13] += (f32)cos(tilt) * SAGA_VR_LEVER_TOP;
        } else {
            top[13] += (f32)cos(tilt) * SAGA_VR_LEVER_TOP;
            top[14] -= (f32)sin(tilt) * SAGA_VR_LEVER_TOP;
        }
        SagaVrMul(placed, top, &world->m00);
        SagaVrMul(base, &joints[2 + h].m00, &world->m00);
        for (c = 0; c < 3; ++c)
            up[c] = placed[12 + c] - base[12 + c];
        SagaVrNormalize(up);
        /* A minifig's hand is a C around its grip axis: that axis (the controller's forward) runs along the
         * lever. Frames here have right x up = forward. */
        right[0] = -(forward[0]), right[1] = -(forward[1]), right[2] = -(forward[2]);
        memset(held[h], 0, sizeof(held[h]));
        for (c = 0; c < 3; ++c) {
            held[h][0 + c] = right[c];
            held[h][8 + c] = up[c];
            held[h][12 + c] = placed[12 + c];
        }
        held[h][4] = up[1] * right[2] - up[2] * right[1];
        held[h][5] = up[2] * right[0] - up[0] * right[2];
        held[h][6] = up[0] * right[1] - up[1] * right[0];
        held[h][15] = 1.0f;
        SagaVrHeldHand[h] = held[h];
    }
}

static i32 SagaVrNameHas(const char *name, const char *word);

/* A lower level of detail by its layer name (LoRes, LowRes, Lo_Res, VeryLo, MedRes, Medium...), but not a
 * name that merely contains "lo", such as Gloves. */
static i32 SagaVrLowerDetail(const char *name) {
    return SagaVrNameHas(name, "lores") || SagaVrNameHas(name, "lowres") || SagaVrNameHas(name, "lo_res") ||
           SagaVrNameHas(name, "verylo") || SagaVrNameHas(name, "medres") || SagaVrNameHas(name, "medium") ||
           SagaVrNameHas(name, "meduim") || SagaVrNameHas(name, "med_");
}

/* The hands in a pod's or a ship's cockpit: a minifig of the level (the vehicle's own model has none for the
 * player) with its body collapsed, as for anyone walking in first person. */
static CHARACTER_MODEL_LAYERS_PC *SagaVrPodHandsModel(void) {
    i32 m, pass;
    /* Prefer a minifig with a 'HighRes' detail layer: its hands are known to draw on their own. */
    for (pass = 0; pass < 2; ++pass)
        for (m = 0; m < apicharsys->model_capacity; ++m) {
            CHARACTER_MODEL_LAYERS_PC *model = &apicharsys->models[m];
            SAGA_VR_RENDER_PART *parts;
            i32 p, high = 0;
            if (model->hierarchy == NULL || model->hierarchy->joint_count < 20 || model->hierarchy->render_count < 2)
                continue;
            parts = (SAGA_VR_RENDER_PART *)model->hierarchy->render_parts;
            if (!SagaVrNameHas(parts[0].name, "alwaysdrawn"))
                continue;
            for (p = 0; p < model->hierarchy->render_count; ++p)
                high |= SagaVrNameHas(parts[p].name, "highres");
            if (high || pass == 1)
                return model;
        }
    return NULL;
}

static void SagaVrPosePodHands(CHARACTER_MODEL_LAYERS_PC *model, NUMTX *world, NUMTX *joints) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    NUHGOBJ_EVAL_PC *hierarchy = model->hierarchy;
    u8 keep[256], posed[2] = {0, 0};
    i32 h, count = hierarchy->joint_count < 256 ? hierarchy->joint_count : 256;
    i32 in_ship;
    SagaVrFindHands(hierarchy, -1);
    memset(keep, 0, sizeof(keep));
    /* In a ship the right hand may hold its flight stick. */
    in_ship = SagaVrInShip((GAMEOBJECT *)Player[0]);
    SagaVrHandSize = SAGA_VR_POD_HAND_SIZE;
    if (in_ship)
        SagaVrHeldHand[0] = NULL, SagaVrHeldHand[1] = SagaVrShipHeldHand();
    for (h = 0; h < 2; ++h)
        if (control->hand_valid[h])
            posed[h] = (u8)SagaVrPoseRestHand(hierarchy, world, joints, h, keep);
    SagaVrHandSize = 1.0f;
    /* Held poses are for this draw only: walking hands follow the controllers again. */
    SagaVrHeldHand[0] = SagaVrHeldHand[1] = NULL;
    SagaVrCollapseBody(hierarchy, joints, count, keep, posed);
}

/* Whether a name contains a lowercase word, ignoring case (layer names vary: HighRes, Highres, HiRes). */
static i32 SagaVrNameHas(const char *name, const char *word) {
    i32 i, j;
    if (name == NULL)
        return 0;
    for (i = 0; name[i] != '\0'; ++i) {
        for (j = 0; word[j] != '\0'; ++j) {
            char c = name[i + j];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != word[j])
                break;
        }
        if (word[j] == '\0')
            return 1;
    }
    return 0;
}

/* A minifig's layers are levels of detail plus its hair and breakup pieces: only the most detailed one, whose
 * body collapses cleanly. The others would double the hands or float around as shards. */
static i32 __cdecl SagaVrPodHandLayers(CHARACTER_MODEL_LAYERS_PC *model, i16 *output, u32 mask) {
    SAGA_VR_RENDER_PART *parts = (SAGA_VR_RENDER_PART *)model->hierarchy->render_parts;
    i32 p, chosen = -1, count;
    static NUHGOBJ_EVAL_PC *logged;
    (void)mask;
    for (p = 0; p < model->hierarchy->render_count && chosen < 0; ++p)
        if (SagaVrNameHas(parts[p].name, "highres") || SagaVrNameHas(parts[p].name, "hires"))
            chosen = p;
    /* Otherwise the first named detail layer that is not a lower level, hair, a head or loose pieces. */
    for (p = 0; p < model->hierarchy->render_count && chosen < 0; ++p)
        if (parts[p].name != NULL && parts[p].name[0] != '\0' && (parts[p].rigid != NULL || parts[p].smooth != NULL) &&
            !SagaVrNameHas(parts[p].name, "alwaysdrawn") &&
            !SagaVrLowerDetail(parts[p].name) && !SagaVrNameHas(parts[p].name, "hair") &&
            !SagaVrNameHas(parts[p].name, "head") && !SagaVrNameHas(parts[p].name, "breakup"))
            chosen = p;
    if (logged != model->hierarchy) {
        logged = model->hierarchy;
        LOG_INFO("VR pod hands: layer %d '%s' of %d", chosen, chosen >= 0 && parts[chosen].name ? parts[chosen].name : "",
                 model->hierarchy->render_count);
        for (p = 0; p < model->hierarchy->render_count; ++p)
            LOG_INFO("VR pod hands: layer %d '%s' rigid %d smooth %d", p, parts[p].name ? parts[p].name : "",
                     parts[p].rigid != NULL, parts[p].smooth != NULL);
    }
    /* Test aid (vr_control hide 200000xx): only the layers in the low byte. */
    if (SagaVrGetControl()->vehicle_hide & 0x20000000u) {
        count = 0;
        for (p = 0; p < model->hierarchy->render_count && p < 8; ++p)
            if (SagaVrGetControl()->vehicle_hide & (1u << p))
                output[count++] = (i16)p;
        return count;
    }
    /* Test aid (vr_control hide 40000000): every layer, to see which one holds a model's hands. */
    if (SagaVrGetControl()->vehicle_hide & 0x40000000u) {
        for (p = 0; p < model->hierarchy->render_count && p < 32; ++p)
            output[p] = (i16)p;
        return p;
    }
    if (chosen < 0)
        return 0;
    output[0] = (i16)chosen;
    count = 1;
    /* Some minifigs keep their hands in another layer: every other layer that is not a lower level of detail,
     * hair, a head or loose pieces (those hang off collapsed joints). The X-wing pilot's gloves are in an
     * "AlwaysDrawn" layer whose rigid and smooth handles both read as empty, so emptiness is not tested here. */
    for (p = 0; p < model->hierarchy->render_count && count < 32; ++p) {
        if (p == chosen || SagaVrLowerDetail(parts[p].name) || SagaVrNameHas(parts[p].name, "hair") ||
            SagaVrNameHas(parts[p].name, "head") || SagaVrNameHas(parts[p].name, "breakup"))
            continue;
        output[count++] = (i16)p;
    }
    return count;
}

static void SagaVrDrawPodHands(GAMEOBJECT *object) {
    static NUMTX joints[64];
    NUMTX world;
    const f32 *rig = SagaVrGetRig();
    CHARACTER_MODEL_LAYERS_PC *model;
    i32(__cdecl * dispatch)(CHARACTER_MODEL_LAYERS_PC *, i16 *, u32);
    if (rig == NULL)
        return;
    if (object == NULL || object != (GAMEOBJECT *)Player[0] || SagaVrDrawingPodHands || !SagaVrFirstPersonActive() ||
        object->character_model == NULL || object->character_model->hierarchy == NULL ||
        !(SagaVrInShip(object) || (SagaVrOnPod(object) && object->character_model->hierarchy->joint_count == SAGA_VR_POD_JOINTS)))
        return;
    model = SagaVrPodHandsModel();
    if (model == NULL || model->hierarchy->joint_count > 64)
        return;
    {
        static CHARACTER_MODEL_LAYERS_PC *logged;
        if (logged != model) {
            logged = model;
            LOG_INFO("VR pod hands: model id %d, %d joints", (i32)model->model_id, model->hierarchy->joint_count);
        }
    }
    /* The minifig's own space is unit scale, unlike the pod's half-scale matrix. Centred under the eyes: the
     * model's bounds stay in view wherever the head turns, and what is left of its collapsed body, which
     * would show as shards at eye level, stays below the cockpit. */
    memset(&world, 0, sizeof(world));
    world.m00 = world.m11 = world.m22 = world.m33 = 1.0f;
    world.m30 = rig[12], world.m31 = rig[13] - SAGA_VR_POD_HANDS_BELOW, world.m32 = rig[14];
    dispatch = ModelAnimLayerListDispatchPc;
    ModelAnimLayerListDispatchPc = SagaVrPodHandLayers;
    SagaVrDrawingPodHands = 1;
    APIDrawCharacterModel(model, NULL, NULL, &world, NULL, NULL, NULL, NULL, NULL, 0xffffffffu, NULL, 0, Paused,
                          FRAMETIME, joints, NULL, NULL);
    SagaVrDrawingPodHands = 0;
    ModelAnimLayerListDispatchPc = dispatch;
}

/* Player 0 in first person: the saber (lit or not) or a bare hand on the right controller, a hand on
 * the left one, and the rest of the body hidden by collapsing every other joint's axes to zero. Arm
 * joints collapse onto their posed wrist, so skin weighted partly to the forearm stays at the hand
 * instead of stretching back to where the arm was. */
void __cdecl SagaVrCharacterJoints(GAMEOBJECT *object, CHARACTER_MODEL_LAYERS_PC *model, NUMTX *world, NUMTX *joints) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    NUHGOBJ_EVAL_PC *hierarchy;
    u8 keep[256], posed[2] = {0, 0};
    i32 k, count, saber_joint = -1;
    if (SagaVrDrawingPodHands) {
        SagaVrPosePodHands(model, world, joints);
        return;
    }
    SagaVrPodPilot(object, model);
    if (object != NULL && object != (GAMEOBJECT *)Player[0] && model != NULL && model->hierarchy != NULL) {
        /* Diagnostics: describe each other character's hands once, so new skeletons can be checked
         * before the player switches to them. */
        static NUHGOBJ_EVAL_PC *seen[96];
        static i32 seen_count = 0;
        for (k = 0; k < seen_count && seen[k] != model->hierarchy; ++k)
            ;
        if (k == seen_count && seen_count < 96) {
            SAGA_VR_HANDS saved = SagaVrHands;
            seen[seen_count++] = model->hierarchy;
            LOG_INFO("VR hands probe: object id %d", (i32)object->id);
            SagaVrHands.hierarchy = NULL;
            SagaVrFindHands(model->hierarchy, -1);
            SagaVrHands = saved;
        }
    }
    if (object != NULL && model != NULL && joints != NULL && model->hierarchy != NULL && SagaVrFirstPersonActive() &&
        SagaVrRider((GAMEOBJECT *)Player[0]) != NULL) {
        /* Driving: the vehicle stays as it is; the rider, whose eyes the camera uses, is hidden. */
        if (object == SagaVrRider((GAMEOBJECT *)Player[0])) {
            count = model->hierarchy->joint_count < 256 ? model->hierarchy->joint_count : 256;
            for (k = 0; k < count; ++k)
                memset(&joints[k].m00, 0, sizeof(f32) * 12);
        }
        return;
    }
    if (object == NULL || object != (GAMEOBJECT *)Player[0] || model == NULL || joints == NULL ||
        model->hierarchy == NULL || !SagaVrFirstPersonActive())
        return;
    /* A pod or ship without a separate rider is both the player and the vehicle: it stays visible. */
    if (VehicleArea != 0) {
        SagaVrLogVehicle(object, model, world, joints);
        if (SagaVrInShip(object)) {
            /* The ship's hull would cut through the cockpit drawn around the eyes. */
            for (k = 0; k < model->hierarchy->joint_count && k < 256; ++k)
                memset(&joints[k].m00, 0, sizeof(f32) * 12);
            return;
        }
        SagaVrPodSaveCockpit(object, world, joints, model->hierarchy->joint_count);
        SagaVrPodLevers(object, world, joints, model->hierarchy->joint_count);
        for (k = 0; k < 32 && k < model->hierarchy->joint_count; ++k)
            if ((control->vehicle_hide >> k) & 1)
                memset(&joints[k].m00, 0, sizeof(f32) * 12);
        return;
    }
    hierarchy = model->hierarchy;
    count = hierarchy->joint_count < 256 ? hierarchy->joint_count : 256;
    if (object == SagaVrSaberPlayer()) {
        PLAYERCHARACTERCONFIG_PC *data = object->character_data->game_character;
        if (model->points_of_interest[data->streak_joints[0][0]] != NULL)
            saber_joint = hierarchy->points[hierarchy->point_index_map[data->streak_joints[0][0]]].joint_index;
    }
    SagaVrFindHands(hierarchy, saber_joint);
    {
        static NUHGOBJ_EVAL_PC *logged = NULL;
        if (logged != hierarchy && object->character_data != NULL && object->character_data->game_character != NULL) {
            PLAYERCHARACTERCONFIG_PC *data = object->character_data->game_character;
            i32 weapon[8];
            logged = hierarchy;
            for (k = 0; k < 8; ++k) {
                i32 locator = k < 4 ? data->weapon_joints[k] : data->weapon_shoot_joints[k - 4];
                weapon[k] = locator >= 0 && model->points_of_interest[locator] != NULL
                                ? hierarchy->points[hierarchy->point_index_map[locator]].joint_index
                                : -1;
            }
            LOG_INFO("VR weapons: joints %d %d %d %d, muzzles %d %d %d %d (wrists %d/%d)", weapon[0], weapon[1],
                     weapon[2], weapon[3], weapon[4], weapon[5], weapon[6], weapon[7], SagaVrHands.wrist[0],
                     SagaVrHands.wrist[1]);
        }
    }
    memset(keep, 0, sizeof(keep));
    /* Right hand: the saber's blade alignment when there is one, otherwise the rest-pose hand. */
    if (saber_joint >= 0 && SagaVrPoseSaber(object, model, world, joints, keep))
        posed[1] = 1;
    else if (control->hand_valid[1] && SagaVrPoseBlaster(object, model, world, joints, keep))
        posed[1] = 1;
    else if (control->hand_valid[1])
        posed[1] = (u8)SagaVrPoseRestHand(hierarchy, world, joints, 1, keep);
    /* Dual wielders (Grievous, Maul's second blade...): the second blade follows the left controller the
     * same way; its other blades stay with the body. */
    if (control->hand_valid[0] && saber_joint >= 0 && posed[1]) {
        PLAYERCHARACTERCONFIG_PC *data = object->character_data->game_character;
        f32 left_world[16];
        if (data->streak_joints[1][0] >= 0 && data->streak_joints[1][1] >= 0 &&
            model->points_of_interest[data->streak_joints[1][0]] != NULL &&
            model->points_of_interest[data->streak_joints[1][1]] != NULL && SagaVrHandWorld(0, left_world)) {
            NUHGOBJ_POI_PC *a = &hierarchy->points[hierarchy->point_index_map[data->streak_joints[1][0]]];
            NUHGOBJ_POI_PC *b = &hierarchy->points[hierarchy->point_index_map[data->streak_joints[1][1]]];
            if (a->joint_index == b->joint_index && a->joint_index != saber_joint && !keep[a->joint_index])
                posed[0] = (u8)SagaVrPoseAlongGrip(hierarchy, world, joints, keep, a, b, left_world,
                                                   control->saber_scale, 0, 0.0f);
        }
    }
    if (control->hand_valid[0] && !posed[0])
        posed[0] = (u8)SagaVrPoseRestHand(hierarchy, world, joints, 0, keep);
    if (posed[0] || posed[1])
        SagaVrHandsFrame = control->frame;
    /* A weapon in the posed right hand stays out: the game holsters blasters between shots
     * (weapon_scale 0), which in first person would leave an empty hand. */
    if (posed[1] && object->character_data != NULL && object->character_data->game_character != NULL &&
        object->character_data->game_character->weapon_joints[0] != -1)
        object->weapon_scale = 1.0f;
    SagaVrCollapseBody(hierarchy, joints, count, keep, posed);
}

i32 __cdecl SagaVrDrawWeaponPart(GAMEOBJECT *object, i32 is_hilt) {
    /* A switched-off VR saber stays in the hand as a bare hilt. */
    if (is_hilt || object == NULL || object != (GAMEOBJECT *)Player[0] || !SagaVrSaberDriving())
        return 1;
    return SagaVrBladeScale > 0.01f;
}

void __cdecl SagaVrBladeMatrix(GAMEOBJECT *object, NUMTX *blade_matrix) {
    PLAYERCHARACTERCONFIG_PC *data;
    f32 direction[3], best = -1.0f, dot, length, *row;
    i32 axis, i, c;
    if (object == NULL || object != (GAMEOBJECT *)Player[0] || !SagaVrSaberDriving() || SagaVrBladeScale >= 1.0f)
        return;
    /* The blade's axis in this matrix's own frame: the local row best aligned with base->tip. */
    data = object->character_data->game_character;
    for (c = 0; c < 3; ++c)
        direction[c] = (&object->locator_matrices[data->streak_joints[0][1]].m30)[c] -
                       (&object->locator_matrices[data->streak_joints[0][0]].m30)[c];
    axis = 2;
    for (i = 0; i < 3; ++i) {
        row = &blade_matrix->m00 + i * 4;
        length = SagaVrLength(row);
        if (length < 1e-6f)
            continue;
        dot = (f32)fabs((row[0] * direction[0] + row[1] * direction[1] + row[2] * direction[2]) / length);
        if (dot > best) {
            best = dot;
            axis = i;
        }
    }
    /* Stretch only along that axis, from the hilt end where the model's origin sits. */
    row = &blade_matrix->m00 + axis * 4;
    for (c = 0; c < 3; ++c)
        row[c] *= SagaVrBladeScale;
}

/* NuHGobjRndrMtxDwa culls the whole model by its rest-pose bounds unless PCSpecialClipState holds a
 * result; in first person those bounds sit behind the camera while the saber is in view. 2 means
 * "intersects the frustum", so the model is drawn and clipped normally. */
static i32 SagaVrSavedClipState = -1;
static i32 SagaVrClipOverridden = 0;

void __cdecl SagaVrDrawBegin(GAMEOBJECT *object) {
    if (object == NULL || object != (GAMEOBJECT *)Player[0] || !SagaVrFirstPersonActive())
        return;
    SagaVrSavedClipState = PCSpecialClipState;
    PCSpecialClipState = 2;
    SagaVrClipOverridden = 1;
}

void __cdecl SagaVrDrawEnd(GAMEOBJECT *object) {
    if (SagaVrClipOverridden) {
        PCSpecialClipState = SagaVrSavedClipState;
        SagaVrClipOverridden = 0;
    }
    SagaVrDrawShipCockpit(object);
    SagaVrDrawPodHands(object);
}

static void SagaVrBlockSabre(GAMEOBJECT *object);

void __cdecl SagaVrSabreFlags(GAMEOBJECT *object) {
    if (object != NULL && object != SagaVrSaberPlayer()) {
        SagaVrBlockSabre(object);
        return;
    }
    if (object == NULL || !SagaVrSaberDriving())
        return;
    /* Switched off (or still igniting): a bare hilt neither hits nor deflects. */
    if (!SagaVrGetControl()->saber_lit || SagaVrBladeScale < 0.9f) {
        object->sabre_flags = 0;
        return;
    }
    /* 2 draws the coloured streak behind the blade (the "aura"); the hand-held blade goes without it. */
    object->sabre_flags &= (u8)~2;
    /* While the hand swings, arm the hit exactly as ComboHitFrame does: bits 1|4 and a damage value.
     * Without sabre_damage the hit reaches NPCs with zero damage (breakables do not need it). Outside a
     * swing the game's own bits stay, so trigger combos still hit from the real blade. */
    if (SagaVrSwinging()) {
        object->sabre_flags |= 5;
        object->sabre_damage = 1;
        if ((object->flags_1fc & 0x80) != 0 && Player_HasDoubleWeaponDamage(object))
            object->sabre_damage = 2;
    }
}

/* The game arms a combo's hit for a single frame, but a swing stays armed while the hand moves and hurt the
 * blade's target on every frame it touched, taking a boss's hearts in one pass. */
i32 __cdecl SagaVrSabreHit(GAMEOBJECT *object, GAMEOBJECT *target) {
    static struct {
        GAMEOBJECT *target;
        f32 time;
    } recent[8];
    static i32 oldest;
    f32 cooldown = SagaVrGetControl()->saber_cooldown, now = GameTimer.time_elapsed, since;
    i32 i;
    if (object != SagaVrSaberPlayer() || !SagaVrSaberDriving())
        return 1;
    for (i = 0; i < 8 && recent[i].target != target; ++i)
        ;
    if (i < 8) {
        since = now - recent[i].time;
        /* A negative time means the level's clock restarted. */
        if (since >= 0.0f && since < cooldown)
            return 0;
    } else {
        i = oldest;
        oldest = (oldest + 1) % 8;
        recent[i].target = target;
    }
    recent[i].time = now;
    LOG_INFO("VR saber hit: object id %d, hp %d", (i32)target->id, (i32)target->current_hp);
    return 1;
}

void __cdecl SagaVrSabrePoints(GAMEOBJECT *object, NUVEC *points, i32 joint_a, i32 joint_b, NUVEC *minimum,
                               NUVEC *maximum) {
    NUVEC difference;
    f32 length, extent;
    if (object == NULL || object != SagaVrSaberPlayer() || !SagaVrSaberDriving())
        return;
    /* Back to the real blade: locators were stored from the hand-posed joint. */
    memcpy(&points[0], &object->locator_matrices[joint_a].m30, sizeof(NUVEC));
    memcpy(&points[1], &object->locator_matrices[joint_b].m30, sizeof(NUVEC));
    length = NuVecDist(&points[0], &points[1], &difference);
    points[2].x = difference.x * 0.5f + points[1].x;
    points[2].y = difference.y * 0.5f + points[1].y;
    points[2].z = difference.z * 0.5f + points[1].z;
    object->sabre_collision_radius = length * 0.25f;
    extent = length * 1.5f;
    minimum->x = points[2].x - extent;
    minimum->y = points[2].y - extent;
    minimum->z = points[2].z - extent;
    maximum->x = points[2].x + extent;
    maximum->y = points[2].y + extent;
    maximum->z = points[2].z + extent;
}

/* The visible barrel of the blaster posed at the hand, in world space: the grip locator's axis closest to
 * grip->muzzle. Returns 0 when no blaster is posed or no axis is close to that line. */
static i32 SagaVrBarrelAxis(GAMEOBJECT *object, f32 *barrel) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    PLAYERCHARACTERCONFIG_PC *data = object->character_data->game_character;
    NUMTX *grip, *muzzle;
    f32 line[3], row[3], axis[3], best = 0.0f, dot;
    i32 r, c;
    if (control->frame - SagaVrBlasterFrame > 1 || data->weapon_joints[0] < 0 || data->weapon_shoot_joints[0] < 0)
        return 0;
    grip = &object->locator_matrices[data->weapon_joints[0]];
    muzzle = &object->locator_matrices[data->weapon_shoot_joints[0]];
    for (c = 0; c < 3; ++c)
        line[c] = (&muzzle->m30)[c] - (&grip->m30)[c];
    if (SagaVrLength(line) < 1e-5f)
        return 0;
    SagaVrNormalize(line);
    for (r = 0; r < 3; ++r) {
        memcpy(row, &grip->m00 + r * 4, sizeof(row));
        if (SagaVrLength(row) < 1e-5f)
            return 0;
        SagaVrNormalize(row);
        dot = row[0] * line[0] + row[1] * line[1] + row[2] * line[2];
        if (fabs(dot) > fabs(best)) {
            best = dot;
            for (c = 0; c < 3; ++c)
                axis[c] = dot < 0.0f ? -row[c] : row[c];
        }
    }
    if (fabs(best) <= 0.8f)
        return 0;
    memcpy(barrel, axis, sizeof(axis));
    return 1;
}

/* Blasters in first person: the shot leaves along the visible barrel, or the right controller's tilted
 * forward axis if there is none. If the weapon's muzzle locator did not follow the hand (it hangs off a
 * hidden joint), it starts at the hand.
 * Aim assist bends the shot onto the targetable enemy closest to that line, within aim_assist degrees. */
void __cdecl SagaVrBoltAim(GAMEOBJECT *object, NUVEC *shot_position, NUMTX *matrix) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    f32 hand[16], forward[3], delta[3], best_cos, distance, cosine, length;
    GAMEOBJECT *target;
    NUVEC aim;
    NUANGVEC angles;
    i32 i, c, from_barrel;
    if (object == NULL || object != (GAMEOBJECT *)Player[0] || shot_position == NULL || matrix == NULL ||
        !SagaVrFirstPersonActive() || !control->hand_valid[1] || !SagaVrHandWorld(1, hand))
        return;
    /* Same barrel tilt as the held weapon (SagaVrPoseBlaster): the handle axis points above the aim. */
    {
        f32 pitch = control->frame - SagaVrBlasterFrame <= 1 ? SagaVrBlasterPitchUsed : control->blaster_pitch;
        f32 cp = (f32)cos(pitch * 0.017453293f), sp = (f32)sin(pitch * 0.017453293f);
        for (c = 0; c < 3; ++c)
            forward[c] = hand[8 + c] * cp + hand[4 + c] * sp;
    }
    length = SagaVrLength(forward);
    if (length < 1e-6f)
        return;
    for (c = 0; c < 3; ++c)
        forward[c] /= length;
    /* Always along the controller, tilted like the posed weapon (SagaVrBlasterPitchUsed). */
    from_barrel = 0;
    (void)SagaVrBarrelAxis;
    delta[0] = shot_position->x - hand[12];
    delta[1] = shot_position->y - hand[13];
    delta[2] = shot_position->z - hand[14];
    if (SagaVrLength(delta) > 0.25f) {
        shot_position->x = hand[12] + forward[0] * 0.05f;
        shot_position->y = hand[13] + forward[1] * 0.05f;
        shot_position->z = hand[14] + forward[2] * 0.05f;
    }
    LOG_INFO("VR shot: aim (%.2f %.2f %.2f) from %s", forward[0], forward[1], forward[2],
             from_barrel ? "barrel" : "controller");
    aim.x = forward[0], aim.y = forward[1], aim.z = forward[2];
    if (control->aim_assist > 0.0f && Obj != NULL) {
        best_cos = (f32)cos(control->aim_assist * 0.017453293f);
        for (i = 0, target = Obj; i < HIGHGAMEOBJECT; ++i, ++target) {
            /* The game's own target filter (Bolt targeting): alive, targetable, on the other side. */
            if (target == object || target->dead != 0 || (target->flags_1fc_word & 0x1000) == 0 ||
                ((target->flags_1f8 ^ object->flags_1f8) & 0x10001) == 0)
                continue;
            delta[0] = target->collision_position.x - shot_position->x;
            delta[1] = target->collision_position.y + Bolt_ObjTargetPosYAdjust(target) - shot_position->y;
            delta[2] = target->collision_position.z - shot_position->z;
            distance = SagaVrLength(delta);
            if (distance < 1e-3f || distance > 25.0f)
                continue;
            cosine = (delta[0] * forward[0] + delta[1] * forward[1] + delta[2] * forward[2]) / distance;
            if (cosine > best_cos) {
                best_cos = cosine;
                aim.x = delta[0], aim.y = delta[1], aim.z = delta[2];
            }
        }
    }
    /* Same orientation the game builds from an aim vector. */
    FindAnglesXY(&aim, NULL, NULL);
    angles.x = temp_zrot;
    angles.y = ConfigBuffer.fields.find_angles_y_rotation;
    angles.z = 0;
    NuMtxSetRotationXYVU0(matrix, &angles);
}

/* Closest points between segments p0-p1 and q0-q1: returns the squared distance, the parameter along
 * p in *s and the point on p in out. */
static f32 SagaVrSegmentDistance(const f32 *p0, const f32 *p1, const f32 *q0, const f32 *q1, f32 *s_out, f32 *out) {
    f32 d1[3], d2[3], r[3], a, e, f, b, c, denominator, s, t, difference[3];
    i32 i;
    for (i = 0; i < 3; ++i)
        d1[i] = p1[i] - p0[i], d2[i] = q1[i] - q0[i], r[i] = p0[i] - q0[i];
    a = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2];
    e = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
    f = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2];
    c = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2];
    b = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2];
    denominator = a * e - b * b;
    s = a > 1e-9f && denominator > 1e-12f ? (b * f - c * e) / denominator : 0.0f;
    s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
    t = e > 1e-9f ? (b * s + f) / e : 0.0f;
    if (t < 0.0f) {
        t = 0.0f;
        s = a > 1e-9f ? -c / a : 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
        s = a > 1e-9f ? (b - c) / a : 0.0f;
    }
    s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
    for (i = 0; i < 3; ++i) {
        out[i] = p0[i] + d1[i] * s;
        difference[i] = out[i] - (q0[i] + d2[i] * t);
    }
    *s_out = s;
    return difference[0] * difference[0] + difference[1] * difference[1] + difference[2] * difference[2];
}

/* Blocking with the motion saber: after each update, every enemy bolt whose next step (or the step it
 * just made) crosses the lit blade is sent back, like the game's own block (Bolt_HitGameObjectRC): the
 * incoming bolt ends and a reflected copy owned by the player flies at the shooter (aimed with the
 * game's intercept when the shooter is alive, otherwise mirrored off the blade). Bolts the game marks
 * as not deflectable (0x40, e.g. rockets) still hit. */
#define SAGA_VR_BLOCK_RADIUS 0.12f /* blade half-thickness plus generous slack, game units (a minifig is ~0.42); 0.06 was too strict */
static void SagaVrBlockedMessage(void);

void __cdecl SagaVrSaberDeflect(void) {
    GAMEOBJECT *player = SagaVrSaberPlayer();
    PLAYERCHARACTERCONFIG_PC *data;
    f32 base[3], tip[3], start[3], end[3], hit[3], s, frame_time;
    i32 i, c;
    if (player == NULL || !SagaVrSaberDriving() || !SagaVrGetControl()->saber_lit || SagaVrBladeScale < 0.9f ||
        SagaVrCinema())
        return;
    data = player->character_data->game_character;
    memcpy(base, &player->locator_matrices[data->streak_joints[0][0]].m30, sizeof(base));
    memcpy(tip, &player->locator_matrices[data->streak_joints[0][1]].m30, sizeof(tip));
    frame_time = FRAMETIME > 0.0f && FRAMETIME < 0.1f ? FRAMETIME : 1.0f / 60.0f;
    for (i = 0; i < 32; ++i) {
        BOLT *bolt = &Bolt[i];
        GAMEOBJECT *shooter = (GAMEOBJECT *)bolt->owner;
        NUVEC direction, position;
        NUMTX matrix;
        NUANGVEC angles;
        BOLT *reflected;
        if (!bolt->active || shooter == player || (bolt->flags & 0x40) != 0 || (bolt->flags & 0x10000000) != 0)
            continue;
        /* The step just taken and the next one: a fast bolt can cross the blade between two samples. */
        for (c = 0; c < 3; ++c) {
            start[c] = (&bolt->position.x)[c] - (&bolt->velocity.x)[c] * frame_time;
            end[c] = (&bolt->position.x)[c] + (&bolt->velocity.x)[c] * frame_time * 1.5f;
        }
        if (SagaVrSegmentDistance(start, end, base, tip, &s, hit) >
            (SAGA_VR_BLOCK_RADIUS + bolt->collision_radius) * (SAGA_VR_BLOCK_RADIUS + bolt->collision_radius))
            continue;
        position.x = hit[0], position.y = hit[1], position.z = hit[2];
        if (shooter != NULL && (shooter->flags_1fc_word & 0x1001) == 0x1001 && shooter->dead == 0) {
            CalculateInterceptVector(&position, &shooter->collision_position, &shooter->velocity, bolt->speed,
                                     &direction, NULL);
        } else {
            /* Mirror off the blade: drop the component along the blade, reverse the rest. */
            f32 axis[3], along;
            for (c = 0; c < 3; ++c)
                axis[c] = tip[c] - base[c];
            SagaVrNormalize(axis);
            along = bolt->velocity.x * axis[0] + bolt->velocity.y * axis[1] + bolt->velocity.z * axis[2];
            direction.x = axis[0] * along * 2.0f - bolt->velocity.x;
            direction.y = axis[1] * along * 2.0f - bolt->velocity.y;
            direction.z = axis[2] * along * 2.0f - bolt->velocity.z;
        }
        FindAnglesXY(&direction, NULL, NULL);
        angles.x = temp_zrot;
        angles.y = ConfigBuffer.fields.find_angles_y_rotation;
        angles.z = 0;
        NuMtxSetRotationXYVU0(&matrix, &angles);
        PlaySfx("SabDefBlas", &position); /* the game's sabre-deflects-blaster sound */
        SagaVrBlockedMessage();
        Bolt_End(bolt, 0);
        reflected = Bolt_Add(player, &position, &matrix, bolt->type_id, 0);
        if (reflected != NULL)
            reflected->flags |= 0x10000000;
    }
}

/* Parrying: another character's blade that would hit this frame (sabre_flags 1|4, set by its combo just
 * before this hook) is stopped when it touches the lit VR blade, with the game's sabre clash sound. The
 * blades are compared where they were last drawn. Same-side characters are left alone. */
void SagaVrBlockSabre(GAMEOBJECT *object) {
    static f32 clash_time = 0.0f;
    GAMEOBJECT *player = SagaVrSaberPlayer();
    PLAYERCHARACTERCONFIG_PC *mine, *theirs;
    f32 base[3], tip[3], their_base[3], their_tip[3], hit[3], s;
    NUVEC position;
    if (player == NULL || (object->sabre_flags & 5) == 0 || !SagaVrSaberDriving() || !SagaVrGetControl()->saber_lit ||
        SagaVrBladeScale < 0.9f || ((object->flags_1f8 ^ player->flags_1f8) & 0x10001) == 0 ||
        object->character_data == NULL || object->character_data->game_character == NULL)
        return;
    theirs = object->character_data->game_character;
    if (theirs->streak_joints[0][0] < 0 || theirs->streak_joints[0][1] < 0)
        return;
    mine = player->character_data->game_character;
    memcpy(base, &player->locator_matrices[mine->streak_joints[0][0]].m30, sizeof(base));
    memcpy(tip, &player->locator_matrices[mine->streak_joints[0][1]].m30, sizeof(tip));
    memcpy(their_base, &object->locator_matrices[theirs->streak_joints[0][0]].m30, sizeof(their_base));
    memcpy(their_tip, &object->locator_matrices[theirs->streak_joints[0][1]].m30, sizeof(their_tip));
    if (SagaVrSegmentDistance(their_base, their_tip, base, tip, &s, hit) >
        SAGA_VR_BLOCK_RADIUS * SAGA_VR_BLOCK_RADIUS * 4.0f)
        return;
    object->sabre_flags &= (u8)~5;
    object->sabre_damage = 0;
    if (GlobalTimer.time_elapsed - clash_time > 0.25f || GlobalTimer.time_elapsed < clash_time) {
        clash_time = GlobalTimer.time_elapsed;
        position.x = hit[0], position.y = hit[1], position.z = hit[2];
        PlaySfx("SaberSaber01", &position);
        SagaVrBlockedMessage();
    }
}

/* "Blocked!" in the game's language, briefly in front of the player, when the VR blade stops a bolt or a blade
 * (the block was easy to miss: only a sound). Text ids would need new game strings, so the words live here. */
static void SagaVrBlockedMessage(void) {
    static f32 last = -10.0f;
    static char text[32];
    const char *word;
    if (GlobalTimer.time_elapsed - last < 0.5f && GlobalTimer.time_elapsed >= last)
        return;
    last = GlobalTimer.time_elapsed;
    LOG_INFO("VR block: blocked");
    switch (Text_Language) {
        case 3:
            word = "\xa1" "Bloqueado!";
            break;
        case 2:
            word = "Bloqu\xe9 !";
            break;
        case 4:
            word = "Geblockt!";
            break;
        case 5:
            word = "Parato!";
            break;
        case 8:
            word = "Blokeret!";
            break;
        default:
            word = "Blocked!";
            break;
    }
    strcpy(text, word);
    AddFancyMessage(text, 0.0f, 0.3f, 0.6f, 0.8f, 1, 1);
}

/* Jedi slam: after a double jump, pointing the lit VR blade down presses attack, which the game turns into its
 * slam (DoubleJump_JediSlam, jump_sequence 2) - the same as attacking in the air on a pad. */
i32 __cdecl SagaVrSlamPress(void) {
    GAMEOBJECT *player = SagaVrSaberPlayer();
    PLAYERCHARACTERCONFIG_PC *data;
    f32 blade[3], length;
    i32 c;
    if (player == NULL || !SagaVrSaberDriving() || !SagaVrGetControl()->saber_lit || SagaVrBladeScale < 0.9f ||
        player->field_281 != 0 || player->jump_sequence != 2)
        return 0;
    data = player->character_data->game_character;
    for (c = 0; c < 3; ++c)
        blade[c] = (&player->locator_matrices[data->streak_joints[0][1]].m30)[c] -
                   (&player->locator_matrices[data->streak_joints[0][0]].m30)[c];
    length = SagaVrLength(blade);
    return length > 1e-4f && blade[1] < -0.7f * length;
}

/* Characters with a gun (no saber) punch with the left hand: a fast swing (over SAGA_VR_PUNCH_SPEED m/s) that
 * reaches an enemy hits it once per SAGA_VR_PUNCH_COOLDOWN, like the game's own close-range brawl. */
#define SAGA_VR_PUNCH_SPEED 1.5f
#define SAGA_VR_PUNCH_COOLDOWN 0.4f
#define SAGA_VR_PUNCH_REACH 0.12f /* game units beyond the target's collision radius */

void __cdecl SagaVrPunchUpdate(void) {
    static f32 previous[3], previous_time = -1.0f, last_hit = -10.0f;
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    GAMEOBJECT *player = (GAMEOBJECT *)Player[0], *target;
    PLAYERCHARACTERCONFIG_PC *data;
    f32 hand[16], now = GlobalTimer.time_elapsed, speed, delta[3], reach;
    i32 i, c;
    if (player == NULL || !control->first_person || !SagaVrFirstPersonActive() || !control->hand_valid[0] ||
        SagaVrCinema() || player->character_data == NULL || player->character_data->game_character == NULL ||
        player->dead != 0 || !SagaVrHandWorld(0, hand)) {
        previous_time = -1.0f;
        return;
    }
    data = player->character_data->game_character;
    /* Only gun characters: sabers have their blade, others their own attacks. */
    if (data->streak_joints[0][0] >= 0 || (data->weapon_shoot_joints[0] < 0 && data->weapon_joints[0] < 0)) {
        previous_time = -1.0f;
        return;
    }
    if (previous_time < 0.0f || now <= previous_time || now - previous_time > 0.2f) {
        memcpy(previous, hand + 12, sizeof(previous));
        previous_time = now;
        return;
    }
    for (c = 0; c < 3; ++c)
        delta[c] = hand[12 + c] - previous[c];
    speed = SagaVrLength(delta) / (now - previous_time) / (control->fp_world_scale > 0.0f ? control->fp_world_scale : 1.0f);
    memcpy(previous, hand + 12, sizeof(previous));
    previous_time = now;
    if (speed < SAGA_VR_PUNCH_SPEED || (now - last_hit < SAGA_VR_PUNCH_COOLDOWN && now >= last_hit))
        return;
    for (i = 0, target = Obj; i < HIGHGAMEOBJECT; ++i, ++target) {
        if (target == player || target->dead != 0 || (target->flags_1fc_word & 0x1001) != 0x1001 ||
            ((target->flags_1f8 ^ player->flags_1f8) & 0x10001) == 0)
            continue;
        for (c = 0; c < 3; ++c)
            delta[c] = (&target->collision_position.x)[c] - hand[12 + c];
        reach = target->collision_radius + SAGA_VR_PUNCH_REACH;
        if (delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] > reach * reach)
            continue;
        last_hit = now;
        LOG_INFO("VR punch: character %d at %.2f m/s", (i32)target->id, speed);
        ObjHitObj(player, target, 1, (u16)ObjHitObj_Flags(player), 0, 1);
        break;
    }
}
