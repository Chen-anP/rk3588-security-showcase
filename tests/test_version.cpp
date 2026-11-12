#include <gtest/gtest.h>

#include <regex>
#include <string>

#include "core/version.hpp"

using namespace rksec;

TEST(Version, SemverLooksLikeMajorMinorPatch)
{
    const std::string v{version::semver()};
    EXPECT_TRUE(std::regex_match(v, std::regex{R"(\d+\.\d+\.\d+)"})) << "实际是: " << v;
}

TEST(Version, GitDescribeIsNeverEmpty)
{
    // 不在 git 仓库里编译时应为 "unknown"，而不是空字符串
    EXPECT_FALSE(version::git_describe().empty());
}

TEST(Version, FullContainsAllParts)
{
    const std::string f = version::full();
    EXPECT_NE(f.find(version::semver()), std::string::npos);
    EXPECT_NE(f.find(version::git_describe()), std::string::npos);
    EXPECT_NE(f.find(version::build_type()), std::string::npos);
    EXPECT_EQ(f.rfind("rk3588-security ", 0), 0U) << "应以程序名开头: " << f;
}
