CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic
SDLFLAGS := $(shell pkg-config --cflags --libs sdl2)
RELEASE_DIR ?= Release v0.3

all: Flowmulator

embedded_assets.o: .assets/frontend_background.bmp .assets/jackhammer.rgba \
		.assets/jackhammer.wav .assets/big_forehead.wav install-dependencies.sh
	ld -r -b binary -o $@ $^

Flowmulator: main.cpp libretro.h embedded_assets.h embedded_assets.o
	$(CXX) $(CXXFLAGS) main.cpp embedded_assets.o -o Flowmulator $(SDLFLAGS) -ldl -lGL

release: clean Flowmulator
	rm -rf "$(RELEASE_DIR)"
	mkdir -p "$(RELEASE_DIR)/ROMS" "$(RELEASE_DIR)/SAVES"
	cp Flowmulator "$(RELEASE_DIR)/Flowmulator"
	cp README.md "$(RELEASE_DIR)/"
	cp install-dependencies.sh "$(RELEASE_DIR)/"
	chmod +x "$(RELEASE_DIR)/Flowmulator" "$(RELEASE_DIR)/install-dependencies.sh"
	strip --strip-unneeded "$(RELEASE_DIR)/Flowmulator"

clean:
	rm -f Flowmulator embedded_assets.o
