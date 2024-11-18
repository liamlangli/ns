CC = clang

OS := $(shell uname -s 2>/dev/null || echo Windows)
NS_LIB_SUFFIX =
NS_PLATFORM_DEF=
ifeq ($(OS), Linux)
	NS_LIB_SUFFIX = .so
	NS_PLATFORM_DEF = -DNS_LINUX
else ifeq ($(OS), Darwin)
	NS_LIB_SUFFIX = .dylib
	NS_PLATFORM_DEF = -DNS_DARWIN
else ifeq ($(OS), Windows)
	NS_LIB_SUFFIX = .dll
	NS_PLATFORM_DEF = -DNS_WIN32
else
	$(error Unsupported platform: $(OS))
endif

LIB_SRC = io.c gui.c gpu.c
LIB_OBJ = $(LIB_SRC:%.c=%.o)
LIBS = $(LIB_SRC:%.c=%$(NS_LIB_SUFFIX))

LIB_FLAGS = -I../include -I. -g -O0 -Wall -Wextra

all: $(LIBS)

$(LIBS): %$(NS_LIB_SUFFIX): %.o
	$(CC) -shared $< -o $@ $(LIB_FLAGS)

%.o: %.c
	$(CC) -fvisibility=hidden -c $< -o $@ $(LIB_FLAGS)

clean:
	rm -f $(LIB_OBJ) $(LIBS)
