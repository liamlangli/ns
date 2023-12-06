#include "ns.h"

#include <stdlib.h>

void usage() {
  const char *intro =
      "nano script interpreter\n"
      "version: v0.0.1\n\n"
      "usage: nsc [-i] [filename]\n"
      "command:\n"
      "   -i interactive mode\n";
  fprintf(stdout, "%s", intro);
}

void repl() { printf("nano script repl mode.\n>"); }

ns_string read_file(ns_string filename) {
  FILE *fd = fopen(filename.data, "rb");
  if (!fd) return ns_string_str("");

  fseek(fd, 0, SEEK_END);
  long length = ftell(fd);
  if (length == 0) return (ns_string){.length = 0};

  fseek(fd, 0, SEEK_SET);
  void *buffer = (void*)malloc(length + 1);
  if (buffer) {
    fread((void*)buffer, 1, length, fd);
  }
  fclose(fd);

  return (ns_string){.data = (str)buffer, .length = (i32)length, .null_end = 1};
}

void eval(ns_string filename) {
  ns_runtime_t *rt = ns_make_runtime();
  ns_context_t *ctx = ns_make_context(rt);

  ns_string source = read_file(filename);
  if (source.length == 0) return;

  ns_eval(ctx, source, filename, NS_EVAL_FLAG_VALIDATE_ONLY);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 0;
  }

  if (strcmp(argv[1], "-i") == 0) {
    repl();
  } else {
    eval(ns_string_str(argv[1]));
  }
  return 0;
}
