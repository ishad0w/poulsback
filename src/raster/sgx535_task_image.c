// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_image.h"
#include "sgx535_clip_w1.h"
#include "sgx535_engine121.h"
#include "sgx535_vertex_dma.h"
#include "sgx535_vertex_use.h"
#include "sgx535_limm_fields.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static void put_word(uint8_t *bytes, uint32_t word)
{
    unsigned int i;
    for (i = 0; i < 4; i++)
        bytes[i] = (uint8_t)(word >> (8u * i));
}

enum sgx535_task_status sgx535_task_image_build(const struct sgx535_task_request *request,
                                                struct sgx535_task_image *out)
{
    struct sgx535_task_image image = {0};
    struct sgx535_vertex_use_projection vertex;
    struct sgx535_vertex_dma_plan dma;
    struct sgx535_engine121_scene scene;
    uint32_t window[4], fragment[2];
    unsigned int i, j;
    if (!request || !out)
        return SGX535_TASK_INVALID;
    if (request->version != SGX535_TASK_IMAGE_VERSION ||
        request->profile != SGX535_TASK_W1_PACKED_CONSTANT ||
        request->core_id != SGX535_ENGINE121_CORE_ID ||
        request->revision != SGX535_ENGINE121_REVISION || request->reserved[0] ||
        request->reserved[1] || request->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    image.version = request->version;
    image.profile = request->profile;
    image.core_id = request->core_id;
    image.revision = request->revision;
    image.width = image.height = 64;
    image.stride_bytes = 256;
    image.packed_argb = request->packed_argb;
    image.missing_contracts = SGX535_TASK_MISSING_ALL;
    image.section_count = SGX535_TASK_IMAGE_SECTIONS;
    image.relocation_count = SGX535_TASK_IMAGE_RELOCS;
    image.sections[0] = (struct sgx535_task_section){SGX535_TASK_WINDOW_VERTEX_WORDS, 0, 48, 16};
    image.sections[1] = (struct sgx535_task_section){SGX535_TASK_VERTEX_USE_WORDS, 48, 16, 16};
    image.sections[2] = (struct sgx535_task_section){SGX535_TASK_FRAGMENT_USE_WORDS, 64, 8, 16};
    for (i = 0; i < 3; i++) {
        if (sgx535_clip_w1_encode(request->clip_position[i], 64, 64, window) != SGX535_CLIP_W1_OK)
            return SGX535_TASK_UNSUPPORTED;
        for (j = 0; j < 4; j++)
            put_word(image.bytes + 16u * i + 4u * j, window[j]);
    }
    if (sgx535_vertex_use_project(4, &vertex) != SGX535_VERTEX_USE_OK ||
        vertex.instruction_count != 2 || sgx535_vertex_dma_build(4, &dma) != SGX535_VERTEX_DMA_OK ||
        dma.transfer_count != 1 ||
        sgx535_engine121_scene_build(64, 64, &scene) != SGX535_ENGINE121_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 4; i++)
        put_word(image.bytes + 48u + 4u * i, vertex.words[i]);
    /* The packed constant remains DATA in a checked single LIMM OUTPUT0.
	 * Its END field and vertex terminal pair remain execution-unqualified. */
    sgx535_limm_compose(1, 0, request->packed_argb, 1, fragment);
    put_word(image.bytes + 64, fragment[0]);
    put_word(image.bytes + 68, fragment[1]);
    image.byte_count = 72;
    image.vertex_words = 4;
    image.vertex_stride_bytes = 16;
    image.dma_descriptor = dma.transfers[0].descriptor_word;
    image.dma_source_byte_offset = dma.transfers[0].source_byte_offset;
    image.dma_word_count = dma.transfers[0].word_count;
    for (i = 0; i < SGX535_TASK_IMAGE_ROLES; i++) {
        image.requirements[i] =
            (struct sgx535_task_requirement){i, SGX535_TASK_ACCESS_READ, 4096, 4096};
    }
    image.requirements[SGX535_TASK_TARGET].minimum_bytes = 64u * 256u;
    image.requirements[SGX535_TASK_TARGET].access = SGX535_TASK_ACCESS_WRITE;
    image.requirements[SGX535_TASK_SCENE].minimum_bytes = (scene.allocation_bytes + 4095u) & ~4095u;
    image.requirements[SGX535_TASK_SCENE].access |= SGX535_TASK_ACCESS_WRITE;
    for (i = SGX535_TASK_DPM_TABLES; i <= SGX535_TASK_DPM_PARAMETERS; i++) {
        image.requirements[i].minimum_bytes = 32u * 1024u * 1024u;
        image.requirements[i].access |= SGX535_TASK_ACCESS_WRITE;
    }
    image.requirements[SGX535_TASK_DPM_PARAMETERS].alignment = 1024u * 1024u;
    image.relocations[0] = (struct sgx535_task_reloc){
        SGX535_TASK_RELOC_COPY_SECTION, 0, SGX535_TASK_VERTEX_DATA, 0, 0, 48, 16, 0};
    image.relocations[1] = (struct sgx535_task_reloc){
        SGX535_TASK_RELOC_COPY_SECTION, 1, SGX535_TASK_USE_VERTEX, 0, 0, 16, 32, 0};
    image.relocations[2] = (struct sgx535_task_reloc){
        SGX535_TASK_RELOC_COPY_SECTION, 2, SGX535_TASK_USE_FRAGMENT, 0, 0, 8, 32, 0};
    *out = image;
    return SGX535_TASK_BLOCKED;
}

enum sgx535_task_status sgx535_task_image_validate(const struct sgx535_task_request *request,
                                                   const struct sgx535_task_image *image)
{
    struct sgx535_task_image expected;
    enum sgx535_task_status status;
    if (!image)
        return SGX535_TASK_INVALID;
    status = sgx535_task_image_build(request, &expected);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(&expected, image, sizeof(expected)) ? SGX535_TASK_INVALID : SGX535_TASK_BLOCKED;
}
