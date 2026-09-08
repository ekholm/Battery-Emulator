#include <gtest/gtest.h>

/* Included the way comm_can.cpp includes it - through the src_dir root, so the
 * host build's -Itest/emul puts the emulated stand-in in front of the vendored
 * header. A relative include here would reach the vendored header directly and
 * this file would pin the wrong one. */
#include "src/lib/pierremolinaro-ACAN2517FD/ACAN2517FD.h"

/* The emulated header owes the vendored one's compatibility-mode define, and
 * without this case nothing on this branch notices when it is missing.
 *
 * The vendored ACAN2517FD.h defines DISABLEMCP2517FDCOMPAT unconditionally, and
 * comm_can.cpp's CAN-FD teardown is written against it: end() runs
 * vTaskDelete(mESP32TaskHandle) between the library's turnOffInterrupts() and
 * turnOnInterrupts(), and that define is the only reason the pair expands to
 * nothing. The stub has no interrupt-mask wrappers, so the define is INERT here
 * and deleting it leaves every other case in the suite green - which is exactly
 * why the mirror needs a case of its own rather than a comment. It stops being
 * inert the moment this suite meets the branch that turns the assumption into an
 * #ifndef / #error on the same include: the guard fires on the STUB's include
 * and the host build stops compiling, with the driver never reached.
 *
 * The failure this catches is silent in both directions - a dropped mirror
 * reads as a green suite here and as a build break there - so the case asserts
 * the define at the point of substitution rather than anywhere downstream. */

/* WHICH HEADER ANSWERED. The define alone cannot tell the two apart, because the
 * vendored header sets it too: if the substitution ever broke, the case above
 * would pass on the vendored define and prove nothing about the stub. The stub
 * mirrors the vendored public API method for method, so there is no member to
 * discriminate on - only the data layout differs, the vendored class carrying a
 * callback array, a task handle, SPI settings and a bus reference where the stub
 * keeps a chip index and two pins. Pinning the stub's size is the same idiom
 * comm_can.cpp already uses to police these headers from a line compiled in both
 * builds. */
static_assert(sizeof(ACAN2517FD) == 8,
              "the emulated ACAN2517FD holds a chip index and two pins - a larger object here means "
              "the vendored header was included instead, and the define below proves nothing");

TEST(CanFdCompatMirror, TheEmulatedHeaderCarriesTheVendoredCompatibilityDefine) {
#ifdef DISABLEMCP2517FDCOMPAT
  SUCCEED() << "the stub mirrors DISABLEMCP2517FDCOMPAT, as the vendored header defines it";
#else
  FAIL() << "the emulated ACAN2517FD.h has stopped mirroring DISABLEMCP2517FDCOMPAT. The vendored "
            "header defines it unconditionally and comm_can.cpp's CAN-FD teardown is written "
            "against it; restore the define in test/emul/src/lib/pierremolinaro-ACAN2517FD/"
            "ACAN2517FD.h rather than weakening whatever now depends on it";
#endif
}
