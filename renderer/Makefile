# Makefile - Pi-Clock renderer build
#
# Builds the renderer binary using SDL2 for display output and
# Cairo for 2D layer rendering. stb_image.h handles JPEG loading.
#
# Targets:
#   make            - Build the renderer binary
#   make clean      - Remove build artifacts
#
# Copyright (C) 2026 Pi-Clock Contributors
# SPDX-License-Identifier: GPL-2.0-only

CC      ?= gcc
# snprintf into fixed buffers truncates by design throughout: those two
# warning classes are noise here
CFLAGS  ?= -Wall -Wextra -Wformat=2 -Wno-format-truncation -Wno-stringop-truncation -O2
LDFLAGS ?=
# musl gives threads 128 KB of stack by default; the fetchers use more
LDFLAGS += -Wl,-z,stack-size=1048576
VERSION ?= 0.0.0-dev

# Use pkg-config to find library flags.
# SDL2 provides the display window / DRM-KMS output.
# Cairo provides 2D rendering for each layer.
CAIRO_CFLAGS := $(shell pkg-config --cflags cairo)
CAIRO_LIBS   := $(shell pkg-config --libs cairo)
DRM_CFLAGS   := $(shell pkg-config --cflags libdrm 2>/dev/null)
SDL2_CFLAGS  := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS    := $(shell pkg-config --libs sdl2 2>/dev/null)

# Combine flags
ALL_CFLAGS  = $(CFLAGS) $(CAIRO_CFLAGS) $(SDL2_CFLAGS) $(DRM_CFLAGS) -DVERSION=\"$(VERSION)\"
ALL_LDFLAGS = $(LDFLAGS) $(CAIRO_LIBS) $(SDL2_LIBS) -lm -lpthread -lssl -lcrypto -ldrm

# Source files and corresponding object files
SOURCES = main.c display.c layer.c spacetrack.c spacedata.c planetview.c planetmath.c craft_icons.c solar.c lunar.c image.c config.c borders.c \
          ctydat.c callsign_districts.c dxspot.c dxcluster.c \
          layers_base.c layers_daylight.c layers_grid.c \
          layers_borders.c layers_timezone.c layers_cqzone.c layers_ituzone.c \
          layers_sun.c layers_moon.c layers_maidenhead.c \
          propagation.c layers_propagation.c \
          layers_dxspots.c layers_satellite.c layers_qth.c layers_hud.c \
          layers_ticker.c \
          applet.c applet_dxfeed.c applet_muf.c applet_voacap.c \
          applet_sysinfo.c applet_features.c \
          solarweather.c applet_solar.c newsticker.c \
          satellite.c tzlookup.c fetch.c \
          lightning.c layers_lightning.c \
          earthquake.c layers_earthquake.c \
          wind.c layers_wind.c layers_cloud.c layers_precip.c \
          aurora.c layers_aurora.c \
          volcano.c layers_volcano.c \
          json.c storm.c layers_storm.c \
          nwsalert.c layers_nwsalert.c \
          radar.c layers_radar.c \
          astro.c applet_astro.c \
          airquality.c layers_airquality.c \
          starlink.c layers_starlink.c \
          aircraft.c layers_aircraft.c \
          feature.c sat_icons.c layers_overhead.c geopoly.c ws.c layers_spc.c layers_launches.c \
          layers_dsn.c applet_passes.c layers_satclouds.c layers_fires.c \
          applet_tides.c layers_planets.c layers_ships.c \
          layers_aurorawatch.c layers_lightpollution.c ha_notify.c \
          layers_sst.c applet_seasons.c season.c layers_pchelper.c sprites.c rocket_icons.c launchvis.c datastatus.c applet_events.c health.c sky_icons.c crew.c skycover.c eclipse.c eclipse_data.c layers_eclipse.c \
          localview.c layers_lsr.c layers_rainfall.c layers_rivers.c \
          layers_ebird.c layers_drought.c applet_coast.c applet_backyard.c \
          layers_twilight.c layers_darksites.c spacelayers.c layers_spacetrack.c
OBJECTS = $(SOURCES:.c=.o)

# Output binary names
TARGET = pi-clock
SPLASH = pi-clock-splash

# Default target: build both binaries
all: $(TARGET) $(SPLASH)

# Link all object files into the final binary
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(ALL_LDFLAGS) -o $@

# Splash tool — standalone, no Cairo/SDL dependencies
$(SPLASH): pi-clock-splash.c stb_image.h
	$(CC) $(CFLAGS) -o $@ $< -lm

# Compile each .c file to a .o file
%.o: %.c
	$(CC) $(ALL_CFLAGS) -MMD -c $< -o $@

# Include auto-generated dependency files (if they exist)
-include $(SOURCES:.c=.d)

# Remove all build artifacts
clean:
	rm -f $(OBJECTS) $(SOURCES:.c=.d) $(TARGET) $(SPLASH)

.PHONY: all clean
