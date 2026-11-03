// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* A CPU model of USSE words for host tests: its own decoder follows
 * docs/re/sgx535/usse-instruction-contracts.md independently of src/compiler's
 * encoder, prints each instruction in rev121's syntax and runs programs on one 2×2
 * quad. A model, not the hardware: transcendentals are the host's, F32 as C
 * float, no pipeline timing. */
#ifndef POULSBACK_SGX535_USE_SIM_H
#define POULSBACK_SGX535_USE_SIM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One texel lookup: sampler, plane and coordinates in, the dword the
 * texture unit returns out (B8G8R8A8 for most formats). */
typedef uint32_t (*sgx535_sim_sample)(void *data, unsigned sampler, unsigned plane,
                                      const float coord[3], unsigned dim);

struct sgx535_sim_pixel {
    uint32_t r[128], o[128], pa[128]; /* bit patterns */
    bool p[4];
    uint32_t g16;   /* bit 0: back face */
    bool discarded; /* atst8's verdict */
    float depth;    /* depthf's input */
};
struct sgx535_sim {
    struct sgx535_sim_pixel pixel[4]; /* (0,0) (1,0) (0,1) (1,1), y down */
    uint32_t sa[128];
    sgx535_sim_sample sample;
    void *sample_data;
    /* atst8's alpha compare reads STATE1 31:24 and 15:13 from its src2. */
    unsigned steps, max_steps;
    char error[96];
};

/* rev121's text for one instruction; false for words the model does not know. */
bool sgx535_sim_text(uint32_t w0, uint32_t w1, char *text, size_t size);
/* Runs to the instruction with END; false and sim->error on a word the model
 * does not know, a branch whose predicate differs between pixels, or
 * max_steps. Registers are 32-bit; F32 values are bit patterns in floats. */
bool sgx535_sim_run(struct sgx535_sim *, const uint32_t *words, uint32_t instruction_count);
/* An SA word as F32. */
float sgx535_sim_f32(uint32_t bits);
uint32_t sgx535_sim_bits(float value);
#endif
