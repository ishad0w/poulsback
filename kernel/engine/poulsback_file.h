/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* Per-file state of an open DRM file: its SGX address space and its execution
 * contexts (docs/kernel/execution.md). */
#ifndef POULSBACK_FILE_H
#define POULSBACK_FILE_H

struct drm_device;
struct drm_file;
struct poulsback_exec_file;
struct poulsback_vm;

/* drm_driver .open and .postclose. Close destroys the execution contexts
 * (their entities flush queued jobs) before dropping the address space; jobs
 * hold their own VM references. */
int poulsback_file_open(struct drm_device *dev, struct drm_file *file);
void poulsback_file_close(struct drm_device *dev, struct drm_file *file);
/* The file's SGX address space; it lives as long as the file. */
struct poulsback_vm *poulsback_file_vm(struct drm_file *file);
/* The file's execution contexts; destroyed at close. */
struct poulsback_exec_file *poulsback_file_exec(struct drm_file *file);
#endif
