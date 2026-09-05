#include <gtest/gtest.h>

#include <fstream>
#include <string>

/* an earlier pass: selecting a CAN controller the board does not have must not cost the
 * boot.
 *
 * On a 2515-fitment T-2CAN, storing `BATTCOMM = 6` (an MCP2518FD the board does
 * not carry) through the settings form the UI offers, then rebooting, left the
 * board completely silent - and restoring ONLY the NVS, with the identical app
 * in flash, brought it straight back. So the stored setting alone is what does
 * it, and recovery needs an esptool NVS rewrite over USB: in the field that is
 * a bricked board from one dropdown choice.
 *
 * Part of the damage is structural and is what this pins. `init_CAN()` was a
 * sequence of per-interface blocks, each of which `return false`d out of the
 * WHOLE function on any failure - a pad it could not allocate, a chip that did
 * not answer. Software.cpp discards that return, so every interface ordered
 * after the failing one was silently never brought up, including a perfectly
 * good native channel. One absent add-on cost the user their whole CAN stack.
 *
 * comm_can.cpp is not linked into this binary - the suite substitutes
 * test/emul/can.cpp, which stubs register_can_receiver() to a no-op - so the
 * behaviour cannot be driven from a test without pulling ACAN_ESP32,
 * ACAN2517FD, MCP2515, SPI and the HAL into the host build. Reading the source
 * is therefore the available oracle, and it is a real one: the property is
 * exactly "no early return survives in init_CAN's own scope".
 *
 * NOT claimed by this test: that the silent hang itself is cured. Where the
 * boot stops has not been established - init_serial() runs well before
 * init_CAN(), so on a UART board a hang inside CAN bring-up would still print
 * the banner first, and the one observation is from a USB-CDC board where an
 * early hang can stop the port ever enumerating. That discrimination needs the
 * bench.
 */
namespace {

std::string init_can_body() {
  std::ifstream src(std::string(TEST_REPO_ROOT) + "/Software/src/communication/can/comm_can.cpp");
  EXPECT_TRUE(src.is_open()) << "comm_can.cpp is where this test looks; it was not there";
  const std::string all((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
  const size_t at = all.find("bool init_CAN() {");
  EXPECT_NE(at, std::string::npos) << "init_CAN() not found";
  // The function ends at the first closing brace in column 0 after it.
  const size_t end = all.find("\n}\n", at);
  return all.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace

TEST(CanInitFailSafe, NoInterfaceFailureAbandonsTheRestOfTheBoot) {
  const std::string body = init_can_body();

  // Which `return false` leaves init_CAN is a question about SCOPE, not
  // indentation: one nested in an `if` at function level is four spaces in and
  // still abandons the boot. So walk the braces and ask whether each `return
  // false` sits inside a per-interface lambda or not. (An indentation-only
  // version of this check was written first and a mutation adding
  // `if (esp32hal == nullptr) { return false; }` walked straight past it.)
  int depth = 0, lambda_at = -1;
  for (size_t i = 0; i < body.size(); ++i) {
    if (body.compare(i, 15, "[&]() -> bool {") == 0) {
      if (lambda_at < 0) {
        lambda_at = depth;  // the depth the lambda body opens at
      }
      ++depth;
      i += 14;
      continue;
    }
    if (body[i] == '{') {
      ++depth;
    } else if (body[i] == '}') {
      --depth;
      if (lambda_at >= 0 && depth <= lambda_at) {
        lambda_at = -1;  // left the lambda
      }
    } else if (body.compare(i, 13, "return false;") == 0) {
      EXPECT_GE(lambda_at, 0) << "init_CAN() can still return early from its own scope, so one absent controller "
                                 "takes every interface ordered after it down with it - and Software.cpp discards "
                                 "the return, so nothing says so. Offending context: "
                              << body.substr(i > 90 ? i - 90 : 0, 120);
    }
  }

  // Each interface that fails has to SAY so and be removed, or a driver stays
  // registered on an interface that was never started.
  EXPECT_NE(body.find("interface_unavailable"), std::string::npos) << "nothing marks a failed interface unavailable";
}

TEST(CanInitFailSafe, EveryInterfaceIsBroughtUpIndependently) {
  const std::string body = init_can_body();
  // The five bring-ups the boot must be able to lose one of without losing the
  // others: native, MCP2515, the shared FD SPI bus, FD, FD-2.
  for (const char* gate : {"native_ok", "addon_ok", "fd_bus_ok", "fd_ok", "fd2_ok"}) {
    EXPECT_NE(body.find(gate), std::string::npos)
        << gate << " is gone, so that bring-up is no longer isolated from the others";
  }
  // ...and the honest answer still aggregates all of them.
  EXPECT_NE(body.find("return native_ok && addon_ok && fd_bus_ok && fd_ok && fd2_ok;"), std::string::npos)
      << "init_CAN no longer reports whether every requested interface came up";
}
