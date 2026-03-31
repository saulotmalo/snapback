#---------------------------------------------------------------------------------
# apple-bt-reconnect — Nintendo Switch sysmodule Makefile
# Requires devkitPro / devkitA64 + libnx
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET   := main
SOURCES  := source
INCLUDES := include
LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CXXFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) \
             -D__SWITCH__ -DSYSMODULE \
             $(foreach d,$(INCLUDES),-I$(CURDIR)/$(d)) \
             $(foreach d,$(LIBDIRS),-I$(d)/include) \
             -std=c++17 -fno-rtti -fno-exceptions

LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
             -Wl,-Map,out/$(TARGET).map

LIBS     := -lnx
LIBPATHS := $(foreach d,$(LIBDIRS),-L$(d)/lib)

#---------------------------------------------------------------------------------
CPPFILES := $(foreach d,$(SOURCES),$(notdir $(wildcard $(d)/*.cpp)))
OFILES   := $(CPPFILES:.cpp=.o)

OUTPUT   := out/$(TARGET)

VPATH    := $(foreach d,$(SOURCES),$(CURDIR)/$(d))
DEPSDIR  := build

.PHONY: all clean install

all: out/exefs.nsp

# Package NSO + NPDM into exefs.nsp
out/exefs.nsp: $(OUTPUT).nso out/main.npdm
	@mkdir -p out/exefs_tmp
	@cp $(OUTPUT).nso out/exefs_tmp/main
	@cp out/main.npdm out/exefs_tmp/main.npdm
	@build_pfs0 out/exefs_tmp $@
	@rm -rf out/exefs_tmp
	@echo "  NSP     exefs.nsp"

out/main.npdm: app.json | out
	@npdmtool $< $@
	@echo "  NPDM    main.npdm"

# switch_rules provides: %.nso: %.elf  (via elf2nso)
$(OUTPUT).nso: $(OUTPUT).elf

$(OUTPUT).elf: $(addprefix build/,$(OFILES)) | out
	@echo "  LINK    $(notdir $@)"
	$(CXX) $(LDFLAGS) $^ $(LIBPATHS) $(LIBS) -o $@

build/%.o: %.cpp | build
	@echo "  CXX     $(notdir $<)"
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build out:
	@mkdir -p $@

clean:
	@echo "  CLEAN"
	@rm -rf build out

#---------------------------------------------------------------------------------
SDCARD_PATH := /mnt/sdcard
TITLE_ID    := 420000000000000C
INSTALL_DIR := $(SDCARD_PATH)/atmosphere/contents/$(TITLE_ID)

install: all
	@echo "  INSTALL -> $(INSTALL_DIR)"
	@mkdir -p $(INSTALL_DIR)/flags
	@cp out/exefs.nsp $(INSTALL_DIR)/exefs.nsp
	@cp toolbox.json  $(INSTALL_DIR)/toolbox.json
	@touch            $(INSTALL_DIR)/flags/boot2.flag
	@echo "  Done. Reboot your Switch."

-include $(wildcard build/*.d)
