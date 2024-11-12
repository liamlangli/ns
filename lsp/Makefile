# nslang lsp Makefile

# FLAGS
MAKEFLAGS += --no-print-directory -j

# PLATFORNM
NS_SUFFIX =
ifeq ($(OS),Windows_NT)
	NS_WIN32 = 1
	NS_SUFFIX = .exe
endif

# OPTIONS
NS_DEBUG ?= 1

# VARIABLES
CC = clang

NS_DEBUG_CFLAGS = -Iinclude -g -O0 -Wall -Wextra -DNS_DEBUG
NS_RELEASE_CFLAGS = -Iinclude -Os -I../include

ifeq ($(NS_DEBUG), 1)
	NS_CFLAGS = $(NS_DEBUG_CFLAGS)
else
	NS_CFLAGS = $(NS_RELEASE_CFLAGS)
endif

BINDIR = bin
OBJDIR = $(BINDIR)

NS_LSP_SRCS = src/ns_lsp.c
NS_LSP_OBJS = $(NS_LSP_SRCS:%.c=$(OBJDIR)/%.o)

TARGET = $(BINDIR)/ns_lsp

all: $(TARGET)
	@echo "building ns_lsp with options:" \
	"NS_BITCODE=$(NS_BITCODE)" \
	"NS_DEBUG=$(NS_DEBUG)"

$(TARGET): $(NS_LSP_OBJS) | $(BINDIR)
	$(CC) $(NS_LSP_OBJS) $ -o $(TARGET)$(NS_SUFFIX) $(NS_LDFLAGS)

$(NS_LSP_OBJS): $(NS_LSP_SRCS) | $(OBJDIR)
	$(CC) -c $< -o $@ $(NS_CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)/src

clean:
	rm -rf $(OBJDIR)