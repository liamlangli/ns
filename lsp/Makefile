NS_LSP_LDFLAGS = -L$(BINDIR) -lns
NS_LSP_TARGET = $(TARGET)_lsp

NS_LSP_INC = $(NS_INC) -Ilsp/include

NS_LSP_SRCS = lsp/src/ns_lsp.c
NS_LSP_OBJS = $(NS_LSP_SRCS:lsp/src/%.c=$(OBJDIR)/lsp/%.o)

NS_LSP_BINDIR = $(BINDIR)/lsp

ns_lsp: $(NS_LSP_TARGET)
	@echo "building ns_lsp at "$(OS)" with options:" \
	"NS_DEBUG=$(NS_DEBUG)"

$(NS_LSP_TARGET): $(NS_LSP_OBJS) | $(NS_LSP_BINDIR) lib
	$(NS_CC) $(NS_LSP_OBJS)  $ -o $(NS_LSP_TARGET)$(NS_SUFFIX) $(NS_LSP_LDFLAGS)

$(NS_LSP_OBJS): $(OBJDIR)/lsp/%.o : lsp/src/%.c | $(OBJDIR)
	$(NS_CC) -c $< -o $@ $(NS_LSP_INC) $(NS_CFLAGS)

$(NS_LSP_BINDIR):
	$(NS_MKDIR) $(NS_LSP_BINDIR)

run_lsp: $(NS_LSP_TARGET)
	$(NS_LSP_TARGET)$(NS_SUFFIX)