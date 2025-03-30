# Source and target directories
SAMPLE_DIR = sample/c
SAMPLE_CFLAGS = $(NS_CFLAGS) -Iinclude
SAMPLE_LINKS = -L$(NS_BINDIR) -lns -lffi
SAMPLE_BINDIR = bin/sample

# Create a list of all C source files and corresponding object files
SAMPLE_C_SRCS = $(wildcard $(SAMPLE_DIR)/*.c)

# Create a list of all binary executables
SAMPLE_C_BINS = $(patsubst $(SAMPLE_DIR)/%.c, $(NS_BINDIR)/sample/%, $(SAMPLE_C_SRCS))

# Rule to build each binary executable
$(NS_BINDIR)/sample/%: $(SAMPLE_DIR)/%.c | $(SAMPLE_BINDIR)
	$(CC) $(SAMPLE_CFLAGS) $(SAMPLE_LINKS) -o $@ $<

$(SAMPLE_BINDIR):
	mkdir -p $(SAMPLE_BINDIR)

# Target to build all binaries
sample: $(SAMPLE_C_BINS) | all