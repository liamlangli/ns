NS_STD_SRCS = lib/src/io.c lib/src/gui.c lib/src/gpu.c
NS_STD_OBJS = $(NS_STD_SRCS:lib/src/%.c=$(OBJDIR)/lib/%.o)
NS_STD_LIBS = $(NS_STD_SRCS:lib/src/%.c=$(OBJDIR)/%$(NS_LIB_SUFFIX))

NS_LIB_CFLAGS = $(NS_CFLAGS) -Ilib/include

NS_LIB_BINDIR = $(BINDIR)/lib

$(NS_STD_LIBS): $(OBJDIR)/%$(NS_LIB_SUFFIX) : $(OBJDIR)/lib/%.o | $(NS_LIB_BINDIR)
	$(CC) -shared $< -o $@ $(NS_LIB_CFLAGS)

$(NS_STD_OBJS): $(OBJDIR)/lib/%.o : lib/src/%.c | $(NS_LIB_BINDIR)
	$(CC) -c $< -o $@ $(NS_LIB_CFLAGS)

$(NS_LIB_BINDIR):
	mkdir -p $(NS_LIB_BINDIR)

std: $(NS_STD_LIBS)
