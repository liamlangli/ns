NS_LIB_INC = $(NS_INC) -Ilib/include

NS_STD_SRCS = lib/src/io.c lib/src/gpu.c
NS_STD_OBJS = $(NS_STD_SRCS:lib/src/%.c=$(NS_BINDIR)/lib/%.o)
NS_STD_LIBS = $(NS_STD_SRCS:lib/src/%.c=$(NS_BINDIR)/%$(NS_DYLIB_SUFFIX))

$(NS_STD_LIBS): $(NS_BINDIR)/%$(NS_DYLIB_SUFFIX) : $(NS_BINDIR)/lib/%.o
	$(NS_CC) -shared -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS) $(NS_LIB_LDFLAGS)

$(NS_STD_OBJS): $(NS_BINDIR)/lib/%.o : lib/src/%.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC)

ifeq ($(NS_OS),darwin)
bin/lib/view.osx.o: lib/src/view.osx.m
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/lib/view.o: lib/src/view.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/view.dylib: bin/lib/view.o bin/lib/view.osx.o
	$(NS_CC) -shared -fPIC -g $^ -o $@ -Ilib/include -framework Cocoa -framework Metal -framework MetalKit

bin/lib/os.osx.o: lib/src/os.osx.m
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/lib/os.o: lib/src/os.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/os.dylib: bin/lib/os.o bin/lib/os.osx.o
	$(NS_CC) -shared -fPIC -g $^ -o $@ -Ilib/include -framework Cocoa -framework Metal -framework MetalKit

std: $(NS_STD_LIBS) bin/os.dylib bin/view.dylib

else ifeq ($(NS_OS),linux)
bin/lib/view.linux.o: lib/src/view.linux.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/lib/view.o: lib/src/view.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/view.so: bin/lib/view.o bin/lib/view.linux.o
	$(NS_CC) -shared -fPIC -g $^ -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/lib/os.linux.o: lib/src/os.linux.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/lib/os.o: lib/src/os.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/os.so: bin/lib/os.o bin/lib/os.linux.o
	$(NS_CC) -shared -fPIC -g $^ -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

std: $(NS_STD_LIBS) bin/os.so bin/view.so
else ifeq ($(NS_OS),windows)
bin/lib/view.win.o: lib/src/view.win.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/lib/view.o: lib/src/view.c
	$(NS_CC) -c -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/libview.a: bin/lib/view.o bin/lib/view.win.o
	$(NS_CC) -shared -fPIC -g $^ -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

bin/libos.a: lib/src/os.c lib/src/os.win.c
	$(NS_CC) -shared -fPIC -g $^ -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

std: $(NS_STD_LIBS) bin/libos.a bin/libview.a
else 
std:
	@echo "Unsupported OS: $(OS)"
endif
