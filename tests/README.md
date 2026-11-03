# Tests

Start with [test selection and commands](../docs/tests.md). That guide owns
test selection and result scopes. Detailed procedures live with the
[kernel fixtures](kernel/README.md).

| Directory | Contents |
| --- | --- |
| `unit/` | C/C++ component fixtures, independent references and Mesa bridges |
| `project/` | Python regressions, integration runners and capture oracles |
| `kernel/` | Userspace kernel-contract fixtures, OS models and snapshot validators |
| `compiler/` | GLSL input programs for compiler and frontend checks |
| `vectors/` | Frozen 2D-encoder and MSVDX video vector corpora |

Mesa-internal fixtures also live beside the [Gallium overlay](../src/gallium/mesa/poulsback/).

For a first standalone CPU run, install the
[build dependencies](../docs/building.md#cpu-libraries), then run from the repository root:

```sh
make check-project check-project-tests check-tool-tests check-userspace
```

The full `make check` adds quality gates that need the external
pinned build image and SDKs; [Building](../docs/building.md#cpu-libraries) explains
that environment. Kernel behaviour is tested on the device
([kernel runtime](../docs/tests.md#kernel-runtime)); a host CPU pass is not a device result.
