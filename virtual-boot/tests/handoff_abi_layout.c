/* Compile-time ABI layout regression test. No runtime handoff is performed. */
#include "../include/a20_handoff.h"

int main(void)
{
    struct a20_handoff_descriptor_v1 descriptor = {0};

    descriptor.magic[0] = 'A';
    descriptor.version = A20_HANDOFF_VERSION;
    descriptor.header_size = A20_HANDOFF_HEADER_SIZE;
    descriptor.flags = 0;
    return (sizeof(descriptor) == 256u) ? 0 : 1;
}
