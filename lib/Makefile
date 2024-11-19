NS_STD_SRCS = lib/src/io.c lib/src/gui.c lib/src/gpu.c
NS_STD_OBJS = $(NS_STD_SRCS:%.c=%.o)
NS_STD_LIBS = $(NS_STD_SRCS:%.c=%$(NS_LIB_SUFFIX))

NS_LIB_CFLAGS = $(NS_CFLAGS)

$(NS_STD_LIBS): %$(NS_LIB_SUFFIX): %.o
	$(CC) -shared $< -o $@ $(NS_LIB_CFLAGS)

$(NS_STD_OBJS): $(OBJDIR)/%.o : std/%.c | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_LIB_CFLAGS)

std: $(NS_STD_LIBS)
