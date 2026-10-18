# wowdecomp build wrapper: CMake presets + Make glue.
#
# CMake is the build system. CMakePresets.json owns the configure/build flags for
# the client. This Makefile only orchestrates the things presets cannot express:
# the pinned third-party checkouts, the standalone Slang compiler, the two
# standalone test projects, and running the client.
#
#   make deps         clone + pin MojoShader and Slang (skips existing checkouts)
#   make slang        build the pinned slangc compiler (slow, one-time)
#   make client       configure + build out/vulkan-client/bin/Wow
#   make run          launch the client against $(GAME_DATA)
#   make tests        build + run the Vulkan shader/GPU tests
#   make portability  build + run the portability regression tests
#   make conan        resolve the pinned deps via Conan (alternative to deps/slang)
#   make conan-client configure + build the client against the Conan packages
#   make clean        remove generated build trees (keeps out/deps)
#   make distclean    also remove out/deps
#
# Everything is overridable, e.g.
#   make run GAME_DATA=/path/to/Data
#   make client CLIENT_PRESET=vulkan-client

SHELL := /bin/bash
.DEFAULT_GOAL := help

# --- configuration ----------------------------------------------------------
DEP_DIR         ?= out/deps
CONAN           ?= $(shell command -v conan 2>/dev/null || echo $(HOME)/.local/bin/conan)
CONAN_DIR       ?= out/conan
MOJOSHADER_REV  ?= ad5dff84830c2863c841f4b1f4e3df78c705b383
SLANG_REV       ?= 5cde9ea5ff3b6c657978de54d2241a86cb3528b1
SLANG_BUILD     ?= $(DEP_DIR)/slang-build
SLANGC          := $(abspath $(SLANG_BUILD))/Release/bin/slangc

JOBS            ?= $(shell nproc)
GAME_DATA       ?= /home/xj/Downloads/ChromieCraft_3.3.5a/Data
RUN_DIR         ?= out/runs/vulkan-next

# CLIENT_PRESET / CLIENT_DIR must stay in sync with CMakePresets.json.
CLIENT_PRESET   ?= vulkan-client
CLIENT_DIR      ?= out/vulkan-client
TESTS_DIR       ?= out/vulkan-tests
PORTABILITY_DIR ?= out/portability-tests

# Shared -D flags for the standalone test projects. The dependency paths are
# duplicated from CMakePresets.json because a preset file cannot point at a
# source directory other than its own.
DEP_FLAGS := \
	-DWOWDECOMP_MOJOSHADER_SOURCE=$(abspath $(DEP_DIR)/mojoshader) \
	-DWOWDECOMP_SLANG_SOURCE=$(abspath $(DEP_DIR)/slang) \
	-DWOWDECOMP_SLANGC=$(SLANGC)

SLANG_CMAKE_FLAGS := \
	-DCMAKE_BUILD_TYPE=Release -DSLANG_ENABLE_PREBUILT_BINARIES=OFF \
	-DSLANG_SLANG_LLVM_FLAVOR=DISABLE -DSLANG_ENABLE_SLANG_GLSLANG=ON \
	-DSLANG_ENABLE_GFX=OFF -DSLANG_ENABLE_SLANG_RHI=OFF \
	-DSLANG_ENABLE_EXAMPLES=OFF -DSLANG_ENABLE_TESTS=OFF -DSLANG_ENABLE_DXIL=OFF \
	-DSLANG_ENABLE_CUDA=OFF -DSLANG_ENABLE_OPTIX=OFF \
	-DSLANG_ENABLE_AFTERMATH=OFF -DSLANG_ENABLE_NVAPI=OFF \
	-DSLANG_ENABLE_SLANGD=OFF -DSLANG_ENABLE_SLANGRT=OFF

SLANG_SUBMODULES := external/lua external/miniz external/lz4 external/unordered_dense \
	external/spirv-tools external/spirv-headers external/glslang external/vulkan

.PHONY: help deps slang conan conan-client configure build client run tests portability clean distclean

# --- help -------------------------------------------------------------------
help: ## List targets
	@echo "wowdecomp targets:"
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | \
		awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

# --- pinned dependencies ----------------------------------------------------
deps: ## Clone + pin MojoShader and Slang (skips existing checkouts)
	@mkdir -p $(DEP_DIR)
	@if [ ! -d $(DEP_DIR)/mojoshader ]; then \
		git clone https://github.com/icculus/mojoshader.git $(DEP_DIR)/mojoshader && \
		git -C $(DEP_DIR)/mojoshader checkout $(MOJOSHADER_REV); \
	else echo "mojoshader: present"; fi
	@if [ ! -f $(DEP_DIR)/mojoshader/mojoshader_version.h ]; then \
		echo "generating mojoshader_version.h (upstream CMake writes it into the checkout)"; \
		cmake -S $(DEP_DIR)/mojoshader -B $(DEP_DIR)/mojoshader-build -G Ninja >/dev/null; \
	fi
	@if [ ! -d $(DEP_DIR)/slang ]; then \
		git clone https://github.com/shader-slang/slang.git $(DEP_DIR)/slang && \
		git -C $(DEP_DIR)/slang checkout $(SLANG_REV) && \
		git -C $(DEP_DIR)/slang submodule update --init --recursive -- $(SLANG_SUBMODULES); \
	else echo "slang: present"; fi
	@echo "mojoshader @ $$(git -C $(DEP_DIR)/mojoshader rev-parse --short HEAD)"
	@echo "slang      @ $$(git -C $(DEP_DIR)/slang rev-parse --short HEAD)"

slang: deps ## Build the pinned Slang compiler (slow, one-time)
	cmake -S $(DEP_DIR)/slang -B $(SLANG_BUILD) -G Ninja $(SLANG_CMAKE_FLAGS)
	cmake --build $(SLANG_BUILD) --target slangc slang-glslang --parallel $(JOBS)
	@test -x $(SLANGC) || { echo "ERROR: slangc not found at $(SLANGC)"; exit 1; }

# --- conan (alternative to deps/slang) --------------------------------------
conan: ## Resolve the pinned deps through Conan + write out/conan/deps.cmake
	$(CONAN) create conan/recipes/mojoshader
	$(CONAN) create conan/recipes/slang
	$(CONAN) install . -of $(CONAN_DIR)
	@echo "wrote $(CONAN_DIR)/deps.cmake"

conan-client: conan ## Configure + build the client with Conan-provided deps
	cmake --workflow --preset vulkan-client-conan

# --- client -----------------------------------------------------------------
configure: ## Configure the client preset only
	cmake --preset $(CLIENT_PRESET)

build: ## Build the client preset only (after configure)
	cmake --build --preset $(CLIENT_PRESET)

client: deps ## Configure + build the client via the workflow preset
	cmake --workflow --preset $(CLIENT_PRESET)

run: ## Run the built client with the Vulkan backend
	@test -x $(CLIENT_DIR)/bin/Wow || { echo "Build first: make client"; exit 1; }
	@mkdir -p $(RUN_DIR)
	@ln -sfn $(abspath $(GAME_DATA)) $(RUN_DIR)/Data
	cp $(CLIENT_DIR)/bin/Wow $(RUN_DIR)/Wow
	cd $(RUN_DIR) && WOWDECOMP_GX_BACKEND=vulkan XDG_CONFIG_HOME="$$PWD/cfg" ./Wow

# --- tests ------------------------------------------------------------------
tests: deps ## Build + run the standalone Vulkan shader/GPU tests
	cmake -S Tools/Vulkan -B $(TESTS_DIR) -G Ninja $(DEP_FLAGS)
	cmake --build $(TESTS_DIR) --parallel $(JOBS)
	ctest --test-dir $(TESTS_DIR) --output-on-failure

portability: ## Build + run the portability regression tests
	cmake -S Tools/Portability -B $(PORTABILITY_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(PORTABILITY_DIR) --parallel $(JOBS)
	ctest --test-dir $(PORTABILITY_DIR) --output-on-failure

# --- cleanup ----------------------------------------------------------------
clean: ## Remove generated build trees (keeps out/deps)
	rm -rf $(CLIENT_DIR) $(TESTS_DIR) $(PORTABILITY_DIR) out/runs

distclean: clean ## Also remove the pinned dependency checkouts
	rm -rf $(DEP_DIR)
