#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <string>

/* CAN-FD is drained from the MCP2518FD's nINT interrupt, with an IRAM-resident
 * handler - not by polling.
 *
 * The polled design that preceded this (INT pin 255, a 1 ms task calling
 * isr_poll_core()) reboot-looped a board under ordinary receive load, and the
 * cause is in the library, not in the task: with no interrupt pin, receive()
 * itself polls from inside its own SPI transaction, and on ESP32 that nests a
 * non-recursive mutex the calling task already holds, with interrupts masked.
 * core_loop never returns from receive(); the task watchdog fires five seconds
 * later. It is reached the first time available() is true, so an idle board
 * passes every boot-time check and a loaded one dies.
 *
 * comm_can.cpp and the library are not in this binary, so these read the source.
 */
namespace {

std::string read_source(const std::string& rel) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../Software/src/" + rel;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "the source this test reads is expected at: " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

std::string comm_can_source() {
  return read_source("communication/can/comm_can.cpp");
}

std::string acan2517fd_source() {
  return read_source("lib/pierremolinaro-ACAN2517FD/ACAN2517FD.cpp");
}

}  // namespace

TEST(CanFdInterruptDrain, BothFdChipsAreConstructedWithTheirRealIntPinAndAHandler) {
  const std::string src = comm_can_source();

  EXPECT_NE(src.find("canfd = new ACAN2517FD(cs_pin, *SPI2517, int_pin);"), std::string::npos)
      << "the first FD chip is no longer given its INT pin - the library then skips attachInterrupt() "
         "and receive() polls from inside its own SPI transaction, which deadlocks core_loop on ESP32";
  EXPECT_NE(src.find("canfd_2 = new ACAN2517FD(cs_pin, *SPI2517_2, int_pin);"), std::string::npos)
      << "the second FD chip is no longer given its INT pin";
  EXPECT_NE(src.find("canfd->begin(*settings2517, canfd_isr)"), std::string::npos)
      << "begin() is not handed the first chip's interrupt handler";
  EXPECT_NE(src.find("canfd_2->begin(*settings2517_2, canfd_2_isr)"), std::string::npos)
      << "begin() is not handed the second chip's interrupt handler";

  // The no-interrupt construction is the defect, whichever chip it is applied to.
  EXPECT_EQ(src.find("*SPI2517, 255)"), std::string::npos) << "the first FD chip is back in no-interrupt mode";
  EXPECT_EQ(src.find("*SPI2517_2, 255)"), std::string::npos) << "the second FD chip is back in no-interrupt mode";
  EXPECT_EQ(src.find("isr_poll_core"), std::string::npos)
      << "comm_can.cpp calls the library's poll core directly - that is the polled drain coming back";
  EXPECT_EQ(src.find("canfd_poll_task"), std::string::npos) << "the polling task is back";
}

/* The handler runs from the GPIO interrupt, which can arrive while a flash
 * operation has the cache off, so both trampolines and the library's isr()
 * must be IRAM-resident. This is the SOURCE half of that check: it catches a
 * dropped attribute. Where the compiler actually placed each function is the
 * linked image's business and is audited there (the notes repo's ISR IRAM audit).
 */
TEST(CanFdInterruptDrain, TheHandlersAreDeclaredIramResident) {
  const std::string src = comm_can_source();
  static const std::regex first(R"(static\s+void\s+IRAM_ATTR\s+canfd_isr\s*\(\s*\)\s*\{\s*canfd->isr\(\);)");
  static const std::regex second(R"(static\s+void\s+IRAM_ATTR\s+canfd_2_isr\s*\(\s*\)\s*\{\s*canfd_2->isr\(\);)");
  EXPECT_TRUE(std::regex_search(src, first))
      << "canfd_isr() is not declared IRAM_ATTR, or does more than give the library's semaphore";
  EXPECT_TRUE(std::regex_search(src, second))
      << "canfd_2_isr() is not declared IRAM_ATTR, or does more than give the library's semaphore";

  const std::string lib = acan2517fd_source();
  static const std::regex lib_isr(R"(void\s+IRAM_ATTR\s+ACAN2517FD::isr\s*\(\s*void\s*\))");
  EXPECT_TRUE(std::regex_search(lib, lib_isr))
      << "ACAN2517FD::isr() lost its IRAM_ATTR - the trampoline would jump from IRAM into flash";
}

/* The premise the interrupt rests on, guarded so a library update re-opens the
 * decision rather than silently keeping it: in no-interrupt mode receive()
 * calls isr_poll_core() inside its own transaction. If a newer library stops
 * doing that, polling becomes possible again and this test says so.
 */
TEST(CanFdInterruptDrain, TheLibraryStillPollsInsideReceiveWhenThereIsNoIntPin) {
  const std::string lib = acan2517fd_source();
  const size_t receive = lib.find("bool ACAN2517FD::receive (CANFDMessage & outMessage) {");
  ASSERT_NE(receive, std::string::npos) << "receive() moved - re-check the premise by hand";
  const size_t next_fn = lib.find("\nbool ACAN2517FD::", receive + 1);
  ASSERT_NE(next_fn, std::string::npos);
  const std::string body = lib.substr(receive, next_fn - receive);
  EXPECT_NE(body.find("if (mINT == 255) {"), std::string::npos)
      << "receive() no longer special-cases the no-interrupt mode - the reason polling was ruled out "
         "may have lapsed; re-decide before trusting either design";
  EXPECT_NE(body.find("isr_poll_core () ;"), std::string::npos)
      << "receive() no longer polls in no-interrupt mode - same: re-decide";
  EXPECT_NE(body.find("mSPI.beginTransaction (mSPISettings) ;"), std::string::npos)
      << "receive() no longer opens its own SPI transaction - the nesting that deadlocks may be gone";
}

/* The OTHER half of the same premise, and the half an upstream fix is most
 * likely to change: the deadlock needs isr_poll_core() to open a SECOND
 * transaction while receive() already holds the first. Guarding only receive()
 * lets the natural upstream repair - have the poll core assume the caller's
 * transaction, the way every other *Assume_SPI_transaction helper in this
 * library already does - land silently, leaving the ruling in force with its
 * reason gone. Then this fails and the decision is re-opened, as intended.
 */
TEST(CanFdInterruptDrain, TheLibraryPollCoreStillOpensATransactionOfItsOwn) {
  const std::string lib = acan2517fd_source();
  const size_t core = lib.find("void ACAN2517FD::isr_poll_core (void) {");
  ASSERT_NE(core, std::string::npos) << "isr_poll_core() moved - re-check the premise by hand";
  const size_t next_fn = lib.find("\nvoid ACAN2517FD::", core + 1);
  ASSERT_NE(next_fn, std::string::npos);
  const std::string body = lib.substr(core, next_fn - core);
  EXPECT_NE(body.find("mSPI.beginTransaction (mSPISettings) ;"), std::string::npos)
      << "isr_poll_core() no longer opens its own SPI transaction, so calling it from inside "
         "receive() no longer re-takes a held non-recursive mutex - the deadlock this branch routed "
         "around may be fixed upstream; re-decide polling instead of keeping the ruling by inertia";
}
