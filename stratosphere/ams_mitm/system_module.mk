#---------------------------------------------------------------------------------
# pull in common stratosphere sysmodule configuration
#---------------------------------------------------------------------------------
THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
CURRENT_DIRECTORY := $(abspath $(dir $(THIS_MAKEFILE)))
include $(dir $(abspath $(lastword $(MAKEFILE_LIST))))/../../libraries/config/templates/stratosphere.mk

ATMOSPHERE_SYSTEM_MODULE_TARGETS := kip

# SD1_WRAP: bounded native-SD counters, transparent to service results.
LDFLAGS += -Wl,--wrap,fsOpenSdCardFileSystem -Wl,--wrap,fsOpenSdCardFileSystemFwd -Wl,--wrap,fsOpenContentStorageFileSystem -Wl,--wrap,fsOpenContentStorageFileSystemFwd -Wl,--wrap,fsFsOpenFile -Wl,--wrap,fsFsOpenDirectory -Wl,--wrap,fsFsClose -Wl,--wrap,fsFileClose -Wl,--wrap,fsDirClose -Wl,--wrap,fsFileRead -Wl,--wrap,fsFileWrite -Wl,--wrap,fsFileFlush -Wl,--wrap,fsFileGetSize -Wl,--wrap,fsFileSetSize -Wl,--wrap,fsFsCommit -Wl,--wrap,fsDirRead -Wl,--wrap,fsDirGetEntryCount -Wl,--wrap,fsFsCreateFile -Wl,--wrap,fsFsGetEntryType -Wl,--wrap,fsFsGetFileTimeStampRaw -Wl,--wrap,fsFileOperateRange -Wl,--wrap,fsFsQueryEntry -Wl,--wrap,fsFsDeleteFile -Wl,--wrap,fsFsCreateDirectory -Wl,--wrap,fsFsDeleteDirectory -Wl,--wrap,fsFsDeleteDirectoryRecursively -Wl,--wrap,fsFsCleanDirectoryRecursively -Wl,--wrap,fsFsGetFreeSpace -Wl,--wrap,fsFsGetTotalSpace -Wl,--wrap,fsFsRenameFile -Wl,--wrap,fsFsRenameDirectory


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(__RECURSIVE__),1)
#---------------------------------------------------------------------------------

export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

CFILES      :=	$(call FIND_SOURCE_FILES,$(SOURCES),c)
CPPFILES    :=	$(filter-out usb_probe_module.cpp,$(call FIND_SOURCE_FILES,$(SOURCES),cpp))
SFILES      :=	$(call FIND_SOURCE_FILES,$(SOURCES),s)

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

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
			$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			$(foreach dir,$(AMS_LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) $(foreach dir,$(AMS_LIBDIRS),-L$(dir)/$(ATMOSPHERE_LIBRARY_DIR))

export BUILD_EXEFS_SRC := $(TOPDIR)/$(EXEFS_SRC)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

.PHONY: clean all check_lib

#---------------------------------------------------------------------------------
all: $(ATMOSPHERE_OUT_DIR) $(ATMOSPHERE_BUILD_DIR) $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/$(ATMOSPHERE_LIBRARY_DIR)/libstratosphere.a
	@$(MAKE) __RECURSIVE__=1 OUTPUT=$(CURDIR)/$(ATMOSPHERE_OUT_DIR)/$(TARGET) \
	DEPSDIR=$(CURDIR)/$(ATMOSPHERE_BUILD_DIR) \
	--no-print-directory -C $(ATMOSPHERE_BUILD_DIR) \
	-f $(THIS_MAKEFILE)

$(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/$(ATMOSPHERE_LIBRARY_DIR)/libstratosphere.a: check_lib
	@$(SILENTCMD)echo "Checked library."

ifeq ($(ATMOSPHERE_CHECKED_LIBSTRATOSPHERE),1)
check_lib:
else
check_lib:
	@$(MAKE) --no-print-directory -C $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere -f $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/libstratosphere.mk
endif

$(ATMOSPHERE_OUT_DIR) $(ATMOSPHERE_BUILD_DIR):
	@[ -d $@ ] || mkdir -p $@

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(ATMOSPHERE_OUT_DIR) $(ATMOSPHERE_BUILD_DIR)


#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(foreach target,$(ATMOSPHERE_SYSTEM_MODULE_TARGETS),$(OUTPUT).$(target))

$(OUTPUT).kip :	$(OUTPUT).elf
$(OUTPUT).nsp :	$(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso :	$(OUTPUT).elf

$(OUTPUT).elf :	$(OFILES)

$(OFILES) : $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/$(ATMOSPHERE_LIBRARY_DIR)/libstratosphere.a

# usbmitm_module.cpp is intentionally large. Keep LTO disabled so the isolated
# experimental rebuild remains responsive on memory-constrained Windows hosts,
# but optimize this object for size so the enabled filesystem route still fits
# in package3's fixed KIP region.
usbmitm_module.o: CXXFLAGS += -fno-lto -Os

# P6's cold NS dispatch/diagnostic wrappers must fit the existing fixed KIP
# region. Size-optimize these two units only; no region/capability changes.
# Depend on this recipe so changing flags cannot reuse an old object silently.
nsmitm_module.o ns_verification_trace.o: CXXFLAGS += -Os
nsmitm_module.o ns_verification_trace.o: $(THIS_MAKEFILE)

%.npdm  :   %.npdm.json
	@echo built ... $< $@
	@npdmtool $< $@
	@echo built ... $(notdir $@)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
