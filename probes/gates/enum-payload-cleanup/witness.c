#include <stdint.h>
static int32_t created, destroyed;
void witness_create(void) { ++created; }
void witness_destroy(void) { ++destroyed; }
int32_t witness_created(void) { return created; }
int32_t witness_destroyed(void) { return destroyed; }
