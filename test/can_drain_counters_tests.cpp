#include <gtest/gtest.h>

#include <fstream>
#include <string>

/* The MCP2515 drain's four loss counters must have a consumer.
 *
 * They were written when the drain was, and nothing read them: the instrument
 * for "is this path lossless" existed, was correct, and produced no numbers,
 * while every measurement that needed it was deferred waiting for a rig. A
 * counter with no reader is not an instrument, it is a comment.
 *
 * These read the source because comm_can.cpp and the webserver are not in the
 * host binary. The properties are about WIRING - that each counter reaches a
 * surface, and that a board without a running drain is not reported as a board
 * that lost nothing - which is what a text scan can honestly check.
 */
namespace {

std::string source(const char* relative) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  std::ifstream src(dir + "/../" + relative);
  EXPECT_TRUE(src.is_open()) << "this test reads " << relative;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

const char* kCommCan = "Software/src/communication/can/comm_can.cpp";
const char* kWebserver = "Software/src/devboard/webserver/webserver.cpp";

}  // namespace

TEST(CanDrainCounters, EveryCounterTheDriverKeepsIsPublished) {
  const std::string comm = source(kCommCan);

  for (const char* accessor : {"isrFramesDrained()", "isrFramesDropped()", "isrBusDeferrals()", "isrBusTimeouts()"}) {
    EXPECT_NE(comm.find(accessor), std::string::npos)
        << accessor
        << " is not read by anything. The driver counts it for a consumer that does not exist, which is "
           "the state this whole surface was added to end.";
  }
}

TEST(CanDrainCounters, TheCountersReachAPageAUserCanRead) {
  const std::string web = source(kWebserver);

  EXPECT_NE(web.find("can_drain_counters()"), std::string::npos)
      << "the webserver no longer asks for the drain counters, so they are unreadable again";
  for (const char* field : {"frames_drained", "frames_dropped", "bus_deferrals", "bus_timeouts"}) {
    EXPECT_NE(web.find(field), std::string::npos) << field << " is collected but never rendered";
  }
}

TEST(CanDrainCounters, ABoardWithNoDrainSaysSoRatherThanReportingZeroLoss) {
  const std::string comm = source(kCommCan);
  const std::string web = source(kWebserver);

  // The publisher must distinguish "no drain" from "drain, nothing lost".
  EXPECT_NE(comm.find("isrDrainActive()"), std::string::npos)
      << "can_drain_counters() no longer asks whether the drain is running, so a board without one reports four "
         "zeros - which reads as a clean run rather than as no measurement";
  EXPECT_NE(web.find("drain.active"), std::string::npos)
      << "the page renders the counters unconditionally; four zeros on a board with no MCP2515 is success it never "
         "measured";
}

TEST(CanDrainCounters, TheCountersCanBeZeroedFromTheSamePage) {
  const std::string web = source(kWebserver);
  const std::string comm = source(kCommCan);

  EXPECT_NE(comm.find("reset_can_drain_counters"), std::string::npos) << "there is no way to zero the counters";
  // Quoted, so a renamed route does not pass on being a prefix of the old name.
  EXPECT_NE(web.find("\"/resetDrainCounters\""), std::string::npos)
      << "the reset is unreachable from the page, so a measurement has to subtract two reads by hand";
}
