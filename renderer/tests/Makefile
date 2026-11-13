# Offline unit tests for the renderer (no display, no network).
#   make check        build and run everything
# Needs: cc, cairo and OpenSSL development files.

CC      ?= cc
CFLAGS  ?= -O1 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
           -Wno-stringop-truncation -Wno-unused-result -D_GNU_SOURCE -I.. \
           -Werror=implicit-function-declaration $(shell pkg-config --cflags cairo)
LIBS    = $(shell pkg-config --libs cairo) -lm -lpthread -lssl -lcrypto

# Renderer sources the tests link against (everything but the display
# loop, which needs the framebuffer / SDL)
CORE    = feature.c geopoly.c ws.c layers_spc.c layers_launches.c layers_dsn.c applet_passes.c layers_satclouds.c layers_fires.c applet_tides.c layers_planets.c layers_ships.c json.c config.c borders.c layers_borders.c image.c astro.c solar.c lunar.c satellite.c nwsalert.c layer.c layers_aurorawatch.c layers_lightpollution.c fetch.c ha_notify.c localview.c layers_lsr.c layers_rainfall.c layers_rivers.c layers_ebird.c layers_drought.c applet_coast.c applet_backyard.c layers_twilight.c layers_darksites.c radar.c layers_base.c layers_sst.c applet_seasons.c sat_icons.c layers_overhead.c layers_pchelper.c sprites.c rocket_icons.c launchvis.c datastatus.c applet_events.c health.c sky_icons.c crew.c skycover.c eclipse.c eclipse_data.c layers_eclipse.c planetmath.c planetview.c craft_icons.c spacetrack.c spacedata.c season.c spacelayers.c layers_spacetrack.c
OBJS    = $(addprefix build/,$(CORE:.c=.o))
TESTS   = test_sgp4 test_tlelist test_rockets test_launchvis test_launches \
          test_composite test_datastatus test_view test_health test_crew test_saticons test_skycover test_fetch test_eclipse test_planet test_spacelayers test_astro

all: $(addprefix build/,$(TESTS))

build/%.o: ../%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/libcore.a: $(OBJS)
	ar rcs $@ $^

build/test_%: test_%.c check.h build/libcore.a
	$(CC) $(CFLAGS) $< build/libcore.a $(LIBS) -o $@

check: all
	@fail=0; for t in $(TESTS); do ./build/$$t || fail=1; done; \
	 if [ $$fail = 0 ]; then echo "All renderer tests passed"; else echo "Some tests FAILED"; exit 1; fi

clean:
	rm -rf build

.PHONY: all check clean
