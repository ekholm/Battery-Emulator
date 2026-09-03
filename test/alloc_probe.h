#ifndef _TEST_ALLOC_PROBE_H_
#define _TEST_ALLOC_PROBE_H_

#include <stddef.h>

/* Counts calls to global operator new, so a test can assert that a code path allocates a fixed
 * number of times rather than "feels cheaper". Counting is off until alloc_probe_reset() arms
 * it, so no other test pays for it.
 *
 * Not built under AddressSanitizer: ASAN provides its own operator new, and replacing it there
 * would disable exactly the checking an ASAN run is for. Tests that use the probe skip
 * themselves under __SANITIZE_ADDRESS__. */
void alloc_probe_reset();
size_t alloc_probe_count();

#endif  // _TEST_ALLOC_PROBE_H_
