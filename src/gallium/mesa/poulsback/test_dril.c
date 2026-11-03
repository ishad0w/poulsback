// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Standard Mesa DRIL entrypoint check. Opens no DRM device or GL context. */
#include <GL/internal/dri_interface.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    void *library, *symbol;
    const __DRIextension **(*get_extensions)(void);
    const __DRIextension **extensions;
    int result = 1;

    if (argc != 2)
        return 1;
    library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "DRIL dlopen failed: %s\n", dlerror());
        return 1;
    }
    symbol = dlsym(library, "__driDriverGetExtensions_poulsback");
    /* POSIX defines dlsym function resolution; memcpy avoids an ISO C cast
     * warning. This target ABI has equal data/function pointer widths. */
    _Static_assert(sizeof(symbol) == sizeof(get_extensions), "Target function pointer ABI");
    memcpy(&get_extensions, &symbol, sizeof(get_extensions));
    if (get_extensions) {
        extensions = get_extensions();
        if (extensions && extensions[0] && extensions[0]->name) {
            puts(
                "PASS actual poulsback DRIL entrypoint returns standard extensions; no screen/context created");
            result = 0;
        }
    }
    if (dlclose(library) != 0)
        result = 1;
    return result;
}
