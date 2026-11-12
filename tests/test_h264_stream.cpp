// H.264 Annex B 码流工具（第 8a 步）
#include <gtest/gtest.h>

#include <vector>

#include "media/h264_stream.hpp"

using rksec::media::extract_parameter_sets;
using rksec::media::has_idr;
using rksec::media::split_annexb;

namespace {

using Bytes = std::vector<std::uint8_t>;

// 拼一个码流：每个 NAL 前面加起始码，long_start = true 用 4 字节起始码
Bytes stream(const std::vector<Bytes> &nals, bool long_start)
{
    Bytes out;
    for (const auto &n : nals) {
        if (long_start) {
            out.push_back(0);
        }
        out.insert(out.end(), {0, 0, 1});
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}

const Bytes kSps{0x67, 0x64, 0x00, 0x29, 0xAC};  // 类型 7
const Bytes kPps{0x68, 0xEE, 0x3C, 0x80};        // 类型 8
const Bytes kIdr{0x65, 0x88, 0x84, 0x00, 0x21};  // 类型 5
const Bytes kP{0x41, 0x9A, 0x24, 0x6C};          // 类型 1

}  // namespace

TEST(H264Stream, SplitsNalUnitsWithShortAndLongStartCodes)
{
    for (const bool long_start : {false, true}) {
        const Bytes s = stream({kSps, kPps, kIdr}, long_start);
        const auto nals = split_annexb(s.data(), s.size());
        ASSERT_EQ(nals.size(), 3U) << (long_start ? "4 字节起始码" : "3 字节起始码");
        EXPECT_EQ(nals[0].type, 7);
        EXPECT_EQ(nals[0].size, kSps.size());
        EXPECT_EQ(nals[1].type, 8);
        EXPECT_EQ(nals[1].size, kPps.size());
        EXPECT_EQ(nals[2].type, 5);
        EXPECT_EQ(nals[2].size, kIdr.size()) << "最后一个 NAL 到数据结尾";
        EXPECT_EQ(s[nals[2].start], 0x65);
    }
}

TEST(H264Stream, TrailingZerosAreNotPartOfTheNal)
{
    Bytes s = stream({kP}, false);
    s.insert(s.end(), {0, 0});  // 尾部填充
    const auto nals = split_annexb(s.data(), s.size());
    ASSERT_EQ(nals.size(), 1U);
    EXPECT_EQ(nals[0].size, kP.size());
}

TEST(H264Stream, NoStartCodeOrEmptyInput)
{
    const Bytes junk{1, 2, 3, 4, 5};
    EXPECT_TRUE(split_annexb(junk.data(), junk.size()).empty());
    EXPECT_TRUE(split_annexb(nullptr, 0).empty());
    EXPECT_TRUE(split_annexb(junk.data(), 0).empty());
    const Bytes only_code{0, 0, 1};
    EXPECT_TRUE(split_annexb(only_code.data(), only_code.size()).empty()) << "起始码后面没有内容";
}

TEST(H264Stream, FindsKeyframes)
{
    const Bytes key = stream({kSps, kPps, kIdr}, true);
    const Bytes plain = stream({kP}, false);
    EXPECT_TRUE(has_idr(key.data(), key.size()));
    EXPECT_FALSE(has_idr(plain.data(), plain.size()));
    EXPECT_FALSE(has_idr(nullptr, 0));
}

TEST(H264Stream, ExtractsParameterSetsWithLongStartCodes)
{
    const Bytes s = stream({kSps, kPps, kIdr}, false);
    const Bytes ps = extract_parameter_sets(s.data(), s.size());
    Bytes want{0, 0, 0, 1};
    want.insert(want.end(), kSps.begin(), kSps.end());
    want.insert(want.end(), {0, 0, 0, 1});
    want.insert(want.end(), kPps.begin(), kPps.end());
    EXPECT_EQ(ps, want) << "统一成 4 字节起始码，IDR 不带";
    const Bytes plain = stream({kP}, false);
    EXPECT_TRUE(extract_parameter_sets(plain.data(), plain.size()).empty());
}
