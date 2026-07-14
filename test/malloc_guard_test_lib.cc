#include <stdlib.h>

extern "C" void* TestMalloc(size_t size) { return malloc(size); }
extern "C" void TestFree(void* ptr) { free(ptr); }
