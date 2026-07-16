#pragma once
#include <stdint.h>

// Fixed 5x7 bitmap font: space, digits, uppercase letters, and a small set
// of punctuation. Each glyph is 7 rows of 5 bits; bit 4 is the leftmost
// column and bit 0 the rightmost, so a literal like 0b01110 reads left-to-
// right the same as the row it draws. Callers fold lowercase to uppercase
// before lookup since there are no lowercase glyphs.
namespace Font5x7 {

constexpr uint8_t GLYPH_WIDTH = 5;
constexpr uint8_t GLYPH_HEIGHT = 7;

struct Glyph {
    uint8_t rows[GLYPH_HEIGHT];
};

// Shown for any character with no glyph below (hollow box).
constexpr Glyph GLYPH_FALLBACK = {{
    0b11111,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b11111,
}};

// .....
// .....
// .....
// .....
// .....
// .....
// .....
constexpr Glyph GLYPH_SPACE = {{
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
}};

// .###.
// #...#
// #..##
// #.#.#
// ##..#
// #...#
// .###.
constexpr Glyph GLYPH_0 = {{
    0b01110,
    0b10001,
    0b10011,
    0b10101,
    0b11001,
    0b10001,
    0b01110,
}};

// ..#..
// .##..
// ..#..
// ..#..
// ..#..
// ..#..
// .###.
constexpr Glyph GLYPH_1 = {{
    0b00100,
    0b01100,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b01110,
}};

// .###.
// #...#
// ....#
// ...#.
// ..#..
// .#...
// #####
constexpr Glyph GLYPH_2 = {{
    0b01110,
    0b10001,
    0b00001,
    0b00010,
    0b00100,
    0b01000,
    0b11111,
}};

// .###.
// #...#
// ....#
// ..##.
// ....#
// #...#
// .###.
constexpr Glyph GLYPH_3 = {{
    0b01110,
    0b10001,
    0b00001,
    0b00110,
    0b00001,
    0b10001,
    0b01110,
}};

// ...#.
// ..##.
// .#.#.
// #..#.
// #####
// ...#.
// ...#.
constexpr Glyph GLYPH_4 = {{
    0b00010,
    0b00110,
    0b01010,
    0b10010,
    0b11111,
    0b00010,
    0b00010,
}};

// #####
// #....
// ####.
// ....#
// ....#
// #...#
// .###.
constexpr Glyph GLYPH_5 = {{
    0b11111,
    0b10000,
    0b11110,
    0b00001,
    0b00001,
    0b10001,
    0b01110,
}};

// ..##.
// .#...
// #....
// ####.
// #...#
// #...#
// .###.
constexpr Glyph GLYPH_6 = {{
    0b00110,
    0b01000,
    0b10000,
    0b11110,
    0b10001,
    0b10001,
    0b01110,
}};

// #####
// ....#
// ...#.
// ..#..
// .#...
// .#...
// .#...
constexpr Glyph GLYPH_7 = {{
    0b11111,
    0b00001,
    0b00010,
    0b00100,
    0b01000,
    0b01000,
    0b01000,
}};

// .###.
// #...#
// #...#
// .###.
// #...#
// #...#
// .###.
constexpr Glyph GLYPH_8 = {{
    0b01110,
    0b10001,
    0b10001,
    0b01110,
    0b10001,
    0b10001,
    0b01110,
}};

// .###.
// #...#
// #...#
// .####
// ....#
// ...#.
// .##..
constexpr Glyph GLYPH_9 = {{
    0b01110,
    0b10001,
    0b10001,
    0b01111,
    0b00001,
    0b00010,
    0b01100,
}};

// .###.
// #...#
// #...#
// #####
// #...#
// #...#
// #...#
constexpr Glyph GLYPH_A = {{
    0b01110,
    0b10001,
    0b10001,
    0b11111,
    0b10001,
    0b10001,
    0b10001,
}};

// ####.
// #...#
// #...#
// ####.
// #...#
// #...#
// ####.
constexpr Glyph GLYPH_B = {{
    0b11110,
    0b10001,
    0b10001,
    0b11110,
    0b10001,
    0b10001,
    0b11110,
}};

// .####
// #....
// #....
// #....
// #....
// #....
// .####
constexpr Glyph GLYPH_C = {{
    0b01111,
    0b10000,
    0b10000,
    0b10000,
    0b10000,
    0b10000,
    0b01111,
}};

// ####.
// #...#
// #...#
// #...#
// #...#
// #...#
// ####.
constexpr Glyph GLYPH_D = {{
    0b11110,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b11110,
}};

// #####
// #....
// #....
// ####.
// #....
// #....
// #####
constexpr Glyph GLYPH_E = {{
    0b11111,
    0b10000,
    0b10000,
    0b11110,
    0b10000,
    0b10000,
    0b11111,
}};

// #####
// #....
// #....
// ####.
// #....
// #....
// #....
constexpr Glyph GLYPH_F = {{
    0b11111,
    0b10000,
    0b10000,
    0b11110,
    0b10000,
    0b10000,
    0b10000,
}};

// .####
// #....
// #....
// #..##
// #...#
// #...#
// .####
constexpr Glyph GLYPH_G = {{
    0b01111,
    0b10000,
    0b10000,
    0b10011,
    0b10001,
    0b10001,
    0b01111,
}};

// #...#
// #...#
// #...#
// #####
// #...#
// #...#
// #...#
constexpr Glyph GLYPH_H = {{
    0b10001,
    0b10001,
    0b10001,
    0b11111,
    0b10001,
    0b10001,
    0b10001,
}};

// .###.
// ..#..
// ..#..
// ..#..
// ..#..
// ..#..
// .###.
constexpr Glyph GLYPH_I = {{
    0b01110,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b01110,
}};

// ..###
// ...#.
// ...#.
// ...#.
// ...#.
// #..#.
// .##..
constexpr Glyph GLYPH_J = {{
    0b00111,
    0b00010,
    0b00010,
    0b00010,
    0b00010,
    0b10010,
    0b01100,
}};

// #...#
// #..#.
// #.#..
// ##...
// #.#..
// #..#.
// #...#
constexpr Glyph GLYPH_K = {{
    0b10001,
    0b10010,
    0b10100,
    0b11000,
    0b10100,
    0b10010,
    0b10001,
}};

// #....
// #....
// #....
// #....
// #....
// #....
// #####
constexpr Glyph GLYPH_L = {{
    0b10000,
    0b10000,
    0b10000,
    0b10000,
    0b10000,
    0b10000,
    0b11111,
}};

// #...#
// ##.##
// #.#.#
// #.#.#
// #...#
// #...#
// #...#
constexpr Glyph GLYPH_M = {{
    0b10001,
    0b11011,
    0b10101,
    0b10101,
    0b10001,
    0b10001,
    0b10001,
}};

// #...#
// ##..#
// #.#.#
// #.#.#
// #..##
// #...#
// #...#
constexpr Glyph GLYPH_N = {{
    0b10001,
    0b11001,
    0b10101,
    0b10101,
    0b10011,
    0b10001,
    0b10001,
}};

// .###.
// #...#
// #...#
// #...#
// #...#
// #...#
// .###.
constexpr Glyph GLYPH_O = {{
    0b01110,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b01110,
}};

// ####.
// #...#
// #...#
// ####.
// #....
// #....
// #....
constexpr Glyph GLYPH_P = {{
    0b11110,
    0b10001,
    0b10001,
    0b11110,
    0b10000,
    0b10000,
    0b10000,
}};

// .###.
// #...#
// #...#
// #...#
// #...#
// #...#
// .##.#
constexpr Glyph GLYPH_Q = {{
    0b01110,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b01101,
}};

// ####.
// #...#
// #...#
// ####.
// #.#..
// #..#.
// #...#
constexpr Glyph GLYPH_R = {{
    0b11110,
    0b10001,
    0b10001,
    0b11110,
    0b10100,
    0b10010,
    0b10001,
}};

// .####
// #....
// #....
// .###.
// ....#
// ....#
// ####.
constexpr Glyph GLYPH_S = {{
    0b01111,
    0b10000,
    0b10000,
    0b01110,
    0b00001,
    0b00001,
    0b11110,
}};

// #####
// ..#..
// ..#..
// ..#..
// ..#..
// ..#..
// ..#..
constexpr Glyph GLYPH_T = {{
    0b11111,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
}};

// #...#
// #...#
// #...#
// #...#
// #...#
// #...#
// .###.
constexpr Glyph GLYPH_U = {{
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b01110,
}};

// #...#
// #...#
// #...#
// #...#
// #...#
// .#.#.
// ..#..
constexpr Glyph GLYPH_V = {{
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b01010,
    0b00100,
}};

// #...#
// #...#
// #...#
// #.#.#
// #.#.#
// #.#.#
// .#.#.
constexpr Glyph GLYPH_W = {{
    0b10001,
    0b10001,
    0b10001,
    0b10101,
    0b10101,
    0b10101,
    0b01010,
}};

// #...#
// #...#
// .#.#.
// ..#..
// .#.#.
// #...#
// #...#
constexpr Glyph GLYPH_X = {{
    0b10001,
    0b10001,
    0b01010,
    0b00100,
    0b01010,
    0b10001,
    0b10001,
}};

// #...#
// #...#
// .#.#.
// ..#..
// ..#..
// ..#..
// ..#..
constexpr Glyph GLYPH_Y = {{
    0b10001,
    0b10001,
    0b01010,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
}};

// #####
// ....#
// ...#.
// ..#..
// .#...
// #....
// #####
constexpr Glyph GLYPH_Z = {{
    0b11111,
    0b00001,
    0b00010,
    0b00100,
    0b01000,
    0b10000,
    0b11111,
}};

// .....
// .....
// .....
// .....
// .....
// .....
// .##..
constexpr Glyph GLYPH_PERIOD = {{
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b01100,
}};

// .....
// .....
// .....
// .....
// .....
// ..#..
// .#...
constexpr Glyph GLYPH_COMMA = {{
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00100,
    0b01000,
}};

// ..#..
// ..#..
// ..#..
// ..#..
// ..#..
// .....
// ..#..
constexpr Glyph GLYPH_EXCLAIM = {{
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b00000,
    0b00100,
}};

// .###.
// #...#
// ....#
// ...#.
// ..#..
// .....
// ..#..
constexpr Glyph GLYPH_QUESTION = {{
    0b01110,
    0b10001,
    0b00001,
    0b00010,
    0b00100,
    0b00000,
    0b00100,
}};

// .....
// .##..
// .##..
// .....
// .##..
// .##..
// .....
constexpr Glyph GLYPH_COLON = {{
    0b00000,
    0b01100,
    0b01100,
    0b00000,
    0b01100,
    0b01100,
    0b00000,
}};

// .....
// .##..
// .##..
// .....
// ..#..
// .#...
// .....
constexpr Glyph GLYPH_SEMICOLON = {{
    0b00000,
    0b01100,
    0b01100,
    0b00000,
    0b00100,
    0b01000,
    0b00000,
}};

// .....
// .....
// .....
// .###.
// .....
// .....
// .....
constexpr Glyph GLYPH_HYPHEN = {{
    0b00000,
    0b00000,
    0b00000,
    0b01110,
    0b00000,
    0b00000,
    0b00000,
}};

// .##..
// .##..
// .....
// .....
// .....
// .....
// .....
constexpr Glyph GLYPH_APOSTROPHE = {{
    0b01100,
    0b01100,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
}};

// .#.#.
// .#.#.
// .....
// .....
// .....
// .....
// .....
constexpr Glyph GLYPH_QUOTE = {{
    0b01010,
    0b01010,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
}};

// Returns the glyph for c, folding lowercase to uppercase, or
// GLYPH_FALLBACK for anything unsupported.
inline const Glyph& glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
    switch (c) {
        case ' ':
            return GLYPH_SPACE;
        case '0':
            return GLYPH_0;
        case '1':
            return GLYPH_1;
        case '2':
            return GLYPH_2;
        case '3':
            return GLYPH_3;
        case '4':
            return GLYPH_4;
        case '5':
            return GLYPH_5;
        case '6':
            return GLYPH_6;
        case '7':
            return GLYPH_7;
        case '8':
            return GLYPH_8;
        case '9':
            return GLYPH_9;
        case 'A':
            return GLYPH_A;
        case 'B':
            return GLYPH_B;
        case 'C':
            return GLYPH_C;
        case 'D':
            return GLYPH_D;
        case 'E':
            return GLYPH_E;
        case 'F':
            return GLYPH_F;
        case 'G':
            return GLYPH_G;
        case 'H':
            return GLYPH_H;
        case 'I':
            return GLYPH_I;
        case 'J':
            return GLYPH_J;
        case 'K':
            return GLYPH_K;
        case 'L':
            return GLYPH_L;
        case 'M':
            return GLYPH_M;
        case 'N':
            return GLYPH_N;
        case 'O':
            return GLYPH_O;
        case 'P':
            return GLYPH_P;
        case 'Q':
            return GLYPH_Q;
        case 'R':
            return GLYPH_R;
        case 'S':
            return GLYPH_S;
        case 'T':
            return GLYPH_T;
        case 'U':
            return GLYPH_U;
        case 'V':
            return GLYPH_V;
        case 'W':
            return GLYPH_W;
        case 'X':
            return GLYPH_X;
        case 'Y':
            return GLYPH_Y;
        case 'Z':
            return GLYPH_Z;
        case '.':
            return GLYPH_PERIOD;
        case ',':
            return GLYPH_COMMA;
        case '!':
            return GLYPH_EXCLAIM;
        case '?':
            return GLYPH_QUESTION;
        case ':':
            return GLYPH_COLON;
        case ';':
            return GLYPH_SEMICOLON;
        case '-':
            return GLYPH_HYPHEN;
        case '\'':
            return GLYPH_APOSTROPHE;
        case '\"':
            return GLYPH_QUOTE;
        default:
            return GLYPH_FALLBACK;
    }
}

}  // namespace Font5x7
