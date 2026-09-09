#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <string>

#include "utils/source_scan.h"

/* The comment stripper the source-scan tests read their subjects through.
 *
 * It exists because a comment inside the region a source-scan test searches is
 * indistinguishable from the code it pins, and both directions of that were
 * live on this branch: `native_can_initialized = false;` deleted and left
 * behind as a comment kept CanInitIsolation green, and a comment mentioning
 * `return false` inside init_CAN() reds a suite about code nobody touched.
 *
 * Offsets are what makes this more than a one-liner: half the assertions over
 * there are orderings, so a stripper that SHORTENS the string would move the
 * very positions those tests compare.
 */

TEST(SourceScan, BlanksLineCommentsAndKeepsTheCodeBesideThem) {
  const std::string in = "  flag = false;  // flag = true; is what this is not\n  next();\n";
  const std::string out = strip_comments(in);

  EXPECT_NE(out.find("flag = false;"), std::string::npos) << "the code beside a comment was blanked too";
  EXPECT_EQ(out.find("flag = true;"), std::string::npos) << "a line comment's text survived the strip";
  EXPECT_NE(out.find("next();"), std::string::npos) << "the strip did not end at the newline";
}

TEST(SourceScan, BlanksBlockCommentsIncludingTheOnesSpanningLines) {
  const std::string in = "a();\n/* set_event(EVENT_X, 0);\n * and more\n */\nb();\n";
  const std::string out = strip_comments(in);

  EXPECT_EQ(out.find("set_event"), std::string::npos) << "a block comment's text survived the strip";
  EXPECT_EQ(out.find("and more"), std::string::npos) << "the strip stopped at the first line of a block comment";
  EXPECT_NE(out.find("a();"), std::string::npos);
  EXPECT_NE(out.find("b();"), std::string::npos) << "the block comment never terminated";
}

/* The whole reason comments are blanked rather than deleted. Every ordering
 * assertion in the source-scan suites - the clear comes after the failure is
 * known and before the branch's own return - compares offsets into this string.
 */
TEST(SourceScan, KeepsEveryOffsetWhereItWas) {
  const std::string in = "one();  // xxxxx\n/* yyy */ two();\n";
  const std::string out = strip_comments(in);

  ASSERT_EQ(out.size(), in.size()) << "the string changed length, so every offset assertion downstream has moved";
  EXPECT_EQ(out.find("one();"), in.find("one();"));
  EXPECT_EQ(out.find("two();"), in.find("two();"));
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), std::count(in.begin(), in.end(), '\n'))
      << "line structure was not preserved";
}

/* A `//` inside a string literal is not a comment, and treating it as one would
 * blank the rest of the file - including every assertion's subject after it.
 */
TEST(SourceScan, DoesNotStartACommentInsideAStringLiteral) {
  const std::string in = "logging.print(\"http://example\");\nkeep_me();\n";
  const std::string out = strip_comments(in);

  EXPECT_NE(out.find("keep_me();"), std::string::npos) << "a // inside a string literal swallowed the rest of the file";
  EXPECT_NE(out.find("http://example"), std::string::npos) << "string literal content was blanked";
}

TEST(SourceScan, DoesNotStartACommentInsideACharLiteralOrAnEscapedQuote) {
  const std::string in = "if (c == '/') {\n}\nchar q = '\\'';\nconst char* s = \"a\\\"/* not a comment */\";\nend();\n";
  const std::string out = strip_comments(in);

  EXPECT_NE(out.find("end();"), std::string::npos) << "an escaped quote left the scanner inside a literal";
  EXPECT_NE(out.find("not a comment"), std::string::npos) << "a block comment was found inside a string literal";
}

/* Braces inside comments are the second half of the same defect: the extractors
 * over there brace-match a function body, and a `}` in a comment closes it early.
 */
TEST(SourceScan, RemovesBracesThatOnlyExistInsideComments) {
  const std::string in = "void f() {\n  // } this brace is not the end of f\n  body();\n}\n";
  const std::string out = strip_comments(in);

  const size_t open_brace = out.find('{');
  ASSERT_NE(open_brace, std::string::npos);
  const size_t close_brace = out.find('}', open_brace);
  ASSERT_NE(close_brace, std::string::npos);
  EXPECT_GT(close_brace, out.find("body();")) << "a brace inside a comment still closes the body a scan extracts";
}

/* function_body(), the extraction the CAN and OTA scans read their subjects
 * through.
 *
 * Four hand-rolled copies of it grew up in those suites and each got a
 * different subset of the three properties right. The one that matters most is
 * the last, because it fails SILENTLY in the direction that reads as good
 * news: the scan reports a property missing from a function that has it, and
 * whoever is looking at the red goes and re-adds code that is already there.
 */

TEST(SourceScanBody, ReadsTheDefinitionAndNotAForwardDeclarationOfTheSameName) {
  const std::string in =
      "static void wanted(void);\n"
      "static void other(void) {\n  decoy();\n}\n"
      "static void wanted(void) {\n  the_real_thing();\n}\n";

  const std::string body = function_body(in, "static void wanted(void)");

  EXPECT_NE(body.find("the_real_thing();"), std::string::npos)
      << "the declaration at the top was read, and the next '{' belongs to the function below it";
  EXPECT_EQ(body.find("decoy();"), std::string::npos) << "the body of a different function was returned: " << body;
}

TEST(SourceScanBody, MatchesBracesRatherThanStoppingAtTheFirstClosingOne) {
  const std::string in = "void f() {\n  std::vector<int> v = {1, 2};\n  after_the_list();\n}\n";

  EXPECT_NE(function_body(in, "void f()").find("after_the_list();"), std::string::npos)
      << "the slice stopped at the end of the initializer list";
}

TEST(SourceScanBody, IsNotEndedByABraceInACommentOrAStringLiteral) {
  const std::string commented = "void f() {\n  // }\n  in_the_body();\n}\nvoid g() {\n  elsewhere();\n}\n";
  EXPECT_NE(function_body(commented, "void f()").find("in_the_body();"), std::string::npos)
      << "a brace in a comment ended the body";

  const std::string quoted = "void f() {\n  log(\"}\");\n  in_the_body();\n}\n";
  EXPECT_NE(function_body(quoted, "void f()").find("in_the_body();"), std::string::npos)
      << "a brace in a string literal ended the body";
}

TEST(SourceScanBody, AnswersEmptyWhenThereIsNoDefinitionAtAll) {
  EXPECT_EQ(function_body("void declared_only(void);\n", "void declared_only(void)"), "")
      << "a declaration with no definition anywhere was read as a body";
}

/* The quiet form's failure mode, which is why the loud one exists: an empty
 * body satisfies every "must not contain" assertion written over it, so a
 * renamed signature turns a guard into a test that cannot fail.
 */
TEST(SourceScanBody, TheRequiredFormSaysSoWhenTheFunctionIsGone) {
  EXPECT_NONFATAL_FAILURE(required_function_body("void here(void) {\n}\n", "void renamed_away(void)"),
                          "has no definition where this test looks");
}
