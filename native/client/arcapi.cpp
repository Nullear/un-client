#include "arcapi.hpp"

#include <array>
#include <algorithm>
#include <cstdint>

using namespace godot;

namespace {
constexpr char KEY[] = "4251a15bb6d1787857E49aedd3475458068ecdD8FCfa2e5dE7CEc527400692887ABaa40eD49C6418";
constexpr char IV[] = "EEB877EdAecFd8e7";
constexpr char HOST[] = "346C26AD6A3771A48591C6F8DECDE95D18BA958FD4A8B5AA400BF739CAF6E74A7B0F61B7BA970C996AD483433458BCA2";
constexpr uint32_t SBOX[] = {
    0x80E39FA4, 0xF517C47F, 0x282C4761, 0xA347E9D7, 0xDF1E6903, 0x6ECD79F0, 0xD32F4221, 0xD01D6F8D, 0x98676EFA,
    0x0B56BFF7, 0x3CCB1C7E, 0x5015581F, 0xB6CFC56A, 0x9F67BE40, 0x82BDC5BC, 0xD50AAB16, 0xD4BD8406, 0x1F895A53,
    0x7CDEBACE, 0xBA4B5B7D, 0xF95EDC61, 0x7A49959C, 0xFFDD50AF, 0x1AE33548, 0x69889CB9, 0xAF5F0665, 0xE0A309E6,
    0x26C345AC, 0x71A4DB2C, 0x06FD973F, 0x796437A1, 0x1440AEC2, 0x0951C4FD, 0xDDCB1EDF, 0x03663B68, 0xA07092FE,
    0x10E184D9, 0xF70F35FD, 0x656AB461, 0x6C8760E0, 0x199F80CC, 0xBDDE6C88, 0x733602B8, 0xCB56A90C, 0xE612F6F9,
    0xF4ABA1B9, 0x307044F2, 0x5FECEC15, 0x858059D1, 0x96807B7C, 0x7A6E3480, 0xAAECEAFC, 0xE56C3233, 0x75460284,
    0x8FA1A952, 0x0D820F35, 0x67DC9909, 0xB4BC718C, 0xFC824590, 0xED06727E, 0xE822E537, 0x084204C0, 0x77D721D6,
    0xBEA9C68C, 0x5A669834, 0xD4721ECD, 0x1AEFD9E3, 0x6F3991E5, 0xA63E8E87, 0xF57BE922, 0xE4FB07FA, 0x7FB2933B,
    0x85E67504, 0xDE1D0520, 0x73221AB5, 0x825E2FE3, 0xBF9C384D, 0xFFA7FFAB, 0x42B57947, 0xD4E814EE, 0x264486A7,
    0xCBE7CE13, 0xAB48C815, 0xF5E1F7AB, 0x48E4EB09, 0x376B415B, 0xCA69CAC4, 0x948B607A, 0x2149A3AD, 0x2272C348,
    0xF94418E1, 0x275D2FB6, 0x19254482, 0x75B77653, 0xA5881676, 0xEC320D45, 0x7652D94A, 0x97E1E51D, 0x81F47D16,
    0xE7B9EFEF, 0x7473EDEE, 0x401C6E15, 0x2029AA9A, 0x2D9694E2, 0x0E42364E, 0x4AC93F1F, 0xDDFBB040, 0x1E912E32,
    0x22FF3B30, 0xD61CFEF4, 0xD615A88B, 0x3ABD9E4D, 0xC492D355, 0xAE2D2F45, 0xE327918E, 0x92364236, 0xE7266C1F,
    0xA8812425, 0x8E2E1EFA, 0x74531610, 0xBA6DE1FE, 0xB1A2DAEE, 0x8F580873, 0xBCE63505, 0x82F049D0, 0x19FAE6AA,
    0x17339E92, 0x418EB7A7, 0xE1346A7F, 0x4FC242AC, 0x3F946302, 0xCE57C0D8, 0xEE85F67D, 0x9A8978BA, 0x685BA82C,
    0xE79BD6A7, 0x8C33D94F, 0x78F69F9F, 0xAE9E41C3, 0x7CFE6303, 0x7156D51C, 0x69903F81, 0xCA939D48, 0xB4C10B21,
    0x3A986477, 0x79F3FA5A, 0x8185F070, 0xAF641176, 0x3ACB1A82, 0x705C1784, 0x3A88466A, 0x1E1AF83F, 0x733E1C4A,
    0xCE387594, 0x6575DFD9, 0x7195C33B, 0xE2C50B55, 0xE2695ED3, 0x8CE18A6E, 0x183D4350, 0xF3DD97CA, 0x0A47C532,
    0xE0818F08, 0xAC22B469, 0x18BF09F1, 0x8B0A4B38, 0xAE7588D0, 0x03ACE383, 0xDBBCDD6C, 0x0809C7FC, 0xDDEE767C,
    0xEC497902, 0xA924B0F5, 0xF39C81D2, 0x8EA98E50, 0xCCF25915, 0xA72E888B, 0xD9782CBD, 0x8D93313B, 0xE356D5D6,
    0x46116C6C, 0x52F06460, 0x68C4F0FE, 0xAFF0BA85, 0x5746520E, 0x611D95DB, 0xE6EFB443, 0x33ACBDF3, 0x7939883F,
    0xA06D6A28, 0xC67446CF, 0x397FE0B6, 0x4E1D1A36, 0xE687C1F4, 0xC2333955, 0x7C016783, 0xF3B9FB21, 0xB75BCE94,
    0xC7D75199, 0x14EDF85B, 0xC286789E, 0x934E4DA7, 0x982C73F9, 0x96D6B2F9, 0x109B2D67, 0x623DD58F, 0xE39FF384,
    0xE2820349, 0xFF2A9209, 0x1D5B3089, 0x4D28DE04, 0x8206363B, 0x8218F2AE, 0x99C8EF58, 0xC21B7790, 0x550BD3CE,
    0x178EF552, 0x29782B88, 0x77B0CDCF, 0x9AA013EE, 0x740419A2, 0x782BB013, 0xEDC3A6CB, 0x14D8F828, 0xCBE032F5,
    0xB5DE2ADB, 0xD09A8312, 0x7E834E02, 0x55FADA65, 0x890A8D15, 0xE150C701, 0x01F5217D, 0xF9A5F799, 0xFCE72FEF,
    0x543229A8, 0x0760A3F6, 0x3C400D79, 0x6870ECA2, 0x7697A201, 0x60159B9B, 0x8DF1793E, 0xBEFA41B4, 0x785FC8A0,
    0xC388E455, 0x8050A56F, 0xF369BC16, 0x607347DD, 0x372F26B9, 0x66B9356C, 0x72B42EBB, 0xE39CAA52, 0xD941118B,
    0xDAAF821C, 0x87F95423, 0xF7D04CEF, 0x9A17BA63,
};
static_assert(std::size(SBOX) == 256);

uint32_t rotate_right(uint32_t value, int count) {
    return (value >> count) | (value << (32 - count));
}

struct Mixer {
    std::array<uint32_t, 17> rk{};
    uint32_t magic = 0x4ADF4219;
    uint32_t count = 0;
    uint32_t step = 0;

    uint32_t mix() {
        ++count;
        const uint32_t sum = magic + rotate_right(rk[0], 13) + rotate_right(rk[15], 23);
        const uint32_t next = sum ^ SBOX[sum >> 24] ^ rk[4];
        for (int i = 0; i < 16; ++i) rk[i] = rk[i + 1];
        rk[16] = next;
        const uint32_t value = (rk[16] + rk[0]) ^ (rk[13] + rk[1]) ^ (rk[6] + magic);
        if (count < 0x10001) return value;
        count -= 0x10001;
        step += 0x10001;
        rk[2] += step;
        magic = value;
        return mix();
    }

    void setup() {
        rk[0] = 1;
        rk[1] = 1;
        for (int i = 0; i < 15; ++i) rk[i + 2] = rk[i + 1] + rk[i];
        for (int i = 0; i < 20; ++i) {
            uint32_t word = 0;
            for (int j = 0; j < 4; ++j) {
                const int position = 4 * i + j;
                if (position < int(sizeof(KEY) - 1)) word |= uint32_t(uint8_t(KEY[position])) << (j * 8);
            }
            rk[15] += word;
            rk[4] = mix() ^ rk[4];
        }
        rk[15] += 0x50;
        for (int i = 0; i < 17; ++i) rk[4] = mix() ^ rk[4];
        magic = mix();
        count = 0;
        step = 0;
    }
};

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

String decrypt(const String &hex) {
    const CharString raw = hex.utf8();
    if ((raw.length() & 1) || raw.length() == 0) return "";
    Mixer ctx;
    ctx.setup();
    uint8_t feedback[16];
    for (int i = 0; i < 16; ++i) feedback[i] = uint8_t(IV[i]);
    CharString output;
    output.resize(raw.length() / 2 + 1);
    const int length = raw.length() / 2;
    for (int offset = 0; offset < length; offset += 16) {
        uint8_t bytes[16] = {};
        const int count = std::min(16, length - offset);
        for (int i = 0; i < count; ++i) {
            const int hi = nibble(raw[(offset + i) * 2]);
            const int lo = nibble(raw[(offset + i) * 2 + 1]);
            if (hi < 0 || lo < 0) return "";
            bytes[i] = uint8_t((hi << 4) | lo);
        }
        for (int word = 0; word < 4; ++word) {
            const uint32_t stream = ctx.mix();
            for (int j = 0; j < 4; ++j) {
                const int slot = word * 4 + j;
                if (slot < count) output[offset + slot] = char(bytes[slot] ^ feedback[slot] ^ (stream >> (j * 8)));
            }
        }
        for (int i = 0; i < 16; ++i) feedback[i] = bytes[i];
    }
    output[length] = '\0';
    return String::utf8(output.get_data()).strip_edges();
}
} // namespace

String arcapi::resolve_base_url(const String &override_url) {
    const String value = override_url.strip_edges();
    if (value.begins_with("http://") || value.begins_with("https://")) return value;
    return decrypt(value.is_empty() ? String(HOST) : value);
}
