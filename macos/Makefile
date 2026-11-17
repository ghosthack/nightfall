APP = build/Nightfall.app
# VERSION=X.Y.Z (the release workflow passes the tag's version)
VERSION ?= 0.0.0
# "-" = ad-hoc signature for local use; a Developer ID identity gets hardened runtime + timestamp
SIGN_IDENTITY ?= -
ifeq ($(SIGN_IDENTITY),-)
SIGN_FLAGS =
else
SIGN_FLAGS = --options runtime --timestamp
endif

$(APP): src/main.swift src/scene.c src/scene.h res/Info.plist build/Nightfall.icns
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	clang -O2 -Wall -Wextra -target arm64-apple-macos13.0 -c src/scene.c -o build/scene-arm64.o
	clang -O2 -Wall -Wextra -target x86_64-apple-macos13.0 -c src/scene.c -o build/scene-x86_64.o
	swiftc -O -swift-version 5 -target arm64-apple-macos13.0 -import-objc-header src/scene.h -o build/Nightfall-arm64 src/main.swift build/scene-arm64.o
	swiftc -O -swift-version 5 -target x86_64-apple-macos13.0 -import-objc-header src/scene.h -o build/Nightfall-x86_64 src/main.swift build/scene-x86_64.o
	lipo -create -output $(APP)/Contents/MacOS/Nightfall build/Nightfall-arm64 build/Nightfall-x86_64
	cp res/Info.plist $(APP)/Contents/Info.plist
	plutil -replace CFBundleShortVersionString -string $(VERSION) $(APP)/Contents/Info.plist
	plutil -replace CFBundleVersion -string $(VERSION) $(APP)/Contents/Info.plist
	cp build/Nightfall.icns $(APP)/Contents/Resources/
	codesign --force $(SIGN_FLAGS) --sign "$(SIGN_IDENTITY)" $(APP)
	@touch $(APP)

build/Nightfall.icns: tools/make_icon.swift
	@mkdir -p build
	swift tools/make_icon.swift build/Nightfall.iconset
	iconutil -c icns build/Nightfall.iconset -o $@

clean:
	rm -rf build
.PHONY: clean
