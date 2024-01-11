#pragma one

#ifdef __cplusplus
extern "C" {
#endif

#include "ns_tokenize.h"

typedef struct ns_ast_t {
    ns_token_t *token;
} ns_ast_t;

ns_ast_t* ns_parse(const char *source, const char *filename);

#ifdef __cplusplus
} // extern "C"
#endif