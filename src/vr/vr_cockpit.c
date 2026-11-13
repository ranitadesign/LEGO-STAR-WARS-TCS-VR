/* LEGO Star Wars: The Complete Saga VR mod by RanitaVR - https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR */
/* VR fork: a cockpit for the ships, which have no interior of their own.
 *
 * Ships are modelled to be seen from outside: from their seat there is nothing to sit in. The same simple
 * cockpit, built from coloured LEGO-like blocks with the engine's primitive drawing, goes around the eyes in
 * every ship: a dashboard, side rails, a floor, the seat, a canopy frame and a flight stick. It moves with
 * the view (level, turned with the ship), so the cockpit never swings around the head.
 *
 * The flight stick is held with the right grip: the hand's move from where it took the stick, across and
 * along, tilts it and is the left stick's input. Without it the thumbstick still flies the ship. */
#include "vr/vr_internal.h"
#include "../../xr/saga_xr.h"

#include <math.h>
#include <string.h>

#define SAGA_VR_STICK_REACH 0.22f  /* metres from the hand to the stick's grip */
#define SAGA_VR_STICK_THROW 0.08f  /* metres of hand travel for a full tilt */
#define SAGA_VR_STICK_TILT 25.0f   /* degrees at a full tilt */
#define SAGA_VR_STICK_LENGTH 0.20f /* metres from the pivot to the grip */
#define SAGA_VR_SIGHT_AHEAD 0.66f  /* metres from the eyes to the sight, on the windshield */

/* Cockpit layout around the eyes, in metres: x right, y up, z forward. */
static const f32 SagaVrStickPivot[3] = {0.16f, -0.56f, 0.22f};

typedef struct SAGA_VR_PRIM_VERTEX {
    f32 x, y, z;
    u32 colour;
    f32 u, v;
} SAGA_VR_PRIM_VERTEX;

static f32 SagaVrStick[2];   /* x right, y forward; -1..1 */
static f32 SagaVrStickGrab[2];
static i32 SagaVrStickHeld;
static u32 SagaVrStickFrame = 0xffffffffu;
static f32 SagaVrStickTop[16]; /* the grip's pose in the world, for the hand holding it */

i32 __cdecl SagaVrInShip(GAMEOBJECT *object) {
    return object != NULL && object == (GAMEOBJECT *)Player[0] && VehicleArea != 0 && object->rider == NULL &&
           !SagaVrOnPod(object);
}

/* The engine's primitive colour packing, as NuRndrPrimSetColourPrivate (untextured: halved for overbright). */
static u32 SagaVrPrimColour(u32 colour) {
    /* Colours here are 0xAARRGGBB; the engine takes red in the low byte. */
    colour = (colour & 0xff00ff00u) | ((colour >> 16) & 0xff) | ((colour & 0xff) << 16);
    if (NuPrimNeedsOverbrightening != 0)
        return ((colour & 0xff) << 16) + ((colour >> 16) & 0xff) + (colour & 0xff00) + (colour & 0xff000000);
    return ((colour & 0xfe) << 15) + ((colour >> 1) & 0x7f00) + ((colour >> 17) & 0x7f) + (colour & 0xff000000);
}

static u32 SagaVrShade(u32 colour, f32 light) {
    u32 r = (u32)(((colour >> 16) & 0xff) * light), g = (u32)(((colour >> 8) & 0xff) * light),
        b = (u32)((colour & 0xff) * light);
    return 0xff000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static void SagaVrPrimVertex(const f32 *p, u32 colour) {
    SAGA_VR_PRIM_VERTEX *vertex = (SAGA_VR_PRIM_VERTEX *)NuPrimStreamBufferPointer->void_ptr;
    vertex->x = p[0], vertex->y = p[1], vertex->z = p[2];
    vertex->colour = SagaVrPrimColour(colour);
    vertex->u = vertex->v = 0.0f;
    NuPrimStreamBufferPointer->addr += sizeof(SAGA_VR_PRIM_VERTEX);
    ++NuPrimVertexCount;
}

/* The primitives go into the frame's render stream, which nothing bounds-checks (overflowing it stops the game
 * at the frame swap). The cockpit leaves this much of it free for whatever is drawn after it. */
#define SAGA_VR_PRIM_MARGIN 0x40000u
static i32 SagaVrPrimSkipped;

static i32 SagaVrPrimRoom(u32 vertices) {
    u32 at = NuPrimStreamBufferPointer->addr, end = nuprim_field_ba8;
    return at < end && end - at > SAGA_VR_PRIM_MARGIN + vertices * (u32)sizeof(SAGA_VR_PRIM_VERTEX);
}

/* A quad as two triangles, both windings so it shows from either side. */
static void SagaVrPrimQuad(const f32 *a, const f32 *b, const f32 *c, const f32 *d, u32 colour) {
    if (!SagaVrPrimRoom(12)) {
        SagaVrPrimSkipped = 1;
        return;
    }
    SagaVrPrimVertex(a, colour), SagaVrPrimVertex(b, colour), SagaVrPrimVertex(c, colour);
    SagaVrPrimVertex(a, colour), SagaVrPrimVertex(c, colour), SagaVrPrimVertex(d, colour);
    SagaVrPrimVertex(a, colour), SagaVrPrimVertex(c, colour), SagaVrPrimVertex(b, colour);
    SagaVrPrimVertex(a, colour), SagaVrPrimVertex(d, colour), SagaVrPrimVertex(c, colour);
}

/* A block given by its centre, half sizes along its own axes (rows of axes, or NULL for the cockpit's)
 * and colour; faces shaded as if lit from above and ahead. */
static void SagaVrPrimBlock(const f32 *centre, const f32 *half, const f32 *axes, u32 colour) {
    static const f32 identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    f32 corner[8][3];
    i32 i, c;
    if (axes == NULL)
        axes = identity;
    for (i = 0; i < 8; ++i)
        for (c = 0; c < 3; ++c)
            corner[i][c] = centre[c] + ((i & 1) ? half[0] : -half[0]) * axes[0 + c] +
                           ((i & 2) ? half[1] : -half[1]) * axes[3 + c] + ((i & 4) ? half[2] : -half[2]) * axes[6 + c];
    SagaVrPrimQuad(corner[2], corner[3], corner[7], corner[6], SagaVrShade(colour, 1.0f));  /* top */
    SagaVrPrimQuad(corner[0], corner[1], corner[5], corner[4], SagaVrShade(colour, 0.45f)); /* bottom */
    SagaVrPrimQuad(corner[0], corner[2], corner[6], corner[4], SagaVrShade(colour, 0.7f));  /* left */
    SagaVrPrimQuad(corner[1], corner[3], corner[7], corner[5], SagaVrShade(colour, 0.7f));  /* right */
    SagaVrPrimQuad(corner[0], corner[1], corner[3], corner[2], SagaVrShade(colour, 0.85f)); /* back */
    SagaVrPrimQuad(corner[4], corner[5], corner[7], corner[6], SagaVrShade(colour, 0.6f));  /* front */
}

static void SagaVrBlock(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, u32 colour) {
    f32 centre[3], half[3];
    centre[0] = (x0 + x1) * 0.5f, centre[1] = (y0 + y1) * 0.5f, centre[2] = (z0 + z1) * 0.5f;
    half[0] = (f32)fabs(x1 - x0) * 0.5f, half[1] = (f32)fabs(y1 - y0) * 0.5f, half[2] = (f32)fabs(z1 - z0) * 0.5f;
    SagaVrPrimBlock(centre, half, NULL, colour);
}

/* A beam from a to b, width across and depth front to back of its own frame. */
static void SagaVrBeam(const f32 *a, const f32 *b, f32 width, f32 depth, u32 colour) {
    f32 axes[9], centre[3], half[3], length, ref[3] = {0.0f, 0.0f, 1.0f};
    i32 c;
    for (c = 0; c < 3; ++c)
        axes[3 + c] = b[c] - a[c], centre[c] = (a[c] + b[c]) * 0.5f;
    length = (f32)sqrt(axes[3] * axes[3] + axes[4] * axes[4] + axes[5] * axes[5]);
    if (length <= 0.0f)
        return;
    axes[3] /= length, axes[4] /= length, axes[5] /= length;
    if (fabs(axes[5]) > 0.9f)
        ref[1] = 1.0f, ref[2] = 0.0f;
    axes[0] = axes[4] * ref[2] - axes[5] * ref[1];
    axes[1] = axes[5] * ref[0] - axes[3] * ref[2];
    axes[2] = axes[3] * ref[1] - axes[4] * ref[0];
    half[0] = (f32)sqrt(axes[0] * axes[0] + axes[1] * axes[1] + axes[2] * axes[2]);
    axes[0] /= half[0], axes[1] /= half[0], axes[2] /= half[0];
    axes[6] = axes[1] * axes[5] - axes[2] * axes[4];
    axes[7] = axes[2] * axes[3] - axes[0] * axes[5];
    axes[8] = axes[0] * axes[4] - axes[1] * axes[3];
    half[0] = width * 0.5f, half[1] = length * 0.5f, half[2] = depth * 0.5f;
    SagaVrPrimBlock(centre, half, axes, colour);
}

/* A LEGO stud (or round button): a prism of `sides` sides standing on y. */
static void SagaVrStud(f32 x, f32 y, f32 z, f32 radius, f32 height, i32 sides, u32 colour) {
    f32 ring[2][8][3];
    i32 i, k;
    u32 side = SagaVrShade(colour, 0.7f), top = SagaVrShade(colour, 1.0f);
    for (i = 0; i < sides; ++i) {
        f32 angle = 6.2831853f * ((f32)i + 0.5f) / (f32)sides;
        for (k = 0; k < 2; ++k) {
            ring[k][i][0] = x + radius * (f32)cos(angle);
            ring[k][i][1] = y + (k ? height : 0.0f);
            ring[k][i][2] = z + radius * (f32)sin(angle);
        }
    }
    for (i = 0; i < sides; ++i)
        SagaVrPrimQuad(ring[0][i], ring[0][(i + 1) % sides], ring[1][(i + 1) % sides], ring[1][i], side);
    for (i = 1; i + 2 < sides; i += 2)
        SagaVrPrimQuad(ring[1][0], ring[1][i], ring[1][i + 1], ring[1][i + 2], top);
}

/* A wall of staggered bricks across x (or z when along_z), with dark joints showing between them. */
static void SagaVrBrickWall(f32 fixed, f32 thick, f32 from, f32 to, f32 bottom, f32 top, i32 along_z, u32 colour) {
    const f32 course = 0.096f, brick = 0.24f, gap = 0.006f;
    f32 y, s;
    i32 row = 0;
    if (along_z)
        SagaVrBlock(fixed - thick * 0.4f, bottom, from, fixed + thick * 0.4f, top, to, 0xff6a6e74);
    else
        SagaVrBlock(from, bottom, fixed - thick * 0.4f, to, top, fixed + thick * 0.4f, 0xff6a6e74);
    for (y = bottom; y < top - 0.01f; y += course, ++row) {
        f32 y1 = y + course > top ? top : y + course;
        u32 shade = (row & 1) ? colour : SagaVrShade(colour, 0.94f);
        for (s = from - ((row & 1) ? brick * 0.5f : 0.0f); s < to; s += brick) {
            f32 s0 = s < from ? from : s, s1 = s + brick > to ? to : s + brick;
            if (s1 - s0 < 0.02f)
                continue;
            if (along_z)
                SagaVrBlock(fixed - thick * 0.5f, y + gap, s0 + gap, fixed + thick * 0.5f, y1 - gap, s1 - gap, shade);
            else
                SagaVrBlock(s0 + gap, y + gap, fixed - thick * 0.5f, s1 - gap, y1 - gap, fixed + thick * 0.5f, shade);
        }
    }
}

/* The stick's axes: tilted right by x and forward by y. Rows: right, up (along the stick), forward. */
static void SagaVrStickAxes(f32 *axes) {
    f32 ax = SagaVrStick[0] * SAGA_VR_STICK_TILT * 0.017453293f, ay = SagaVrStick[1] * SAGA_VR_STICK_TILT * 0.017453293f;
    f32 up[3], length;
    up[0] = (f32)sin(ax), up[1] = (f32)(cos(ax) * cos(ay)), up[2] = (f32)sin(ay);
    length = (f32)sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    up[0] /= length, up[1] /= length, up[2] /= length;
    /* right = up x forward(0,0,1), then forward = right x up. */
    axes[0] = up[1], axes[1] = -up[0], axes[2] = 0.0f;
    length = (f32)sqrt(axes[0] * axes[0] + axes[1] * axes[1]);
    axes[0] /= length, axes[1] /= length;
    axes[3] = up[0], axes[4] = up[1], axes[5] = up[2];
    axes[6] = axes[1] * up[2] - axes[2] * up[1];
    axes[7] = axes[2] * up[0] - axes[0] * up[2];
    axes[8] = axes[0] * up[1] - axes[1] * up[0];
}

/* One handle: a black post from the floor to its pivot, the shaft along axes (NULL: upright) and a tan grip. */
static void SagaVrHandle(const f32 *pivot, const f32 *axes) {
    static const f32 upright[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const u32 black = 0xff1c1c1e, tan = 0xffd8c08c;
    f32 centre[3], half[3];
    i32 i;
    if (axes == NULL)
        axes = upright;
    SagaVrBlock(pivot[0] - 0.07f, -0.80f, pivot[2] - 0.07f, pivot[0] + 0.07f, -0.77f, pivot[2] + 0.07f, black);
    SagaVrBlock(pivot[0] - 0.035f, -0.77f, pivot[2] - 0.035f, pivot[0] + 0.035f, pivot[1], pivot[2] + 0.035f, black);
    half[0] = half[2] = 0.014f, half[1] = SAGA_VR_STICK_LENGTH * 0.5f;
    for (i = 0; i < 3; ++i)
        centre[i] = pivot[i] + axes[3 + i] * SAGA_VR_STICK_LENGTH * 0.5f;
    SagaVrPrimBlock(centre, half, axes, black);
    half[0] = 0.032f, half[1] = 0.026f, half[2] = 0.028f;
    for (i = 0; i < 3; ++i)
        centre[i] = pivot[i] + axes[3 + i] * (SAGA_VR_STICK_LENGTH + 0.03f);
    SagaVrPrimBlock(centre, half, axes, tan);
    half[0] = 0.01f, half[1] = 0.012f, half[2] = 0.008f;
    for (i = 0; i < 3; ++i)
        centre[i] = pivot[i] + axes[3 + i] * (SAGA_VR_STICK_LENGTH + 0.03f) + axes[6 + i] * 0.032f;
    SagaVrPrimBlock(centre, half, axes, 0xffc4281b);
}

/* A flat ring of small blocks facing the pilot (or lying on y when flat), for the radar and the sight. */
static void SagaVrRing(f32 x, f32 y, f32 z, f32 radius, f32 size, i32 flat, u32 colour) {
    i32 i;
    for (i = 0; i < 16; ++i) {
        f32 angle = 6.2831853f * (f32)i / 16.0f, a = radius * (f32)cos(angle), b = radius * (f32)sin(angle);
        if (flat)
            SagaVrBlock(x + a - size, y, z + b - size, x + a + size, y + 0.002f, z + b + size, colour);
        else
            SagaVrBlock(x + a - size, y + b - size, z, x + a + size, y + b + size, z + 0.003f, colour);
    }
}

/* The ship fires along its heading, which is the cockpit's forward: a sight on the windshield straight ahead
 * of the eyes. */
static void SagaVrSight(void) {
    const u32 green = 0xff48ff6a;
    const f32 z = SAGA_VR_SIGHT_AHEAD;
    SagaVrRing(0.0f, 0.0f, z, 0.032f, 0.004f, 0, green);
    SagaVrBlock(-0.004f, -0.004f, z, 0.004f, 0.004f, z + 0.003f, green);
    SagaVrBlock(-0.070f, -0.002f, z, -0.044f, 0.002f, z + 0.003f, green);
    SagaVrBlock(0.044f, -0.002f, z, 0.070f, 0.002f, z + 0.003f, green);
    SagaVrBlock(-0.002f, -0.070f, z, 0.002f, -0.044f, z + 0.003f, green);
    SagaVrBlock(-0.002f, 0.044f, z, 0.002f, 0.070f, z + 0.003f, green);
}

static void SagaVrCockpitGeometry(void) {
    const u32 panel = 0xff4a4e55, top = 0xff3a3d42, light = 0xffc4c8cc, white = 0xffe2e4e6, black = 0xff1c1c1e,
              floor = 0xff4c5056, red = 0xffb0261e;
    static const u32 keys[4] = {0xffe8e8e8, 0xffc4281b, 0xff1c1c1e, 0xffa0a4a8};
    static const u32 big[3] = {0xffc4281b, 0xfff5cd2f, 0xff0d69ab};
    static const u32 little[4] = {0xff8a8e94, 0xffb8bcc0, 0xff5b8fd0, 0xff8a8e94};
    f32 axes[9], a[3], b[3], x, z, left[3];
    i32 s, i, r, c;
    /* Floor: a stud plate, clear under the seat and the handles. */
    SagaVrBlock(-0.57f, -0.84f, -0.46f, 0.57f, -0.80f, 0.76f, floor);
    for (x = -0.50f; x < 0.51f; x += 0.10f)
        for (z = -0.38f; z < 0.47f; z += 0.10f)
            if (!(fabs(x) < 0.30f && z < 0.14f) && !(fabs(fabs(x) - SagaVrStickPivot[0]) < 0.09f && fabs(z - SagaVrStickPivot[2]) < 0.09f))
                SagaVrStud(x, -0.80f, z, 0.03f, 0.018f, 6, 0xff5a5e64);
    /* Side walls of bricks up to the sills, closed behind the side windows; the rear wall and the roof. */
    for (s = -1; s <= 1; s += 2) {
        SagaVrBrickWall((f32)s * 0.60f, 0.06f, -0.46f, 0.76f, -0.80f, -0.24f, 1, white);
        SagaVrBlock((f32)s * 0.52f, -0.24f, -0.52f, (f32)s * 0.64f, -0.21f, 0.78f, 0xfff0f0f0);
        SagaVrBlock((f32)s * 0.57f, -0.21f, -0.52f, (f32)s * 0.63f, 0.42f, -0.10f, light);
        SagaVrBlock((f32)s * 0.55f, 0.34f, -0.52f, (f32)s * 0.64f, 0.42f, 0.46f, light);
        /* Window posts: the slanted front one and the one behind the side window. */
        a[0] = (f32)s * 0.56f, a[1] = -0.21f, a[2] = 0.76f;
        b[0] = (f32)s * 0.50f, b[1] = 0.40f, b[2] = 0.44f;
        SagaVrBeam(a, b, 0.08f, 0.06f, light);
        a[0] = b[0] = (f32)s * 0.60f, a[1] = -0.21f, b[1] = 0.40f, a[2] = b[2] = -0.10f;
        SagaVrBeam(a, b, 0.06f, 0.07f, light);
    }
    SagaVrBlock(-0.64f, -0.84f, -0.54f, 0.64f, 0.47f, -0.46f, 0xff9ea2a6);
    SagaVrBlock(-0.64f, 0.42f, -0.54f, 0.64f, 0.47f, 0.48f, 0xff5a5e64);
    SagaVrBlock(-0.62f, 0.35f, 0.38f, 0.62f, 0.44f, 0.48f, light);
    for (x = -0.45f; x < 0.46f; x += 0.15f)
        SagaVrStud(x, 0.35f, 0.43f, 0.025f, -0.012f, 6, 0xff3a3d42);
    /* Dashboard: its body down to the floor, the console top and a light lip under the windshield. */
    SagaVrBlock(-0.57f, -0.50f, 0.48f, 0.57f, -0.27f, 0.78f, panel);
    SagaVrBlock(-0.44f, -0.80f, 0.58f, 0.44f, -0.50f, 0.78f, 0xff34373c);
    SagaVrBlock(-0.55f, -0.27f, 0.46f, 0.55f, -0.24f, 0.72f, top);
    SagaVrBlock(-0.57f, -0.24f, 0.70f, 0.57f, -0.18f, 0.78f, light);
    /* Radar screen in the middle. */
    SagaVrBlock(-0.15f, -0.24f, 0.49f, 0.15f, -0.226f, 0.67f, black);
    SagaVrBlock(-0.13f, -0.226f, 0.51f, 0.13f, -0.220f, 0.65f, 0xff4caf50);
    SagaVrRing(0.0f, -0.220f, 0.58f, 0.05f, 0.004f, 1, 0xff2e7d32);
    SagaVrBlock(-0.12f, -0.220f, 0.578f, 0.12f, -0.218f, 0.582f, 0xff2e7d32);
    SagaVrBlock(-0.002f, -0.220f, 0.52f, 0.002f, -0.218f, 0.64f, 0xff2e7d32);
    for (s = -1; s <= 1; s += 2) {
        /* A keypad beside the screen. */
        SagaVrBlock((f32)s * 0.16f, -0.24f, 0.50f, (f32)s * 0.27f, -0.232f, 0.66f, 0xff6a6e74);
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 4; ++c) {
                x = (f32)s * (0.175f + 0.026f * (f32)c);
                z = 0.525f + 0.045f * (f32)r;
                SagaVrBlock(x - 0.009f, -0.232f, z - 0.013f, x + 0.009f, -0.224f, z + 0.013f, keys[(r * 4 + c + (s > 0)) % 4]);
            }
        /* Round buttons: big coloured ones near the pilot, small ones behind. */
        for (i = 0; i < 3; ++i)
            SagaVrStud((s < 0 ? -0.48f : 0.32f) + 0.08f * (f32)i, -0.24f, 0.52f, 0.027f, 0.022f, 8, big[i]);
        for (i = 0; i < 4; ++i)
            SagaVrStud((s < 0 ? -0.49f : 0.31f) + 0.06f * (f32)i, -0.24f, 0.61f, 0.019f, 0.016f, 8, little[i]);
    }
    /* Seat: red cushion and back. */
    SagaVrBlock(-0.26f, -0.80f, -0.40f, 0.26f, -0.66f, 0.10f, red);
    SagaVrBlock(-0.26f, -0.80f, -0.46f, 0.26f, 0.02f, -0.38f, SagaVrShade(red, 0.85f));
    /* Two handles: the right one is the flight stick, the left one stays put. */
    SagaVrStickAxes(axes);
    SagaVrHandle(SagaVrStickPivot, axes);
    left[0] = -SagaVrStickPivot[0], left[1] = SagaVrStickPivot[1], left[2] = SagaVrStickPivot[2];
    SagaVrHandle(left, NULL);
    SagaVrSight();
}

/* The cockpit's frame in the world: the view's rig, level and turned with the ship; metres to world units. */
static i32 SagaVrCockpitFrame(f32 *frame) {
    const f32 *rig = SagaVrGetRig();
    f32 scale = SagaVrGetControl()->fp_world_scale;
    i32 i;
    if (rig == NULL)
        return 0;
    for (i = 0; i < 12; ++i)
        frame[i] = rig[i] * ((i % 4) == 3 ? 1.0f : scale);
    memcpy(frame + 12, rig + 12, sizeof(f32) * 4);
    return 1;
}

/* Grip handling, once per frame: the right grip near the stick's top holds it. */
static void SagaVrStickUpdate(const f32 *frame) {
    SAGA_VR_CONTROL *control = SagaVrGetControl();
    f32 hand[16], top_local[3], top[3], axes[9], r[3], scale = control->fp_world_scale, blend, frame_time;
    i32 grip = control->hand_valid[1] && (control->controller_buttons & SAGA_XR_BUTTON_SABER) != 0, i, c;
    if (SagaVrStickFrame == control->frame)
        return;
    SagaVrStickFrame = control->frame;
    SagaVrStickAxes(axes);
    for (i = 0; i < 3; ++i)
        top_local[i] = SagaVrStickPivot[i] + axes[3 + i] * (SAGA_VR_STICK_LENGTH + 0.03f);
    for (c = 0; c < 3; ++c)
        top[c] = frame[12 + c] + top_local[0] * frame[0 + c] + top_local[1] * frame[4 + c] + top_local[2] * frame[8 + c];
    if (grip && !SagaVrStickHeld && SagaVrHandWorld(1, hand)) {
        for (c = 0; c < 3; ++c)
            r[c] = hand[12 + c] - top[c];
        if ((f32)sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) < SAGA_VR_STICK_REACH * scale) {
            SagaVrStickHeld = 1;
            SagaVrStickGrab[0] = control->hand_position[1][0];
            SagaVrStickGrab[1] = -control->hand_position[1][2];
            LOG_INFO("VR flight stick held");
        }
    } else if (!grip && SagaVrStickHeld) {
        SagaVrStickHeld = 0;
        LOG_INFO("VR flight stick released");
    }
    frame_time = nuapi.frame_time > 0.0f && nuapi.frame_time < 0.1f ? nuapi.frame_time : 1.0f / 60.0f;
    blend = (f32)(1.0 - exp(-frame_time / 0.05f));
    for (i = 0; i < 2; ++i) {
        f32 target = 0.0f;
        if (SagaVrStickHeld) {
            f32 along = i == 0 ? control->hand_position[1][0] : -control->hand_position[1][2];
            target = (along - SagaVrStickGrab[i]) / SAGA_VR_STICK_THROW;
            target = target > 1.0f ? 1.0f : target < -1.0f ? -1.0f : target;
        }
        SagaVrStick[i] += (target - SagaVrStick[i]) * blend;
    }
    /* The holding hand sits on the grip, its grip axis along the stick. */
    memset(SagaVrStickTop, 0, sizeof(SagaVrStickTop));
    for (c = 0; c < 3; ++c) {
        SagaVrStickTop[0 + c] = -(axes[6] * frame[0 + c] + axes[7] * frame[4 + c] + axes[8] * frame[8 + c]) / scale;
        SagaVrStickTop[8 + c] = (axes[3] * frame[0 + c] + axes[4] * frame[4 + c] + axes[5] * frame[8 + c]) / scale;
        SagaVrStickTop[12 + c] = top[c];
    }
    SagaVrStickTop[4] = SagaVrStickTop[9] * SagaVrStickTop[2] - SagaVrStickTop[10] * SagaVrStickTop[1];
    SagaVrStickTop[5] = SagaVrStickTop[10] * SagaVrStickTop[0] - SagaVrStickTop[8] * SagaVrStickTop[2];
    SagaVrStickTop[6] = SagaVrStickTop[8] * SagaVrStickTop[1] - SagaVrStickTop[9] * SagaVrStickTop[0];
    SagaVrStickTop[15] = 1.0f;
    if (SagaVrStickHeld && control->frame % 30 == 0)
        LOG_INFO("VR flight stick: x %.2f y %.2f", SagaVrStick[0], SagaVrStick[1]);
}

const f32 *__cdecl SagaVrShipHeldHand(void) {
    return SagaVrStickHeld ? SagaVrStickTop : NULL;
}

i32 __cdecl SagaVrShipStickInput(f32 *x, f32 *y) {
    if (!SagaVrStickHeld || SagaVrStickFrame + 2 < SagaVrGetControl()->frame)
        return 0;
    *x = SagaVrStick[0];
    *y = SagaVrStick[1];
    return 1;
}

void __cdecl SagaVrDrawShipCockpit(GAMEOBJECT *object) {
    NUMTX matrix;
    if (!SagaVrFirstPersonActive() || !SagaVrInShip(object) || !SagaVrCockpitFrame(&matrix.m00))
        return;
    SagaVrStickUpdate(&matrix.m00);
    NuPrim3DBegin(12, 5, NULL, &matrix);
    {
        static i32 logged = 0, skipped = 0;
        u32 before = NuPrimStreamBufferPointer->addr;
        SagaVrPrimSkipped = 0;
        SagaVrCockpitGeometry();
        if (!logged) {
            logged = 1;
            LOG_INFO("VR ship cockpit: %u vertices, %u bytes of the render stream, %u left after it", NuPrimVertexCount,
                     NuPrimStreamBufferPointer->addr - before, nuprim_field_ba8 - NuPrimStreamBufferPointer->addr);
        }
        if (SagaVrPrimSkipped && !skipped) {
            skipped = 1;
            LOG_WARN("VR ship cockpit: render stream nearly full, parts of the cockpit left out");
        }
    }
    NuPrim3DEnd();
}
