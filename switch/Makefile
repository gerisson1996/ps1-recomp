ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := ps1recomp_switch
BUILD := build
SOURCES := source ../ps1Runtime/src
INCLUDES := include ../ps1Runtime/include ../third_party/fmt/include source
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS += $(INCLUDE) -D__SWITCH__ -DPS1_SWITCH_GPU_DMA_TEST
CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-rtti -ffunction-sections -fdata-sections
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)
LIBS := -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

# Milestone 2 links the recompiler ABI stub plus portable CPU/emuptr headers.
CPPFILES := main.cpp recompiled_generated.cpp input.cpp timers.cpp gpu.cpp metrics.cpp renderer_switch.cpp dma.cpp gte.cpp

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export DEPSDIR := $(CURDIR)/$(BUILD)
export LD := $(CXX)
export VPATH := $(CURDIR)/source $(CURDIR)/../ps1Runtime/src $(CURDIR)/../ps1Runtime/src/input $(CURDIR)/../ps1Runtime/src/timers $(CURDIR)/../ps1Runtime/src/gpu $(CURDIR)/../ps1Runtime/src/dma
export OFILES := $(addsuffix .o,$(CPPFILES:.cpp=)) test_psx.o
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
.PHONY: all clean
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
.PHONY: all
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

test_psx.o: $(TOPDIR)/testdata/minimal.psx.exe
	@echo embedding $(notdir $<)
	@cd $(TOPDIR)/testdata && $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-objcopy -I binary -O elf64-littleaarch64 -B aarch64 \
		--redefine-sym _binary_minimal_psx_exe_start=_binary_test_psx_start \
		--redefine-sym _binary_minimal_psx_exe_end=_binary_test_psx_end \
		--redefine-sym _binary_minimal_psx_exe_size=_binary_test_psx_size \
		minimal.psx.exe $(CURDIR)/$@

-include $(DEPENDS)
endif
