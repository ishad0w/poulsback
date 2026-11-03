// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_BACKGROUND_LAUNCH_H
#define SGX535_BACKGROUND_LAUNCH_H
#include "sgx535_background_object.h"
#include "sgx535_task_launch.h"

struct sgx535_background_launch_workspace {
    struct sgx535_background_pair_workspace pair;
    struct sgx535_background_bound background;
    struct sgx535_task_launch_workspace launch;
};
/* New fixed16 companion path only. Regenerates both complete components and
 * all relocations before constructing the raster prefix. It never edits an
 * already validated launch, changes old packet bytes or grants publication.
 * The kernel must supply actual same-job owner/bank identities; numeric
 * validation alone does not authenticate PFNs, CAC or a physical source.
 * Every output/workspace range must be disjoint from all inputs. */
enum sgx535_task_status sgx535_background_launch_build(
    const struct sgx535_task_pair_request *, const struct sgx535_background_request *,
    const struct sgx535_background_pair_image *, const struct sgx535_task_launch_binding *,
    const struct sgx535_background_binding *, const struct sgx535_background_bound *,
    struct sgx535_task_launch *, struct sgx535_background_launch_workspace *);
enum sgx535_task_status sgx535_background_launch_validate(
    const struct sgx535_task_pair_request *, const struct sgx535_background_request *,
    const struct sgx535_background_pair_image *, const struct sgx535_task_launch_binding *,
    const struct sgx535_background_binding *, const struct sgx535_background_bound *,
    const struct sgx535_task_launch *, struct sgx535_background_launch_workspace *);
enum sgx535_task_status sgx535_background_second_launch_build(
    const struct sgx535_task_pair_request *, const struct sgx535_background_request *,
    const struct sgx535_background_pair_image *, const struct sgx535_task_launch_binding *,
    const struct sgx535_background_binding *, const struct sgx535_background_bound *,
    struct sgx535_task_launch *, struct sgx535_background_launch_workspace *);
enum sgx535_task_status sgx535_background_second_launch_validate(
    const struct sgx535_task_pair_request *, const struct sgx535_background_request *,
    const struct sgx535_background_pair_image *, const struct sgx535_task_launch_binding *,
    const struct sgx535_background_binding *, const struct sgx535_background_bound *,
    const struct sgx535_task_launch *, struct sgx535_background_launch_workspace *);

#endif
