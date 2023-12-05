#include "ns.h"

#include <stdlib.h>

typedef struct ns_runtime_t {
  int ref_count;
} ns_runtime_t;

typedef struct ns_context_t {
  ns_value this;
} ns_context_t;

ns_runtime_t *ns_make_runtime() {
  ns_runtime_t *rt = (ns_runtime_t *)malloc(sizeof(ns_runtime_t));
  rt->ref_count = 0;
  return rt;
}

ns_context_t *ns_make_context(ns_runtime_t *rt) {
  ns_context_t *ctx = (ns_context_t *)malloc(sizeof(ns_context_t));
  ctx->this = NS_NULL;
  return ctx;
}

int ns_is_null(ns_value value) {
  return NS_VALUE_GET_TAG(value) == NS_TAG_NULL;
}

ns_value ns_eval(ns_context_t *ctx, ns_string content, ns_string filename,
int flag) {
  if (content.length == 0) return NS_NULL;

  int c;
  const char *buff = content.data;

  // c = skip();
  return NS_NULL;
}