#include <gtest/gtest.h>

#include <vector>

#include "ui/router.hpp"

using rksec::ui::Page;
using rksec::ui::Router;

TEST(Router, StartsAtHome)
{
    Router r;
    EXPECT_EQ(r.current(), Page::home);
    EXPECT_FALSE(r.back()) << "首页按返回：没有上一页";
    EXPECT_EQ(r.depth(), 0U);
}

TEST(Router, GoAndBack)
{
    Router r;
    r.go(Page::settings);
    r.go(Page::about);
    EXPECT_EQ(r.current(), Page::about);
    EXPECT_EQ(r.depth(), 2U);
    EXPECT_TRUE(r.back());
    EXPECT_EQ(r.current(), Page::settings);
    EXPECT_TRUE(r.back());
    EXPECT_EQ(r.current(), Page::home);
    EXPECT_FALSE(r.back());
}

TEST(Router, SamePageTwiceDoesNotPushTwice)
{
    // 手指抖了一下点了两次
    Router r;
    r.go(Page::events);
    r.go(Page::events);
    EXPECT_EQ(r.depth(), 1U);
    EXPECT_TRUE(r.back());
    EXPECT_EQ(r.current(), Page::home);
}

TEST(Router, HomeClearsHistory)
{
    Router r;
    r.go(Page::settings);
    r.go(Page::about);
    r.home();
    EXPECT_EQ(r.current(), Page::home);
    EXPECT_EQ(r.depth(), 0U);
    r.go(Page::events);
    r.go(Page::home);  // go(home) 等同于 home()
    EXPECT_EQ(r.depth(), 0U);
}

TEST(Router, HistoryIsBounded)
{
    Router r{3};
    for (int i = 0; i < 50; ++i) {
        r.go(Page::settings);
        r.go(Page::about);
    }
    EXPECT_LE(r.depth(), 3U) << "来回点击不能让历史无限增长";
}

TEST(Router, NotifiesOnEveryChange)
{
    Router r;
    std::vector<std::pair<Page, Page>> seen;
    r.on_change([&](Page from, Page to) { seen.emplace_back(from, to); });
    r.go(Page::settings);
    r.go(Page::settings);  // 没变化，不通知
    r.back();
    r.home();  // 已经在首页，不通知
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0], std::make_pair(Page::home, Page::settings));
    EXPECT_EQ(seen[1], std::make_pair(Page::settings, Page::home));
}
