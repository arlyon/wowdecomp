# wowdecomp build wrapper: CMake presets + Make glue.
#
# CMake is the build system. CMakePresets.json owns the configure/build flags for
# the client. This Makefile only orchestrates the things presets cannot express:
# resolving the pinned third-party dependencies through Conan, the two standalone
# test projects, and running the client.
#
#   make deps         resolve MojoShader + Slang via Conan (builds only if needed)
#   make client       configure + build out/vulkan-client/bin/Wow
#   make run          launch the client against $(GAME_DATA)
#   make tests        build + run the Vulkan shader/GPU tests
#   make portability  build + run the portability regression tests
#   make clean        remove generated build trees (keeps out/conan)
#   make distclean    also remove out/conan and legacy out/deps
#
# The Conan recipes in conan/ pin the same revisions that cmake/MojoShader.cmake
# and cmake/SlangCompiler.cmake verify at configure time; see conan/README.md.
#
# Everything is overridable, e.g.
#   make run GAME_DATA=/path/to/Data
#   make deps CONAN=/path/to/conan

SHELL := /bin/bash
.DEFAULT_GOAL := help

# --- configuration ----------------------------------------------------------
CONAN           ?= $(shell command -v conan 2>/dev/null || echo $(HOME)/.local/bin/conan)
CONAN_DIR       ?= out/conan

JOBS            ?= $(shell nproc)
GAME_DATA       ?= /home/xj/Downloads/ChromieCraft_3.3.5a/Data
RUN_DIR         ?= out/runs/vulkan-next

# CLIENT_PRESET / CLIENT_DIR must stay in sync with CMakePresets.json.
CLIENT_PRESET   ?= vulkan-client
CLIENT_DIR      ?= out/vulkan-client
TESTS_DIR       ?= out/vulkan-tests
PORTABILITY_DIR ?= out/portability-tests

# The dependency cache variables that the Conan umbrella generates (conanfile.py).
# The client preset injects the file via CMAKE_PROJECT_INCLUDE; the standalone
# test projects are pointed at it explicitly below.
DEPS_CMAKE      := $(abspath $(CONAN_DIR)/deps.cmake)

.PHONY: help deps configure build client run tests portability clean distclean

# --- help -------------------------------------------------------------------
help: ## List targets
	@echo "wowdecomp targets:"
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | \
		awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

# --- dependencies (Conan) ---------------------------------------------------
deps: ## Resolve the pinned deps through Conan + write out/conan/deps.cmake
	@command -v $(CONAN) >/dev/null 2>&1 || \
		{ echo "ERROR: Conan 2 not found; install it or set CONAN= (see conan/README.md)"; exit 1; }
	$(CONAN) create conan/recipes/mojoshader --build=missing
	$(CONAN) create conan/recipes/slang --build=missing
	$(CONAN) install . -of $(CONAN_DIR)
	@echo "wrote $(CONAN_DIR)/deps.cmake"

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
	cmake -S Tools/Vulkan -B $(TESTS_DIR) -G Ninja -DCMAKE_PROJECT_INCLUDE=$(DEPS_CMAKE)
	cmake --build $(TESTS_DIR) --parallel $(JOBS)
	ctest --test-dir $(TESTS_DIR) --output-on-failure

portability: ## Build + run the portability regression tests
	cmake -S Tools/Portability -B $(PORTABILITY_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(PORTABILITY_DIR) --parallel $(JOBS)
	ctest --test-dir $(PORTABILITY_DIR) --output-on-failure

# --- cleanup ----------------------------------------------------------------
clean: ## Remove generated build trees (keeps out/conan)
	rm -rf $(CLIENT_DIR) $(TESTS_DIR) $(PORTABILITY_DIR) out/runs

distclean: clean ## Also remove out/conan and legacy out/deps
	rm -rf $(CONAN_DIR) out/deps
