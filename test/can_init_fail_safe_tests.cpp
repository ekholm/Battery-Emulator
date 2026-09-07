#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "utils/source_scan.h"

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
  /* Comments BLANKED before anything is searched. Every case in this file
   * decides whether the firmware does something by looking for the text of a
   * statement, and init_CAN() is surrounded by prose that names the very
   * expressions being pinned - the doc comment above it discusses `return
   * false`, the block comments inside name pins_present() and the gates. A
   * guard commented out with its name left on the line then satisfies the
   * search on dead code, which is how a scan over this same file was made
   * decorative once already. Newlines are kept, so the brace walk below still
   * measures the structure it means to.
   */
  const std::string all =
      strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
  const size_t at = all.find("void init_CAN() {");
  if (at == std::string::npos) {
    // Returning here rather than indexing from npos: every case below asserts
    // the body is non-empty, and substr() from npos throws out of the fixture
    // instead of failing it.
    return std::string();
  }
  // The function ends at the first closing brace in column 0 after it.
  const size_t end = all.find("\n}\n", at);
  return all.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace

TEST(CanInitFailSafe, NoInterfaceFailureAbandonsTheRestOfTheBoot) {
  const std::string body = init_can_body();
  ASSERT_FALSE(body.empty()) << "init_CAN() was not found in comm_can.cpp - the scan has drifted";

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
  ASSERT_FALSE(body.empty()) << "init_CAN() was not found in comm_can.cpp - the scan has drifted";
  // The five bring-ups the boot must be able to lose one of without losing the
  // others: native, MCP2515, the shared FD SPI bus, FD, FD-2.
  for (const char* gate : {"native_ok", "addon_ok", "fd_bus_ok", "fd_ok", "fd2_ok"}) {
    EXPECT_NE(body.find(gate), std::string::npos)
        << gate << " is gone, so that bring-up is no longer isolated from the others";
  }
  /* ...and each of them is READ. A gate nothing consults is a gate that cannot
   * take an interface out of service, and the five were introduced together
   * with a trailing `return native_ok && addon_ok && ...` that made them look
   * consulted while nothing examined the answer. That whole-board bool is gone
   * - init_CAN() is void, because its one caller discards what it returns - so
   * the property worth pinning is that every gate reaches a handler here.
   */
  for (const char* consulted : {"!native_ok", "!addon_ok", "!fd_bus_ok", "!fd_ok", "!fd2_ok"}) {
    EXPECT_NE(body.find(consulted), std::string::npos)
        << consulted << " is never read, so that bring-up's failure is recorded and then ignored";
  }
}

/* A gate that cannot be false is not a gate.
 *
 * Each per-interface lambda has to ANSWER from the state the rest of this file
 * already reads as "not there" - the null pointer, or the native flag - and not
 * from a literal. The temptation is a plain `return true;` at the end of each
 * one, and it is not hypothetical: merging the lambda shape onto a body whose
 * failure paths had already been changed to leave the interface inert rather
 * than to `return false` produces exactly that, because the two sides edit
 * different lines. Every other case in this file and in the isolation suite
 * stays green over it, and the five gates are then permanently true, so
 * interface_unavailable() is unreachable and no failed interface is ever
 * removed from the receiver map.
 *
 * `return false` is not available to fix it here: init_CAN() has none by rule
 * (CanInitIsolation.InitCanReturnsNothingBecauseNobodyReadIt), because a bool
 * leaving this function is what made "a failed chip stops the boot" look true.
 * Answering with the interface's own end state is the shape that satisfies both
 * - and it cannot drift from what receive_can() and the transmit paths test,
 * because it is the same expression.
 */
TEST(CanInitFailSafe, EachGateAnswersFromTheStateTheRestOfTheFileReads) {
  const std::string body = init_can_body();
  ASSERT_FALSE(body.empty()) << "init_CAN() was not found in comm_can.cpp - the scan has drifted";

  for (const char* answer : {"return native_can_initialized;", "return can2515 != nullptr;", "return canfd != nullptr;",
                             "return canfd_2 != nullptr;"}) {
    EXPECT_NE(body.find(answer), std::string::npos)
        << "a bring-up no longer reports its own outcome (" << answer
        << "). A lambda ending in a literal makes its gate permanently true and the "
           "interface_unavailable() call below it dead code";
  }
}

/* The presence gates have to be READ by init_CAN(), not merely to exist.
 *
 * pins_present() and plan_canfd_init() have thorough tests of their own, and
 * every one of them would stay green if init_CAN() stopped consulting either:
 * the suite does not link comm_can.cpp, so the call sites are only reachable by
 * reading the source. That is not hypothetical here - the gates arrived as the
 * `if (...)` heads of four blocks and had to be re-expressed as short-circuits
 * when the blocks became per-interface lambdas, which is exactly the kind of
 * rewrite that drops one of four and passes.
 *
 * The FD gates are asserted with their registration test still in front of
 * them. plan_canfd_init() is computed once, before the shared bus is brought
 * up, and a bus failure erases the FD entries afterwards - so the plan can
 * still say "first chip" for an interface that is no longer registered, and the
 * lambda would read ->second.speed off end(). The order of the two clauses is
 * the guard, not decoration.
 */
TEST(CanInitFailSafe, EveryPresenceGateIsConsultedWhereItDecides) {
  const std::string body = init_can_body();
  ASSERT_FALSE(body.empty()) << "init_CAN() was not found in comm_can.cpp - the scan has drifted";

  EXPECT_NE(body.find("esp32hal->pins_present(\"CAN\", esp32hal->MCP2515_CS()"), std::string::npos)
      << "the MCP2515 block no longer asks whether this board routes its pads, so an add-on the board does "
         "not have is attempted and reported as a pin fault";
  /* The plan has to BE the call's answer. `find("plan_canfd_init(")` alone is
   * satisfied by a call whose result is discarded next to a hand-built
   * CanFdInitPlan{true, true, true}, which is a stub that skips nothing and
   * leaves every case here green - so bind the two together.
   */
  const size_t decl = body.find("const CanFdInitPlan fd_plan =");
  ASSERT_NE(decl, std::string::npos) << "init_CAN() no longer computes an FD plan at all";
  const size_t call = body.find("plan_canfd_init(esp32hal,", decl);
  EXPECT_NE(call, std::string::npos) << "init_CAN() never calls plan_canfd_init()";
  EXPECT_LT(call - decl, size_t{40}) << "fd_plan is not what plan_canfd_init() returned - something else is "
                                        "deciding which FD blocks may run";
  EXPECT_NE(body.find("if (fd_plan.bus) {"), std::string::npos) << "the shared FD bus block ignores the plan";
  EXPECT_NE(body.find("(fdNativeIt == can_receivers.end() && fdAddonIt == can_receivers.end()) || !fd_plan.first_chip"),
            std::string::npos)
      << "the first FD chip either ignores the plan, or consults it before checking that the interface is "
         "still registered - and a shared-bus failure erases those entries after the plan is computed";
  EXPECT_NE(body.find("fdAddonIt_2 == can_receivers.end() || !fd_plan.second_chip"), std::string::npos)
      << "the second FD chip either ignores the plan, or consults it before checking that the interface is "
         "still registered";
}
