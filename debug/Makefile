NS_DAP_LDFLAGS = -lreadline `pkg-config --libs libffi` -ldl -flto -L/usr/lib
NS_DAP_TARGET = $(TARGET)_debug

NS_DAP_CFLAGS = $(NS_CFLAGS) -Idebug/include

NS_DAP_SRCS = debug/src/ns_debug.c debug/src/ns_debug_protocol.c debug/src/ns_debug_repl.c
NS_DAP_OBJS = $(NS_DAP_SRCS:debug/src/%.c=$(OBJDIR)/debug/%.o)

NS_DAP_BINDIR = $(BINDIR)/debug

ns_debug: $(NS_DAP_TARGET)
	@echo "building ns_debug at "$(OS)" with options:" \
	"NS_DEBUG=$(NS_DEBUG)"

$(NS_DAP_TARGET): $(NS_DAP_OBJS) | $(NS_DAP_BINDIR) lib
	$(CC) $(NS_DAP_OBJS) $(BINDIR)/libns.a $ -o $(NS_DAP_TARGET)$(NS_SUFFIX) $(NS_DAP_LDFLAGS)

$(NS_DAP_OBJS): $(OBJDIR)/debug/%.o : debug/src/%.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_DAP_CFLAGS)

$(NS_DAP_BINDIR):
	mkdir -p $(NS_DAP_BINDIR)

run_debug: $(NS_DAP_TARGET)
	$(NS_DAP_TARGET)$(NS_SUFFIX)