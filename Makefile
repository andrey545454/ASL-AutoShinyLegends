.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set. Run make from the devkitPro MSYS2 shell")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET   := ASL-AutoShinyLegends
BUILD    := build
SOURCES  := source
INCLUDES := include
PLGINFO  := ASL-AutoShinyLegends.plgInfo
LIBDIRS  := $(CTRULIB)

ARCH     := -march=armv6k -mlittle-endian -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -Os -mword-relocations -Wall -Wextra -Werror \
            -fomit-frame-pointer -ffunction-sections -fdata-sections \
            -fno-strict-aliasing $(ARCH) $(INCLUDE) -D__3DS__
ASFLAGS  := $(ARCH)
LDFLAGS  := -T $(TOPDIR)/3ds.ld $(ARCH) -Os \
            -Wl,-Map,$(notdir $*.map),--gc-sections,--strip-discarded,--strip-debug,-z,noexecstack
LIBS     := -lctru

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export OFILES := $(CFILES:.c=.o) $(SFILES:.s=.o)
export LD := $(CC)

.PHONY: all clean verify $(BUILD)
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(OUTPUT).elf $(OUTPUT).3gx

verify: all
	@sig="$$(head -c 8 $(OUTPUT).3gx)"; \
	 if [ "$$sig" != '3GX$$0002' ]; then \
	   echo "ERROR: expected 3GX\$$0002, got '$$sig'"; exit 1; \
	 fi
	@echo "OK: $(OUTPUT).3gx is 3GX\$$0002"
else
DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3gx: $(OFILES)

%.3gx: %.elf
	@echo creating $(notdir $@)
	@3gxtool -s $< $(TOPDIR)/$(PLGINFO) $@

.PRECIOUS: %.elf
-include $(DEPENDS)
endif
