#include <string.h>

#include "ns.h"

enum NS_TOKEN { NS_TOKEN_LET, NS_TOKEN_PLUS, NS_TOKEN_MINUS, NS_TOKEN_STAR, NS_TOKEN_SLASH, NS_TOKEN_INT_LITERAL };

typedef struct ns_token_t {
  int type;
  int value;
} ns_token_t;

ns_value scan(ns_context_t* ctx, ns_string content, ns_string filename, int flag) {
  if (content.length == 0) return NS_NULL;

  const char* buffer = content.data;

  int i;
  char c;

  i = 0;
  c = buffer[i];
  while (c != EOF) {
    switch (i) {

    }
  }
  // c = skip();
  return NS_NULL;
}
