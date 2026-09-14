CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic
SDLFLAGS := $(shell pkg-config --cflags --libs sdl2)
RELEASE_VERSION ?= 0.8
RELEASE_DIR ?= Releases/Release v$(RELEASE_VERSION)
RELEASE_ZIP ?= Flowmulator v$(RELEASE_VERSION).zip

all: Flowmulator

embedded_assets.o: .assets/frontend_background.bmp .assets/edgerunners_background.bmp \
		.assets/berserk_background.bmp \
		.assets/gba_overlay.rgba \
		.assets/nds_overlay.rgba \
		.assets/nds_top_overlay.rgba \
		.assets/jackhammer.rgba \
		.assets/jackhammer.wav .assets/big_forehead.wav \
		.assets/skywalker.rgba .assets/water_splash.rgba \
		.assets/skywalker_splash.wav .assets/skywalker_oooooh.wav \
		.assets/flowmulator.svg \
		.runtime/azahar.AppImage .runtime/azahar_libretro.so \
		.runtime/mgba_libretro.so .runtime/desmume_libretro.so \
		.runtime/MiiFix.3dsx Helper.sh
	ld -r -b binary -o $@ $^

Flowmulator: main.cpp libretro.h libretro_vulkan.h vulkan_minimal.h \
		embedded_assets.h embedded_assets.o
	$(CXX) $(CXXFLAGS) main.cpp embedded_assets.o -o Flowmulator $(SDLFLAGS) -ldl -lGL

release: clean Flowmulator
	rm -rf "$(RELEASE_DIR)"
	mkdir -p "$(RELEASE_DIR)/ROMS/GBA" "$(RELEASE_DIR)/ROMS/NDS" \
		"$(RELEASE_DIR)/ROMS/3DS" "$(RELEASE_DIR)/ROMS/WII" \
		"$(RELEASE_DIR)/ROMS/GAMECUBE" \
		"$(RELEASE_DIR)/SAVES"
	cp Flowmulator "$(RELEASE_DIR)/Flowmulator"
	cp README.md "$(RELEASE_DIR)/"
	cp Helper.sh "$(RELEASE_DIR)/"
	chmod +x "$(RELEASE_DIR)/Flowmulator" "$(RELEASE_DIR)/Helper.sh"
	strip --strip-unneeded "$(RELEASE_DIR)/Flowmulator"
	rm -f "$(RELEASE_ZIP)"
	(cd "$(RELEASE_DIR)" && zip -qr "../../$(RELEASE_ZIP)" .)

clean:
	rm -f Flowmulator embedded_assets.o
