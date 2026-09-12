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
  EXPECT_NE(src.find("come back...'; revPoll(cur,st,Date.now(),0);"), std::string::npos)
      << "the success branch no longer STARTS the poll - the definition alone reverts to a blind timer";
  /* Both copies of the was/now text, separately. The literal appears TWICE -
     once in the sessionStorage verdict that survives the reload, once in the
     innerHTML the live page shows - and a bare `find` of the shared substring
     is satisfied by whichever one is left. Blanking the on-screen message to
     'Reverted. Reloading...' kept this test green while the user lost exactly
     the information the case exists to guarantee (W04 in scripts/r522.mut).
     Each half is pinned with the code around it that makes it that half. */
  EXPECT_NE(src.find("st.innerHTML='Reverted: was '+cur+', now running '+d.firmware+'. Reloading...'"),
            std::string::npos)
      << "the LIVE page no longer announces was/now - two dev builds differ only in a hash suffix, "
         "so the version change must be said, not left to be spotted";
  EXPECT_NE(src.find("sessionStorage.setItem('revDone','Reverted: was '+cur+', now running '+d.firmware"),
            std::string::npos)
      << "the verdict stored for after the reload no longer says was/now, so the fresh page cannot "
         "repeat what just happened";
}

/* THE SUCCESS BRANCH MUST NOT CONSULT THE FAILURE COUNT, and after the count
   replaced the boolean that stopped being obvious.

   A real revert goes through the count: the board is down for 3.1-5.1 s
   (measured over /reboot on a T-CAN485, which shares this endpoint's
   200-then-graceful_restart tail) and the page polls every second, so by the
   time a NEW version answers, `down` is at least two - the same value the
   rollback branch is looking for. The only thing keeping those two outcomes
   apart is that the success test is asked FIRST and asks only whether the
   version changed.

   Narrow it to `d.firmware!=cur&&down<2` and every real revert falls through
   to the rollback branch and is announced as "came back on the SAME version"
   for a board that plainly did not - a worse verdict than the one this branch
   was written to remove, and it survived the whole suite (W01 in
   scripts/r522.mut). Nothing pinned this condition; the message beside it was
   pinned, which is the shape this file warns about twice elsewhere. */
TEST(OtaRevertFeedback, ARealRevertIsSuccessNoMatterHowManyPollsItWasAwayFor) {
  const std::string src = webserver_source();

  const size_t success = src.find("if(d&&d.firmware&&d.firmware!=cur){");
  EXPECT_NE(success, std::string::npos)
      << "the success branch's condition changed - it must be exactly 'the board answered and the "
         "version is not the one we left', with no reference to how long it was away";

  // And it is asked BEFORE the rollback branch, which is the other half of what
  // keeps a slow-but-real revert out of the rollback verdict.
  const size_t rollback = src.find("} else if(d&&d.firmware&&down>=2){");
  ASSERT_NE(rollback, std::string::npos) << "the rollback branch is gone - see the case above";
  EXPECT_LT(success, rollback) << "the rollback verdict is tested before the version change, so a "
                                  "revert that was away for two polls is announced as a rollback";
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
  EXPECT_NE(src.find("else if(d&&d.firmware&&down>=2){"), std::string::npos)
      << "the rollback branch's condition changed - if it is unreachable, a failed arrival "
         "polls to timeout instead of being named";
  EXPECT_NE(src.find(".catch(function(){ revPoll(cur,st,t0,down+1); })"), std::string::npos)
      << "an unreachable board no longer counts against the rollback test - a rollback can "
         "never be detected";
}

/* ONE DROPPED REQUEST IS NOT A REBOOT, and the three pieces that make that
   true are pinned separately because each can be lost on its own.

   /revertFirmware answers 200 and then asks for a graceful restart, which
   normally fires at the 5 s PAUSED deadline rather than the 10 s hard one - so
   the board stays up and answering for several more polls, every one of them
   correctly reporting the version still running.
   The first shipped flow latched a boolean on any rejected fetch, so a single
   drop anywhere in that window made the next successful poll print "came back
   on the SAME version" and RETURN, and the revert that completed seconds later
   was never reported. The window is a busy one to drop a request in: the
   graceful restart pauses the emulator and opens the contactors first.

   The three pieces: the count goes UP on a failure (without it nothing ever
   reaches the threshold), the branch DEMANDS two (without it one drop is still
   a reboot), and a poll that ANSWERS resets to zero - which is what makes the
   two CONSECUTIVE rather than two-in-the-whole-window. The reset is the one a
   reader is most likely to drop as redundant, so it gets a pin that says what
   it is for. */
TEST(OtaRevertFeedback, OneDroppedPollIsNotReadAsAReboot) {
  const std::string src = webserver_source();
  EXPECT_NE(src.find("revPoll(cur,st,t0,down+1)"), std::string::npos)
      << "a failed poll no longer counts - the rollback threshold can never be reached";
  EXPECT_EQ(src.find("revPoll(cur,st,t0,true)"), std::string::npos)
      << "the latching boolean is back - one dropped request in the ten-second graceful-restart "
         "window reads as a reboot and a successful revert is announced as a rollback";
  EXPECT_NE(src.find("down>=2"), std::string::npos)
      << "the threshold is gone - a single dropped request is a reboot again";
  EXPECT_NE(src.find("} else { revPoll(cur,st,t0,0); }"), std::string::npos)
      << "a poll that ANSWERED no longer resets the run of failures, so the two stop being "
         "consecutive: two drops anywhere in the window would report a rollback";

  /* The interval is half of the threshold and has to be pinned with it.
     Measured on a T-CAN485 over /reboot, which has this endpoint's own
     200-then-graceful_restart tail: the outage is 3.1-5.1 s across three
     rounds. On a 2 s grid a 3.1 s outage can drop as few as ONE poll, so the
     "two consecutive" test would miss a real rollback; on a 1 s grid the
     shortest measured outage is three polls wide. A reader who raises this
     back to 2000 as a politeness tweak silently breaks the threshold, and
     nothing else in the suite would notice. */
  EXPECT_NE(src.find("},1000); }"), std::string::npos)
      << "the poll interval left 1 s - the rollback threshold of two consecutive failures is "
         "only meaningful while the interval is short enough for a 3 s outage to span two polls";
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
      "    content += \"function revPoll(cur,st,t0,down){ }\";\n";
  const std::string line =
      "    // It polls /GetFirmwareInfo until the board answers.\n"
      "    content += \"function revPoll(cur,st,t0,down){ }\";\n";

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

/* THE FAILURE VERDICT HAS TO OUTLIVE THE PAGE'S OWN REFRESH, and the three
   pieces that make that true are pinned separately.

   The main page arms an unconditional `setTimeout(reload, 15000)` on every
   load. The rollback branch used to set innerHTML and return, writing nothing
   anywhere, so the one message that says an update failed was wiped by the next
   refresh and never came back - measured on a bench board at 8.9 seconds on
   screen, and less than that for a click late in a refresh cycle. Storing it is
   only the first piece: a verdict that is consumed as it is rendered, the way
   the success verdict is, is wiped again 15 s later. So it is STICKY, and the
   thing that keeps sticky from becoming stale is the version guard, not a
   timer. */
TEST(OtaRevertFeedback, TheRollbackVerdictIsStoredSoTheRefreshCannotWipeIt) {
  const std::string src = webserver_source();

  // Stored FROM THE ROLLBACK ARM. A setItem elsewhere in the file satisfies a
  // bare name pin while the arm that produces the verdict still writes nothing.
  const size_t rollback = src.find("} else if(d&&d.firmware&&down>=2){");
  ASSERT_NE(rollback, std::string::npos) << "the rollback branch is gone - see the cases above";
  const size_t arm_end = src.find("} else { revPoll", rollback);
  ASSERT_NE(arm_end, std::string::npos);
  const std::string arm = src.substr(rollback, arm_end - rollback);

  EXPECT_NE(arm.find("came back on the SAME version"), std::string::npos) << "this is not the rollback arm";
  EXPECT_NE(arm.find("sessionStorage.setItem('revFail',m)"), std::string::npos)
      << "the rollback verdict is not stored - the page's own 15 s reload wipes the only message "
         "that says the update failed, which is the whole defect";
  EXPECT_NE(arm.find("revFailShow(m)"), std::string::npos)
      << "the rollback verdict is stored but never shown on the live page";

  // And read back on the next load.
  EXPECT_NE(src.find("var f=sessionStorage.getItem('revFail');"), std::string::npos)
      << "the reloaded page no longer looks for a stored rollback verdict";
}

TEST(OtaRevertFeedback, TheRollbackVerdictIsStickyRatherThanConsumedOnRender) {
  /* The success verdict is removeItem'd as it is rendered - one reload is all
     it gets, and that is right for it: it sits beside a version string in the
     header that already says what it announced. The failure verdict has no such
     corroboration on the page, so being consumed on render would put it back to
     a message with a 15 s life, one refresh further along. */
  const std::string src = webserver_source();

  const size_t at = src.find("var f=sessionStorage.getItem('revFail');");
  ASSERT_NE(at, std::string::npos);
  const size_t show = src.find("revFailShow(f)", at);
  ASSERT_NE(show, std::string::npos) << "the stored rollback verdict is never rendered on reload";
  EXPECT_EQ(src.substr(at, show - at).find("removeItem('revFail')"), std::string::npos)
      << "the rollback verdict is consumed as it is rendered, so the very next auto-refresh wipes "
         "it again - a fix that moves the defect 15 seconds later";

  // The dismiss control is what ends it, so a sticky message has an exit.
  EXPECT_NE(src.find("id='revFailX'"), std::string::npos)
      << "the dismiss control is gone - a message that never clears is its own defect";
  const size_t fx = src.find("var x=document.getElementById('revFailX');");
  ASSERT_NE(fx, std::string::npos) << "nothing binds the dismiss control - it is decoration";
  const std::string handler = src.substr(fx, 300);
  EXPECT_NE(handler.find("removeItem('revFail')"), std::string::npos)
      << "dismiss hides the message without clearing the key, so the next load brings it back";
  EXPECT_NE(handler.find("removeChild(st)"), std::string::npos)
      << "dismiss clears the key without removing the message, so it is still on screen";
}

TEST(OtaRevertFeedback, AStickyRollbackVerdictIsDroppedOnceTheBoardRunsSomethingElse) {
  /* What makes sticky safe. The verdict says "you are still on this version
     because the update failed", which is true exactly while the board is
     running the version it was written about. A later revert or a fresh update
     boots something else, and without this the stale sentence would sit on the
     page of an image it is not about until somebody dismissed it. */
  const std::string src = webserver_source();

  const size_t rollback = src.find("} else if(d&&d.firmware&&down>=2){");
  ASSERT_NE(rollback, std::string::npos);
  const size_t arm_end = src.find("} else { revPoll", rollback);
  ASSERT_NE(arm_end, std::string::npos);
  EXPECT_NE(src.substr(rollback, arm_end - rollback).find("sessionStorage.setItem('revFailVer',cur)"),
            std::string::npos)
      << "the verdict is stored without the version it is about, so nothing can tell later whether "
         "it still applies";

  EXPECT_NE(src.find("if(f&&sessionStorage.getItem('revFailVer')==cur){"), std::string::npos)
      << "the stored verdict is replayed without checking it is still about the running image";
  EXPECT_NE(src.find("else if(f){ sessionStorage.removeItem('revFail'); sessionStorage.removeItem('revFailVer'); }"),
            std::string::npos)
      << "a verdict about a version the board is no longer running is kept rather than dropped";
}

TEST(OtaRevertFeedback, TheStatusMessageAnchorsToTheButtonInBothOfItsStates) {
  /* revSt() inserts itself after `revBtn` and falls back to the bottom of the
     document when there is none. A rollback verdict is replayed on a page whose
     revert offer has just been WITHDRAWN - the passive slot is marked failed -
     so the disabled button is the state the message appears under most, and
     while only the enabled branch carried the id the message landed at the foot
     of the page, away from the control it is about. */
  const std::string src = webserver_source();
  EXPECT_NE(src.find("<button id='revBtn' disabled title="), std::string::npos)
      << "the disabled revert button lost its id, so a replayed verdict is appended to the bottom "
         "of the document instead of under the button";
}

/* THE ASYMMETRY IS THE DESIGN, so its OTHER half needs a pin as well.

   The sticky failure verdict is argued for against the success verdict, which
   is replayed exactly ONCE - removeItem'd as it is rendered, because it lands
   beside a header version string that already corroborates it. Nothing held
   that half. A change that made 'revDone' sticky too would leave two messages
   that never clear on a page which reloads itself every 15 s, and the
   paragraphs justifying an asymmetry would be describing code that no longer
   has one, with the whole suite green. */
TEST(OtaRevertFeedback, TheSuccessVerdictIsStillConsumedAsItIsRendered) {
  const std::string src = webserver_source();

  const size_t at = src.find("var m=sessionStorage.getItem('revDone');");
  ASSERT_NE(at, std::string::npos) << "the success verdict is no longer replayed on load at all";
  const size_t render = src.find("st.innerHTML=m;", at);
  ASSERT_NE(render, std::string::npos) << "the replayed success verdict is never rendered";

  EXPECT_NE(src.substr(at, render - at).find("sessionStorage.removeItem('revDone')"), std::string::npos)
      << "the success verdict is no longer consumed as it is rendered - it is sticky like the "
         "failure one now, and the asymmetry the sticky rollback verdict is argued from is gone";
}

/* THE PREMISE OF STICKY, PINNED - because the argument is not "a failure
   message ought to persist", it is "this page wipes it".

   The rollback verdict is sticky rather than replayed once because the main
   page arms an unconditional reload on every load, so a verdict consumed on
   render is wiped one cycle later. Remove that reload and the design above is
   over-engineering with a stale justification; shorten it and the window the
   measured 8.9 s came from is a different number. Neither would fail a test.

   What a source scan CAN decide is that the statement is still there, still
   emitted from the top level of the main-page branch (four-space indent - a
   further condition would nest it) and still carrying the interval the
   reasoning quotes. Whether it is truly unconditional is a runtime property
   and is NOT claimed here. */
TEST(OtaRevertFeedback, TheRefreshTheStickyVerdictOutlivesIsStillArmedOnEveryLoad) {
  const std::string src = webserver_source();

  EXPECT_NE(src.find("\n    content += \"setTimeout(function(){ location.reload(true); }, 15000);\";"),
            std::string::npos)
      << "the main page's own 15 s reload is gone, moved or re-timed - that reload is the whole "
         "reason the rollback verdict is sticky rather than replayed once, and the comments "
         "arguing for sticky now describe a page that does not do this";
}
