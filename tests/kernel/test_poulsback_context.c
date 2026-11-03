// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* A file's execution contexts (kernel/engine/poulsback_context.c) on CPU
 * stand-ins (context-mocks/, vm-mocks/): a submission holds the context it
 * looked up, so CONTEXT_DESTROY (or the file's close) during the submission
 * leaves its entities and push lock usable until the submission drops it, and
 * the last holder destroys both entities and the scene reference. Failed
 * creates unwind. CPU evidence only; arm/push ordering under the lock is not
 * modelled here. */
#include "../../kernel/engine/poulsback_context.c"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "context check %u line %d: %s\n", checks, __LINE__, #x);               \
        }                                                                                          \
    } while (0)

static struct drm_device dev;
static struct drm_file file;
static struct poulsback_exec_file *exec;
static struct drm_gpu_scheduler sched3d, sched2d;
static unsigned scenes_live;

struct poulsback_exec_file *poulsback_file_exec(struct drm_file *f)
{
    (void)f;
    return exec;
}
struct poulsback_vm *poulsback_file_vm(struct drm_file *f)
{
    (void)f;
    return (struct poulsback_vm *)&dev; /* a token: the scene stand-in ignores it */
}
struct poulsback_engine *poulsback_engine_get(struct drm_device *d)
{
    (void)d;
    return (struct poulsback_engine *)&sched3d;
}
struct drm_gpu_scheduler *poulsback_engine_scheduler(struct poulsback_engine *engine, bool twod)
{
    (void)engine;
    return twod ? &sched2d : &sched3d;
}
void poulsback_engine_link_vm(struct poulsback_engine *engine, struct poulsback_vm *vm)
{
    (void)engine, (void)vm;
}
struct poulsback_scene *poulsback_scene_create(struct poulsback_vm *vm, u32 handle)
{
    struct poulsback_scene *scene = kzalloc(sizeof(*scene), GFP_KERNEL);
    (void)vm, (void)handle;
    if (scene) {
        kref_init(&scene->ref);
        scenes_live++;
    }
    return scene;
}
static void scene_free(struct kref *ref)
{
    kfree(container_of(ref, struct poulsback_scene, ref));
    scenes_live--;
}
void poulsback_scene_put(struct poulsback_scene *scene)
{
    if (scene)
        kref_put(&scene->ref, scene_free);
}

static int create(u32 *handle)
{
    struct poulsback_context_create args = {POULSBACK_EXEC_ABI, 0, 0, {0}};
    int ret = poulsback_context_create_ioctl(&dev, &args, &file);
    *handle = args.handle;
    return ret;
}
static int destroy(u32 handle)
{
    struct poulsback_context_destroy args = {POULSBACK_EXEC_ABI, handle, {0}};
    return poulsback_context_destroy_ioctl(&dev, &args, &file);
}

int main(void)
{
    struct poulsback_exec_ctx *ctx, *ctx2;
    struct drm_sched_entity *entity, *entity2;
    struct poulsback_scene *scene;
    u32 h, i;

    exec = poulsback_exec_file_create();
    CHECK(exec != NULL);

    /* A destroy between a 3D submission's lookup and its push: the context,
     * its entities and its lock stay until the submission drops its hold. */
    CHECK(!create(&h) && h == 1 && mock_entities_live == 2);
    CHECK(!poulsback_exec_context(&file, h, &ctx, &entity, &scene));
    CHECK(entity == &ctx->entity && scene && scenes_live == 1);
    CHECK(!destroy(h));
    CHECK(mock_entities_live == 2 && entity->live);
    CHECK(poulsback_exec_context(&file, h, &ctx2, &entity2, NULL) == -ENOENT);
    CHECK(destroy(h) == -ENOENT);
    poulsback_exec_ctx_lock(ctx);
    poulsback_exec_ctx_unlock(ctx);
    poulsback_exec_ctx_put(ctx);
    CHECK(mock_entities_live == 0 && scenes_live == 1); /* the job's scene reference */
    poulsback_scene_put(scene);
    CHECK(scenes_live == 0);

    /* Without a holder, CONTEXT_DESTROY destroys at once. */
    CHECK(!create(&h) && mock_entities_live == 2);
    CHECK(!destroy(h) && mock_entities_live == 0);

    /* A 3D and a 2D submission hold it: the last of them destroys it. */
    CHECK(!create(&h));
    CHECK(!poulsback_exec_context(&file, h, &ctx, &entity, &scene));
    CHECK(!poulsback_exec_context(&file, h, &ctx2, &entity2, NULL));
    CHECK(ctx == ctx2 && entity2 == &ctx->entity2d && entity != entity2);
    CHECK(!destroy(h));
    poulsback_exec_ctx_put(ctx2);
    CHECK(mock_entities_live == 2);
    poulsback_exec_ctx_put(ctx);
    CHECK(mock_entities_live == 0);
    poulsback_scene_put(scene);
    CHECK(scenes_live == 0);

    /* Entity initialisation failures unwind; the table's limit too. */
    mock_entity_fail_in = 1;
    CHECK(create(&h) == -ENOMEM && mock_entities_live == 0);
    mock_entity_fail_in = 2;
    CHECK(create(&h) == -ENOMEM && mock_entities_live == 0);
    for (i = 0; i < FILE_CONTEXTS; i++)
        CHECK(!create(&h));
    CHECK(create(&h) == -ENOSPC && mock_entities_live == 2 * FILE_CONTEXTS);
    for (i = 1; i <= FILE_CONTEXTS; i++)
        CHECK(!destroy(i));
    CHECK(mock_entities_live == 0);

    /* The file closes while a submission holds a context: the table's
     * references go, the held context stays until the submission drops it. */
    CHECK(!create(&h) && !create(&h));
    CHECK(!poulsback_exec_context(&file, h, &ctx, &entity, NULL));
    poulsback_exec_file_destroy(exec);
    exec = NULL;
    CHECK(mock_entities_live == 2 && entity->live);
    poulsback_exec_ctx_put(ctx);
    CHECK(mock_entities_live == 0);

    printf("context: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
