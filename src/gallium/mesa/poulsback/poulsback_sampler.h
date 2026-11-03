// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SAMPLER_H
#define POULSBACK_SAMPLER_H
#include "compiler/shader_enums.h"
#include "pipe/p_state.h"
struct pipe_context;
struct pipe_sampler_state;
struct pipe_sampler_view;
struct pipe_resource;
struct poulsback_sampler_bank;

/* Serialized Gallium host descriptor/ownership layer; no sampling program,
 * native descriptors, capability or software texture renderer. Contexts must
 * outlive their caller-held views as required by Gallium. close freezes new
 * work and reports EBUSY while external view references remain. Destroy only
 * after close succeeds and the enclosing context's other owners can release.
 * Deleted states remain owned while bound; views retain their resource until
 * the final bound/external reference. That final drop reclaims the object and
 * its live-allocation budget, including a cross-context view destructor. */
int poulsback_sampler_bank_create(struct pipe_context *, struct poulsback_sampler_bank **);
int poulsback_sampler_bank_close(struct poulsback_sampler_bank *);
int poulsback_sampler_bank_destroy(struct poulsback_sampler_bank *);
int poulsback_sampler_state_create(struct poulsback_sampler_bank *,
                                   const struct pipe_sampler_state *, void **);
int poulsback_sampler_state_delete(struct poulsback_sampler_bank *, void *);
int poulsback_sampler_states_bind(struct poulsback_sampler_bank *, mesa_shader_stage,
                                  unsigned start, unsigned count, void **);
int poulsback_sampler_state_snapshot(struct poulsback_sampler_bank *, void *,
                                     struct pipe_sampler_state *);
/* One serialized binding lease, independent of mutable binding slots and of
 * caller view/state references. All descriptions are copied; only resource is
 * retained by an ordinary pipe_resource reference, released by the recipient.
 * This is an internal preparation edge, with no sampler/native admission.
 * A mutated public view descriptor never authenticates as its original view.
 * Output is disjoint from source/objects and unchanged on error. */
struct poulsback_sampler_binding {
    struct pipe_sampler_state sampler;
    uint32_t texture_slot, sampler_slot, target, format;
    uint32_t swizzle[4], first_level, last_level, first_layer, last_layer;
    struct pipe_resource *resource;
};
int poulsback_sampler_view_create(struct poulsback_sampler_bank *, struct pipe_resource *,
                                  const struct pipe_sampler_view *, struct pipe_sampler_view **);
int poulsback_sampler_views_bind(struct poulsback_sampler_bank *, mesa_shader_stage, unsigned start,
                                 unsigned count, unsigned trailing, struct pipe_sampler_view **);
/* Current context can differ from creator, per p_context.h. Canonical registry
 * lookup precedes dereferencing a handle; only a zero-ref view can be destroyed. */
int poulsback_sampler_view_destroy(struct pipe_context *, struct pipe_sampler_view *);
/* Every nonempty binding refuses native draw until physical sampling has an
 * independent contract. Empty state never adds shader/sampler capabilities.
 * Per draw: the calling context's bank, one counter read. A backend draw
 * samples its own units through their leases instead. */
int poulsback_sampler_draw_admit(struct poulsback_sampler_bank *);
/* The lease of fragment unit UNIT: its bound state and view, the resource
 * referenced for the recipient. -ENOENT: no state or no view bound there. */
int poulsback_sampler_fragment_binding(struct poulsback_sampler_bank *, unsigned unit,
                                       struct poulsback_sampler_binding *out);
#endif
