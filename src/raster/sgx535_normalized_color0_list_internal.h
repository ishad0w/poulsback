// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_NORMALIZED_COLOR0_LIST_INTERNAL_H
#define SGX535_NORMALIZED_COLOR0_LIST_INTERNAL_H
/* Private canonical construction only. Both typed frontdoors supply a freshly
 * built, unpublished mask seed and checked copied window records. No accepted
 * packet or caller-controlled policy is consumed by this repacker. */
static enum sgx535_task_status
normalized_color0_list_repack(const struct sgx535_normalized_color0_task *seed_task,
                              uint32_t triangle_count, uint32_t *vertex_words,
                              struct sgx535_normalized_color0_mask_list_task *out)
{
    struct sgx535_task_packet *p = &out->task.packet;
    const struct sgx535_task_packet *seed = &seed_task->packet;
    static const uint32_t mask_indices[6] = {0, 1, 2, 3, 2, 1};
    uint32_t i, j, vertex_count = triangle_count * 3u;
    uint32_t changed = 0, found_vertices = 0, found_indices = 0, found_stream = 0;
    if (seed->section_count != 16 || seed->sections[0].kind != SGX535_PACKET_VERTICES ||
        seed->sections[0].byte_count != 160 || seed->sections[1].kind != SGX535_PACKET_INDICES ||
        seed->sections[1].byte_count != 20)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 16; i++) {
        const uint8_t *v = seed->bytes + seed->sections[0].byte_offset + 96u + i * 4u;
        vertex_words[vertex_count * 8u + i] =
            (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
    }
    memset(out, 0, sizeof(*out));
    out->task = *seed_task;
    out->triangle_count = triangle_count;
    out->vertex_count = vertex_count;
    out->foreground_vertex_bytes = vertex_count * 32u;
    out->mask_vertex_offset_bytes = out->foreground_vertex_bytes;
    out->vertex_storage_bytes = out->foreground_vertex_bytes + 64u;
    /* Driver placement, retaining the one-triangle160B index start; only the
     * independently derived U16 alignment is a wire requirement. */
    out->index_offset_bytes = out->vertex_storage_bytes;
    out->foreground_index_offset_bytes = out->index_offset_bytes + 12u;
    out->foreground_index_bytes = vertex_count * 2u;
    out->index_storage_bytes = ((vertex_count + 6u + 1u) / 2u) * 4u;
    if (out->index_offset_bytes + out->index_storage_bytes > 4096u)
        return SGX535_TASK_UNSUPPORTED;
    p->byte_count = 0;
    memset(p->bytes, 0, sizeof(p->bytes));
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *old = &seed->sections[i];
        struct sgx535_task_packet_section *s = &p->sections[i];
        uint32_t start = (p->byte_count + 15u) & ~15u;
        uint32_t bytes = old->kind == SGX535_PACKET_VERTICES  ? out->vertex_storage_bytes
                         : old->kind == SGX535_PACKET_INDICES ? out->index_storage_bytes
                                                              : old->byte_count;
        if (start > SGX535_TASK_PACKET_BYTES || bytes > SGX535_TASK_PACKET_BYTES - start)
            return SGX535_TASK_UNSUPPORTED;
        s->byte_offset = start;
        s->byte_count = bytes;
        if (old->kind == SGX535_PACKET_VERTICES) {
            if (s->role != SGX535_TASK_VERTEX_DATA || s->resource_byte_offset || s->alignment != 16)
                return SGX535_TASK_UNSUPPORTED;
            for (j = 0; j < bytes / 4u; j++)
                put(p->bytes + start + j * 4u, vertex_words[j]);
            found_vertices++;
        } else if (old->kind == SGX535_PACKET_INDICES) {
            if (s->role != SGX535_TASK_VERTEX_DATA || s->alignment != 2)
                return SGX535_TASK_UNSUPPORTED;
            s->resource_byte_offset = out->index_offset_bytes;
            for (j = 0; j < 6; j++)
                put_index(p->bytes + start + j * 2u, mask_indices[j]);
            for (j = 0; j < vertex_count; j++)
                put_index(p->bytes + start + 12u + j * 2u, j);
            found_indices++;
        } else {
            memcpy(p->bytes + start, seed->bytes + old->byte_offset, bytes);
            if (old->kind == SGX535_PACKET_TA_STREAM) {
                if (bytes != 68 || i != p->ta_stream_section)
                    return SGX535_TASK_UNSUPPORTED;
                put(p->bytes + start + 36u, 0x81400000u | vertex_count);
                found_stream++;
            }
        }
        p->byte_count = start + bytes;
    }
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *rel = &p->relocations[i];
        uint32_t kind = p->sections[rel->section].kind;
        if (rel->role != SGX535_TASK_VERTEX_DATA)
            continue;
        if (kind == SGX535_PACKET_VERTEX_PDS && rel->kind == SGX535_PACKET_DRI_OFFSET &&
            !rel->section_byte_offset && !rel->resource_byte_offset && rel->span_bytes == 96) {
            rel->span_bytes = out->foreground_vertex_bytes;
            changed++;
        } else if (kind == SGX535_PACKET_VERTEX_PDS && rel->kind == SGX535_PACKET_DRI_OFFSET &&
                   rel->section_byte_offset == 64 && rel->resource_byte_offset == 96 &&
                   rel->span_bytes == 64) {
            rel->resource_byte_offset = out->mask_vertex_offset_bytes;
            changed++;
        } else if (kind == SGX535_PACKET_TA_STREAM && rel->kind == SGX535_PACKET_DRI_OFFSET &&
                   rel->section_byte_offset == 12 && rel->resource_byte_offset == 160 &&
                   rel->span_bytes == 12) {
            rel->resource_byte_offset = out->index_offset_bytes;
            changed++;
        } else if (kind == SGX535_PACKET_TA_STREAM && rel->kind == SGX535_PACKET_DRI_OFFSET &&
                   rel->section_byte_offset == 40 && rel->resource_byte_offset == 172 &&
                   rel->span_bytes == 6) {
            rel->resource_byte_offset = out->foreground_index_offset_bytes;
            rel->span_bytes = out->foreground_index_bytes;
            changed++;
        } else
            return SGX535_TASK_UNSUPPORTED;
    }
    if (changed != 4 || found_vertices != 1 || found_indices != 1 || found_stream != 1)
        return SGX535_TASK_UNSUPPORTED;
    return SGX535_TASK_BLOCKED;
}
#endif
