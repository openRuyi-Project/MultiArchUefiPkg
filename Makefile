# SPDX-License-Identifier: LGPL-2.1-or-later
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.ONESHELL:
.DEFAULT_GOAL := all

PKG_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
EDK2_DIR ?= $(PKG_DIR)/../edk2
MAU_BUILD_DIR ?= $(PKG_DIR)/../build/multiarch
MAU_BUILD_DIR := $(abspath $(MAU_BUILD_DIR))
TARGET ?= RELEASE
MAU_DYNAMIC_POLICY ?= FALSE
JOBS ?= 8
CROSS_COMPILE ?= riscv64-linux-gnu-
PYTHON ?= python3

.PHONY: all
all:
	$(MAKE) -C "$(EDK2_DIR)/BaseTools/Source/C" -j$(JOBS)
	mkdir -p "$(MAU_BUILD_DIR)/Conf"
	export WORKSPACE="$(abspath $(PKG_DIR)/..)"
	export PACKAGES_PATH="$$WORKSPACE:$(abspath $(EDK2_DIR))"
	export CONF_PATH="$(MAU_BUILD_DIR)/Conf"
	export GCC_RISCV64_PREFIX="$(CROSS_COMPILE)" PYTHON_COMMAND="$(PYTHON)"
	unset EDK_TOOLS_PATH
	set +u
	source "$(EDK2_DIR)/edksetup.sh"
	set -u
	unset MAKEFLAGS MAKEOVERRIDES
	build -a RISCV64 -t GCC -p MultiArchUefiPkg/Emulator.dsc -b $(TARGET) -n $(JOBS) \
	  -D MAU_DYNAMIC_POLICY=$(MAU_DYNAMIC_POLICY) \
	  -D MAU_OUTPUT_DIRECTORY="$(MAU_BUILD_DIR)/Build/MultiArchUefiPkg" \
	  -D MAU_STANDALONE_LOGGING=SERIAL -D MAU_TRY_WITHOUT_MMU=NO \
	  -D MAU_WRAPPED_ENTRY_POINTS=NO -D MAU_SUPPORTS_X64_BINS=YES \
	  -D MAU_SUPPORTS_AARCH64_BINS=NO -D MAU_EMU_X64_RAZ_WI_PIO=NO
