# ns/Makefile
# Build/run the self-hosted ("ns in ns") front-end and its unit tests.
#
# Nano Script's `use` only imports modules from the interpreter's lib path, so
# there is no local file import. We therefore "link" multiple .ns sources the
# simplest way that works: concatenation. Each runnable program is built by
# concatenating a `use std` header, the lexer module, and a driver/test body
# into bin/, then executed with the repo's ns interpreter.

# This Makefile lives in and runs from ns/. The interpreter, however, resolves
# `use std` relative to ITS working directory (lib/<name>.ns), so it is invoked
# from the repo root (cd ..) on the concatenated program in ns/bin/.
# NS: interpreter path relative to repo root. BIN: output dir relative to ns/.
# (Keep these free of trailing inline comments - make would fold the spaces
#  into the value.)
NS ?= bin/ns
BIN = bin
LEXER = lexer.ns
HEADER = use std

.PHONY: all test demo clean

all: test

$(BIN):
	mkdir -p $(BIN)

# Concatenate header + lexer + harness + tests, then run from the repo root.
test: | $(BIN)
	@printf '%s\n' '$(HEADER)' > $(BIN)/_test.ns
	@cat $(LEXER) test/harness.ns test/test_lexer.ns >> $(BIN)/_test.ns
	@cd .. && $(NS) ns/$(BIN)/_test.ns

# Concatenate header + lexer + demo driver, then run from the repo root.
demo: | $(BIN)
	@printf '%s\n' '$(HEADER)' > $(BIN)/_demo.ns
	@cat $(LEXER) demo_main.ns >> $(BIN)/_demo.ns
	@cd .. && $(NS) ns/$(BIN)/_demo.ns

clean:
	rm -rf $(BIN)
