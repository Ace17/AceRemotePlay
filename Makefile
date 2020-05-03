BIN?=bin

all: all_targets

CROSS_COMPILE?=
ifneq (,$(CROSS_COMPILE))
CXX:=$(CROSS_COMPILE)g++
endif

HOST:=$(shell $(CXX) -dumpmachine | sed 's/.*-//')

TARGETS:=

-include $(HOST).mk

PKGS:=\
	sdl2\
	libavcodec\
	libavformat\
	libavutil\
	libswresample\
	libswscale\

SRCS_PLAYER:=\
	src/aceplayer/player.cpp\
	src/aceplayer/main.cpp\
	$(SRCS_COMMON)\

SRCS_ECHOSERVER:=\
	src/echoserver/main.cpp\
	$(SRCS_COMMON)\

SRCS_HOOK:=\
	src/acehook/acehook.cpp\
	src/acehook/streamer.cpp\
	$(SRCS_COMMON)\

CXXFLAGS+=-Wno-deprecated-declarations
CXXFLAGS+=-Wall
CXXFLAGS+=-Wextra
CXXFLAGS+=-Werror

CXXFLAGS+=-Isrc
CXXFLAGS+=-fPIC

# CXXFLAGS+=-g3
# LDFLAGS+=-g
LDFLAGS+=-s

CXXFLAGS+=$(shell pkg-config $(PKGS) --cflags)
LDFLAGS+=$(shell pkg-config $(PKGS) --libs)

TARGETS+=$(BIN)/aceplayer.exe
$(BIN)/aceplayer.exe: $(SRCS_PLAYER:%=$(BIN)/%.o)

TARGETS+=$(BIN)/echoserver.exe
$(BIN)/echoserver.exe: $(SRCS_ECHOSERVER:%=$(BIN)/%.o)

TARGETS+=$(BIN)/acehook.so
$(BIN)/acehook.so: $(SRCS_HOOK:%=$(BIN)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) -shared -s -o "$@" $^ $(LDFLAGS)

###############################################################################

$(BIN)/%.exe:
	@mkdir -p $(dir $@)
	$(CXX) -o "$@" $^ $(LDFLAGS)


$(BIN)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@$(CXX) -MM -MT "$@" -c -o "$(BIN)/$*.cpp.o.dep" $< $(CXXFLAGS)
	$(CXX) -c -o "$@" $< $(CXXFLAGS)

-include $(shell test -d $(BIN) && find $(BIN) -name "*.dep")

clean:
	rm -rf $(BIN)

all_targets: $(TARGETS)

