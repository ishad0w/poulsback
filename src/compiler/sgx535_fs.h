// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The fragment backend (docs/compiler.md#fragment-backend): structured NIR -> this scalar IR ->
 * USSE words through the one encoder (sgx535_use_encode.h).
 *
 * The IR is a list of scalar instructions. Each instruction is a value that
 * later instructions name by index. Per-pixel conditions are values (1.0 or
 * 0.0) used by SEL and KILL; the only branches are IF and loops on conditions
 * that are uniform per task: the backend emits no divergent flow
 * (docs/compiler.md#library-contracts). Values that cross a loop iteration or
 * an IF's merge go through VAR stores and loads. */
#ifndef POULSBACK_SGX535_FS_H
#define POULSBACK_SGX535_FS_H
#include "sgx535_use_encode.h"
#include <stdbool.h>

#define SGX535_FS_MAX_INSTRUCTIONS 2048u
#define SGX535_FS_MAX_WORDS 2048u  /* 1,024 instructions; PC reaches 4,096 */
#define SGX535_FS_MAX_VARYINGS 10u /* TC0-9 */
#define SGX535_FS_MAX_SAMPLERS 16u
#define SGX535_FS_MAX_VARS 64u
#define SGX535_FS_MAX_CONSTANTS 128u /* SA words, sa0 included */
#define SGX535_FS_MAX_ITERATIONS 24u
#define SGX535_FS_TEMPS 64u     /* the caps' temporaries */
#define SGX535_FS_MAX_DEPTH 32u /* nested IF and loops */

enum sgx535_fs_op {
    SGX535_FS_CONST = 0, /* imm: F32 bits */
    SGX535_FS_INPUT,     /* index: varying, comp: component */
    SGX535_FS_UNIFORM,   /* index: constant buffer dword; comp 1: an int, 2: a bool the upload
                          * converts */
    SGX535_FS_SYSTEM,    /* index: enum sgx535_fs_system */
    SGX535_FS_MOV,
    SGX535_FS_ADD,
    SGX535_FS_MUL,
    SGX535_FS_MAD, /* src0 * src1 + src2 */
    SGX535_FS_MIN = 8,
    SGX535_FS_MAX,
    SGX535_FS_RCP,
    SGX535_FS_RSQ,
    SGX535_FS_LOG2,
    SGX535_FS_EXP2,
    SGX535_FS_FRACT, /* x - floor(x) */
    SGX535_FS_DDX,
    SGX535_FS_DDY = 16,
    SGX535_FS_SLT, /* 1.0 when src0 < src1, else 0.0 */
    SGX535_FS_SGE,
    SGX535_FS_SEQ,
    SGX535_FS_SNE,
    SGX535_FS_SEL,       /* src0 != 0 ? src1 : src2 */
    SGX535_FS_TEX,       /* index: sampler; src: coordinates, then LOD, bias or gradients,
                          * then a shadow read's depth reference (imm 1) */
    SGX535_FS_TEXC,      /* comp of TEX src0 */
    SGX535_FS_KILL = 24, /* the pixel is discarded where src0 != 0 */
    SGX535_FS_OUTPUT,    /* index: enum sgx535_fs_output; src0 */
    SGX535_FS_STORE,     /* VAR index = src0 */
    SGX535_FS_LOAD,      /* the current value of VAR index */
    SGX535_FS_IF,        /* src0 != 0, uniform per task */
    SGX535_FS_ELSE,
    SGX535_FS_ENDIF,
    SGX535_FS_LOOP,
    SGX535_FS_BREAK = 32,
    SGX535_FS_CONTINUE,
    SGX535_FS_ENDLOOP,
    SGX535_FS_TEXSIZE, /* index: sampler, comp: 0 width, 1 height, 2 depth (texels, F32) */
};
enum sgx535_fs_system {
    SGX535_FS_FRAG_X, /* gl_FragCoord, window y down flipped to GL's y up */
    SGX535_FS_FRAG_Y,
    SGX535_FS_FRAG_Z,
    SGX535_FS_FRAG_W,
    SGX535_FS_FRONT_FACING, /* 1.0 or 0.0 */
};
enum sgx535_fs_output {
    SGX535_FS_COLOR_R,
    SGX535_FS_COLOR_G,
    SGX535_FS_COLOR_B,
    SGX535_FS_COLOR_A,
    SGX535_FS_DEPTH,
};
/* TEX forms (comp field of TEX). */
enum sgx535_fs_tex {
    SGX535_FS_TEX_IMPLICIT, /* LOD from the quad's derivatives */
    SGX535_FS_TEX_BIAS,     /* src[dim]: bias */
    SGX535_FS_TEX_LOD,      /* src[dim]: LOD */
};

struct sgx535_fs_instruction {
    uint8_t op;
    uint8_t comp;   /* INPUT, TEXC component; TEX form */
    uint8_t dim;    /* TEX: coordinates, 1-3 */
    uint8_t mod[4]; /* per source: SGX535_USE_NEG | SGX535_USE_ABS */
    uint16_t index;
    uint16_t src[4];
    uint32_t imm;
};

/* What the texture unit returns for a sampler, by the stored format
 * (docs/re/emgd-linux/fragment-path.md#textures): its channels, in order. */
enum sgx535_fs_result {
    SGX535_FS_RESULT_U8X4,  /* one dword; R, G, B, A in bytes 2, 1, 0, 3 (B8G8R8A8, 565, 4444,
                             * 5551, DXT) */
    SGX535_FS_RESULT_U8,    /* byte 0 (CHANREPLICATE): L8, A8, I8 */
    SGX535_FS_RESULT_U88,   /* bytes 0, 1: L8A8 */
    SGX535_FS_RESULT_U16,   /* the low half, 0-65535: depth16 */
    SGX535_FS_RESULT_F32,   /* one F32: depth24, stored as F32 */
    SGX535_FS_RESULT_F16X4, /* two F1616 planes, R G then B A, low half first: RGBA16F */
    SGX535_FS_RESULT_F32X4, /* four F32 planes: RGBA32F */
};
/* GL's R, G, B, A from a read's channels (the view's swizzle and the
 * format's: L8 R R R 1, A8 0 0 0 R, the depth modes); SAME keeps it. */
enum sgx535_fs_swizzle {
    SGX535_FS_SWIZZLE_SAME,
    SGX535_FS_SWIZZLE_R,
    SGX535_FS_SWIZZLE_G,
    SGX535_FS_SWIZZLE_B,
    SGX535_FS_SWIZZLE_A,
    SGX535_FS_SWIZZLE_ZERO,
    SGX535_FS_SWIZZLE_ONE,
};
/* A stride (NPOT) texture's REPEAT or MIRRORED_REPEAT, wrapped in USE before
 * the read; nearest filtering only (docs/native-3d.md#texture-memory). */
enum sgx535_fs_wrap { SGX535_FS_WRAP_HW, SGX535_FS_WRAP_REPEAT, SGX535_FS_WRAP_MIRRORED };
/* Per sampler, a variant's; zero is a U8X4 texture read as it is. */
struct sgx535_fs_sampler {
    uint8_t result;     /* enum sgx535_fs_result */
    uint8_t swizzle[4]; /* enum sgx535_fs_swizzle per GL component */
    uint8_t compare;    /* a shadow read's function + 1 (GL's NEVER 0 to ALWAYS 7); 0: the depth */
    uint8_t wrap[2];    /* enum sgx535_fs_wrap for s and t */
};
/* A varying as the pixel PDS iterates it. */
enum sgx535_fs_interp {
    SGX535_FS_SMOOTH,
    SGX535_FS_FLAT, /* the triangle's last vertex (FLATSHADE vertex 2) */
};
struct sgx535_fs_varying {
    uint8_t interp, components; /* components 1-4; 0: not read */
    /* 0: F32 through a TC issue; 10 or 11: four U8 in one PA dword through
     * the V0 or V1 issue (USECOLFLOAT clear), EMGD's colour form */
    uint8_t packed;
};
struct sgx535_fs_key {
    struct sgx535_fs_varying varying[SGX535_FS_MAX_VARYINGS];
    struct sgx535_fs_sampler sampler[SGX535_FS_MAX_SAMPLERS];
};

/* A SECONDARY word the draw supplies; sa0 is reserved (the memory-constant
 * base, docs/compiler.md#fragment-backend), so entry n is sa n. */
enum sgx535_fs_constant_kind {
    SGX535_FS_CONSTANT_NONE,
    SGX535_FS_CONSTANT_UNIFORM,     /* value: constant buffer dword */
    SGX535_FS_CONSTANT_UNIFORM_INT, /* value: constant buffer dword holding an int, as F32 */
    SGX535_FS_CONSTANT_TEXSIZE,     /* value: sampler << 2 | dimension, the size as F32 */
    SGX535_FS_CONSTANT_LITERAL,     /* value: the bits */
    SGX535_FS_CONSTANT_TEXTURE,     /* value: sampler << 4 | plane << 2 | word (DOUTT0-2) */
    SGX535_FS_CONSTANT_FRAG_Y, /* the target's height: y = height - y in the window, y in an FBO */
    SGX535_FS_CONSTANT_FRAG_YSIGN, /* -1.0 in the window, 1.0 in a bottom-up FBO */
    SGX535_FS_CONSTANT_FACING,     /* 0 in the window, 1 in a bottom-up FBO (g16 bit 0 sense) */
    SGX535_FS_CONSTANT_EPILOGUE,   /* value: words the epilogue owns */
    /* value: constant buffer dword holding a bool (Mesa's ~0 with native
     * integers; any nonzero is true), as F32 1.0 or 0.0: nir_lower_bool_to_float's
     * form */
    SGX535_FS_CONSTANT_UNIFORM_BOOL,
};
struct sgx535_fs_constant {
    uint32_t kind, value;
};

/* One pixel PDS issue: a varying iterated for the USE, or a non-dependent
 * texture read. DOUTI fields: docs/re/sgx535/interpolation-pack-contract.md
 * "Linux: compiled inputs reach the fragment PDS program". */
struct sgx535_fs_iteration {
    uint32_t douti;     /* without USELASTISSUE/TEXLASTISSUE; the PDS builder adds them */
    uint8_t sampler;    /* texture reads: the DOUTT */
    uint8_t texture;    /* 1: a texture read */
    uint8_t pa, dwords; /* where the result lands */
};

enum sgx535_fs_status {
    SGX535_FS_OK = 0,
    SGX535_FS_ERR_ARGUMENT = -1,
    SGX535_FS_ERR_LIMIT = -2,       /* instructions, temporaries, constants */
    SGX535_FS_ERR_UNSUPPORTED = -3, /* a form the backend does not express */
};

struct sgx535_fs_program {
    uint32_t words[SGX535_FS_MAX_WORDS];
    uint32_t instruction_count, temps, primary;
    uint32_t iteration_count, constant_count;
    struct sgx535_fs_iteration iteration[SGX535_FS_MAX_ITERATIONS];
    struct sgx535_fs_constant constant[SGX535_FS_MAX_CONSTANTS];
    /* What the epilogue reads: COLOR0's components and the depth as operands
     * (a temporary of the body, PA, SA or a constant), the kill flag's
     * temporary (1.0: discard) or 0xff. */
    struct sgx535_use_operand color[4], depth;
    uint8_t has_color, writes_depth, kill, uses_derivatives;
    uint8_t kill_pred; /* p1 holds the passing condition (EMGD's discard form) */
    /* COLOR0 is a packed varying unchanged: the PA dword with B, G, R, A */
    uint8_t color_packed, color_pa;
    /* Why a compile failed: the IR instruction and a short reason. */
    uint32_t failed_at;
    const char *reason;
};

/* The body: every IR instruction, no END. Sources name earlier instructions;
 * control flow must be balanced; IF and loop conditions must be uniform per
 * task (the caller's divergence analysis). The epilogue appends the rest. */
enum sgx535_fs_status sgx535_fs_compile(const struct sgx535_fs_instruction *, uint32_t count,
                                        const struct sgx535_fs_key *, struct sgx535_fs_program *);

/* Shared by the body and the epilogue. The SA word for KIND/VALUE in TABLE
 * (sa0 reserved; appended once), or -1 when full. */
int sgx535_fs_constant(struct sgx535_fs_constant *table, uint32_t *count, uint32_t kind,
                       uint32_t value);
/* N free temporaries in a row among the first LIMIT of BUSY, marked busy, the
 * high mark HIGH raised; the first, or -1. */
int sgx535_fs_temps(bool *busy, unsigned limit, uint32_t *high, unsigned n);
/* A DOUTI for a USE-side issue (USEISSUE 13 position, 14 ZABS) of COMPONENTS
 * dwords into PA, perspective, gouraud
 * (docs/re/sgx535/depth-contract.md#selecting-a-pre-d24-observation). */
struct sgx535_fs_iteration sgx535_fs_use_issue(uint32_t issue, uint32_t components, uint32_t pa);

/* Epilogue variants (docs/compiler.md#fragment-backend): one per (blend, colour mask,
 * logic op, alpha test, target format); discard and depth output come from
 * the body. The alpha reference and the ISP state the visibility test feeds
 * back are SA data per draw (sgx535_fs_feedback_words), not variants. */
enum sgx535_fs_target {
    SGX535_FS_TARGET_B8G8R8A8,
    SGX535_FS_TARGET_RGB8, /* B8G8R8A8 memory, alpha 255: destination alpha reads one */
};
enum sgx535_fs_factor {
    SGX535_FS_ZERO,
    SGX535_FS_ONE,
    SGX535_FS_SRC_COLOR,
    SGX535_FS_ONE_MINUS_SRC_COLOR,
    SGX535_FS_DST_COLOR,
    SGX535_FS_ONE_MINUS_DST_COLOR,
    SGX535_FS_SRC_ALPHA,
    SGX535_FS_ONE_MINUS_SRC_ALPHA,
    SGX535_FS_DST_ALPHA,
    SGX535_FS_ONE_MINUS_DST_ALPHA,
    SGX535_FS_CONSTANT_COLOR,
    SGX535_FS_ONE_MINUS_CONSTANT_COLOR,
    SGX535_FS_CONSTANT_ALPHA,
    SGX535_FS_ONE_MINUS_CONSTANT_ALPHA,
    SGX535_FS_SRC_ALPHA_SATURATE,
};
enum sgx535_fs_equation {
    SGX535_FS_EQ_ADD,
    SGX535_FS_EQ_SUBTRACT,
    SGX535_FS_EQ_REVERSE_SUBTRACT,
    SGX535_FS_EQ_MIN,
    SGX535_FS_EQ_MAX,
};
/* GL's orders: logic ops CLEAR (0) to SET (15), compare functions NEVER (0) to
 * ALWAYS (7). */
struct sgx535_fs_epilogue_key {
    uint8_t target;
    uint8_t blend, rgb_src, rgb_dst, alpha_src, alpha_dst, rgb_eq, alpha_eq;
    uint8_t colormask; /* bit 0 R, 1 G, 2 B, 3 A */
    uint8_t logic, logic_op;
    uint8_t alpha_test; /* the function and reference are feedback data */
};
/* The epilogue's SA words, after the body's (sgx535_fs_constant.value of
 * SGX535_FS_CONSTANT_EPILOGUE). */
enum sgx535_fs_epilogue_word {
    SGX535_FS_BLEND_COLOR, /* B8G8R8A8 */
    SGX535_FS_FEEDBACK0,   /* sgx535_fs_feedback_words */
    SGX535_FS_FEEDBACK1,
};
#define SGX535_FS_MAX_EPILOGUE 32u
struct sgx535_fs_variant {
    uint32_t words[SGX535_FS_MAX_WORDS + 2u * SGX535_FS_MAX_EPILOGUE];
    uint32_t instruction_count, temps, primary;
    uint32_t iteration_count, constant_count;
    struct sgx535_fs_iteration iteration[SGX535_FS_MAX_ITERATIONS + 1u];
    struct sgx535_fs_constant constant[SGX535_FS_MAX_CONSTANTS];
    /* ISPA 27:25: 0 opaque, 1 translucent, 2 translucent punch-through, 4 fast
     * punch-through, 5 depth feedback */
    uint8_t pass_type;
    uint8_t punchthrough; /* DOUTU1 PUNCHTHROUGH PHASE1 */
    /* Punch-through and depth feedback run in two phases
     * (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues): the
     * words up to the visibility test, then from instruction PHASE2 (even, so
     * 16-byte aligned) the colour write; DOUTU1 EXE1 names it. 0: one phase. */
    uint32_t phase2;
    uint8_t no_color; /* nothing reaches the target: no pixel task (EMGD's mask-none, ZERO/ONE) */
    const char *reason;
};
/* The body followed by the epilogue for KEY: the program's words, its pixel
 * PDS issues (ZABS added for punch-through) and SA layout. */
enum sgx535_fs_status sgx535_fs_link(const struct sgx535_fs_program *,
                                     const struct sgx535_fs_epilogue_key *,
                                     struct sgx535_fs_variant *);

/* VISTEST_STATE0/1, the ISP state atst8 and depthf hand back
 * (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues): stencil per face, depth function
 * and write, alpha
 * function and 8-bit reference. */
struct sgx535_fs_feedback {
    uint8_t depth_func, depth_write;
    uint8_t stencil_func, stencil_ref, stencil_mask, stencil_writemask;
    uint8_t stencil_fail, stencil_zfail, stencil_zpass; /* KEEP 0 ... DECR_WRAP 7 */
    uint8_t back_func, back_fail, back_zfail, back_zpass;
    uint8_t alpha_func, alpha_ref;
};
void sgx535_fs_feedback_words(const struct sgx535_fs_feedback *, uint32_t[2]);
#endif
