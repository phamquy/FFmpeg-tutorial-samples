# use pkg-config for getting CFLAGS and LDLIBS
FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil                          \

CFLAGS += -Wall -O2 -g
CFLAGS := $(shell /usr/local/bin/pkg-config --cflags $(FFMPEG_LIBS)) $(CFLAGS)
CFLAGS := $(shell /usr/X11/bin/freetype-config --cflags) $(CFLAGS)
CFLAGS := $(shell /usr/local/bin/sdl-config --cflags) $(CFLAGS)

LDLIBS := $(shell /usr/local/bin/pkg-config --libs $(FFMPEG_LIBS)) $(LDLIBS)
LDLIBS := $(shell /usr/X11/bin/freetype-config --libs) $(LDLIBS)
LDLIBS := $(shell /usr/local/bin/sdl-config --libs) $(LDLIBS)

EXAMPLES=       tutorial01                         \
                tutorial02                         \
                tutorial03                         \
                tutorial04                         \
                tutorial05                         \
                tutorial06                         \
                tutorial07

OBJS=$(addsuffix .o,$(EXAMPLES))

# the following examples make explicit use of the math library
decoding_encoding: LDLIBS += -lm
muxing:            LDLIBS += -lm

.phony: all clean-test clean

all: $(OBJS) $(EXAMPLES)

clean-test:
	$(RM) test*.pgm test.h264 test.mp2 test.sw test.mpg

clean: clean-test
	$(RM) $(EXAMPLES) $(OBJS)
