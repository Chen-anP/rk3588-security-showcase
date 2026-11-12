#include <gtest/gtest.h>

#include "core/ini.hpp"

using rksec::ini::Document;
using rksec::ini::Error;

TEST(Ini, ParsesSectionsKeysAndLineNumbers)
{
    std::vector<Error> errors;
    const auto doc = Document::parse("# 注释\n"
                                     "[camera]\n"
                                     "width = 1280\n"
                                     "  device=/dev/v4l/by-id/cam#1  \n",
                                     errors);
    EXPECT_TRUE(errors.empty());
    const auto *w = doc.find("camera", "width");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->text, "1280");
    EXPECT_EQ(w->line, 3);
    const auto *d = doc.find("camera", "device");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->text, "/dev/v4l/by-id/cam#1") << "值里的 # 不是注释，前后空格要去掉";
}

TEST(Ini, MissingKeyReturnsNull)
{
    std::vector<Error> errors;
    const auto doc = Document::parse("[a]\nx = 1\n", errors);
    EXPECT_EQ(doc.find("a", "y"), nullptr);
    EXPECT_EQ(doc.find("b", "x"), nullptr);
}

TEST(Ini, HandlesWindowsLineEndingsAndNoTrailingNewline)
{
    std::vector<Error> errors;
    const auto doc = Document::parse("[a]\r\nx = 1\r\ny = 2", errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_NE(doc.find("a", "x"), nullptr);
    EXPECT_EQ(doc.find("a", "x")->text, "1");
    ASSERT_NE(doc.find("a", "y"), nullptr);
    EXPECT_EQ(doc.find("a", "y")->line, 3);
}

TEST(Ini, EmptyValueIsAllowed)
{
    std::vector<Error> errors;
    const auto doc = Document::parse("[a]\nx =\n", errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_NE(doc.find("a", "x"), nullptr);
    EXPECT_EQ(doc.find("a", "x")->text, "");
}

TEST(Ini, ReportsEveryBadLineAndKeepsParsing)
{
    std::vector<Error> errors;
    const auto doc = Document::parse("[a\n"           // 1 缺 ]
                                     "[b c]\n"        // 2 分区名有空格
                                     "no equals\n"    // 3 没有 =
                                     "bad-key = 1\n"  // 4 键名有 -
                                     "ok = 1\n",      // 5 正常
                                     errors);
    ASSERT_EQ(errors.size(), 4U);
    EXPECT_EQ(errors[0].line, 1);
    EXPECT_EQ(errors[1].line, 2);
    EXPECT_EQ(errors[2].line, 3);
    EXPECT_EQ(errors[3].line, 4);
    EXPECT_NE(doc.find("", "ok"), nullptr) << "出错的行之后还要继续解析";
}

TEST(Ini, DuplicateKeyIsAnErrorAndFirstValueWins)
{
    std::vector<Error> errors;
    const auto doc = Document::parse("[a]\nx = 1\nx = 2\n", errors);
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].line, 3);
    EXPECT_NE(errors[0].message.find("第 2 行"), std::string::npos) << errors[0].message;
    EXPECT_EQ(doc.find("a", "x")->text, "1");
}

TEST(Ini, UnusedListsKeysNeverLookedUp)
{
    std::vector<Error> errors;
    const auto doc = Document::parse("[camera]\nfps = 30\nfsp = 30\n", errors);
    doc.find("camera", "fps");
    const auto unused = doc.unused();
    ASSERT_EQ(unused.size(), 1U);
    EXPECT_EQ(unused[0].first, "camera.fsp");
    EXPECT_EQ(unused[0].second->line, 3);
}
