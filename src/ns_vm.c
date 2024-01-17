#include "ns_vm.h"
#include "ns_parse.h"
#include "ns_type.h"

#include <stdlib.h>

ns_value ns_eval(const char* source, const char *filename) {
    ns_parse_context_t *ctx = ns_parse(source, filename);
    return NS_NIL;
}