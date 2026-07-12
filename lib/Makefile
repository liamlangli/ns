NS_LIB_INC = $(NS_INC) -Ilib/include

ifeq ($(NS_OS), $(NS_DARWIN))
	NS_DYLIB_CREATE = -dynamiclib
	NS_DYLIB_COMMON = -fPIC -g -Wl,-install_name,@rpath/$(@F) -Wl,-rpath,@loader_path -undefined dynamic_lookup
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
	NS_OS_DYLIB_DEPS = $(NS_DYLIB_DEPS) $(NS_COCOA_LDFLAGS)
	NS_VIEW_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_COCOA_LDFLAGS) $(NS_METAL_LDFLAGS)
	NS_GPU_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_METAL_LDFLAGS)
else ifeq ($(NS_OS), $(NS_LINUX))
	NS_DYLIB_CREATE = -shared
	NS_DYLIB_COMMON = -fPIC -g -Wl,-soname,$(@F) -Wl,-rpath,'$$ORIGIN'
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
else
	NS_DYLIB_CREATE = -shared
	NS_DYLIB_COMMON = -fPIC -g
	NS_DYLIB_DEPS = $(NS_LDFLAGS)
	NS_OS_DYLIB_DEPS = $(NS_DYLIB_DEPS) -lcomdlg32
	NS_GPU_DYLIB_DEPS = $(NS_LDFLAGS) $(NS_GPU_LDFLAGS)
endif

NS_OS_DYLIB_DEPS ?= $(NS_DYLIB_DEPS)
NS_VIEW_DYLIB_DEPS ?= $(NS_DYLIB_DEPS)
NS_GPU_DYLIB_DEPS ?= $(NS_DYLIB_DEPS)
NS_UI_DYLIB_DEPS ?= $(NS_GPU_DYLIB_DEPS)

# Position-independent compile rules for the C and Objective-C feature sources.
$(NS_BINDIR)/lib/%.o: lib/src/%.c
	$(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

$(NS_BINDIR)/lib/%.o: lib/src/%.m
	$(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

NS_IO_DYLIB = $(NS_BINDIR)/io$(NS_DYLIB_SUFFIX)
NS_OS_DYLIB = $(NS_BINDIR)/os$(NS_DYLIB_SUFFIX)
NS_NET_DYLIB = $(NS_BINDIR)/net$(NS_DYLIB_SUFFIX)
NS_HTTP_DYLIB = $(NS_BINDIR)/http$(NS_DYLIB_SUFFIX)
NS_TERM_DYLIB = $(NS_BINDIR)/term$(NS_DYLIB_SUFFIX)
NS_VIEW_DYLIB = $(NS_BINDIR)/view$(NS_DYLIB_SUFFIX)
NS_GPU_DYLIB = $(NS_BINDIR)/gpu$(NS_DYLIB_SUFFIX)
NS_UI_DYLIB = $(NS_BINDIR)/ui$(NS_DYLIB_SUFFIX)

NS_IO_OBJS = $(NS_BINDIR)/lib/io.o
NS_OS_OBJS = $(NS_BINDIR)/lib/os.o
NS_NET_OBJS = $(NS_BINDIR)/lib/net.o
NS_HTTP_OBJS = $(NS_BINDIR)/lib/http.o
NS_VIEW_OBJS = $(NS_BINDIR)/lib/view.o
NS_GPU_OBJS = $(NS_BINDIR)/lib/gpu.o
NS_UI_OBJS = $(NS_BINDIR)/lib/ui.o

ifeq ($(NS_OS), $(NS_LINUX))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.linux.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.posix.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.linux.o
else ifeq ($(NS_OS), $(NS_DARWIN))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.osx.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.posix.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.osx.o
	NS_GPU_OBJS += $(NS_BINDIR)/lib/gpu.metal.o
else ifeq ($(NS_OS), $(NS_WIN))
	NS_OS_OBJS += $(NS_BINDIR)/lib/os.win.o
	NS_TERM_OBJS = $(NS_BINDIR)/lib/term.win.o
	NS_VIEW_OBJS += $(NS_BINDIR)/lib/view.win.o
	NS_GPU_OBJS += $(NS_BINDIR)/lib/gpu.dx12.o
endif

NS_STD_SHARED = \
	$(NS_IO_DYLIB) \
	$(NS_OS_DYLIB) \
	$(NS_NET_DYLIB) \
	$(NS_HTTP_DYLIB) \
	$(NS_TERM_DYLIB) \
	$(NS_GPU_DYLIB) \
	$(NS_VIEW_DYLIB) \
	$(NS_UI_DYLIB)

std: $(NS_STD_SHARED)

shared: std

$(NS_IO_DYLIB): $(NS_IO_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_OS_DYLIB): $(NS_OS_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_OS_DYLIB_DEPS)

$(NS_NET_DYLIB): $(NS_NET_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_HTTP_DYLIB): $(NS_HTTP_OBJS) $(NS_NET_DYLIB) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_HTTP_OBJS) $(NS_NET_DYLIB) -o $@ $(NS_DYLIB_DEPS)

$(NS_TERM_DYLIB): $(NS_TERM_OBJS) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $^ -o $@ $(NS_DYLIB_DEPS)

$(NS_GPU_DYLIB): $(NS_GPU_OBJS) $(if $(filter $(NS_WIN),$(NS_OS)),$(NS_VIEW_DYLIB),) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_GPU_OBJS) $(if $(filter $(NS_WIN),$(NS_OS)),$(NS_VIEW_DYLIB),) -o $@ $(NS_GPU_DYLIB_DEPS)

$(NS_VIEW_DYLIB): $(NS_VIEW_OBJS) $(if $(filter $(NS_DARWIN),$(NS_OS)),$(NS_GPU_DYLIB),) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_VIEW_OBJS) $(if $(filter $(NS_DARWIN),$(NS_OS)),$(NS_GPU_DYLIB),) -o $@ $(NS_VIEW_DYLIB_DEPS)

$(NS_UI_DYLIB): $(NS_UI_OBJS) $(NS_IO_DYLIB) $(NS_GPU_DYLIB) | $(NS_BINDIR)
	$(NS_CC) $(NS_DYLIB_CREATE) $(NS_DYLIB_COMMON) $(NS_UI_OBJS) $(NS_IO_DYLIB) $(NS_GPU_DYLIB) -o $@ $(NS_UI_DYLIB_DEPS)
