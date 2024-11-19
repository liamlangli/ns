NS_LSP_LDFLAGS = -L$(BINDIR) -lns
NS_LSP_TARGET = $(TARGET)_lsp

NS_LSP_CFLAGS = $(NS_CFLAGS) -Ilsp/include

NS_LSP_SRCS = lsp/src/ns_lsp.c lsp/src/ns_json.c
NS_LSP_OBJS = $(NS_LSP_SRCS:lsp/%.c=$(OBJDIR)/%.o)

lsp: $(NS_LSP_TARGET) lib
	@echo "building lsp at "$(OS)" with options:" \
	"NS_DEBUG=$(NS_DEBUG)"

$(NS_LSP_TARGET): $(NS_LSP_OBJS) | $(BINDIR)
	$(CC) $(NS_LSP_OBJS) $ -o $(NS_LSP_TARGET)$(NS_SUFFIX) $(NS_LSP_LDFLAGS)

$(NS_LSP_OBJS): $(OBJDIR)/%.o : lsp/%.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_LSP_CFLAGS)

run_lsp: $(NS_LSP_TARGET)
	$(NS_LSP_TARGET)$(NS_SUFFIX)