ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

REAL_GAME ?= 0
ifeq ($(REAL_GAME),1)
TARGET := ps1recomp_game
APP_CPPFILES := main_real.cpp real_recompiled.cpp
EXTRA_OFILES :=
else
TARGET := ps1recomp_switch
APP_CPPFILES := main.cpp recompiled_generated.cpp
EXTRA_OFILES := test_psx.o
endif

BUILD := build
SOURCES := source ../ps1Runtime/src
INCLUDES := include ../ps1Runtime/include ../third_party/fmt/include source
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS += $(INCLUDE) -D__SWITCH__ -DFMT_HEADER_ONLY=1
CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-rtti -ffunction-sections -fdata-sections
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)
LIBS := -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

# Full portable PS1 runtime for Switch. Host-only SDL/OpenGL front-end files
# remain excluded; RendererSwitch/libnx owns presentation and input.
CPPFILES := $(APP_CPPFILES) renderer_switch.cpp \
	gte.cpp metrics.cpp overlay_manager.cpp \
	input.cpp timers.cpp gpu.cpp dma.cpp spu.cpp mdec.cpp \
	cue_parser.cpp bin_reader.cpp iso9660.cpp exe_extractor.cpp virtual_fs.cpp cdrom_controller.cpp \
	bios.cpp syscall_a.cpp syscall_b.cpp syscall_c.cpp heap.cpp event_system.cpp file_io.cpp \
	psyq_hle.cpp psyq_registry.cpp psyq_libapi.cpp psyq_libcd.cpp psyq_libetc.cpp \
	psyq_libgte.cpp psyq_libgpu.cpp psyq_font.cpp psyq_pad.cpp psyq_libc.cpp psyq_state.cpp

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export DEPSDIR := $(CURDIR)/$(BUILD)
export LD := $(CXX)
export VPATH := $(CURDIR)/source \
	$(CURDIR)/../ps1Runtime/src \
	$(CURDIR)/../ps1Runtime/src/input \
	$(CURDIR)/../ps1Runtime/src/timers \
	$(CURDIR)/../ps1Runtime/src/gpu \
	$(CURDIR)/../ps1Runtime/src/dma \
	$(CURDIR)/../ps1Runtime/src/spu \
	$(CURDIR)/../ps1Runtime/src/mdec \
	$(CURDIR)/../ps1Runtime/src/cdrom \
	$(CURDIR)/../ps1Runtime/src/bios \
	$(CURDIR)/../ps1Runtime/src/psyq
export OFILES := $(addsuffix .o,$(CPPFILES:.cpp=)) $(EXTRA_OFILES)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
.PHONY: all clean $(BUILD)
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

ifeq ($(REAL_GAME),0)
test_psx.o: $(TOPDIR)/testdata/minimal.psx.exe
	@echo embedding $(notdir $<)
	@cd $(TOPDIR)/testdata && $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-objcopy -I binary -O elf64-littleaarch64 -B aarch64 \
		--redefine-sym _binary_minimal_psx_exe_start=_binary_test_psx_start \
		--redefine-sym _binary_minimal_psx_exe_end=_binary_test_psx_end \
		--redefine-sym _binary_minimal_psx_exe_size=_binary_test_psx_size \
		minimal.psx.exe $(CURDIR)/$@
endif

-include $(DEPENDS)
endif
