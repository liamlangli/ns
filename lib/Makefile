NS_LIB_INC = $(NS_INC) -Ilib/include

# The standard library is compiled position-independent (-fPIC) and linked
# statically into the ns binary (see NS_LIBFN_OBJS in the top-level Makefile).
# `ref fn` symbols are resolved by the compiled-in table in ns_vm_lib.c rather
# than dlopen()/dlsym(), so there is no shared object to map or relocate at
# startup. `make std` simply ensures those objects are built.

# Position-independent compile rules for the C and Objective-C lib sources.
$(NS_BINDIR)/lib/%.o: lib/src/%.c
	$(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

$(NS_BINDIR)/lib/%.o: lib/src/%.m
	$(NS_CC) -c -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS)

std: $(NS_LIBFN_OBJS)

# Optional: build the standard library as shared objects (legacy dynamic
# loading). Not used by the default build, kept for embedding scenarios.
NS_STD_SHARED = $(NS_LIBFN_OBJS:$(NS_BINDIR)/lib/%.o=$(NS_BINDIR)/%$(NS_DYLIB_SUFFIX))

$(NS_BINDIR)/%$(NS_DYLIB_SUFFIX): $(NS_BINDIR)/lib/%.o
	$(NS_CC) -shared -fPIC -g $< -o $@ $(NS_LIB_INC) $(NS_CFLAGS) $(NS_LIB_LDFLAGS)

shared: $(NS_STD_SHARED)
