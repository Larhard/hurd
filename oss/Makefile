dir := oss
makemode := server

target = oss
SRCS = oss.c logging.c

HURDLIBS = trivfs ports shouldbeinlibc
LDFLAGS = -lrump -lrumpuser -lrumpvfs -lrumpdev -lrumpdev_audio \
          -lrumpdev_pci -lrumpdev_pci_auich -lrumpdev_audio_ac97 \
          -lrumpdev_pci_hdaudio -lrumpdev_hdaudio_hdafg

MIGSTUBS = oioctlServer.o
MIGSFLAGS += -imacros $(srcdir)/mig-mutate.h

OBJS = $(subst .c,.o,$(SRCS)) $(MIGSTUBS)

include ../Makeconf
