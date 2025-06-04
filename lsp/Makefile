NS_LSP_LDFLAGS = -L$(NS_BINDIR) -lns
NS_LSP_TARGET = $(TARGET)_lsp

NS_LSP_INC = $(NS_INC) -Ilsp/include

NS_LSP_SRCS = lsp/src/ns_lsp.c
NS_LSP_OBJS = $(NS_LSP_SRCS:lsp/src/%.c=$(NS_BINDIR)/lsp/%.o)

ns_lsp: $(NS_LSP_TARGET)

ns_lsp_client: $(NS_LSP_CLIENT_TARGET)

$(NS_LSP_TARGET): $(NS_LSP_OBJS) | $(NS_LIB)
	$(NS_LD) $(NS_LSP_OBJS) $ -o $(NS_LSP_TARGET)$(NS_SUFFIX) $(NS_LSP_LDFLAGS)

$(NS_LSP_OBJS): $(NS_BINDIR)/lsp/%.o : lsp/src/%.c
	$(NS_CC) -c $< -o $@ $(NS_LSP_INC) $(NS_CFLAGS)

run_lsp: $(NS_LSP_TARGET)
	$(NS_LSP_TARGET)$(NS_SUFFIX)
