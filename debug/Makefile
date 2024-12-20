NS_DAP_LDFLAGS = $(NS_LDFLAGS) -L$(BINDIR) -lns
NS_DAP_TARGET = $(TARGET)_debug

NS_DAP_INC = $(NS_INC) -Idebug/include

NS_DAP_SRCS = debug/src/ns_debug.c debug/src/ns_debug_protocol.c debug/src/ns_debug_repl.c
NS_DAP_OBJS = $(NS_DAP_SRCS:debug/src/%.c=$(OBJDIR)/debug/%.o)

NS_DAP_BINDIR = $(BINDIR)/debug

ns_debug: $(NS_DAP_TARGET)
	@echo "building ns_debug at "$(OS)" with options:" \
	"NS_DEBUG=$(NS_DEBUG)"

$(NS_DAP_TARGET): $(NS_DAP_OBJS) | $(NS_DAP_BINDIR) lib
	$(NS_LD) $(NS_DAP_OBJS) $ -o $(NS_DAP_TARGET)$(NS_SUFFIX) $(NS_DAP_LDFLAGS)

$(NS_DAP_OBJS): $(OBJDIR)/debug/%.o : debug/src/%.c | $(OBJDIR)
	$(NS_CC) -c $< -o $@ $(NS_DAP_INC) $(NS_CFLAGS)

$(NS_DAP_BINDIR):
	$(NS_MKDIR) $(NS_DAP_BINDIR)

run_debug: $(NS_DAP_TARGET)
	$(NS_DAP_TARGET)$(NS_SUFFIX)