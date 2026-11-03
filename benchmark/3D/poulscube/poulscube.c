// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* poulscube: the poulsback terminal logo as a retro neon GLX demo.
 *
 * The logo's cube turns about its vertical axis at glxgears' 70 degrees per
 * second, seen from below like the logo, between the logo's terminal brackets
 * (which breathe, 4 % larger and smaller every 3 seconds) and under the
 * "poulsback" wordmark with its blinking block cursor. The brackets and letters
 * are the logo's own pixel geometry (assets/logo: the terminal SVG's bracket
 * rectangles, lettering.json's glyph rows), extruded into blocks. The cube is
 * 3x3x3 pieces in retro neon colours (hot pink, violet, indigo, cyan, mint,
 * chartreuse, yellow, orange) that flow along its diagonal and glow; every 8
 * seconds it shatters, turns one layer a quarter turn and back like a Rubik's
 * cube, and closes again.
 *
 * Drawn for a CPU-bound driver: per frame the cube is one glDrawElements of
 * the faces that can be seen, built on the CPU in eye space (pieces placed,
 * back faces and hidden inner faces dropped, shading baked into flat
 * colours); the static frame is three display lists whose faces were culled
 * and shaded once. No GL lighting runs. A frame is three or four draws:
 * wordmark, cursor, brackets, cube. GL used: client vertex and colour arrays,
 * display lists, flat shading, depth test, back-face culling, a frustum and
 * double buffering; no blending, textures or FBOs. Every 5 seconds a line in
 * glxgears' format gives the frame count and rate, followed by the mean frame
 * time and the system's (/proc/stat) and this process's CPU load: nothing is
 * drawn for it.
 *
 * usage: poulscube [-display NAME] [-geometry WxH[+X+Y]] [-fullscreen] [-info]
 *                  [-swapinterval N] [-noeffect] [-duration SECONDS]
 *                  [-dump PREFIX] [-record FILE [-record-fps FPS]]
 * -duration runs that long, then prints a summary and exits: frames, the
 * frame rate, the frame time (mean, median, 99th percentile, worst), the
 * system's and this process's CPU load, and the same as one JSON line.
 * -dump writes the first frame after 1.0, 3.5, 5.0 and 6.5 seconds (solid,
 * opening, mid-turn, closing) to PREFIX-N.ppm, read back from the back buffer.
 * -record reads animation times in seconds from stdin and writes consecutive
 * binary PPM frames to FILE without swapping or collecting performance data.
 * -record-fps clocks the cursor from the exported frame index, a quarter-cycle
 * ahead, instead of the supplied scene time. FPS is in the range 1..1000.
 * Keys: Escape or q quits; e toggles the effect.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>

#define LOGO_PIECE 2.2f            /* the logo cube's piece size, which sets the layout */
#define PIECE (0.85f * LOGO_PIECE) /* the turning cube, a bit smaller inside the brackets */
#define GAP 0.2f                   /* extra spacing per piece when fully shattered */
#define CYCLE 8.0                  /* seconds per effect cycle */
#define DEGREES_PER_SECOND 70.0
#define TILT -30.0f /* seen from below, edges at the logo's 2:1 slope */
#define VIEW_DISTANCE 45.0f

/* World units per logo pixel: the logo cube's 96-pixel width is its diagonal,
 * 3 * LOGO_PIECE * sqrt(2). Logo pixels are the terminal SVG's, centred on the
 * cube (118, 80), y down. */
#define LOGO_UNIT (3.0f * LOGO_PIECE * 1.41421356f / 96.0f)
#define LOGO_X(px) (((px) - 118.0f) * LOGO_UNIT)
#define LOGO_Y(py) ((80.0f - (py)) * LOGO_UNIT)

static const GLfloat background[3] = {0.0f, 0.0f, 0.0f}; /* #000000 */
static const GLfloat foreground[3] = {0.961f, 0.965f, 0.969f}; /* #F5F6F7 */
static const GLfloat cursor_color[3] = {1.0f, 0.0f, 1.0f};      /* #FF00FF */
static const GLfloat inner_color[3] = {0.10f, 0.10f, 0.12f};

/* The neon colours, looped in this order with linear blends between them. */
static const GLfloat neon_palette[][3] = {
    {1.00f, 0.00f, 0.63f}, /* hot pink */
    {0.69f, 0.00f, 1.00f}, /* violet */
    {0.30f, 0.30f, 1.00f}, /* electric indigo */
    {0.00f, 0.90f, 1.00f}, /* cyan */
    {0.00f, 1.00f, 0.61f}, /* mint */
    {0.80f, 1.00f, 0.00f}, /* chartreuse */
    {1.00f, 0.90f, 0.00f}, /* yellow */
    {1.00f, 0.48f, 0.00f}, /* orange */
};
#define NEON_COLOURS (int)(sizeof(neon_palette) / sizeof(neon_palette[0]))
#define NEON_SPEED 2.0 /* palette colours per second */
#define NEON_SPAN 3.0  /* palette colours along the cube's diagonal */

/* Baked shading: one directional light in eye space, the ambient share, and
 * the neon faces' glow added to it. */
static const float light_dir[3] = {0.2683f, -0.3578f, 0.8944f}; /* (3, -4, 10), unit */
#define AMBIENT 0.5f
#define GLOW 0.35f

/* The terminal SVG's brackets, in logo pixels: left x, top y; each is a 21 x 96
 * "[" or "]" of 9-pixel strokes. */
#define BRACKET_WIDTH 21
#define BRACKET_HEIGHT 96
#define BRACKET_STROKE 9
static const int bracket_at[2][2] = {{37, 32}, {178, 32}};

/* lettering.json "wordmark": 15 rows per glyph, advance includes 2 cells of tracking. */
#define GLYPH_ROWS 15
static const struct glyph {
    int advance;
    const char *rows[GLYPH_ROWS];
} wordmark[] = {
    {11, {".........", ".........", ".........", "###.####.", "#########", "#########", "###...###",
          "###...###", "###...###", "#########", "#########", "###.####.", "###......", "###......",
          "###......"}}, /* p */
    {11, {".........", ".........", ".........", ".#######.", "#########", "#########", "###...###",
          "###...###", "###...###", "#########", "#########", ".#######.", ".........", ".........",
          "........."}}, /* o */
    {11, {".........", ".........", ".........", "###...###", "###...###", "###...###", "###...###",
          "###...###", "###..####", "#########", "#########", ".####.###", ".........", ".........",
          "........."}}, /* u */
    {7, {"###..", "###..", "###..", "###..", "###..", "###..", "###..", "###..", "###..", "#####",
         "#####", ".####", ".....", ".....", "....."}}, /* l */
    {10, {"........", "........", "........", ".#######", "########", "########", "###.....",
          ".######.", "..######", ".....###", "########", "#######.", "........", "........",
          "........"}}, /* s */
    {11, {"###......", "###......", "###......", "###.####.", "#########", "#########", "###...###",
          "###...###", "###...###", "#########", "#########", "###.####.", ".........", ".........",
          "........."}}, /* b */
    {10, {"........", "........", "........", "#######.", "########", ".....###", ".#######",
          "########", "###..###", "###..###", "########", ".###.###", "........", "........",
          "........"}}, /* a */
    {10, {"........", "........", "........", ".#######", "########", "########", "###.....",
          "###.....", "###.....", "########", "########", ".#######", "........", "........",
          "........"}}, /* c */
    {10, {"###.....", "###.....", "###.....", "###...##", "###..###", "###.###.", "######..",
          "#####...", "######..", "###.###.", "###..###", "###...##", "........", "........",
          "........"}}, /* k */
};
#define WORDMARK_GLYPHS (int)(sizeof(wordmark) / sizeof(wordmark[0]))
#define TRACKING 2      /* cells of tracking inside each advance */
#define CURSOR_GAP 3    /* cells between the last glyph and the cursor */
#define CURSOR_WIDTH 6  /* the cursor is 6 x 9 cells, rows 0-8 */
#define CURSOR_ROWS 9
#define WORDMARK_GAP 10 /* logo pixels between the brackets and the wordmark */
#define BREATH 0.04      /* the brackets' scale swing about the cube */
#define BREATH_PERIOD 3.0 /* seconds */

/* The cube: 26 pieces (the hidden centre is left out) of 6 faces. */
#define PIECES 26
#define MAX_QUADS (PIECES * 6)
static int piece_pos[PIECES][3]; /* lattice position, -1..1 */
static GLfloat cube_xyz[MAX_QUADS * 4][3];
static GLubyte cube_rgba[MAX_QUADS * 4][4];
static GLushort cube_index[MAX_QUADS * 6];

static GLuint bracket_list, wordmark_list, cursor_list;
static float scene_shift; /* moves the scene's vertical middle to the view's */
static int effect = 1;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* 3x3 matrices, row-major. */
static void rotation(int axis, float degrees, float r[9])
{
    float c = cosf(degrees * (float)M_PI / 180.0f), s = sinf(degrees * (float)M_PI / 180.0f);
    int b = (axis + 1) % 3, d = (axis + 2) % 3;
    memset(r, 0, 9 * sizeof(*r));
    r[axis * 3 + axis] = 1;
    r[b * 3 + b] = c;
    r[b * 3 + d] = -s;
    r[d * 3 + b] = s;
    r[d * 3 + d] = c;
}

static void multiply(const float a[9], const float b[9], float out[9])
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}

static void apply(const float r[9], const float v[3], float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = r[i * 3] * v[0] + r[i * 3 + 1] * v[1] + r[i * 3 + 2] * v[2];
}

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* COLOUR lit by the light from eye-space NORMAL: BASE plus the diffuse term,
 * clamped, as fixed-function lighting would give it. */
static void shade(const float colour[3], const float normal[3], float base, GLubyte out[4])
{
    float light = base + fmaxf(0.0f, dot(normal, light_dir));
    for (int k = 0; k < 3; k++)
        out[k] = (GLubyte)(255.0f * fminf(1.0f, colour[k] * light));
    out[3] = 255;
}

/* The looped neon colour at PHASE, in palette colours. */
static void neon(double phase, float out[3])
{
    double x = fmod(phase, NEON_COLOURS), f;
    int i;
    if (x < 0)
        x += NEON_COLOURS;
    i = (int)x;
    f = x - i;
    for (int k = 0; k < 3; k++)
        out[k] = neon_palette[i][k] +
                 (float)f * (neon_palette[(i + 1) % NEON_COLOURS][k] - neon_palette[i][k]);
}

/* ---- The static frame, built once ---- */

/* A pixel grid: glyph rows ("#" filled), or a bracket (MIRROR: "]"). */
struct grid {
    int width, height;
    const char *const *rows;
    int bracket, mirror;
};

static int filled(const struct grid *g, int x, int y)
{
    if (x < 0 || y < 0 || x >= g->width || y >= g->height)
        return 0;
    if (g->bracket)
        return (g->mirror ? x >= g->width - BRACKET_STROKE : x < BRACKET_STROKE) ||
               y < BRACKET_STROKE || y >= g->height - BRACKET_STROKE;
    return x < (int)strlen(g->rows[y]) && g->rows[y][x] == '#';
}

/* One face of a block, inside glBegin(GL_QUADS): its outward NORMAL (an axis,
 * sign in N), the rectangle X0..X1 x Y0..Y1 at depth range Z0..Z1, emitted only
 * when the eye (the origin, the scene at EYE) can see it, shaded in COLOUR. */
static void frame_face(int axis, int sign, float x0, float y0, float x1, float y1, float z0,
                       float z1, const float eye[3], const float colour[3])
{
    float n[3] = {0, 0, 0}, centre[3];
    GLubyte rgba[4];
    n[axis] = (float)sign;
    centre[0] = (axis == 0 ? (sign > 0 ? x1 : x0) : (x0 + x1) / 2) + eye[0];
    centre[1] = (axis == 1 ? (sign > 0 ? y1 : y0) : (y0 + y1) / 2) + eye[1];
    centre[2] = (axis == 2 ? z1 : (z0 + z1) / 2) + eye[2];
    if (dot(n, centre) >= 0)
        return; /* faces away from the eye */
    shade(colour, n, AMBIENT, rgba);
    glColor4ubv(rgba);
    if (axis == 2) {
        glVertex3f(x0, y0, z1), glVertex3f(x1, y0, z1), glVertex3f(x1, y1, z1), glVertex3f(x0, y1, z1);
    } else if (axis == 0 && sign < 0) {
        glVertex3f(x0, y0, z0), glVertex3f(x0, y0, z1), glVertex3f(x0, y1, z1), glVertex3f(x0, y1, z0);
    } else if (axis == 0) {
        glVertex3f(x1, y0, z1), glVertex3f(x1, y0, z0), glVertex3f(x1, y1, z0), glVertex3f(x1, y1, z1);
    } else if (sign > 0) {
        glVertex3f(x0, y1, z1), glVertex3f(x1, y1, z1), glVertex3f(x1, y1, z0), glVertex3f(x0, y1, z0);
    } else {
        glVertex3f(x0, y0, z0), glVertex3f(x1, y0, z0), glVertex3f(x1, y0, z1), glVertex3f(x0, y0, z1);
    }
}

/* A grid extruded to DEPTH, cell (0, 0) at LEFT, TOP: the front as greedily
 * merged rectangles (each run of a row extends down while the rows below are
 * filled under it) and, of the sides, only the outline, merged into runs and
 * kept where the eye sees it. Inside glBegin(GL_QUADS). */
static void grid_geometry(const struct grid *g, float left, float top, float cell, float depth,
                          const float eye[3], const float colour[3])
{
    unsigned char *used = calloc((size_t)g->width * g->height, 1);
    float z0 = -depth / 2, z1 = depth / 2;
    if (!used)
        return;
    for (int r = 0; r < g->height; r++)
        for (int c = 0; c < g->width; c++) {
            int c1 = c, r1 = r + 1;
            if (!filled(g, c, r) || used[r * g->width + c])
                continue;
            while (c1 < g->width && filled(g, c1, r) && !used[r * g->width + c1])
                c1++;
            for (; r1 < g->height; r1++) {
                int k = c;
                while (k < c1 && filled(g, k, r1) && !used[r1 * g->width + k])
                    k++;
                if (k < c1)
                    break;
            }
            for (int rr = r; rr < r1; rr++)
                memset(&used[rr * g->width + c], 1, (size_t)(c1 - c));
            frame_face(2, 1, left + c * cell, top - r1 * cell, left + c1 * cell, top - r * cell, z0,
                       z1, eye, colour);
            c = c1 - 1;
        }
    free(used);
    /* Horizontal outline edges: above row b, +1 is the bottom of a filled cell
     * above (facing down), -1 the top of a filled cell below (facing up). */
    for (int b = 0; b <= g->height; b++)
        for (int c = 0; c < g->width;) {
            int state = filled(g, c, b - 1) - filled(g, c, b), c1 = c + 1;
            float y = top - b * cell;
            while (c1 < g->width && filled(g, c1, b - 1) - filled(g, c1, b) == state)
                c1++;
            if (state)
                frame_face(1, -state, left + c * cell, y, left + c1 * cell, y, z0, z1, eye, colour);
            c = c1;
        }
    /* Vertical outline edges: left of column b, +1 is the right side of a filled
     * cell on the left, -1 the left side of a filled cell on the right. */
    for (int b = 0; b <= g->width; b++)
        for (int r = 0; r < g->height;) {
            int state = filled(g, b - 1, r) - filled(g, b, r), r1 = r + 1;
            float x = left + b * cell;
            while (r1 < g->height && filled(g, b - 1, r1) - filled(g, b, r1) == state)
                r1++;
            if (state)
                frame_face(0, state, x, top - r1 * cell, x, top - r * cell, z0, z1, eye, colour);
            r = r1;
        }
}

/* The frame: the brackets (a list of their own, as they breathe), the wordmark
 * spanning them above, and the cursor (its own list, as it blinks). */
static void frame_lists(void)
{
    float span = LOGO_X(bracket_at[1][0] + BRACKET_WIDTH) - LOGO_X(bracket_at[0][0]), cell, left,
          top, eye[3];
    int cells = CURSOR_GAP + CURSOR_WIDTH - TRACKING;
    for (int i = 0; i < WORDMARK_GLYPHS; i++)
        cells += wordmark[i].advance;
    cell = span / (float)cells;
    top = LOGO_Y(bracket_at[0][1]) + WORDMARK_GAP * LOGO_UNIT + GLYPH_ROWS * cell;
    scene_shift = -(top + LOGO_Y(bracket_at[0][1] + BRACKET_HEIGHT)) / 2;
    eye[0] = 0;
    eye[1] = scene_shift;
    eye[2] = -VIEW_DISTANCE;

    bracket_list = glGenLists(1);
    glNewList(bracket_list, GL_COMPILE);
    glBegin(GL_QUADS);
    for (int i = 0; i < 2; i++) {
        struct grid g = {BRACKET_WIDTH, BRACKET_HEIGHT, NULL, 1, i};
        grid_geometry(&g, LOGO_X(bracket_at[i][0]), LOGO_Y(bracket_at[i][1]), LOGO_UNIT,
                      BRACKET_STROKE * LOGO_UNIT, eye, foreground);
    }
    glEnd();
    glEndList();

    wordmark_list = glGenLists(1);
    glNewList(wordmark_list, GL_COMPILE);
    glBegin(GL_QUADS);
    left = LOGO_X(bracket_at[0][0]);
    for (int i = 0; i < WORDMARK_GLYPHS; i++) {
        struct grid g = {wordmark[i].advance - TRACKING, GLYPH_ROWS, wordmark[i].rows, 0, 0};
        grid_geometry(&g, left, top, cell, 3 * cell, eye, foreground);
        left += wordmark[i].advance * cell;
    }
    glEnd();
    glEndList();

    left += (CURSOR_GAP - TRACKING) * cell;
    cursor_list = glGenLists(1);
    glNewList(cursor_list, GL_COMPILE);
    glBegin(GL_QUADS);
    {
        static const char *const block[CURSOR_ROWS] = {"######", "######", "######",
                                                       "######", "######", "######",
                                                       "######", "######", "######"};
        struct grid g = {CURSOR_WIDTH, CURSOR_ROWS, block, 0, 0};
        grid_geometry(&g, left, top, cell, 3 * cell, eye, cursor_color);
    }
    glEnd();
    glEndList();
}

/* ---- The cube, built every frame ---- */

static double smooth(double u)
{
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return u * u * u * (10 + u * (-15 + 6 * u));
}

/* The effect at time T: how far the pieces stand apart (0..1), which layer
 * turns (axis, layer) and by how many degrees. */
static void effect_state(double t, double *open, int *axis, int *layer, double *turn)
{
    double u = fmod(t, CYCLE);
    unsigned cycle = (unsigned)(t / CYCLE) * 2654435761u;
    *axis = (int)(cycle >> 8) % 3;
    *layer = (int)(cycle >> 16) % 3 - 1;
    *open = smooth(u - 3.0) - smooth(u - 6.0);
    *turn = 90.0 * (smooth(u - 4.0) - smooth(u - 5.0));
}

/* The cube's visible faces at time T, in eye space: each piece placed (spacing,
 * the turning layer, the cube's own turn and the logo pose), a face kept only
 * when it faces the eye and, for an inner face, while the cube is open or
 * turning a layer; its neon or inner colour shaded. Returns the quad count. */
static int build_cube(double t)
{
    static const float corner[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    double open = 0, turn = 0;
    int axis = 0, layer = 0, quads = 0;
    float tilt[9], spin[9], pose[9], twist[9], twisted[9], eye[3] = {0, 0, -VIEW_DISTANCE};
    float spacing;
    if (effect)
        effect_state(t, &open, &axis, &layer, &turn);
    eye[1] = scene_shift;
    spacing = PIECE * (1.0f + GAP * (float)open);
    rotation(0, TILT, tilt);
    rotation(1, (float)fmod(t * DEGREES_PER_SECOND, 360.0) - 45.0f, spin);
    multiply(tilt, spin, pose);
    rotation(axis, (float)turn, twist);
    multiply(pose, twist, twisted);
    for (int i = 0; i < PIECES; i++) {
        const int *pos = piece_pos[i];
        const float *r = turn != 0.0 && pos[axis] == layer ? twisted : pose;
        float neon_colour[3];
        /* The piece's place along the diagonal the colours flow along, 0..1. */
        neon(NEON_SPEED * t + NEON_SPAN * (pos[0] + pos[1] + pos[2] + 3) / 6.0, neon_colour);
        for (int f = 0; f < 6; f++) {
            int a = f / 2, sign = f % 2 ? -1 : 1, b = (a + 1) % 3, c = (a + 2) % 3;
            int outer = pos[a] == sign;
            float dir[3] = {0, 0, 0}, normal[3], centre[3], model[3], p[3];
            GLubyte rgba[4];
            if (!outer && open < 0.001 && turn == 0.0)
                continue; /* an inner face of the closed cube */
            dir[a] = (float)sign;
            apply(r, dir, normal);
            for (int k = 0; k < 3; k++)
                model[k] = spacing * pos[k] + 0.5f * PIECE * dir[k];
            apply(r, model, centre);
            for (int k = 0; k < 3; k++)
                centre[k] += eye[k];
            if (dot(normal, centre) >= 0)
                continue; /* faces away from the eye */
            shade(outer ? neon_colour : inner_color, normal, outer ? AMBIENT + GLOW : AMBIENT, rgba);
            for (int v = 0; v < 4; v++) {
                const float *uv = corner[sign > 0 ? v : 3 - v];
                float local[3];
                local[a] = 0.5f * (float)sign;
                local[b] = uv[0];
                local[c] = uv[1];
                for (int k = 0; k < 3; k++)
                    model[k] = spacing * pos[k] + PIECE * local[k];
                apply(r, model, p);
                for (int k = 0; k < 3; k++)
                    cube_xyz[quads * 4 + v][k] = p[k] + eye[k];
                memcpy(cube_rgba[quads * 4 + v], rgba, 4);
            }
            quads++;
        }
    }
    return quads;
}

static void init(void)
{
    int n = 0;
    glClearColor(background[0], background[1], background[2], 1.0f);
    glShadeModel(GL_FLAT);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, cube_xyz);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, cube_rgba);
    for (int q = 0; q < MAX_QUADS; q++) {
        static const GLushort split[6] = {0, 1, 2, 0, 2, 3}; /* a quad as two triangles */
        for (int k = 0; k < 6; k++)
            cube_index[q * 6 + k] = (GLushort)(q * 4 + split[k]);
    }
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            for (int z = -1; z <= 1; z++)
                if (x || y || z) { /* the hidden centre is left out */
                    piece_pos[n][0] = x;
                    piece_pos[n][1] = y;
                    piece_pos[n][2] = z;
                    n++;
                }
    frame_lists();
}

/* The smaller window side sees +-1 at the near plane, so the scene always fits. */
static void reshape(int width, int height)
{
    GLdouble w = 1.0, h = 1.0;
    if (width >= height)
        w = (GLdouble)width / height;
    else
        h = (GLdouble)height / width;
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-w, w, -h, h, 5.0, 70.0);
    glMatrixMode(GL_MODELVIEW);
}

static void draw(double scene_time, double cursor_time)
{
    int quads = build_cube(scene_time);
    GLfloat breath = 1.0f + (GLfloat)(BREATH * sin(2.0 * M_PI * scene_time / BREATH_PERIOD));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, scene_shift, -VIEW_DISTANCE);
    glCallList(wordmark_list);
    if (fmod(cursor_time, 1.0) < 0.5)
        glCallList(cursor_list);
    glScalef(breath, breath, breath);
    glCallList(bracket_list);
    glLoadIdentity(); /* the cube is already in eye space */
    glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_SHORT, cube_index);
}

/* ---- metrics, timed runs, window ---- */

/* The system's busy and total CPU time from /proc/stat, in clock ticks. */
static int cpu_times(unsigned long long *busy, unsigned long long *total)
{
    unsigned long long v[8] = {0};
    FILE *f = fopen("/proc/stat", "r");
    int n = f ? fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                       &v[3], &v[4], &v[5], &v[6], &v[7])
              : 0;
    if (f)
        fclose(f);
    *total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
    *busy = *total - v[3] - v[4]; /* minus idle and iowait */
    return n >= 4;
}

/* Frame times of a timed run, in milliseconds, from one swap's return to the next. */
static float *frame_ms;
static size_t frame_count, frame_capacity;

static void record_frame(double ms)
{
    if (frame_count == frame_capacity) {
        size_t capacity = frame_capacity ? 2 * frame_capacity : 4096;
        float *grown = realloc(frame_ms, capacity * sizeof(*frame_ms));
        if (!grown)
            return;
        frame_ms = grown;
        frame_capacity = capacity;
    }
    frame_ms[frame_count++] = (float)ms;
}

static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static double process_seconds(void)
{
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (double)(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
           (double)(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

/* The timed run's summary, for people and as one JSON line for scripts. The
 * 99th percentile is the nearest rank. */
static void summary(double seconds, unsigned long frames, int cpu_system, int cpu_process,
                    int width, int height, int interval)
{
    double mean = 0, median = 0, p99 = 0, worst = 0, fps = seconds > 0 ? frames / seconds : 0;
    if (frame_count) {
        for (size_t i = 0; i < frame_count; i++)
            mean += frame_ms[i];
        mean /= (double)frame_count;
        qsort(frame_ms, frame_count, sizeof(*frame_ms), compare_float);
        median = frame_ms[frame_count / 2];
        p99 = frame_ms[(size_t)ceil(0.99 * (double)frame_count) - 1];
        worst = frame_ms[frame_count - 1];
    }
    printf("poulscube: %.1f s, %lu frames, %.2f FPS\n", seconds, frames, fps);
    printf("poulscube: frame time ms: mean %.2f, median %.2f, p99 %.2f, max %.2f\n", mean, median,
           p99, worst);
    printf("poulscube: CPU: system %d%%, process %d%%\n", cpu_system, cpu_process);
    printf("{\"poulscube_summary\": {\"seconds\": %.3f, \"frames\": %lu, \"fps\": %.3f, "
           "\"frame_ms\": {\"mean\": %.3f, \"median\": %.3f, \"p99\": %.3f, \"max\": %.3f}, "
           "\"cpu_system_percent\": %d, \"cpu_process_percent\": %d, \"window\": \"%dx%d\", "
           "\"swapinterval\": %d, \"effect\": %s}}\n",
           seconds, frames, fps, mean, median, p99, worst, cpu_system, cpu_process, width, height,
           interval, effect ? "true" : "false");
    fflush(stdout);
}

/* A window of WIDTH x HEIGHT at X, Y; FULLSCREEN asks a window manager for the whole screen. */
static Window make_window(Display *dpy, XVisualInfo *visual, int x, int y, int width, int height,
                          int fullscreen)
{
    Window root = RootWindow(dpy, visual->screen);
    XSetWindowAttributes attr = {0};
    XSizeHints hints = {0};
    Window win;
    attr.colormap = XCreateColormap(dpy, root, visual->visual, AllocNone);
    attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
    win = XCreateWindow(dpy, root, x, y, (unsigned)width, (unsigned)height, 0, visual->depth,
                        InputOutput, visual->visual, CWColormap | CWEventMask, &attr);
    hints.x = x;
    hints.y = y;
    hints.width = width;
    hints.height = height;
    hints.flags = USSize | USPosition;
    XSetNormalHints(dpy, win, &hints);
    XSetStandardProperties(dpy, win, "poulscube", "poulscube", None, NULL, 0, &hints);
    if (fullscreen) {
        Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&full, 1);
    }
    return win;
}

/* Each PPM is self-delimiting; the same writer serves stills and frame streams. */
static int write_ppm(FILE *f, int width, int height)
{
    unsigned char *rgb;
    size_t stride;
    GLenum error = glGetError();
    int failed = 0;
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "poulscube: GL error before readback: 0x%x\n", (unsigned)error);
        return 1;
    }
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / 3 ||
        (size_t)height > SIZE_MAX / ((size_t)width * 3)) {
        fprintf(stderr, "poulscube: invalid PPM dimensions\n");
        return 1;
    }
    stride = (size_t)width * 3;
    rgb = malloc(stride * (size_t)height);
    if (!rgb) {
        perror("poulscube: allocating readback");
        return 1;
    }
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "poulscube: GL readback error: 0x%x\n", (unsigned)error);
        free(rgb);
        return 1;
    }
    failed = fprintf(f, "P6\n%d %d\n255\n", width, height) < 0;
    for (int row = height - 1; row >= 0 && !failed; row--)
        failed = fwrite(rgb + (size_t)row * stride, 1, stride, f) != stride;
    if (failed)
        perror("poulscube: writing PPM frame");
    free(rgb);
    return failed;
}

static int dump(const char *prefix, int n, int width, int height)
{
    char path[512];
    FILE *f;
    int length = snprintf(path, sizeof(path), "%s-%d.ppm", prefix, n), failed;
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "poulscube: dump path is too long\n");
        return 1;
    }
    f = fopen(path, "wb");
    if (!f) {
        perror("poulscube: opening dump");
        return 1;
    }
    failed = write_ppm(f, width, height);
    if (fclose(f)) {
        perror("poulscube: closing dump");
        failed = 1;
    }
    return failed;
}

static int record(const char *path, int width, int height, int record_fps)
{
    FILE *output = NULL;
    char *line = NULL;
    size_t capacity = 0, frames = 0;
    double first = 0, last = 0;
    ssize_t length;
    int failed = 0;
    /* A closed FIFO reader must report an I/O error and reach shared cleanup. */
    signal(SIGPIPE, SIG_IGN);
    while ((length = getline(&line, &capacity, stdin)) >= 0) {
        char *p = line;
        if (memchr(line, '\0', (size_t)length)) {
            fprintf(stderr, "poulscube: embedded NUL in animation times\n");
            failed = 1;
            goto done;
        }
        while (*p) {
            char *end;
            double t;
            while (isspace((unsigned char)*p))
                p++;
            if (!*p)
                break;
            errno = 0;
            t = strtod(p, &end);
            if (p == end || errno == ERANGE || !isfinite(t) || t < 0 ||
                t > (double)UINT_MAX * CYCLE || (*end && !isspace((unsigned char)*end))) {
                fprintf(stderr, "poulscube: expected finite animation times in [0, %.17g]\n",
                        (double)UINT_MAX * CYCLE);
                failed = 1;
                goto done;
            }
            if (frames == SIZE_MAX) {
                fprintf(stderr, "poulscube: too many animation times\n");
                failed = 1;
                goto done;
            }
            if (!output) {
                output = fopen(path, "wb");
                if (!output) {
                    perror("poulscube: opening recording");
                    failed = 1;
                    goto done;
                }
            }
            draw(t, record_fps ? frames / (double)record_fps + 0.25 : t);
            if (write_ppm(output, width, height)) {
                failed = 1;
                goto done;
            }
            if (!frames)
                first = t;
            last = t;
            frames++;
            p = end;
        }
    }
    if (ferror(stdin) || !feof(stdin)) {
        perror("poulscube: reading animation times");
        failed = 1;
    } else if (!frames) {
        fprintf(stderr, "poulscube: empty animation time stream\n");
        failed = 1;
    }
done:
    free(line);
    if (output && fclose(output)) {
        perror("poulscube: closing recording");
        failed = 1;
    }
    if (!failed && fprintf(stderr,
                           "{\"poulscube_record\":{\"frames\":%zu,\"first_time\":%.17g,"
                           "\"last_time\":%.17g,\"cursor_fps\":%d}}\n",
                           frames, first, last, record_fps) < 0)
        failed = 1;
    return failed;
}

static void set_swap_interval(int interval)
{
    typedef int (*mesa_proc)(unsigned);
    typedef void (*ext_proc)(Display *, GLXDrawable, int);
    ext_proc ext = (ext_proc)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    mesa_proc mesa = (mesa_proc)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
    if (ext)
        ext(glXGetCurrentDisplay(), glXGetCurrentDrawable(), interval);
    else if (mesa)
        mesa((unsigned)interval);
    else
        fprintf(stderr, "poulscube: no swap interval extension\n");
}

int main(int argc, char **argv)
{
    int attrib[] = {GLX_RGBA, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1, GLX_BLUE_SIZE, 1,
                    GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None};
    static const double dump_at[] = {1.0, 3.5, 5.0, 6.5};
    const char *display_name = NULL, *dump_prefix = NULL, *record_path = NULL;
    int x = 0, y = 0, width = 300, height = 300, fullscreen = 0, info = 0, interval = -1;
    int dumped = 0, frames = 0, running = 1, failed = 0, duration_set = 0, record_fps = 0;
    unsigned long long busy0 = 0, total0 = 0, run_busy0 = 0, run_total0 = 0;
    unsigned long run_frames = 0;
    double duration = 0, previous_end = 0, process0 = 0;
    Display *dpy;
    XVisualInfo *visual;
    GLXContext ctx;
    Window win;
    Atom wm_delete;
    double start, last, process_last;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-display") && i + 1 < argc)
            display_name = argv[++i];
        else if (!strcmp(argv[i], "-geometry") && i + 1 < argc)
            XParseGeometry(argv[++i], &x, &y, (unsigned *)&width, (unsigned *)&height);
        else if (!strcmp(argv[i], "-fullscreen"))
            fullscreen = 1;
        else if (!strcmp(argv[i], "-info"))
            info = 1;
        else if (!strcmp(argv[i], "-swapinterval") && i + 1 < argc)
            interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-noeffect"))
            effect = 0;
        else if (!strcmp(argv[i], "-duration") && i + 1 < argc) {
            duration = atof(argv[++i]);
            duration_set = 1;
        } else if (!strcmp(argv[i], "-dump") && i + 1 < argc)
            dump_prefix = argv[++i];
        else if (!strcmp(argv[i], "-record") && i + 1 < argc)
            record_path = argv[++i];
        else if (!strcmp(argv[i], "-record-fps") && i + 1 < argc) {
            char *end;
            long value;
            errno = 0;
            value = strtol(argv[++i], &end, 10);
            if (errno == ERANGE || *end || value < 1 || value > 1000) {
                fprintf(stderr, "poulscube: -record-fps expects an integer in [1, 1000]\n");
                return 2;
            }
            record_fps = (int)value;
        } else {
            fprintf(stderr, "usage: poulscube [-display NAME] [-geometry WxH[+X+Y]] [-fullscreen] "
                            "[-info] [-swapinterval N] [-noeffect] [-duration SECONDS] "
                            "[-dump PREFIX] [-record FILE [-record-fps FPS]]\n");
            return 2;
        }
    }
    if (record_path && (duration_set || dump_prefix)) {
        fprintf(stderr, "poulscube: -record cannot be combined with -duration or -dump\n");
        return 2;
    }
    if (record_fps && !record_path) {
        fprintf(stderr, "poulscube: -record-fps requires -record\n");
        return 2;
    }
    dpy = XOpenDisplay(display_name);
    if (!dpy) {
        fprintf(stderr, "poulscube: cannot open display %s\n", XDisplayName(display_name));
        return 1;
    }
    if (fullscreen) {
        x = y = 0;
        width = DisplayWidth(dpy, DefaultScreen(dpy));
        height = DisplayHeight(dpy, DefaultScreen(dpy));
    }
    visual = glXChooseVisual(dpy, DefaultScreen(dpy), attrib);
    if (!visual) {
        fprintf(stderr, "poulscube: no RGB, double-buffered visual with depth\n");
        return 1;
    }
    win = make_window(dpy, visual, x, y, width, height, fullscreen);
    ctx = glXCreateContext(dpy, visual, NULL, True);
    if (!ctx) {
        fprintf(stderr, "poulscube: glXCreateContext failed\n");
        return 1;
    }
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);
    if (!glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "poulscube: glXMakeCurrent failed\n");
        failed = 1;
        goto cleanup;
    }
    if (info) {
        printf("GL_RENDERER   = %s\n", (const char *)glGetString(GL_RENDERER));
        printf("GL_VERSION    = %s\n", (const char *)glGetString(GL_VERSION));
        printf("GL_VENDOR     = %s\n", (const char *)glGetString(GL_VENDOR));
        printf("VisualID %d, 0x%x\n", (int)visual->visualid, (int)visual->visualid);
    }
    if (interval >= 0)
        set_swap_interval(interval);
    init();
    reshape(width, height);

    if (record_path) {
        failed = record(record_path, width, height, record_fps);
        goto cleanup;
    }

    start = last = now();
    cpu_times(&busy0, &total0);
    run_busy0 = busy0;
    run_total0 = total0;
    process0 = process_last = process_seconds();
    while (running) {
        double t;
        if (duration > 0 && now() - start >= duration)
            break;
        while (XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);
            if (event.type == ConfigureNotify) {
                width = event.xconfigure.width;
                height = event.xconfigure.height;
                reshape(width, height);
            } else if (event.type == KeyPress) {
                KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape || key == XK_q)
                    running = 0;
                else if (key == XK_e)
                    effect = !effect;
            } else if (event.type == ClientMessage &&
                       (Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }
        t = now();
        draw(t - start, t - start);
        if (dump_prefix && dumped < 4 && t - start >= dump_at[dumped] &&
            dump(dump_prefix, dumped++, width, height)) {
            failed = 1;
            break;
        }
        glXSwapBuffers(dpy, win);
        frames++;
        run_frames++;
        if (duration > 0) {
            double end = now();
            if (previous_end > 0)
                record_frame(1000 * (end - previous_end));
            previous_end = end;
        }
        if (t - last >= 5.0) {
            unsigned long long busy, total;
            double process = process_seconds();
            int cpu = 0;
            if (cpu_times(&busy, &total) && total > total0) {
                cpu = (int)(100 * (busy - busy0) / (total - total0));
                busy0 = busy;
                total0 = total;
            }
            printf("%d frames in %3.1f seconds = %6.3f FPS, frame %.2f ms, CPU system %d%%, "
                   "process %d%%\n",
                   frames, t - last, frames / (t - last), 1000 * (t - last) / frames, cpu,
                   (int)(100 * (process - process_last) / (t - last)));
            fflush(stdout);
            process_last = process;
            last = t;
            frames = 0;
        }
    }
    if (duration > 0) {
        unsigned long long busy, total;
        double seconds = now() - start;
        int cpu_system = 0;
        if (cpu_times(&busy, &total) && total > run_total0)
            cpu_system = (int)(100 * (busy - run_busy0) / (total - run_total0));
        summary(seconds, run_frames, cpu_system,
                (int)(100 * (process_seconds() - process0) / (seconds > 0 ? seconds : 1)), width,
                height, interval);
    }
cleanup:
    free(frame_ms);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return failed;
}
