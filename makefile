CC = clang
CFLAGS = -std=c99 -O3 -g -Wall -Wextra
FRAMEWORKS = -framework CoreFoundation
LDLIBS = -ldl
TARGET = swipe

LAUNCH_AGENTS_DIR = $(HOME)/Library/LaunchAgents
PLIST_FILE = com.acsandmann.swipe.plist
PLIST_TEMPLATE = com.acsandmann.swipe.plist.in

ABS_TARGET_PATH = $(shell pwd)/$(TARGET)

SRC_FILES = src/aerospace.c src/cJSON.c src/swipe.c

.PHONY: all clean sign install_plist load_plist uninstall_plist install uninstall

ifeq ($(shell uname -sm),Darwin arm64)
	ARCH= -arch arm64
else
	ARCH= -arch x86_64
endif

all: $(TARGET)

$(TARGET): $(SRC_FILES)
	$(CC) $(CFLAGS) $(ARCH) -o $(TARGET) $(SRC_FILES) $(FRAMEWORKS) $(LDLIBS)

sign: $(TARGET)
	@echo "Signing $(TARGET) with accessibility entitlement..."
	codesign --entitlements accessibility.entitlements --sign - $(TARGET)

install_plist:
	@echo "Generating launch agent plist with binary path $(ABS_TARGET_PATH)..."
	mkdir -p $(LAUNCH_AGENTS_DIR)
	sed "s|@TARGET_PATH@|$(ABS_TARGET_PATH)|g" $(PLIST_TEMPLATE) > $(LAUNCH_AGENTS_DIR)/$(PLIST_FILE)
	@echo "Launch agent plist installed to $(LAUNCH_AGENTS_DIR)/$(PLIST_FILE)"

load_plist:
	@echo "Loading launch agent..."
	launchctl load $(LAUNCH_AGENTS_DIR)/$(PLIST_FILE)

uninstall_plist:
	@echo "Unloading launch agent..."
	launchctl unload $(LAUNCH_AGENTS_DIR)/$(PLIST_FILE)
	@echo "Removing launch agent plist from $(LAUNCH_AGENTS_DIR)..."
	rm -f $(LAUNCH_AGENTS_DIR)/$(PLIST_FILE)

build: all sign

install: all sign install_plist load_plist

uninstall: uninstall_plist clean

clean:
	rm -f $(TARGET)
