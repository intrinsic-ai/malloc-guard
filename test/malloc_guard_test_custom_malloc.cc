#include <stddef.h>

static char dummy_buffer[1024];

extern "C" void* malloc(size_t size) { return dummy_buffer; }

extern "C" void free(void* ptr) {}

extern "C" void* GetDummyBuffer() { return dummy_buffer; }
