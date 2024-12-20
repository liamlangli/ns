NS_DAP_LDFLAGS = $(NS_LDFLAGS) -L$(NS_BINDIR) -lns
NS_DAP_TARGET = $(TARGET)_debug

NS_DAP_INC = $(NS_INC) -Idebug/include

NS_DAP_SRCS = debug/src/ns_debug.c debug/src/ns_debug_protocol.c debug/src/ns_debug_repl.c
NS_DAP_OBJS = $(NS_DAP_SRCS:debug/src/%.c=$(NS_BINDIR)/debug/%.o)

ns_debug: $(NS_DAP_TARGET)

$(NS_DAP_TARGET): $(NS_DAP_OBJS) | $(NS_LIB)
	$(NS_LD) $(NS_DAP_OBJS) $ -o $(NS_DAP_TARGET)$(NS_SUFFIX) $(NS_DAP_LDFLAGS)

$(NS_DAP_OBJS): $(NS_BINDIR)/debug/%.o : debug/src/%.c
	$(NS_CC) -c $< -o $@ $(NS_DAP_INC) $(NS_CFLAGS)

run_debug: $(NS_DAP_TARGET)
	$(NS_DAP_TARGET)$(NS_SUFFIX)