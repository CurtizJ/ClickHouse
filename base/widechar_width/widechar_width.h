/**
 * widechar_width.h, generated on 2018-07-09.
 * See https://github.com/ridiculousfish/widecharwidth/
 *
 * SHA1 file hashes:
 *  UnicodeData.txt:     0e060fafb08d6722fbec56d9f9ebe8509f01d0ee
 *  EastAsianWidth.txt:  240b5b2c235671d330860742615f798e3d334c4c
 *  emoji-data.txt:      11fd60a01e17df80035c459728350073cd9ed37b
 */

#ifndef WIDECHAR_WIDTH_H
#define WIDECHAR_WIDTH_H

#include <algorithm>
#include <iterator>
#include <cstddef>
#include <cstdint>

// NOLINTBEGIN(*)

/* Special width values */
enum {
  widechar_nonprint = -1,     // The character is not printable.
  widechar_combining = -2,    // The character is a zero-width combiner.
  widechar_ambiguous = -3,    // The character is East-Asian ambiguous width.
  widechar_private_use = -4,  // The character is for private use.
  widechar_unassigned = -5,   // The character is unassigned.
  widechar_widened_in_9 = -6  // Width is 1 in Unicode 8, 2 in Unicode 9+.
};

/* An inclusive range of characters. */
struct widechar_range {
  int32_t lo;
  int32_t hi;
};

/* Private usage range. */
static const struct widechar_range widechar_private_table[] = {
    {0x0E000, 0x0F8FF}, {0xF0000, 0xFFFFD}, {0x100000, 0x10FFFD}
};

/* Nonprinting characters. */
static const struct widechar_range widechar_nonprint_table[] = {
    {0x00000, 0x0001F}, {0x0007F, 0x0009F}, {0x000AD, 0x000AD},
    {0x00600, 0x00605}, {0x0061C, 0x0061C}, {0x006DD, 0x006DD},
    {0x0070F, 0x0070F}, {0x008E2, 0x008E2}, {0x0180E, 0x0180E},
    {0x0200B, 0x0200F}, {0x02028, 0x0202E}, {0x02060, 0x02064},
    {0x02066, 0x0206F}, {0x0D800, 0x0DFFF}, {0x0FEFF, 0x0FEFF},
    {0x0FFF9, 0x0FFFB}, {0x110BD, 0x110BD}, {0x110CD, 0x110CD},
    {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001},
    {0xE0020, 0xE007F}
};

/* Width 0 combining marks. */
static const struct widechar_range widechar_combining_table[] = {
    {0x00300, 0x0036F}, {0x00483, 0x00489}, {0x00591, 0x005BD},
    {0x005BF, 0x005BF}, {0x005C1, 0x005C2}, {0x005C4, 0x005C5},
    {0x005C7, 0x005C7}, {0x00610, 0x0061A}, {0x0064B, 0x0065F},
    {0x00670, 0x00670}, {0x006D6, 0x006DC}, {0x006DF, 0x006E4},
    {0x006E7, 0x006E8}, {0x006EA, 0x006ED}, {0x00711, 0x00711},
    {0x00730, 0x0074A}, {0x007A6, 0x007B0}, {0x007EB, 0x007F3},
    {0x007FD, 0x007FD}, {0x00816, 0x00819}, {0x0081B, 0x00823},
    {0x00825, 0x00827}, {0x00829, 0x0082D}, {0x00859, 0x0085B},
    {0x008D3, 0x008E1}, {0x008E3, 0x00903}, {0x0093A, 0x0093C},
    {0x0093E, 0x0094F}, {0x00951, 0x00957}, {0x00962, 0x00963},
    {0x00981, 0x00983}, {0x009BC, 0x009BC}, {0x009BE, 0x009C4},
    {0x009C7, 0x009C8}, {0x009CB, 0x009CD}, {0x009D7, 0x009D7},
    {0x009E2, 0x009E3}, {0x009FE, 0x009FE}, {0x00A01, 0x00A03},
    {0x00A3C, 0x00A3C}, {0x00A3E, 0x00A42}, {0x00A47, 0x00A48},
    {0x00A4B, 0x00A4D}, {0x00A51, 0x00A51}, {0x00A70, 0x00A71},
    {0x00A75, 0x00A75}, {0x00A81, 0x00A83}, {0x00ABC, 0x00ABC},
    {0x00ABE, 0x00AC5}, {0x00AC7, 0x00AC9}, {0x00ACB, 0x00ACD},
    {0x00AE2, 0x00AE3}, {0x00AFA, 0x00AFF}, {0x00B01, 0x00B03},
    {0x00B3C, 0x00B3C}, {0x00B3E, 0x00B44}, {0x00B47, 0x00B48},
    {0x00B4B, 0x00B4D}, {0x00B56, 0x00B57}, {0x00B62, 0x00B63},
    {0x00B82, 0x00B82}, {0x00BBE, 0x00BC2}, {0x00BC6, 0x00BC8},
    {0x00BCA, 0x00BCD}, {0x00BD7, 0x00BD7}, {0x00C00, 0x00C04},
    {0x00C3E, 0x00C44}, {0x00C46, 0x00C48}, {0x00C4A, 0x00C4D},
    {0x00C55, 0x00C56}, {0x00C62, 0x00C63}, {0x00C81, 0x00C83},
    {0x00CBC, 0x00CBC}, {0x00CBE, 0x00CC4}, {0x00CC6, 0x00CC8},
    {0x00CCA, 0x00CCD}, {0x00CD5, 0x00CD6}, {0x00CE2, 0x00CE3},
    {0x00D00, 0x00D03}, {0x00D3B, 0x00D3C}, {0x00D3E, 0x00D44},
    {0x00D46, 0x00D48}, {0x00D4A, 0x00D4D}, {0x00D57, 0x00D57},
    {0x00D62, 0x00D63}, {0x00D82, 0x00D83}, {0x00DCA, 0x00DCA},
    {0x00DCF, 0x00DD4}, {0x00DD6, 0x00DD6}, {0x00DD8, 0x00DDF},
    {0x00DF2, 0x00DF3}, {0x00E31, 0x00E31}, {0x00E34, 0x00E3A},
    {0x00E47, 0x00E4E}, {0x00EB1, 0x00EB1}, {0x00EB4, 0x00EB9},
    {0x00EBB, 0x00EBC}, {0x00EC8, 0x00ECD}, {0x00F18, 0x00F19},
    {0x00F35, 0x00F35}, {0x00F37, 0x00F37}, {0x00F39, 0x00F39},
    {0x00F3E, 0x00F3F}, {0x00F71, 0x00F84}, {0x00F86, 0x00F87},
    {0x00F8D, 0x00F97}, {0x00F99, 0x00FBC}, {0x00FC6, 0x00FC6},
    {0x0102B, 0x0103E}, {0x01056, 0x01059}, {0x0105E, 0x01060},
    {0x01062, 0x01064}, {0x01067, 0x0106D}, {0x01071, 0x01074},
    {0x01082, 0x0108D}, {0x0108F, 0x0108F}, {0x0109A, 0x0109D},
    {0x0135D, 0x0135F}, {0x01712, 0x01714}, {0x01732, 0x01734},
    {0x01752, 0x01753}, {0x01772, 0x01773}, {0x017B4, 0x017D3},
    {0x017DD, 0x017DD}, {0x0180B, 0x0180D}, {0x01885, 0x01886},
    {0x018A9, 0x018A9}, {0x01920, 0x0192B}, {0x01930, 0x0193B},
    {0x01A17, 0x01A1B}, {0x01A55, 0x01A5E}, {0x01A60, 0x01A7C},
    {0x01A7F, 0x01A7F}, {0x01AB0, 0x01ABE}, {0x01B00, 0x01B04},
    {0x01B34, 0x01B44}, {0x01B6B, 0x01B73}, {0x01B80, 0x01B82},
    {0x01BA1, 0x01BAD}, {0x01BE6, 0x01BF3}, {0x01C24, 0x01C37},
    {0x01CD0, 0x01CD2}, {0x01CD4, 0x01CE8}, {0x01CED, 0x01CED},
    {0x01CF2, 0x01CF4}, {0x01CF7, 0x01CF9}, {0x01DC0, 0x01DF9},
    {0x01DFB, 0x01DFF}, {0x020D0, 0x020F0}, {0x02CEF, 0x02CF1},
    {0x02D7F, 0x02D7F}, {0x02DE0, 0x02DFF}, {0x0302A, 0x0302F},
    {0x03099, 0x0309A}, {0x0A66F, 0x0A672}, {0x0A674, 0x0A67D},
    {0x0A69E, 0x0A69F}, {0x0A6F0, 0x0A6F1}, {0x0A802, 0x0A802},
    {0x0A806, 0x0A806}, {0x0A80B, 0x0A80B}, {0x0A823, 0x0A827},
    {0x0A880, 0x0A881}, {0x0A8B4, 0x0A8C5}, {0x0A8E0, 0x0A8F1},
    {0x0A8FF, 0x0A8FF}, {0x0A926, 0x0A92D}, {0x0A947, 0x0A953},
    {0x0A980, 0x0A983}, {0x0A9B3, 0x0A9C0}, {0x0A9E5, 0x0A9E5},
    {0x0AA29, 0x0AA36}, {0x0AA43, 0x0AA43}, {0x0AA4C, 0x0AA4D},
    {0x0AA7B, 0x0AA7D}, {0x0AAB0, 0x0AAB0}, {0x0AAB2, 0x0AAB4},
    {0x0AAB7, 0x0AAB8}, {0x0AABE, 0x0AABF}, {0x0AAC1, 0x0AAC1},
    {0x0AAEB, 0x0AAEF}, {0x0AAF5, 0x0AAF6}, {0x0ABE3, 0x0ABEA},
    {0x0ABEC, 0x0ABED}, {0x0FB1E, 0x0FB1E}, {0x0FE00, 0x0FE0F},
    {0x0FE20, 0x0FE2F}, {0x101FD, 0x101FD}, {0x102E0, 0x102E0},
    {0x10376, 0x1037A}, {0x10A01, 0x10A03}, {0x10A05, 0x10A06},
    {0x10A0C, 0x10A0F}, {0x10A38, 0x10A3A}, {0x10A3F, 0x10A3F},
    {0x10AE5, 0x10AE6}, {0x10D24, 0x10D27}, {0x10F46, 0x10F50},
    {0x11000, 0x11002}, {0x11038, 0x11046}, {0x1107F, 0x11082},
    {0x110B0, 0x110BA}, {0x11100, 0x11102}, {0x11127, 0x11134},
    {0x11145, 0x11146}, {0x11173, 0x11173}, {0x11180, 0x11182},
    {0x111B3, 0x111C0}, {0x111C9, 0x111CC}, {0x1122C, 0x11237},
    {0x1123E, 0x1123E}, {0x112DF, 0x112EA}, {0x11300, 0x11303},
    {0x1133B, 0x1133C}, {0x1133E, 0x11344}, {0x11347, 0x11348},
    {0x1134B, 0x1134D}, {0x11357, 0x11357}, {0x11362, 0x11363},
    {0x11366, 0x1136C}, {0x11370, 0x11374}, {0x11435, 0x11446},
    {0x1145E, 0x1145E}, {0x114B0, 0x114C3}, {0x115AF, 0x115B5},
    {0x115B8, 0x115C0}, {0x115DC, 0x115DD}, {0x11630, 0x11640},
    {0x116AB, 0x116B7}, {0x1171D, 0x1172B}, {0x1182C, 0x1183A},
    {0x11A01, 0x11A0A}, {0x11A33, 0x11A39}, {0x11A3B, 0x11A3E},
    {0x11A47, 0x11A47}, {0x11A51, 0x11A5B}, {0x11A8A, 0x11A99},
    {0x11C2F, 0x11C36}, {0x11C38, 0x11C3F}, {0x11C92, 0x11CA7},
    {0x11CA9, 0x11CB6}, {0x11D31, 0x11D36}, {0x11D3A, 0x11D3A},
    {0x11D3C, 0x11D3D}, {0x11D3F, 0x11D45}, {0x11D47, 0x11D47},
    {0x11D8A, 0x11D8E}, {0x11D90, 0x11D91}, {0x11D93, 0x11D97},
    {0x11EF3, 0x11EF6}, {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36},
    {0x16F51, 0x16F7E}, {0x16F8F, 0x16F92}, {0x1BC9D, 0x1BC9E},
    {0x1D165, 0x1D169}, {0x1D16D, 0x1D172}, {0x1D17B, 0x1D182},
    {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1D242, 0x1D244},
    {0x1DA00, 0x1DA36}, {0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75},
    {0x1DA84, 0x1DA84}, {0x1DA9B, 0x1DA9F}, {0x1DAA1, 0x1DAAF},
    {0x1E000, 0x1E006}, {0x1E008, 0x1E018}, {0x1E01B, 0x1E021},
    {0x1E023, 0x1E024}, {0x1E026, 0x1E02A}, {0x1E8D0, 0x1E8D6},
    {0x1E944, 0x1E94A}, {0xE0100, 0xE01EF}
};

/* Width.2 characters. */
static const struct widechar_range widechar_doublewide_table[] = {
    {0x01100, 0x0115F}, {0x0231A, 0x0231B}, {0x02329, 0x0232A},
    {0x023E9, 0x023EC}, {0x023F0, 0x023F0}, {0x023F3, 0x023F3},
    {0x025FD, 0x025FE}, {0x02614, 0x02615}, {0x02648, 0x02653},
    {0x0267F, 0x0267F}, {0x02693, 0x02693}, {0x026A1, 0x026A1},
    {0x026AA, 0x026AB}, {0x026BD, 0x026BE}, {0x026C4, 0x026C5},
    {0x026CE, 0x026CE}, {0x026D4, 0x026D4}, {0x026EA, 0x026EA},
    {0x026F2, 0x026F3}, {0x026F5, 0x026F5}, {0x026FA, 0x026FA},
    {0x026FD, 0x026FD}, {0x02705, 0x02705}, {0x0270A, 0x0270B},
    {0x02728, 0x02728}, {0x0274C, 0x0274C}, {0x0274E, 0x0274E},
    {0x02753, 0x02755}, {0x02757, 0x02757}, {0x02795, 0x02797},
    {0x027B0, 0x027B0}, {0x027BF, 0x027BF}, {0x02B1B, 0x02B1C},
    {0x02B50, 0x02B50}, {0x02B55, 0x02B55}, {0x02E80, 0x02E99},
    {0x02E9B, 0x02EF3}, {0x02F00, 0x02FD5}, {0x02FF0, 0x02FFB},
    {0x03000, 0x0303E}, {0x03041, 0x03096}, {0x03099, 0x030FF},
    {0x03105, 0x0312F}, {0x03131, 0x0318E}, {0x03190, 0x031BA},
    {0x031C0, 0x031E3}, {0x031F0, 0x0321E}, {0x03220, 0x03247},
    {0x03250, 0x032FE}, {0x03300, 0x04DBF}, {0x04E00, 0x0A48C},
    {0x0A490, 0x0A4C6}, {0x0A960, 0x0A97C}, {0x0AC00, 0x0D7A3},
    {0x0F900, 0x0FAFF}, {0x0FE10, 0x0FE19}, {0x0FE30, 0x0FE52},
    {0x0FE54, 0x0FE66}, {0x0FE68, 0x0FE6B}, {0x0FF01, 0x0FF60},
    {0x0FFE0, 0x0FFE6}, {0x16FE0, 0x16FE1}, {0x17000, 0x187F1},
    {0x18800, 0x18AF2}, {0x1B000, 0x1B11E}, {0x1B170, 0x1B2FB},
    {0x1F200, 0x1F200}, {0x1F210, 0x1F219}, {0x1F21B, 0x1F22E},
    {0x1F230, 0x1F231}, {0x1F23B, 0x1F23B}, {0x1F240, 0x1F248},
    {0x1F260, 0x1F265}, {0x1F57A, 0x1F57A}, {0x1F5A4, 0x1F5A4},
    {0x1F6D1, 0x1F6D2}, {0x1F6F4, 0x1F6F9}, {0x1F919, 0x1F93E},
    {0x1F940, 0x1F970}, {0x1F973, 0x1F976}, {0x1F97A, 0x1F97A},
    {0x1F97C, 0x1F97F}, {0x1F985, 0x1F9A2}, {0x1F9B0, 0x1F9B9},
    {0x1F9C1, 0x1F9C2}, {0x1F9D0, 0x1F9FF}, {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD}
};

/* Ambiguous-width characters. */
static const struct widechar_range widechar_ambiguous_table[] = {
    {0x000A1, 0x000A1}, {0x000A4, 0x000A4}, {0x000A7, 0x000A8},
    {0x000AA, 0x000AA}, {0x000AD, 0x000AE}, {0x000B0, 0x000B4},
    {0x000B6, 0x000BA}, {0x000BC, 0x000BF}, {0x000C6, 0x000C6},
    {0x000D0, 0x000D0}, {0x000D7, 0x000D8}, {0x000DE, 0x000E1},
    {0x000E6, 0x000E6}, {0x000E8, 0x000EA}, {0x000EC, 0x000ED},
    {0x000F0, 0x000F0}, {0x000F2, 0x000F3}, {0x000F7, 0x000FA},
    {0x000FC, 0x000FC}, {0x000FE, 0x000FE}, {0x00101, 0x00101},
    {0x00111, 0x00111}, {0x00113, 0x00113}, {0x0011B, 0x0011B},
    {0x00126, 0x00127}, {0x0012B, 0x0012B}, {0x00131, 0x00133},
    {0x00138, 0x00138}, {0x0013F, 0x00142}, {0x00144, 0x00144},
    {0x00148, 0x0014B}, {0x0014D, 0x0014D}, {0x00152, 0x00153},
    {0x00166, 0x00167}, {0x0016B, 0x0016B}, {0x001CE, 0x001CE},
    {0x001D0, 0x001D0}, {0x001D2, 0x001D2}, {0x001D4, 0x001D4},
    {0x001D6, 0x001D6}, {0x001D8, 0x001D8}, {0x001DA, 0x001DA},
    {0x001DC, 0x001DC}, {0x00251, 0x00251}, {0x00261, 0x00261},
    {0x002C4, 0x002C4}, {0x002C7, 0x002C7}, {0x002C9, 0x002CB},
    {0x002CD, 0x002CD}, {0x002D0, 0x002D0}, {0x002D8, 0x002DB},
    {0x002DD, 0x002DD}, {0x002DF, 0x002DF}, {0x00300, 0x0036F},
    {0x00391, 0x003A1}, {0x003A3, 0x003A9}, {0x003B1, 0x003C1},
    {0x003C3, 0x003C9}, {0x00401, 0x00401}, {0x00410, 0x0044F},
    {0x00451, 0x00451}, {0x02010, 0x02010}, {0x02013, 0x02016},
    {0x02018, 0x02019}, {0x0201C, 0x0201D}, {0x02020, 0x02022},
    {0x02024, 0x02027}, {0x02030, 0x02030}, {0x02032, 0x02033},
    {0x02035, 0x02035}, {0x0203B, 0x0203B}, {0x0203E, 0x0203E},
    {0x02074, 0x02074}, {0x0207F, 0x0207F}, {0x02081, 0x02084},
    {0x020AC, 0x020AC}, {0x02103, 0x02103}, {0x02105, 0x02105},
    {0x02109, 0x02109}, {0x02113, 0x02113}, {0x02116, 0x02116},
    {0x02121, 0x02122}, {0x02126, 0x02126}, {0x0212B, 0x0212B},
    {0x02153, 0x02154}, {0x0215B, 0x0215E}, {0x02160, 0x0216B},
    {0x02170, 0x02179}, {0x02189, 0x02189}, {0x02190, 0x02199},
    {0x021B8, 0x021B9}, {0x021D2, 0x021D2}, {0x021D4, 0x021D4},
    {0x021E7, 0x021E7}, {0x02200, 0x02200}, {0x02202, 0x02203},
    {0x02207, 0x02208}, {0x0220B, 0x0220B}, {0x0220F, 0x0220F},
    {0x02211, 0x02211}, {0x02215, 0x02215}, {0x0221A, 0x0221A},
    {0x0221D, 0x02220}, {0x02223, 0x02223}, {0x02225, 0x02225},
    {0x02227, 0x0222C}, {0x0222E, 0x0222E}, {0x02234, 0x02237},
    {0x0223C, 0x0223D}, {0x02248, 0x02248}, {0x0224C, 0x0224C},
    {0x02252, 0x02252}, {0x02260, 0x02261}, {0x02264, 0x02267},
    {0x0226A, 0x0226B}, {0x0226E, 0x0226F}, {0x02282, 0x02283},
    {0x02286, 0x02287}, {0x02295, 0x02295}, {0x02299, 0x02299},
    {0x022A5, 0x022A5}, {0x022BF, 0x022BF}, {0x02312, 0x02312},
    {0x02460, 0x024E9}, {0x024EB, 0x0254B}, {0x02550, 0x02573},
    {0x02580, 0x0258F}, {0x02592, 0x02595}, {0x025A0, 0x025A1},
    {0x025A3, 0x025A9}, {0x025B2, 0x025B3}, {0x025B6, 0x025B7},
    {0x025BC, 0x025BD}, {0x025C0, 0x025C1}, {0x025C6, 0x025C8},
    {0x025CB, 0x025CB}, {0x025CE, 0x025D1}, {0x025E2, 0x025E5},
    {0x025EF, 0x025EF}, {0x02605, 0x02606}, {0x02609, 0x02609},
    {0x0260E, 0x0260F}, {0x0261C, 0x0261C}, {0x0261E, 0x0261E},
    {0x02640, 0x02640}, {0x02642, 0x02642}, {0x02660, 0x02661},
    {0x02663, 0x02665}, {0x02667, 0x0266A}, {0x0266C, 0x0266D},
    {0x0266F, 0x0266F}, {0x0269E, 0x0269F}, {0x026BF, 0x026BF},
    {0x026C6, 0x026CD}, {0x026CF, 0x026D3}, {0x026D5, 0x026E1},
    {0x026E3, 0x026E3}, {0x026E8, 0x026E9}, {0x026EB, 0x026F1},
    {0x026F4, 0x026F4}, {0x026F6, 0x026F9}, {0x026FB, 0x026FC},
    {0x026FE, 0x026FF}, {0x0273D, 0x0273D}, {0x02776, 0x0277F},
    {0x02B56, 0x02B59}, {0x03248, 0x0324F}, {0x0E000, 0x0F8FF},
    {0x0FE00, 0x0FE0F}, {0x0FFFD, 0x0FFFD}, {0x1F100, 0x1F10A},
    {0x1F110, 0x1F12D}, {0x1F130, 0x1F169}, {0x1F172, 0x1F17D},
    {0x1F180, 0x1F18D}, {0x1F18F, 0x1F190}, {0x1F19B, 0x1F1AC},
    {0xE0100, 0xE01EF}, {0xF0000, 0xFFFFD}, {0x100000, 0x10FFFD}
};


/* Unassigned characters. */
static const struct widechar_range widechar_unassigned_table[] = {
    {0x00378, 0x00379}, {0x00380, 0x00383}, {0x0038B, 0x0038B},
    {0x0038D, 0x0038D}, {0x003A2, 0x003A2}, {0x00530, 0x00530},
    {0x00557, 0x00558}, {0x0058B, 0x0058C}, {0x00590, 0x00590},
    {0x005C8, 0x005CF}, {0x005EB, 0x005EE}, {0x005F5, 0x005FF},
    {0x0061D, 0x0061D}, {0x0070E, 0x0070E}, {0x0074B, 0x0074C},
    {0x007B2, 0x007BF}, {0x007FB, 0x007FC}, {0x0082E, 0x0082F},
    {0x0083F, 0x0083F}, {0x0085C, 0x0085D}, {0x0085F, 0x0085F},
    {0x0086B, 0x0089F}, {0x008B5, 0x008B5}, {0x008BE, 0x008D2},
    {0x00984, 0x00984}, {0x0098D, 0x0098E}, {0x00991, 0x00992},
    {0x009A9, 0x009A9}, {0x009B1, 0x009B1}, {0x009B3, 0x009B5},
    {0x009BA, 0x009BB}, {0x009C5, 0x009C6}, {0x009C9, 0x009CA},
    {0x009CF, 0x009D6}, {0x009D8, 0x009DB}, {0x009DE, 0x009DE},
    {0x009E4, 0x009E5}, {0x009FF, 0x00A00}, {0x00A04, 0x00A04},
    {0x00A0B, 0x00A0E}, {0x00A11, 0x00A12}, {0x00A29, 0x00A29},
    {0x00A31, 0x00A31}, {0x00A34, 0x00A34}, {0x00A37, 0x00A37},
    {0x00A3A, 0x00A3B}, {0x00A3D, 0x00A3D}, {0x00A43, 0x00A46},
    {0x00A49, 0x00A4A}, {0x00A4E, 0x00A50}, {0x00A52, 0x00A58},
    {0x00A5D, 0x00A5D}, {0x00A5F, 0x00A65}, {0x00A77, 0x00A80},
    {0x00A84, 0x00A84}, {0x00A8E, 0x00A8E}, {0x00A92, 0x00A92},
    {0x00AA9, 0x00AA9}, {0x00AB1, 0x00AB1}, {0x00AB4, 0x00AB4},
    {0x00ABA, 0x00ABB}, {0x00AC6, 0x00AC6}, {0x00ACA, 0x00ACA},
    {0x00ACE, 0x00ACF}, {0x00AD1, 0x00ADF}, {0x00AE4, 0x00AE5},
    {0x00AF2, 0x00AF8}, {0x00B00, 0x00B00}, {0x00B04, 0x00B04},
    {0x00B0D, 0x00B0E}, {0x00B11, 0x00B12}, {0x00B29, 0x00B29},
    {0x00B31, 0x00B31}, {0x00B34, 0x00B34}, {0x00B3A, 0x00B3B},
    {0x00B45, 0x00B46}, {0x00B49, 0x00B4A}, {0x00B4E, 0x00B55},
    {0x00B58, 0x00B5B}, {0x00B5E, 0x00B5E}, {0x00B64, 0x00B65},
    {0x00B78, 0x00B81}, {0x00B84, 0x00B84}, {0x00B8B, 0x00B8D},
    {0x00B91, 0x00B91}, {0x00B96, 0x00B98}, {0x00B9B, 0x00B9B},
    {0x00B9D, 0x00B9D}, {0x00BA0, 0x00BA2}, {0x00BA5, 0x00BA7},
    {0x00BAB, 0x00BAD}, {0x00BBA, 0x00BBD}, {0x00BC3, 0x00BC5},
    {0x00BC9, 0x00BC9}, {0x00BCE, 0x00BCF}, {0x00BD1, 0x00BD6},
    {0x00BD8, 0x00BE5}, {0x00BFB, 0x00BFF}, {0x00C0D, 0x00C0D},
    {0x00C11, 0x00C11}, {0x00C29, 0x00C29}, {0x00C3A, 0x00C3C},
    {0x00C45, 0x00C45}, {0x00C49, 0x00C49}, {0x00C4E, 0x00C54},
    {0x00C57, 0x00C57}, {0x00C5B, 0x00C5F}, {0x00C64, 0x00C65},
    {0x00C70, 0x00C77}, {0x00C8D, 0x00C8D}, {0x00C91, 0x00C91},
    {0x00CA9, 0x00CA9}, {0x00CB4, 0x00CB4}, {0x00CBA, 0x00CBB},
    {0x00CC5, 0x00CC5}, {0x00CC9, 0x00CC9}, {0x00CCE, 0x00CD4},
    {0x00CD7, 0x00CDD}, {0x00CDF, 0x00CDF}, {0x00CE4, 0x00CE5},
    {0x00CF0, 0x00CF0}, {0x00CF3, 0x00CFF}, {0x00D04, 0x00D04},
    {0x00D0D, 0x00D0D}, {0x00D11, 0x00D11}, {0x00D45, 0x00D45},
    {0x00D49, 0x00D49}, {0x00D50, 0x00D53}, {0x00D64, 0x00D65},
    {0x00D80, 0x00D81}, {0x00D84, 0x00D84}, {0x00D97, 0x00D99},
    {0x00DB2, 0x00DB2}, {0x00DBC, 0x00DBC}, {0x00DBE, 0x00DBF},
    {0x00DC7, 0x00DC9}, {0x00DCB, 0x00DCE}, {0x00DD5, 0x00DD5},
    {0x00DD7, 0x00DD7}, {0x00DE0, 0x00DE5}, {0x00DF0, 0x00DF1},
    {0x00DF5, 0x00E00}, {0x00E3B, 0x00E3E}, {0x00E5C, 0x00E80},
    {0x00E83, 0x00E83}, {0x00E85, 0x00E86}, {0x00E89, 0x00E89},
    {0x00E8B, 0x00E8C}, {0x00E8E, 0x00E93}, {0x00E98, 0x00E98},
    {0x00EA0, 0x00EA0}, {0x00EA4, 0x00EA4}, {0x00EA6, 0x00EA6},
    {0x00EA8, 0x00EA9}, {0x00EAC, 0x00EAC}, {0x00EBA, 0x00EBA},
    {0x00EBE, 0x00EBF}, {0x00EC5, 0x00EC5}, {0x00EC7, 0x00EC7},
    {0x00ECE, 0x00ECF}, {0x00EDA, 0x00EDB}, {0x00EE0, 0x00EFF},
    {0x00F48, 0x00F48}, {0x00F6D, 0x00F70}, {0x00F98, 0x00F98},
    {0x00FBD, 0x00FBD}, {0x00FCD, 0x00FCD}, {0x00FDB, 0x00FFF},
    {0x010C6, 0x010C6}, {0x010C8, 0x010CC}, {0x010CE, 0x010CF},
    {0x01249, 0x01249}, {0x0124E, 0x0124F}, {0x01257, 0x01257},
    {0x01259, 0x01259}, {0x0125E, 0x0125F}, {0x01289, 0x01289},
    {0x0128E, 0x0128F}, {0x012B1, 0x012B1}, {0x012B6, 0x012B7},
    {0x012BF, 0x012BF}, {0x012C1, 0x012C1}, {0x012C6, 0x012C7},
    {0x012D7, 0x012D7}, {0x01311, 0x01311}, {0x01316, 0x01317},
    {0x0135B, 0x0135C}, {0x0137D, 0x0137F}, {0x0139A, 0x0139F},
    {0x013F6, 0x013F7}, {0x013FE, 0x013FF}, {0x0169D, 0x0169F},
    {0x016F9, 0x016FF}, {0x0170D, 0x0170D}, {0x01715, 0x0171F},
    {0x01737, 0x0173F}, {0x01754, 0x0175F}, {0x0176D, 0x0176D},
    {0x01771, 0x01771}, {0x01774, 0x0177F}, {0x017DE, 0x017DF},
    {0x017EA, 0x017EF}, {0x017FA, 0x017FF}, {0x0180F, 0x0180F},
    {0x0181A, 0x0181F}, {0x01879, 0x0187F}, {0x018AB, 0x018AF},
    {0x018F6, 0x018FF}, {0x0191F, 0x0191F}, {0x0192C, 0x0192F},
    {0x0193C, 0x0193F}, {0x01941, 0x01943}, {0x0196E, 0x0196F},
    {0x01975, 0x0197F}, {0x019AC, 0x019AF}, {0x019CA, 0x019CF},
    {0x019DB, 0x019DD}, {0x01A1C, 0x01A1D}, {0x01A5F, 0x01A5F},
    {0x01A7D, 0x01A7E}, {0x01A8A, 0x01A8F}, {0x01A9A, 0x01A9F},
    {0x01AAE, 0x01AAF}, {0x01ABF, 0x01AFF}, {0x01B4C, 0x01B4F},
    {0x01B7D, 0x01B7F}, {0x01BF4, 0x01BFB}, {0x01C38, 0x01C3A},
    {0x01C4A, 0x01C4C}, {0x01C89, 0x01C8F}, {0x01CBB, 0x01CBC},
    {0x01CC8, 0x01CCF}, {0x01CFA, 0x01CFF}, {0x01DFA, 0x01DFA},
    {0x01F16, 0x01F17}, {0x01F1E, 0x01F1F}, {0x01F46, 0x01F47},
    {0x01F4E, 0x01F4F}, {0x01F58, 0x01F58}, {0x01F5A, 0x01F5A},
    {0x01F5C, 0x01F5C}, {0x01F5E, 0x01F5E}, {0x01F7E, 0x01F7F},
    {0x01FB5, 0x01FB5}, {0x01FC5, 0x01FC5}, {0x01FD4, 0x01FD5},
    {0x01FDC, 0x01FDC}, {0x01FF0, 0x01FF1}, {0x01FF5, 0x01FF5},
    {0x01FFF, 0x01FFF}, {0x02065, 0x02065}, {0x02072, 0x02073},
    {0x0208F, 0x0208F}, {0x0209D, 0x0209F}, {0x020C0, 0x020CF},
    {0x020F1, 0x020FF}, {0x0218C, 0x0218F}, {0x02427, 0x0243F},
    {0x0244B, 0x0245F}, {0x02B74, 0x02B75}, {0x02B96, 0x02B97},
    {0x02BC9, 0x02BC9}, {0x02BFF, 0x02BFF}, {0x02C2F, 0x02C2F},
    {0x02C5F, 0x02C5F}, {0x02CF4, 0x02CF8}, {0x02D26, 0x02D26},
    {0x02D28, 0x02D2C}, {0x02D2E, 0x02D2F}, {0x02D68, 0x02D6E},
    {0x02D71, 0x02D7E}, {0x02D97, 0x02D9F}, {0x02DA7, 0x02DA7},
    {0x02DAF, 0x02DAF}, {0x02DB7, 0x02DB7}, {0x02DBF, 0x02DBF},
    {0x02DC7, 0x02DC7}, {0x02DCF, 0x02DCF}, {0x02DD7, 0x02DD7},
    {0x02DDF, 0x02DDF}, {0x02E4F, 0x02E7F}, {0x02E9A, 0x02E9A},
    {0x02EF4, 0x02EFF}, {0x02FD6, 0x02FEF}, {0x02FFC, 0x02FFF},
    {0x03040, 0x03040}, {0x03097, 0x03098}, {0x03100, 0x03104},
    {0x03130, 0x03130}, {0x0318F, 0x0318F}, {0x031BB, 0x031BF},
    {0x031E4, 0x031EF}, {0x0321F, 0x0321F}, {0x032FF, 0x032FF},
    {0x03401, 0x04DB4}, {0x04DB6, 0x04DBF}, {0x04E01, 0x09FEE},
    {0x09FF0, 0x09FFF}, {0x0A48D, 0x0A48F}, {0x0A4C7, 0x0A4CF},
    {0x0A62C, 0x0A63F}, {0x0A6F8, 0x0A6FF}, {0x0A7BA, 0x0A7F6},
    {0x0A82C, 0x0A82F}, {0x0A83A, 0x0A83F}, {0x0A878, 0x0A87F},
    {0x0A8C6, 0x0A8CD}, {0x0A8DA, 0x0A8DF}, {0x0A954, 0x0A95E},
    {0x0A97D, 0x0A97F}, {0x0A9CE, 0x0A9CE}, {0x0A9DA, 0x0A9DD},
    {0x0A9FF, 0x0A9FF}, {0x0AA37, 0x0AA3F}, {0x0AA4E, 0x0AA4F},
    {0x0AA5A, 0x0AA5B}, {0x0AAC3, 0x0AADA}, {0x0AAF7, 0x0AB00},
    {0x0AB07, 0x0AB08}, {0x0AB0F, 0x0AB10}, {0x0AB17, 0x0AB1F},
    {0x0AB27, 0x0AB27}, {0x0AB2F, 0x0AB2F}, {0x0AB66, 0x0AB6F},
    {0x0ABEE, 0x0ABEF}, {0x0ABFA, 0x0ABFF}, {0x0AC01, 0x0D7A2},
    {0x0D7A4, 0x0D7AF}, {0x0D7C7, 0x0D7CA}, {0x0D7FC, 0x0D7FF},
    {0x0FA6E, 0x0FA6F}, {0x0FADA, 0x0FAFF}, {0x0FB07, 0x0FB12},
    {0x0FB18, 0x0FB1C}, {0x0FB37, 0x0FB37}, {0x0FB3D, 0x0FB3D},
    {0x0FB3F, 0x0FB3F}, {0x0FB42, 0x0FB42}, {0x0FB45, 0x0FB45},
    {0x0FBC2, 0x0FBD2}, {0x0FD40, 0x0FD4F}, {0x0FD90, 0x0FD91},
    {0x0FDC8, 0x0FDEF}, {0x0FDFE, 0x0FDFF}, {0x0FE1A, 0x0FE1F},
    {0x0FE53, 0x0FE53}, {0x0FE67, 0x0FE67}, {0x0FE6C, 0x0FE6F},
    {0x0FE75, 0x0FE75}, {0x0FEFD, 0x0FEFE}, {0x0FF00, 0x0FF00},
    {0x0FFBF, 0x0FFC1}, {0x0FFC8, 0x0FFC9}, {0x0FFD0, 0x0FFD1},
    {0x0FFD8, 0x0FFD9}, {0x0FFDD, 0x0FFDF}, {0x0FFE7, 0x0FFE7},
    {0x0FFEF, 0x0FFF8}, {0x0FFFE, 0x0FFFF}, {0x1000C, 0x1000C},
    {0x10027, 0x10027}, {0x1003B, 0x1003B}, {0x1003E, 0x1003E},
    {0x1004E, 0x1004F}, {0x1005E, 0x1007F}, {0x100FB, 0x100FF},
    {0x10103, 0x10106}, {0x10134, 0x10136}, {0x1018F, 0x1018F},
    {0x1019C, 0x1019F}, {0x101A1, 0x101CF}, {0x101FE, 0x1027F},
    {0x1029D, 0x1029F}, {0x102D1, 0x102DF}, {0x102FC, 0x102FF},
    {0x10324, 0x1032C}, {0x1034B, 0x1034F}, {0x1037B, 0x1037F},
    {0x1039E, 0x1039E}, {0x103C4, 0x103C7}, {0x103D6, 0x103FF},
    {0x1049E, 0x1049F}, {0x104AA, 0x104AF}, {0x104D4, 0x104D7},
    {0x104FC, 0x104FF}, {0x10528, 0x1052F}, {0x10564, 0x1056E},
    {0x10570, 0x105FF}, {0x10737, 0x1073F}, {0x10756, 0x1075F},
    {0x10768, 0x107FF}, {0x10806, 0x10807}, {0x10809, 0x10809},
    {0x10836, 0x10836}, {0x10839, 0x1083B}, {0x1083D, 0x1083E},
    {0x10856, 0x10856}, {0x1089F, 0x108A6}, {0x108B0, 0x108DF},
    {0x108F3, 0x108F3}, {0x108F6, 0x108FA}, {0x1091C, 0x1091E},
    {0x1093A, 0x1093E}, {0x10940, 0x1097F}, {0x109B8, 0x109BB},
    {0x109D0, 0x109D1}, {0x10A04, 0x10A04}, {0x10A07, 0x10A0B},
    {0x10A14, 0x10A14}, {0x10A18, 0x10A18}, {0x10A36, 0x10A37},
    {0x10A3B, 0x10A3E}, {0x10A49, 0x10A4F}, {0x10A59, 0x10A5F},
    {0x10AA0, 0x10ABF}, {0x10AE7, 0x10AEA}, {0x10AF7, 0x10AFF},
    {0x10B36, 0x10B38}, {0x10B56, 0x10B57}, {0x10B73, 0x10B77},
    {0x10B92, 0x10B98}, {0x10B9D, 0x10BA8}, {0x10BB0, 0x10BFF},
    {0x10C49, 0x10C7F}, {0x10CB3, 0x10CBF}, {0x10CF3, 0x10CF9},
    {0x10D28, 0x10D2F}, {0x10D3A, 0x10E5F}, {0x10E7F, 0x10EFF},
    {0x10F28, 0x10F2F}, {0x10F5A, 0x10FFF}, {0x1104E, 0x11051},
    {0x11070, 0x1107E}, {0x110C2, 0x110CC}, {0x110CE, 0x110CF},
    {0x110E9, 0x110EF}, {0x110FA, 0x110FF}, {0x11135, 0x11135},
    {0x11147, 0x1114F}, {0x11177, 0x1117F}, {0x111CE, 0x111CF},
    {0x111E0, 0x111E0}, {0x111F5, 0x111FF}, {0x11212, 0x11212},
    {0x1123F, 0x1127F}, {0x11287, 0x11287}, {0x11289, 0x11289},
    {0x1128E, 0x1128E}, {0x1129E, 0x1129E}, {0x112AA, 0x112AF},
    {0x112EB, 0x112EF}, {0x112FA, 0x112FF}, {0x11304, 0x11304},
    {0x1130D, 0x1130E}, {0x11311, 0x11312}, {0x11329, 0x11329},
    {0x11331, 0x11331}, {0x11334, 0x11334}, {0x1133A, 0x1133A},
    {0x11345, 0x11346}, {0x11349, 0x1134A}, {0x1134E, 0x1134F},
    {0x11351, 0x11356}, {0x11358, 0x1135C}, {0x11364, 0x11365},
    {0x1136D, 0x1136F}, {0x11375, 0x113FF}, {0x1145A, 0x1145A},
    {0x1145C, 0x1145C}, {0x1145F, 0x1147F}, {0x114C8, 0x114CF},
    {0x114DA, 0x1157F}, {0x115B6, 0x115B7}, {0x115DE, 0x115FF},
    {0x11645, 0x1164F}, {0x1165A, 0x1165F}, {0x1166D, 0x1167F},
    {0x116B8, 0x116BF}, {0x116CA, 0x116FF}, {0x1171B, 0x1171C},
    {0x1172C, 0x1172F}, {0x11740, 0x117FF}, {0x1183C, 0x1189F},
    {0x118F3, 0x118FE}, {0x11900, 0x119FF}, {0x11A48, 0x11A4F},
    {0x11A84, 0x11A85}, {0x11AA3, 0x11ABF}, {0x11AF9, 0x11BFF},
    {0x11C09, 0x11C09}, {0x11C37, 0x11C37}, {0x11C46, 0x11C4F},
    {0x11C6D, 0x11C6F}, {0x11C90, 0x11C91}, {0x11CA8, 0x11CA8},
    {0x11CB7, 0x11CFF}, {0x11D07, 0x11D07}, {0x11D0A, 0x11D0A},
    {0x11D37, 0x11D39}, {0x11D3B, 0x11D3B}, {0x11D3E, 0x11D3E},
    {0x11D48, 0x11D4F}, {0x11D5A, 0x11D5F}, {0x11D66, 0x11D66},
    {0x11D69, 0x11D69}, {0x11D8F, 0x11D8F}, {0x11D92, 0x11D92},
    {0x11D99, 0x11D9F}, {0x11DAA, 0x11EDF}, {0x11EF9, 0x11FFF},
    {0x1239A, 0x123FF}, {0x1246F, 0x1246F}, {0x12475, 0x1247F},
    {0x12544, 0x12FFF}, {0x1342F, 0x143FF}, {0x14647, 0x167FF},
    {0x16A39, 0x16A3F}, {0x16A5F, 0x16A5F}, {0x16A6A, 0x16A6D},
    {0x16A70, 0x16ACF}, {0x16AEE, 0x16AEF}, {0x16AF6, 0x16AFF},
    {0x16B46, 0x16B4F}, {0x16B5A, 0x16B5A}, {0x16B62, 0x16B62},
    {0x16B78, 0x16B7C}, {0x16B90, 0x16E3F}, {0x16E9B, 0x16EFF},
    {0x16F45, 0x16F4F}, {0x16F7F, 0x16F8E}, {0x16FA0, 0x16FDF},
    {0x16FE2, 0x16FFF}, {0x17001, 0x187F0}, {0x187F2, 0x187FF},
    {0x18AF3, 0x1AFFF}, {0x1B11F, 0x1B16F}, {0x1B2FC, 0x1BBFF},
    {0x1BC6B, 0x1BC6F}, {0x1BC7D, 0x1BC7F}, {0x1BC89, 0x1BC8F},
    {0x1BC9A, 0x1BC9B}, {0x1BCA4, 0x1CFFF}, {0x1D0F6, 0x1D0FF},
    {0x1D127, 0x1D128}, {0x1D1E9, 0x1D1FF}, {0x1D246, 0x1D2DF},
    {0x1D2F4, 0x1D2FF}, {0x1D357, 0x1D35F}, {0x1D379, 0x1D3FF},
    {0x1D455, 0x1D455}, {0x1D49D, 0x1D49D}, {0x1D4A0, 0x1D4A1},
    {0x1D4A3, 0x1D4A4}, {0x1D4A7, 0x1D4A8}, {0x1D4AD, 0x1D4AD},
    {0x1D4BA, 0x1D4BA}, {0x1D4BC, 0x1D4BC}, {0x1D4C4, 0x1D4C4},
    {0x1D506, 0x1D506}, {0x1D50B, 0x1D50C}, {0x1D515, 0x1D515},
    {0x1D51D, 0x1D51D}, {0x1D53A, 0x1D53A}, {0x1D53F, 0x1D53F},
    {0x1D545, 0x1D545}, {0x1D547, 0x1D549}, {0x1D551, 0x1D551},
    {0x1D6A6, 0x1D6A7}, {0x1D7CC, 0x1D7CD}, {0x1DA8C, 0x1DA9A},
    {0x1DAA0, 0x1DAA0}, {0x1DAB0, 0x1DFFF}, {0x1E007, 0x1E007},
    {0x1E019, 0x1E01A}, {0x1E022, 0x1E022}, {0x1E025, 0x1E025},
    {0x1E02B, 0x1E7FF}, {0x1E8C5, 0x1E8C6}, {0x1E8D7, 0x1E8FF},
    {0x1E94B, 0x1E94F}, {0x1E95A, 0x1E95D}, {0x1E960, 0x1EC70},
    {0x1ECB5, 0x1EDFF}, {0x1EE04, 0x1EE04}, {0x1EE20, 0x1EE20},
    {0x1EE23, 0x1EE23}, {0x1EE25, 0x1EE26}, {0x1EE28, 0x1EE28},
    {0x1EE33, 0x1EE33}, {0x1EE38, 0x1EE38}, {0x1EE3A, 0x1EE3A},
    {0x1EE3C, 0x1EE41}, {0x1EE43, 0x1EE46}, {0x1EE48, 0x1EE48},
    {0x1EE4A, 0x1EE4A}, {0x1EE4C, 0x1EE4C}, {0x1EE50, 0x1EE50},
    {0x1EE53, 0x1EE53}, {0x1EE55, 0x1EE56}, {0x1EE58, 0x1EE58},
    {0x1EE5A, 0x1EE5A}, {0x1EE5C, 0x1EE5C}, {0x1EE5E, 0x1EE5E},
    {0x1EE60, 0x1EE60}, {0x1EE63, 0x1EE63}, {0x1EE65, 0x1EE66},
    {0x1EE6B, 0x1EE6B}, {0x1EE73, 0x1EE73}, {0x1EE78, 0x1EE78},
    {0x1EE7D, 0x1EE7D}, {0x1EE7F, 0x1EE7F}, {0x1EE8A, 0x1EE8A},
    {0x1EE9C, 0x1EEA0}, {0x1EEA4, 0x1EEA4}, {0x1EEAA, 0x1EEAA},
    {0x1EEBC, 0x1EEEF}, {0x1EEF2, 0x1EFFF}, {0x1F02C, 0x1F02F},
    {0x1F094, 0x1F09F}, {0x1F0AF, 0x1F0B0}, {0x1F0C0, 0x1F0C0},
    {0x1F0D0, 0x1F0D0}, {0x1F0F6, 0x1F0FF}, {0x1F10D, 0x1F10F},
    {0x1F16C, 0x1F16F}, {0x1F1AD, 0x1F1E5}, {0x1F203, 0x1F20F},
    {0x1F23C, 0x1F23F}, {0x1F249, 0x1F24F}, {0x1F252, 0x1F25F},
    {0x1F266, 0x1F2FF}, {0x1F6D5, 0x1F6DF}, {0x1F6ED, 0x1F6EF},
    {0x1F6FA, 0x1F6FF}, {0x1F774, 0x1F77F}, {0x1F7D9, 0x1F7FF},
    {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F},
    {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F90F},
    {0x1F93F, 0x1F93F}, {0x1F971, 0x1F972}, {0x1F977, 0x1F979},
    {0x1F97B, 0x1F97B}, {0x1F9A3, 0x1F9AF}, {0x1F9BA, 0x1F9BF},
    {0x1F9C3, 0x1F9CF}, {0x1FA00, 0x1FA5F}, {0x1FA6E, 0x1FFFF},
    {0x20001, 0x2A6D5}, {0x2A6D7, 0x2A6FF}, {0x2A701, 0x2B733},
    {0x2B735, 0x2B73F}, {0x2B741, 0x2B81C}, {0x2B81E, 0x2B81F},
    {0x2B821, 0x2CEA0}, {0x2CEA2, 0x2CEAF}, {0x2CEB1, 0x2EBDF},
    {0x2EBE1, 0x2F7FF}, {0x2FA1E, 0xE0000}, {0xE0002, 0xE001F},
    {0xE0080, 0xE00FF}, {0xE01F0, 0xEFFFF}, {0xFFFFE, 0xFFFFF},
    {0x10FFFE, 0x110000}
};

/* Characters that were widened from with 1 to 2 in Unicode 9. */
static const struct widechar_range widechar_widened_table[] = {
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F170, 0x1F171},
    {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F202}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F250, 0x1F251},
    {0x1F300, 0x1F321}, {0x1F324, 0x1F393}, {0x1F396, 0x1F397},
    {0x1F399, 0x1F39B}, {0x1F39E, 0x1F3F0}, {0x1F3F3, 0x1F3F5},
    {0x1F3F7, 0x1F4FD}, {0x1F4FF, 0x1F53D}, {0x1F549, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F56F, 0x1F570}, {0x1F573, 0x1F579},
    {0x1F587, 0x1F587}, {0x1F58A, 0x1F58D}, {0x1F590, 0x1F590},
    {0x1F595, 0x1F596}, {0x1F5A5, 0x1F5A5}, {0x1F5A8, 0x1F5A8},
    {0x1F5B1, 0x1F5B2}, {0x1F5BC, 0x1F5BC}, {0x1F5C2, 0x1F5C4},
    {0x1F5D1, 0x1F5D3}, {0x1F5DC, 0x1F5DE}, {0x1F5E1, 0x1F5E1},
    {0x1F5E3, 0x1F5E3}, {0x1F5E8, 0x1F5E8}, {0x1F5EF, 0x1F5EF},
    {0x1F5F3, 0x1F5F3}, {0x1F5FA, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CB, 0x1F6D0}, {0x1F6E0, 0x1F6E5}, {0x1F6E9, 0x1F6E9},
    {0x1F6EB, 0x1F6EC}, {0x1F6F0, 0x1F6F0}, {0x1F6F3, 0x1F6F3},
    {0x1F910, 0x1F918}, {0x1F980, 0x1F984}, {0x1F9C0, 0x1F9C0}
};

template <typename Collection>
bool widechar_in_table(const Collection &arr, int32_t c) {
    auto where = std::lower_bound(std::begin(arr), std::end(arr), c,
        [](widechar_range p, int32_t chr) { return p.hi < chr; });
    return where != std::end(arr) && where->lo <= c;
}

/* Return the width of character c, or a special negative value. */
inline int widechar_wcwidth(wchar_t c) {
    if (widechar_in_table(widechar_private_table, c))
        return widechar_private_use;
    if (widechar_in_table(widechar_nonprint_table, c))
        return widechar_nonprint;
    if (widechar_in_table(widechar_combining_table, c))
        return widechar_combining;
    if (widechar_in_table(widechar_doublewide_table, c))
        return 2;
    if (widechar_in_table(widechar_ambiguous_table, c))
        return widechar_ambiguous;
    if (widechar_in_table(widechar_unassigned_table, c))
        return widechar_unassigned;
    if (widechar_in_table(widechar_widened_table, c))
        return widechar_widened_in_9;
    return 1;
}

// NOLINTEND(*)

#endif // WIDECHAR_WIDTH_H
