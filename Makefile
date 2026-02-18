#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET		:=	Javelin
BUILD		:=	build
SOURCES		:=	source source/mtp source/install source/ui source/core source/tickets source/i18n source/dump source/service libs/imgui \
			libs/libnx-ext/libnx-ext/source libs/libnx-ext/libnx-ipcext/source
DATA		:=	data romfs
INCLUDES	:=	include include/mtp include/install include/ui include/core include/tickets include/i18n include/dump include/service libs/imgui libs/imgui/backends \
			libs/libnx-ext/libnx-ext/include libs/libnx-ext/libnx-ipcext/include
APP_TITLE	:=	Javelin
APP_AUTHOR	:=	Delta
APP_VERSION	:=	1.0.0

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# Handle DEBUG flag (default: 0 for production releases)
# Set DEBUG=1 to enable debug output and nxlink networking
ifndef DEBUG
DEBUG := 0
endif


CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -DIMGUI_USER_CONFIG=\"../../../include/imconfig.h\" -DDEBUG=$(DEBUG) -DENABLE_NXLINK=$(ENABLE_NXLINK)

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=c++17

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lglad -lEGL -lglapi -ldrm_nouveau -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

# Translation generation
TRANS_GEN_TOOL := build/.gen_trans_tool
TRANS_SRC_H := include/i18n/EmbeddedTranslations.h
TRANS_SRC_CPP := source/i18n/embedded_translations.cpp

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export BUILD_EXEFS_SRC := $(TOPDIR)/$(BUILD)/exefs_src

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg *.png)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else ifneq (,$(findstring icon.jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/icon.jpg
	else ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else ifneq (,$(findstring Javelin.png,$(icons)))
		export APP_ICON := $(TOPDIR)/Javelin.png
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all send gen_translations

#---------------------------------------------------------------------------------
# Main target (must be first non-pattern rule)
#---------------------------------------------------------------------------------
all: gen_translations $(BUILD)

#---------------------------------------------------------------------------------
# Translation generation
#---------------------------------------------------------------------------------
gen_translations: $(TRANS_SRC_H) $(TRANS_SRC_CPP)

$(TRANS_GEN_TOOL): tools/gen_translations.cpp
	@mkdir -p build
	@echo "Building translation generator..."
	@g++ -std=c++17 -O2 -o $(TRANS_GEN_TOOL) tools/gen_translations.cpp

$(TRANS_SRC_H) $(TRANS_SRC_CPP): $(TRANS_GEN_TOOL) $(wildcard romfs/javelin/i18n/*.json)
	@echo "Generating translations..."
	@$(TRANS_GEN_TOOL) $(CURDIR)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TRANS_GEN_TOOL) $(TRANS_SRC_H) $(TRANS_SRC_CPP)

#---------------------------------------------------------------------------------
send: all
	@echo Sending to Switch at $(SWITCH_IP)...
	@nxlink -s -a $(SWITCH_IP) $(TARGET).nro


#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
