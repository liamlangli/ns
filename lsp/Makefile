NS_LSP_LDFLAGS = -L$(BINDIR) -lns
NS_LSP_TARGET = $(TARGET)_lsp

NS_LSP_CFLAGS = $(NS_CFLAGS) -Ilsp/include

NS_LSP_SRCS = lsp/src/ns_lsp.c lsp/src/ns_json.c
NS_LSP_OBJS = $(NS_LSP_SRCS:lsp/src/%.c=$(OBJDIR)/lsp/%.o)

NS_LSP_BINDIR = $(BINDIR)/lsp

lsp: $(NS_LSP_TARGET) lib
	@echo "building lsp at "$(OS)" with options:" \
	"NS_DEBUG=$(NS_DEBUG)"

$(NS_LSP_TARGET): $(NS_LSP_OBJS) | $(NS_LSP_BINDIR)
	$(CC) $(NS_LSP_OBJS) $ -o $(NS_LSP_TARGET)$(NS_SUFFIX) $(NS_LSP_LDFLAGS)

$(NS_LSP_OBJS): $(OBJDIR)/lsp/%.o : lsp/src/%.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_LSP_CFLAGS)

$(NS_LSP_BINDIR):
	mkdir -p $(NS_LSP_BINDIR)

run_lsp: $(NS_LSP_TARGET)
	$(NS_LSP_TARGET)$(NS_SUFFIX)