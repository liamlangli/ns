NS_LIB_CFLAGS = $(NS_CFLAGS) -Ilib/include
NS_LIB_BINDIR = $(BINDIR)/lib

NS_STD_SRCS = lib/src/io.c lib/src/gpu.c
NS_STD_OBJS = $(NS_STD_SRCS:lib/src/%.c=$(OBJDIR)/lib/%.o)
NS_STD_LIBS = $(NS_STD_SRCS:lib/src/%.c=$(OBJDIR)/%$(NS_LIB_SUFFIX))

$(NS_STD_LIBS): $(OBJDIR)/%$(NS_LIB_SUFFIX) : $(OBJDIR)/lib/%.o | $(NS_LIB_BINDIR)
	$(CC) -shared $< -o $@ $(NS_LIB_CFLAGS)

$(NS_STD_OBJS): $(OBJDIR)/lib/%.o : lib/src/%.c | $(NS_LIB_BINDIR)
	$(CC) -c $< -o $@ $(NS_LIB_CFLAGS)

$(NS_LIB_BINDIR):
	mkdir -p $(NS_LIB_BINDIR)

ifeq ($(NS_OS),darwin)
bin/lib/os.osx.o: lib/src/os.osx.m
	clang -c $< -o $@ -Ilib/include -Iinclude

bin/lib/os.o: lib/src/os.c
	$(CC) -c $< -o $@ $(NS_LIB_CFLAGS)

bin/os.dylib: bin/lib/os.o bin/lib/os.osx.o
	clang -shared $^ -o $@ -Ilib/include -framework Cocoa -framework Metal -framework MetalKit

std: $(NS_STD_LIBS) bin/os.dylib

else ifeq ($(NS_OS),linux)

bin/lib/os.linux.o: lib/src/os.linux.c
	$(CC) -c $< -o $@ $(NS_LIB_CFLAGS)

bin/os.so: bin/lib/os.linux.o
	$(CC) -shared $< -o $@ $(NS_LIB_CFLAGS) -ldl

else ifeq ($(NS_OS),windows)

else 
std:
	@echo "Unsupported OS: $(OS)"
endif