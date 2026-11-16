APP = build/Nightfall.app

$(APP): src/main.swift res/Info.plist build/Nightfall.icns
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	swiftc -O -swift-version 5 -target arm64-apple-macos13.0 -o build/Nightfall-arm64 src/main.swift
	swiftc -O -swift-version 5 -target x86_64-apple-macos13.0 -o build/Nightfall-x86_64 src/main.swift
	lipo -create -output $(APP)/Contents/MacOS/Nightfall build/Nightfall-arm64 build/Nightfall-x86_64
	cp res/Info.plist $(APP)/Contents/Info.plist
	cp build/Nightfall.icns $(APP)/Contents/Resources/
	codesign --force --sign - $(APP)
	@touch $(APP)

build/Nightfall.icns: tools/make_icon.swift
	@mkdir -p build
	swift tools/make_icon.swift build/Nightfall.iconset
	iconutil -c icns build/Nightfall.iconset -o $@

clean:
	rm -rf build
.PHONY: clean
