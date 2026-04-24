# ------------ use '>' instead of TAB for recipes (GNU make) --------------
.RECIPEPREFIX := >

# ------------ cross tool‑chain -------------------------------------------
CROSS_PREFIX ?= /opt/toolchains/arm-gnu-toolchain-12.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
CC      := $(CROSS_PREFIX)gcc
AR      := $(CROSS_PREFIX)ar

# ------------ flags -------------------------------------------------------
CFLAGS_BASE   := -O3 -Wall -march=armv8-a+crypto
CFLAGS_STATIC := $(CFLAGS_BASE) -static
CFLAGS_DYN    := $(CFLAGS_BASE)

# where dpkg installs arm64 dev packages (for mbedtls/kcapi)
ARM64_INC ?= /usr/include
ARM64_LIB ?= /usr/lib/aarch64-linux-gnu

# link flags per flavour
LDLIBS_MBED  := -L$(ARM64_LIB) -lmbedcrypto
LDLIBS_KCAPI := -L$(ARM64_LIB) -lkcapi

# ---- OpenSSL (cross-built sysroot) --------------------------------------
OPENSSL_SYSROOT ?= /opt/arm64-openssl
OPENSSL_SO := $(OPENSSL_SYSROOT)/lib/libcrypto.so
OPENSSL_AA := $(OPENSSL_SYSROOT)/lib/libcrypto.a

HAVE_OPENSSL := 0
ifneq ($(wildcard $(OPENSSL_SO))$(wildcard $(OPENSSL_AA)),)
HAVE_OPENSSL := 1
endif

OPENSSL_STATIC ?= 0
OPENSSL_CFLAGS ?= -I$(OPENSSL_SYSROOT)/include
ifeq ($(OPENSSL_STATIC),1)
  ifeq ($(wildcard $(OPENSSL_AA)),)
    $(warning [Makefile] OPENSSL_STATIC=1 requested but $(OPENSSL_AA) not found; falling back to shared.)
    OPENSSL_STATIC := 0
  endif
endif
ifeq ($(OPENSSL_STATIC),1)
  OPENSSL_LIBS   ?= $(OPENSSL_AA) -ldl -lpthread
  CFLAGS_OPENSSL := $(CFLAGS_STATIC)
else
  OPENSSL_LIBS   ?= -L$(OPENSSL_SYSROOT)/lib -lcrypto
  CFLAGS_OPENSSL := $(CFLAGS_DYN)
endif

# ----------- targets / sources -------------------------------------------
SRC      := shmem_test.c
RAW      := shmem_test_raw
MBED     := shmem_test_mbedtls
KAPI     := shmem_test_kcapi
OPENSSL  := shmem_test_openssl

# Auto-copy after build? (1=yes default, 0=no)
AUTO_INSTALL ?= 1

# Destinations to copy to (resolve globs now; empty => skip with message)
DESTS := $(wildcard ../out)

VMROOTS_MSG = [Makefile] install: no destinations matched. Checked: ../out

# Build list
TARGETS := $(RAW) $(MBED) $(KAPI)
ifeq ($(HAVE_OPENSSL),1)
  TARGETS += $(OPENSSL)
else
  $(info [Makefile] OpenSSL sysroot not found at $(OPENSSL_SYSROOT); skipping $(OPENSSL).)
endif

.PHONY: all postcopy install clean

all: $(TARGETS) $(if $(filter 1,$(AUTO_INSTALL)),postcopy,)

# ---- build rules ----
# Raw build: silence "key defined but not used" in no-crypto compile
$(RAW): $(SRC)
>$(CC) $(CFLAGS_STATIC) -Wno-unused-const-variable -Wno-unused-function $< -o $@

$(MBED): $(SRC)
>$(CC) $(CFLAGS_STATIC) -I$(ARM64_INC) -DUSE_MBEDTLS $< $(LDLIBS_MBED) -o $@

$(KAPI): $(SRC)
>$(CC) $(CFLAGS_STATIC) -I$(ARM64_INC) -DUSE_KCAPI   $< $(LDLIBS_KCAPI) -o $@

$(OPENSSL): $(SRC)
>$(CC) $(CFLAGS_OPENSSL) $(OPENSSL_CFLAGS) -DUSE_OPENSSL $< $(OPENSSL_LIBS) -o $@

# ---- copy helpers ----
define do_copy
@if [ -z "$(DESTS)" ]; then \
  echo "$(VMROOTS_MSG)"; \
else \
  for d in $(DESTS); do \
    echo "Copying $(1) to $$d/"; \
    install -d "$$d"; \
    install -m 0755 "$(1)" "$$d/"; \
  done; \
fi
endef

# Post-build copy (runs when AUTO_INSTALL=1)
postcopy: $(TARGETS)
>$(call do_copy,$(RAW))
>$(call do_copy,$(MBED))
>$(call do_copy,$(KAPI))
ifneq ($(filter $(OPENSSL),$(TARGETS)),)
>$(call do_copy,$(OPENSSL))
endif

# Manual install (always available)
install: $(TARGETS)
>$(call do_copy,$(RAW))
>$(call do_copy,$(MBED))
>$(call do_copy,$(KAPI))
ifneq ($(filter $(OPENSSL),$(TARGETS)),)
>$(call do_copy,$(OPENSSL))
endif

clean:
>rm -f $(RAW) $(MBED) $(KAPI) $(OPENSSL)
