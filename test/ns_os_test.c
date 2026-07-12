#include "ns_test.h"
#include "ns_os.h"

int main(void) {
    ns_str path = ns_str_cstr("test/ns_os_test.c");
    i64 size = ns_os_file_size(path);
    ns_expect(size > 8, "ns_os_file_size returns the file size.");

    ns_str prefix = ns_os_read_file_part(path, 0, 8);
    ns_expect(prefix.data != ns_null && ns_str_equals(prefix, ns_str_cstr("#include")),
              "ns_os_read_file_part reads the requested range.");

    ns_str tail = ns_os_read_file_part(path, size - 4, 100);
    ns_expect(tail.data != ns_null && tail.len == 4,
              "ns_os_read_file_part truncates a range at EOF.");

    ns_str invalid = ns_os_read_file_part(path, size + 1, 1);
    ns_expect(invalid.data == ns_null, "ns_os_read_file_part rejects offsets beyond EOF.");

    ns_str missing = ns_str_cstr("test/does-not-exist");
    ns_expect(ns_os_file_size(missing) == -1, "ns_os_file_size reports a missing file.");
    ns_expect(ns_os_read_file(missing).data == ns_null, "ns_os_read_file reports a missing file.");

    ns_str_free(prefix);
    ns_str_free(tail);
    return 0;
}
