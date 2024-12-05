NS_DAP_LDFLAGS =
NS_DAP_TARGET = $(TARGET)_dap

NS_DAP_CFLAGS = $(NS_CFLAGS) -Idap/include

NS_DAP_SRCS = dap/src/ns_dap.c
NS_DAP_OBJS = $(NS_DAP_SRCS:dap/src/%.c=$(OBJDIR)/dap/%.o)

NS_DAP_BINDIR = $(BINDIR)/dap

dap: $(NS_DAP_TARGET)
	@echo "building dap at "$(OS)" with options:" \
	"NS_DEBUG=$(NS_DEBUG)"

$(NS_DAP_TARGET): $(NS_DAP_OBJS) | $(NS_DAP_BINDIR) lib
	$(CC) $(NS_DAP_OBJS) $(BINDIR)/libns.a $ -o $(NS_DAP_TARGET)$(NS_SUFFIX) $(NS_DAP_LDFLAGS)

$(NS_DAP_OBJS): $(OBJDIR)/dap/%.o : dap/src/%.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_DAP_CFLAGS)

$(NS_DAP_BINDIR):
	mkdir -p $(NS_DAP_BINDIR)

run_dap: $(NS_DAP_TARGET)
	$(NS_DAP_TARGET)$(NS_SUFFIX)