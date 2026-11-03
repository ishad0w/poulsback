# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

PYTHON ?= python3
BUILD_DIR ?= build
# Prepared build-root inputs for the quality gates (docs/tests.md#quality-gates).
# Empty: $POULSBACK_WORKSPACE, else the directory above the driver's git checkout.
WORKSPACE ?=
QUALITY_ENV = $(if $(WORKSPACE),--workspace $(WORKSPACE)) --build-dir $(BUILD_DIR)
JOBS ?= 5

.PHONY: all check check-all check-ubsan check-project check-project-tests check-tool-tests check-userspace check-quality check-reach reconcile coverage lint format format-check help

all: check

lint:
	$(PYTHON) -I -B tools/style.py lint $(FILES)

format:
	$(PYTHON) -I -B tools/style.py format $(FILES)

format-check:
	$(PYTHON) -I -B tools/style.py format-check $(FILES)

# The parts are independent; they run side by side and print in turn. One check
# per build directory at a time: a second caller of the same BUILD_DIR waits for
# the first instead of failing on its locks. Independent callers can pass
# build directories of their own.
check:
	@mkdir -p $(BUILD_DIR) && flock $(BUILD_DIR)/.check.lock \
		$(MAKE) --no-print-directory -j$(JOBS) --output-sync=target check-all

check-all: check-project check-project-tests check-tool-tests check-userspace check-quality

check-userspace:
	$(PYTHON) -I -B tools/build-userspace.py --build-target host --output $(BUILD_DIR)/userspace

check-quality:
	$(PYTHON) -I -B tools/quality.py check $(QUALITY_ENV)

# Slow reachability gates use the prepared module, Mesa and DDX links.
check-reach:
	$(PYTHON) -I -B tools/quality.py reach $(QUALITY_ENV)

# Reconcile merged quality debt entries: deletes and lowers only.
reconcile:
	$(PYTHON) -I -B tools/quality.py reconcile $(QUALITY_ENV)

check-ubsan:
	$(PYTHON) -I -B tools/build-userspace.py --build-target host --output $(BUILD_DIR)/ubsan --flavor ubsan

check-project:
	$(PYTHON) -I -B tools/check-project.py check

check-project-tests:
	$(PYTHON) -I -B -m unittest discover -s tests/project -p 'test_*.py'

check-tool-tests:
	$(PYTHON) -I -B -m unittest discover -s tests/unit -p 'test_*.py'

# Host gcov plus device function records from DELL_COVERAGE (none ship with the
# source); the reach gates run in the same build.
coverage:
	$(PYTHON) -I -B tools/coverage.py check $(QUALITY_ENV) $(if $(DELL_COVERAGE),--dell $(DELL_COVERAGE))

help:
	@echo 'make lint / format / format-check: FILES="path ..." or staged sources; developer tools only'
	@echo 'make check: documentation, Python, strict/UBSAN host CPU suites and quality gates; needs the prepared build image and SDKs'
	@echo 'Standalone: make check-project check-project-tests check-tool-tests check-userspace'
	@echo 'make check-reach: prepared-build reachability and LTO gates; make coverage: those plus per-function coverage'
	@echo 'Kernel, Mesa and DDX build routes are described in docs/building.md'
