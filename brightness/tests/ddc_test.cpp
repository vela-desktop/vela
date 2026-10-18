// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The DDC/CI messages (ddc.h), against the bytes captured on the monitors of
// docs/brightness.md §3.

extern "C" {
#include "ddc.h"
#include "fake.h"
}

#include <gtest/gtest.h>

#include <vector>

namespace {

// The Mi's real reply at 100/100 (§3).
const std::vector<uint8_t> captured = { 0x6e, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x64, 0xa4 };

enum vela_ddc_reply parse(const std::vector<uint8_t>& bytes, uint16_t* value = nullptr, uint16_t* max = nullptr)
{
    uint16_t v = 0, m = 0;
    enum vela_ddc_reply reply = vela_ddc_parse_reply(bytes.data(), bytes.size(), VELA_DDC_BRIGHTNESS, &v, &m);
    if (value) {
        *value = v;
    }
    if (max) {
        *max = m;
    }
    return reply;
}

} // namespace

TEST(Ddc, GetRequestIsTheStandardOne)
{
    uint8_t bytes[VELA_DDC_GET_LENGTH];
    ASSERT_EQ(vela_ddc_encode_get(bytes, VELA_DDC_BRIGHTNESS), 5u);
    EXPECT_EQ(std::vector<uint8_t>(bytes, bytes + 5), (std::vector<uint8_t> { 0x51, 0x82, 0x01, 0x10, 0xac }));
}

TEST(Ddc, SetRequestCarriesTheValueBigEndian)
{
    uint8_t bytes[VELA_DDC_SET_LENGTH];
    ASSERT_EQ(vela_ddc_encode_set(bytes, VELA_DDC_BRIGHTNESS, 100), 7u);
    EXPECT_EQ(std::vector<uint8_t>(bytes, bytes + 7),
        (std::vector<uint8_t> { 0x51, 0x84, 0x03, 0x10, 0x00, 0x64, 0xcc }));
    vela_ddc_encode_set(bytes, VELA_DDC_BRIGHTNESS, 0x1234);
    EXPECT_EQ(bytes[4], 0x12);
    EXPECT_EQ(bytes[5], 0x34);
}

TEST(Ddc, ParsesTheCapturedReply)
{
    uint16_t value = 0, max = 0;
    ASSERT_EQ(parse(captured, &value, &max), VELA_DDC_REPLY_OK);
    EXPECT_EQ(value, 100);
    EXPECT_EQ(max, 100);
}

TEST(Ddc, RejectsTheMiFixedBlock)
{
    std::vector<uint8_t> block(vela_fake_mi_block, vela_fake_mi_block + VELA_DDC_REPLY_LENGTH);
    EXPECT_EQ(parse(block), VELA_DDC_REPLY_BAD);
}

TEST(Ddc, RecognizesANullMessage)
{
    std::vector<uint8_t> null = { 0x6e, 0x80, 0xbe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    EXPECT_EQ(parse(null), VELA_DDC_REPLY_NULL);
    null[2] = 0xbf;
    EXPECT_EQ(parse(null), VELA_DDC_REPLY_BAD);
}

TEST(Ddc, RejectsShortReads)
{
    for (size_t length = 0; length < captured.size(); ++length) {
        EXPECT_NE(parse(std::vector<uint8_t>(captured.begin(), captured.begin() + length)), VELA_DDC_REPLY_OK)
            << length;
    }
}

// Every single bit flipped anywhere in the reply is caught.
TEST(Ddc, NoCorruptedReplyIsAccepted)
{
    for (size_t byte = 0; byte < captured.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> bytes = captured;
            bytes[byte] ^= 1 << bit;
            EXPECT_EQ(parse(bytes), VELA_DDC_REPLY_BAD) << "byte " << byte << " bit " << bit;
        }
    }
}

TEST(Ddc, ReplyForAnotherFeatureIsBad)
{
    uint8_t bytes[VELA_DDC_REPLY_LENGTH];
    vela_ddc_encode_reply(bytes, 0x12, true, 50, 100);
    EXPECT_EQ(parse(std::vector<uint8_t>(bytes, bytes + sizeof(bytes))), VELA_DDC_REPLY_BAD);
}

TEST(Ddc, UnsupportedFeature)
{
    uint8_t bytes[VELA_DDC_REPLY_LENGTH];
    vela_ddc_encode_reply(bytes, VELA_DDC_BRIGHTNESS, false, 0, 0);
    EXPECT_EQ(parse(std::vector<uint8_t>(bytes, bytes + sizeof(bytes))), VELA_DDC_REPLY_UNSUPPORTED);
}

TEST(Ddc, EncodedReplyIsTheCapturedOne)
{
    uint8_t bytes[VELA_DDC_REPLY_LENGTH];
    vela_ddc_encode_reply(bytes, VELA_DDC_BRIGHTNESS, true, 100, 100);
    EXPECT_EQ(std::vector<uint8_t>(bytes, bytes + sizeof(bytes)), captured);
}

TEST(Ddc, MonitorSideDecodesRequests)
{
    uint8_t bytes[VELA_DDC_SET_LENGTH];
    vela_ddc_encode_get(bytes, VELA_DDC_BRIGHTNESS);
    vela_ddc_request request = vela_ddc_parse_request(bytes, VELA_DDC_GET_LENGTH);
    EXPECT_EQ(request.kind, VELA_DDC_REQUEST_GET);
    EXPECT_EQ(request.feature, VELA_DDC_BRIGHTNESS);

    vela_ddc_encode_set(bytes, VELA_DDC_BRIGHTNESS, 321);
    request = vela_ddc_parse_request(bytes, VELA_DDC_SET_LENGTH);
    EXPECT_EQ(request.kind, VELA_DDC_REQUEST_SET);
    EXPECT_EQ(request.value, 321);

    bytes[6] ^= 1;
    EXPECT_EQ(vela_ddc_parse_request(bytes, VELA_DDC_SET_LENGTH).kind, VELA_DDC_REQUEST_INVALID);
    EXPECT_EQ(vela_ddc_parse_request(bytes, 3).kind, VELA_DDC_REQUEST_INVALID);
}
