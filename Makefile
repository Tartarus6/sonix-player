# Unified firmware build for the three Sonix Player devices.
#
#   make r1        -> build/firmware/r1/r1.upt
#   make r3proii   -> build/firmware/r3proii/r3proii.upt
#   make v1        -> build/firmware/v1/v1.upt
#   make all       -> all three, with the application built only once
#   make validate  -> re-run the mandatory validator on whatever was built
#
# One application binary is built by delegating to sonix-player's `target`
# target. Each device is then packed in its own staging tree with only its own
# verified stock input and assets, so images cannot cross-contaminate.
#
# SAFETY: tools/validate_upt.py checks package structure and device identity.
# A structural PASS is NOT flashability approval. No target here flashes,
# publishes or releases anything.

SHELL := /bin/bash

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PLAYER_DIR := $(ROOT)/sonix-player
PACKER_DIR := $(ROOT)/sonix-packer
V1_PACKER_DIR := $(PACKER_DIR)/v1
PREP_STOCK := $(ROOT)/tools/prepare_stock_inputs.sh
VALIDATOR := $(ROOT)/tools/validate_upt.py
BUILD_ROOT := $(ROOT)/build/firmware

DEVICES := r1 r3proii v1

.PHONY: all app r1 r3proii v1 validate clean help

all: r1 r3proii v1

# Delegate to the application Makefile. It is owned by another worker; call
# it, do not edit it. Make runs this only once even for `make all`.
app:
	$(MAKE) -C $(PLAYER_DIR) target
	@test -f $(PLAYER_DIR)/sonix_player || { echo "expected $(PLAYER_DIR)/sonix_player after make target" >&2; exit 1; }

# $(1) device key, $(2) full path to the device asset directory,
# $(3) output image name, $(4) full path to the device packer script.
define BUILD_DEVICE
	@set -euo pipefail; \
	dev="$(1)"; \
	asset_path="$(2)"; \
	image="$(3)"; \
	packer="$(4)"; \
	stage="$(BUILD_ROOT)/$$dev"; \
	asset_name="$$(basename "$$asset_path")"; \
	\
	echo "==> staging $$dev in $$stage"; \
	rm -rf "$$stage"; \
	mkdir -p "$$stage/assets"; \
	install -m 0755 "$(PLAYER_DIR)/sonix_player" "$$stage/sonix_player"; \
	if [ -f "$(PLAYER_DIR)/sonix_launch" ]; then \
		install -m 0755 "$(PLAYER_DIR)/sonix_launch" "$$stage/sonix_launch"; \
	fi; \
	install -m 0755 "$$packer" "$$stage/sonix_firmware_packer.sh"; \
	if [ "$$dev" = "v1" ]; then \
		install -m 0644 "$(V1_PACKER_DIR)/extract_base_build.py" "$$stage/extract_base_build.py"; \
		install -m 0755 "$(V1_PACKER_DIR)/verify_upt.sh" "$$stage/verify_upt.sh"; \
		cp -a "$(V1_PACKER_DIR)/tools" "$$stage/tools"; \
	fi; \
	ln -s "$$asset_path" "$$stage/assets/$$asset_name"; \
	if [ "$$dev" != "v1" ]; then \
		ln -s "$(PACKER_DIR)/kernel" "$$stage/kernel"; \
	fi; \
	\
	"$(PREP_STOCK)" --device "$$dev" --dest "$$stage"; \
	\
	echo "==> packing $$dev"; \
	( cd "$$stage" && ./sonix_firmware_packer.sh ) 2>&1 | tee "$$stage/packer.log"; \
	[ -f "$$stage/$$image" ] || { echo "packer produced no $$stage/$$image" >&2; exit 1; }; \
	\
	echo "==> validating $$dev (mandatory gate)"; \
	python3 "$(VALIDATOR)" "$$stage/$$image" --expect-device "$$dev" \
		--report "$$stage/validation.json" > "$$stage/validator.log" 2>&1 || { \
		cat "$$stage/validator.log" >&2; \
		echo "$$dev: validation FAILED; image rejected" >&2; \
		exit 1; \
	}; \
	result="$$(REPORT="$$stage/validation.json" python3 -c 'import json,os; print(json.load(open(os.environ["REPORT"]))["result"])')"; \
	\
	echo "---------------- $$dev build report ----------------"; \
	echo "device:     $$dev"; \
	echo "image:      $$stage/$$image"; \
	echo "bytes:      $$(stat -c%s "$$stage/$$image")"; \
	echo "sha256:     $$(sha256sum "$$stage/$$image" | awk '{print $$1}')"; \
	echo "validation: $$result"; \
	echo "safety:     structural result only; NOT flashability approval"; \
	echo "------------------------------------------------------"; \
	cat "$$stage/validator.log"; \
	echo "SAFETY: a structural PASS is not a claim that any image is safe to flash."
endef

r1: app
	$(call BUILD_DEVICE,r1,$(PACKER_DIR)/assets/R1,r1.upt,$(PACKER_DIR)/sonix_firmware_packer.sh)

r3proii: app
	$(call BUILD_DEVICE,r3proii,$(PACKER_DIR)/assets/R3PII,r3proii.upt,$(PACKER_DIR)/sonix_firmware_packer.sh)

v1: app
	$(call BUILD_DEVICE,v1,$(V1_PACKER_DIR)/assets/V1,v1.upt,$(V1_PACKER_DIR)/sonix_firmware_packer.sh)

# Re-check every image that exists in the staging tree, with the expected
# device taken from its directory name. Exits non-zero on any FAIL.
validate:
	@set -euo pipefail; \
	found=0; \
	for dev in $(DEVICES); do \
		stage="$(BUILD_ROOT)/$$dev"; \
		case "$$dev" in \
			r1) image=r1.upt ;; \
			r3proii) image=r3proii.upt ;; \
			v1) image=v1.upt ;; \
		esac; \
		if [ -f "$$stage/$$image" ]; then \
			echo "==> validating $$dev"; \
			python3 "$(VALIDATOR)" "$$stage/$$image" --expect-device "$$dev" \
				--report "$$stage/validation.json" > "$$stage/validator.log" 2>&1 || { \
				cat "$$stage/validator.log" >&2; exit 1; \
			}; \
			sha256sum "$$stage/$$image"; \
			found=1; \
		fi; \
	done; \
	if [ "$$found" -eq 0 ]; then \
		echo "no built images in $(BUILD_ROOT); run make r1|r3proii|v1 first" >&2; \
		exit 1; \
	fi; \
	echo "SAFETY: structural validation only; no image is approved for flashing."

clean:
	rm -rf $(BUILD_ROOT)

help:
	@echo "Targets: r1 r3proii v1 all validate clean"
	@echo "Images are written under $(BUILD_ROOT)/<device>/ and validated against the expected device."
