ifndef VCM_PAF_SDK
$(error Set VCM_PAF_SDK to a VitaSDK prefix with vitasdk-paf-component installed)
endif
VITASDK := $(VCM_PAF_SDK)
PREFIX := $(VITASDK)/bin/arm-vita-eabi
CC := $(PREFIX)-gcc
CXX := $(PREFIX)-g++
OBJCOPY := $(PREFIX)-objcopy
PSP2CXML ?= psp2cxml-tool
TARGET := vita_content_manager
PACKAGE := content_manager_pro
TITLE_ID := VCMR00001
BUILD := build

CFLAGS := -std=gnu11 -Os -g -Wall -Wextra -Werror -fshort-wchar \
	-ffunction-sections -fdata-sections
CXXFLAGS := -std=gnu++11 -Os -g -Wall -Wextra -fno-rtti -fno-exceptions \
	-fno-builtin -fshort-wchar -ffunction-sections -fdata-sections \
	-D__VITA__ -D__declspec\(x\)=
LDFLAGS := -nostdlib -nostartfiles -Wl,-q,-z,nocopyreloc,--gc-sections,-e,module_start
LIBS := -lScePaf_stub_weak -lSceCommonGuiDialog_stub -lSceIofilemgr_stub -lSceLibKernel_stub -lSceKernelThreadMgr_stub -lSceProcessmgr_stub -lSceShellSvc_stub \
	-lSceSysmodule_stub -lSceSysmem_stub -lSceVideoExport_stub \
	-lScePhotoExport_stub -lSceMusicExport_stub -lSceAppUtil_stub -lSceAppMgr_stub \
	-lSceSqlite_stub -lSceNet_stub -lSceNetCtl_stub -lSceMtpIfDriver_stub_weak -lgcc

.PHONY: all clean
all: $(BUILD)/$(PACKAGE).vpk

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/paf_main.o: queue_ui.h common_gui_progress.h sync_queue.h wifi_service.h
$(BUILD)/sync_queue.o $(BUILD)/sync_import.o $(BUILD)/wifi_service.o: sync_queue.h
$(BUILD)/library.o: library.h media_database.h
$(BUILD)/wifi_service.o: library.h
$(BUILD)/mtp_snapshot.o: mtp_snapshot.h mtp_catalog.h library.h

$(BUILD)/$(TARGET).elf: $(BUILD)/paf_main.o $(BUILD)/media_database.o $(BUILD)/music_rw_vfs.o $(BUILD)/id3_tags.o $(BUILD)/wifi_service.o $(BUILD)/library.o $(BUILD)/mtp_catalog.o $(BUILD)/mtp_snapshot.o $(BUILD)/mtp_thumb.o $(BUILD)/sync_queue.o $(BUILD)/sync_import.o
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/$(TARGET).stripped.elf: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) --strip-debug $< $@

$(BUILD)/$(TARGET).velf: $(BUILD)/$(TARGET).stripped.elf exports.yml
	$(VITASDK)/bin/vita-elf-create -e exports.yml $< $@

# The FSELF attribute makes this a system application; 0xC800 KiB = 50 MiB.
$(BUILD)/eboot.bin: $(BUILD)/$(TARGET).velf
	$(VITASDK)/bin/vita-make-fself -at 0x0E -m 0xC800 $< $@

$(BUILD)/param.sfo: | $(BUILD)
	$(VITASDK)/bin/vita-mksfoex -s TITLE_ID=$(TITLE_ID) -s APP_VER=01.00 "Content Manager Pro" $@

$(BUILD)/$(TARGET).rco: ui/$(TARGET).xml ui/assets/photo_system_rounded.png ui/assets/music_system_rounded.png ui/assets/video_system_rounded.png ui/assets/cm_background.png ui/assets/cm_row.png | $(BUILD)
	$(PSP2CXML) $<
	test -f ui/$(TARGET).rco
	mv ui/$(TARGET).rco $@

$(BUILD)/$(PACKAGE).vpk: $(BUILD)/eboot.bin $(BUILD)/param.sfo $(BUILD)/$(TARGET).rco sce_sys/icon0.png
	$(VITASDK)/bin/vita-pack-vpk -s $(BUILD)/param.sfo -b $(BUILD)/eboot.bin \
		-a $(BUILD)/$(TARGET).rco=$(TARGET).rco \
		-a sce_sys/icon0.png=sce_sys/icon0.png $@

clean:
	rm -rf $(BUILD)
