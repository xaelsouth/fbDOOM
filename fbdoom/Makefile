################################################################
#
# $Id:$
#
# $Log:$
#

CROSS_COMPILE ?= #arm-linux-gnueabihf-
BACKEND ?= fbdev
CC=$(CROSS_COMPILE)gcc  # gcc or g++

ifeq ($(V),1)
	VB=''
else
	VB=@
endif

#LIBS+=-lXext -lX11 -lnsl -lm -lSDL
#CFLAGS+=-Wunused-const-variable=0 
#CFLAGS+=-fsanitize=address
ifeq ($(BACKEND),fbdev)
OBJS+=$(OBJDIR)/i_video_fbdev.o
OBJS+=$(OBJDIR)/i_input_evdev.o

# Optional i.MX23 PXP (V4L2 mem2mem) video path. It is enabled when the
# toolchain has <linux/videodev2.h> and the target is little-endian; force it
# on or off with PXP=1 / PXP=0.
PXP ?= auto
ifeq ($(PXP),auto)
ifneq ($(shell printf '\#include <linux/videodev2.h>\n' | $(CC) -E -x c - >/dev/null 2>&1 && \
	$(CC) -dM -E -x c /dev/null | grep -q '__ORDER_LITTLE_ENDIAN__ __BYTE_ORDER__\|__BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__' && echo yes),)
PXP=1
else
PXP=0
endif
endif

ifeq ($(PXP),1)
CFLAGS+=-DHAVE_PXP
OBJS+=$(OBJDIR)/i_video_pxp.o

# Optional DRM/KMS scanout for the PXP path (needs libdrm); DRM=1 / DRM=0
# forces it. Without it the PXP result is copied into the fbdev framebuffer.
DRM ?= auto
# A cross compiler with a sysroot looks there; pkg-config is for native builds.
DRM_SYSROOT := $(shell $(CC) -print-sysroot 2>/dev/null)
ifneq ($(DRM_SYSROOT),)
DRM_CFLAGS := -I$(DRM_SYSROOT)/usr/include/libdrm
else
DRM_CFLAGS := $(shell pkg-config --cflags libdrm 2>/dev/null)
endif
ifeq ($(DRM),auto)
ifneq ($(shell printf '\#include <xf86drm.h>\n' | $(CC) -E -x c $(DRM_CFLAGS) - >/dev/null 2>&1 && echo yes),)
DRM=1
else
DRM=0
endif
endif
ifeq ($(DRM),1)
CFLAGS+=-DHAVE_DRM $(DRM_CFLAGS)
LIBS+=-ldrm
endif
endif

else ifeq ($(BACKEND),x11)
OBJS+=$(OBJDIR)/i_video_x11.o
LIBS+=-lX11
else
$(error Unsupported BACKEND '$(BACKEND)'; choose fbdev or x11)
endif

# Optional ALSA sound and OPL music. Enabled when <alsa/asoundlib.h> is
# available; force it on or off with ALSA=1 / ALSA=0. Without it the game
# builds and runs silently and needs no libasound.
ALSA ?= auto
ifeq ($(ALSA),auto)
ifneq ($(shell printf '\#include <alsa/asoundlib.h>\n' | $(CC) -E -x c - >/dev/null 2>&1 && echo yes),)
ALSA=1
else
ALSA=0
endif
endif

ifeq ($(ALSA),1)
CFLAGS+=-DHAVE_ALSA
OBJS+=$(OBJDIR)/i_sound_alsa.o
OBJS+=$(addprefix $(OBJDIR)/, opl_alsa.o opl.o opl_queue.o opl2.o i_oplmusic.o midifile.o mus2mid.o)
LIBS+=-lasound
endif

CFLAGS+=-ggdb3 -O2 -fomit-frame-pointer
# Optional CPU tuning, e.g. make CPU_FLAGS=-mcpu=arm926ej-s for i.MX23.
CFLAGS+=$(CPU_FLAGS)
LDFLAGS+=-Wl,--gc-sections
CFLAGS+=-std=gnu99 -ggdb3 -Wall -DNORMALUNIX -DLINUX -DFBDEV_ALSA
LIBS+=-lm -lc -lpthread

# Keep backend object files separate so changing BACKEND never mixes objects.
OBJDIR=build-$(BACKEND)
OUTPUT=fbdoom-$(BACKEND)

SRC_DOOM = i_fps.o i_main.o dummy.o am_map.o doomdef.o doomstat.o dstrings.o d_event.o d_items.o d_iwad.o d_loop.o d_main.o d_mode.o d_net.o f_finale.o f_wipe.o g_game.o hu_lib.o hu_stuff.o info.o i_cdmus.o i_endoom.o i_joystick.o i_scale.o i_sound.o i_system.o i_timer.o memio.o m_argv.o m_bbox.o m_cheat.o m_config.o m_controls.o m_fixed.o m_menu.o m_misc.o m_random.o p_ceilng.o p_doors.o p_enemy.o p_floor.o p_inter.o p_lights.o p_map.o p_maputl.o p_mobj.o p_plats.o p_pspr.o p_saveg.o p_setup.o p_sight.o p_spec.o p_switch.o p_telept.o p_tick.o p_user.o r_bsp.o r_data.o r_draw.o r_main.o r_plane.o r_segs.o r_sky.o r_things.o sha1.o sounds.o statdump.o st_lib.o st_stuff.o s_sound.o tables.o v_video.o wi_stuff.o w_checksum.o w_file.o w_file_stdc_unbuffered.o w_main.o w_wad.o z_zone.o
OBJS += $(addprefix $(OBJDIR)/, $(SRC_DOOM))

all:	 $(OUTPUT)

clean:
	rm -rf $(OBJDIR)
	rm -f $(OUTPUT)
	rm -f $(OUTPUT).gdb
	rm -f $(OUTPUT).map

$(OUTPUT):	$(OBJS)
	@echo [Linking $@]
	$(VB)$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) \
	-o $(OUTPUT) $(LIBS) -Wl,-Map,$(OUTPUT).map
	@echo [Size]
	-$(CROSS_COMPILE)size $(OUTPUT)

$(OBJS): | $(OBJDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o:	%.c
	@echo [Compiling $<]
	$(VB)$(CC) $(CFLAGS) -c $< -o $@

print:
	@echo OBJS: $(OBJS)

config:
	@echo PXP=$(PXP) DRM=$(DRM)
	@echo CFLAGS=$(CFLAGS)
