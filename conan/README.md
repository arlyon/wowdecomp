# Conan dependency management

This directory carries the [Conan 2](https://conan.io) recipes for wowdecomp's
two pinned third-party source dependencies:

| Recipe | Pins | Verified by |
| --- | --- | --- |
| `recipes/mojoshader` | `ad5dff84830c2863c841f4b1f4e3df78c705b383` | `cmake/MojoShader.cmake` |
| `recipes/slang` | `5cde9ea5ff3b6c657978de54d2241a86cb3528b1` | `cmake/SlangCompiler.cmake` |

It is an **alternative** to `make deps` / `make slang`; neither path is required
by the other and no `CMakeLists.txt` is touched.

## Usage

```sh
make conan          # create the recipes + write out/conan/deps.cmake
make conan-client   # configure + build the client against the Conan packages
```

`make conan` runs, roughly:

```sh
conan create conan/recipes/mojoshader
conan create conan/recipes/slang
conan install . -of out/conan
```

The last step writes `out/conan/deps.cmake`, which the `vulkan-client-conan`
CMake preset injects with `CMAKE_PROJECT_INCLUDE`. It sets the same three cache
variables the manual preset sets:

```cmake
set(WOWDECOMP_MOJOSHADER_SOURCE "…/mojoshader/…/source" CACHE PATH "" FORCE)
set(WOWDECOMP_SLANG_SOURCE "…/slang/…/source"        CACHE PATH "" FORCE)
set(WOWDECOMP_SLANGC "…/slang/…/bin/slangc"          CACHE FILEPATH "" FORCE)
```

## Why source directories, not a linked package

wowdecomp compiles the MojoShader frontend itself and reads Slang's public
headers, then loads a locally built `slangc` at build time. That makes these
dependencies *source directories + a binary*, not libraries to link with, so
the umbrella `conanfile.py` generates plain CMake variables instead of using
Conan's `CMakeDeps`.

Both checks in the project hard-fail unless `git rev-parse HEAD` inside the
dependency directory equals the pinned revision. The recipes therefore package
the **full checkout including `.git`** (Conan's `copy("*")` keeps dotfiles), and
the Slang package keeps its build artifacts in a `bin/` + `lib/` layout because
`slangc` uses a relative `RUNPATH` (`$ORIGIN/../lib`).

## Requirements

* Conan 2 (`uv tool install conan` installs it without root).
* A detected profile (`conan profile detect`). The recipes are source-only for
  MojoShader and build Slang with whatever compiler the profile selects.
