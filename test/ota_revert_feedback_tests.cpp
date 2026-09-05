#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>

/* The revert button's outcome must reach the user.
 *
 * /revertFirmware answers with text in every case: a 400 carries the reason a
 * revert is not offered, a 500 carries esp_ota_set_boot_partition's refusal,
 * and the 200 says a reboot is coming. The first shipped RevertFW() discarded
 * all of it - fired the request, ignored the response, reloaded after 5 s -
 * so a refused revert, or one whose arriving image failed its verification
 * boot and rolled back, looked like "nothing happens": same page, same
 * version, no message. That is a real user's actual bug report, verbatim.
 *
 * These pin the repaired contract at the source level, the same shape the
 * IRAM and drain pins use: the JS lives inside a C++ string literal, so a
 * behavioural test would need a browser; what a test CAN hold is that the
 * handler reads the response and branches on the status.
 */
namespace {

/* Strip comments out, before anything is asserted about the code.
 *
 * Every pin below decides whether the page does something by looking for the
 * text of a call, and the JS is surrounded by a comment paragraph that NAMES
 * what the code does - "/GetFirmwareInfo" appears in it verbatim. So a mutant
 * that deletes the poll but leaves the paragraph explaining it satisfied that
 * pin on dead code, which is the shape the placement checks in this suite
 * already strip for. Newlines are kept so nothing line-oriented drifts. */
std::string strip_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    if (src.compare(i, 2, "//") == 0) {
      while (i < src.size() && src[i] != '\n') {
        ++i;
      }
    } else if (src.compare(i, 2, "/*") == 0) {
      const size_t end = src.find("*/", i + 2);
      const size_t stop = end == std::string::npos ? src.size() : end + 2;
      for (; i < stop; ++i) {
        if (src[i] == '\n') {
          out += '\n';
        }
      }
    } else {
      out += src[i++];
    }
  }
  return out;
}

std::string webserver_source() {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  std::ifstream src(dir + "/../Software/src/devboard/webserver/webserver.cpp");
  EXPECT_TRUE(src.is_open());
  return strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

}  // namespace

TEST(OtaRevertFeedback, TheRevertHandlerShowsTheServersAnswerInsteadOfDiscardingIt) {
  const std::string src = webserver_source();
  const size_t fn = src.find("function RevertFW()");
  ASSERT_NE(fn, std::string::npos);
  const std::string body = src.substr(fn, 700);

  EXPECT_NE(body.find("x.onload"), std::string::npos)
      << "RevertFW no longer reads the response - a refusal becomes 'nothing happens' again";
  EXPECT_NE(body.find("x.status==200"), std::string::npos) << "RevertFW no longer distinguishes success from refusal";
  EXPECT_NE(body.find("responseText"), std::string::npos)
      << "RevertFW no longer surfaces the server's text - the reason is written for the user, show it";
}

TEST(OtaRevertFeedback, ProgressIsShownWhileTheBoardReboots) {
  /* The user-reported half of the defect: after confirming, the page just
     reloaded a few seconds later with no sign anything was happening, and a
     successful revert changed only a hash suffix nobody can spot. So: a
     spinner while the board is down, and the outcome announced as was/now. */
  const std::string src = webserver_source();
  EXPECT_NE(src.find("revspin"), std::string::npos) << "the reboot-in-progress spinner is gone";
  // The FETCH, not the endpoint name: webserver.cpp also REGISTERS /GetFirmwareInfo,
  // so a bare name pin is satisfied by the route while the poll is gone.
  EXPECT_NE(src.find("fetch('/GetFirmwareInfo',{cache:'no-store'})"), std::string::npos)
      << "the flow no longer polls the version endpoint - it cannot know when the board is back";
  // The CALL, not just the definition: a mutant that defines revPoll but never
  // starts it (a fixed reload timer, say) leaves every other pin in place.
  EXPECT_NE(src.find("come back...'; revPoll(cur,st,Date.now(),false);"), std::string::npos)
      << "the success branch no longer STARTS the poll - the definition alone reverts to a blind timer";
  EXPECT_NE(src.find("'Reverted: was '+cur+', now running '+d.firmware"), std::string::npos)
      << "the was/now announcement is gone - two dev builds differ only in a hash suffix, so the "
         "version change must be said, not left to be spotted";
}

TEST(OtaRevertFeedback, ARollbackIsReportedAsARollbackNotAsSuccess) {
  /* The board going down and coming back with the SAME version is the arriving
     image dying and being rolled back. Without this branch that outcome polls
     to a timeout or, worse, reads as a slow success. */
  const std::string src = webserver_source();
  EXPECT_NE(src.find("came back on the SAME version"), std::string::npos)
      << "the rollback verdict is gone - a failed arrival reads as 'nothing happens' again";
  // The CONDITION, not just the message: the verdict string sitting in a dead
  // branch satisfies a looser pin.
  EXPECT_NE(src.find("else if(d&&d.firmware&&sawDown){"), std::string::npos)
      << "the rollback branch's condition changed - if it is unreachable, a failed arrival "
         "polls to timeout instead of being named";
  EXPECT_NE(src.find(".catch(function(){ revPoll(cur,st,t0,true); })"), std::string::npos)
      << "an unreachable board no longer sets sawDown - a rollback can never be detected";
}

TEST(OtaRevertFeedback, TheVerdictSurvivesTheReload) {
  const std::string src = webserver_source();
  EXPECT_NE(src.find("sessionStorage.setItem('revDone'"), std::string::npos)
      << "the verdict is no longer stored - the reload wipes the only message saying what happened";
  EXPECT_NE(src.find("sessionStorage.getItem('revDone')"), std::string::npos)
      << "the reloaded page no longer replays the verdict";
}

TEST(OtaRevertFeedback, TheBlindReloadIsGone) {
  const std::string src = webserver_source();
  const size_t fn = src.find("function RevertFW()");
  ASSERT_NE(fn, std::string::npos);
  const std::string body = src.substr(fn, 700);

  // The old shape: send() followed by an unconditional reload timer. A reload
  // is fine AFTER a confirmed success; unconditional, it eats the refusal.
  EXPECT_EQ(body.find("x.send(); setTimeout"), std::string::npos)
      << "the unconditional reload is back - it hides every non-200 outcome";
}

/* The board that never comes back. Every other outcome ends the poll; without
   a bound this one does not, and the user is left watching a spinner that
   means nothing. The elapsed check is pinned with its own bound because a
   message alone survives a mutant that never reaches it. */
TEST(OtaRevertFeedback, ABoardThatNeverComesBackIsReportedRatherThanSpunOnForever) {
  const std::string src = webserver_source();
  EXPECT_NE(src.find("if(Date.now()-t0>120000){"), std::string::npos)
      << "the poll has no elapsed bound - a board that never answers spins the user forever";
  EXPECT_NE(src.find("has not come back within 2 minutes"), std::string::npos)
      << "the timeout no longer says what happened, so the bound reads as a stuck page";
  // The bound must END the poll. A mutant that reports and recurses anyway is
  // the same forever-spin with a message on it.
  const size_t at = src.find("if(Date.now()-t0>120000){");
  ASSERT_NE(at, std::string::npos);
  const std::string arm = src.substr(at, src.find("setTimeout", at) - at);
  EXPECT_NE(arm.find("return;"), std::string::npos)
      << "the timeout arm no longer returns - it reports and keeps polling";
}

/* The strip above is load-bearing, so it gets its own pin: the comment
   paragraph over the revert JS names the endpoint the poll uses, and a pin
   read against the raw file is satisfied by that prose alone. */
TEST(OtaRevertFeedback, AnEndpointNamedOnlyInACommentDoesNotSatisfyAPin) {
  const std::string block =
      "    /* It polls /GetFirmwareInfo until the board answers. */\n"
      "    content += \"function revPoll(cur,st,t0,sawDown){ }\";\n";
  const std::string line =
      "    // It polls /GetFirmwareInfo until the board answers.\n"
      "    content += \"function revPoll(cur,st,t0,sawDown){ }\";\n";

  // Both comment forms, because the file carries both and a stripper that
  // handles one of them leaves the other free to satisfy a pin.
  for (const std::string& sample : {block, line}) {
    const std::string stripped = strip_comments(sample);

    EXPECT_EQ(stripped.find("/GetFirmwareInfo"), std::string::npos)
        << "the comment survived the strip, so a deleted poll can still satisfy its own pin";
    EXPECT_NE(stripped.find("function revPoll"), std::string::npos) << "the strip ate code";
    EXPECT_EQ(std::count(stripped.begin(), stripped.end(), '\n'), std::count(sample.begin(), sample.end(), '\n'))
        << "the strip changed the line count, so line-oriented matching would drift";
  }
}
