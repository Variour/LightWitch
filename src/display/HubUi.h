#pragma once
#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <esp_heap_caps.h>

#include "../game/GameTypes.h"
#include "../logging/Logger.h"
#include "HubBoard.h"
#include "HubView.h"

namespace display {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Hub display (FEATURE_DISPLAY). Portrait 320 x 480:
//   header  y 0-56    session, console, channel, members, login, low battery
//   content y 56-432  page by state (Auto) or the page picked on the tab bar
//   tabs    y 432-480 Auto | Geräte | Funk | Hell
// The hub runs without operation: Auto follows the session by itself. Touch
// and the BOOT key only pick a page and the brightness; a picked page falls
// back to Auto after kManualMs without input. Session control stays with the
// console and the teacher device.
//
// The frame is drawn into a PSRAM canvas; only the rows that changed since
// the last frame go out over SPI.
class HubUi {
   public:
    enum class Page : uint8_t { Auto, Devices, Radio };

    bool begin() {
        _board.begin();
        _frame = new GFXcanvas16(HubBoard::kWidth, HubBoard::kHeight);
        size_t bytes = (size_t)HubBoard::kWidth * HubBoard::kHeight * 2;
        _last = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!_frame || !_frame->getBuffer() || !_last) {
            Logger::e("[disp] no memory for the frame");
            return false;
        }
        memset(_last, 0xA5, bytes);  // first frame goes out in full
        _text.begin(*_frame);
        _text.setFontMode(1);
        _board.setBacklight(kBrightness[_brightness]);
        _ok = true;
        Logger::i("[disp] hub display ready, touch %s", _board.touchOk() ? "found" : "missing");
        return true;
    }

    // Call every loop; draws at most every kFrameMs.
    void tick(const HubView& v, uint32_t now) {
        if (!_ok) return;
        _input(now);
        if (_page != Page::Auto && now - _lastInput > kManualMs) _page = Page::Auto;
        if (now - _lastDraw < kFrameMs) return;
        _lastDraw = now;
        _draw(v);
        _flush();
    }

   private:
    // ── Look ─────────────────────────────────────────────────────────────────
    static constexpr uint16_t W = HubBoard::kWidth;
    static constexpr int16_t kHeaderH = 56, kTabY = 432, kTabH = 48, kPad = 12;
    static constexpr int16_t kContentY = kHeaderH, kContentH = kTabY - kHeaderH;
    static constexpr uint32_t kFrameMs = 200;
    static constexpr uint32_t kManualMs = 30000;
    static constexpr uint32_t kLongPressMs = 800;
    static constexpr uint8_t kBrightness[3] = {100, 60, 30};

    static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return rgb565(r, g, b); }
    static uint16_t rgb(game::Rgb c) { return rgb565(c.r, c.g, c.b); }

    // farben-standard: green for right and logged in, orange red (dimmed)
    // for wrong, orange for no connection, blue for login / invite.
    static constexpr uint16_t kBg = rgb565(0x07, 0x50, 0x33);     // dark green 075033
    static constexpr uint16_t kPanel = rgb565(0x05, 0x3C, 0x26);  // header, tabs, boxes
    static constexpr uint16_t kLine = rgb565(0x2E, 0x7A, 0x58);
    static constexpr uint16_t kText = rgb565(255, 255, 255);
    static constexpr uint16_t kDim = rgb565(0xA8, 0xC2, 0xB8);
    static constexpr uint16_t kGrey = rgb565(0xC3, 0xC3, 0xC3);  // closed, other answers
    static constexpr uint16_t kRingOff =
        rgb565(0x0B, 0x33, 0x23);  // expired segment, like an LED off
    static constexpr uint16_t kGreen = rgb565(0x2F, 0xFB, 0x95);  // right, and only that
    static constexpr uint16_t kNeben = rgb565(0xFF, 0x9C, 0x5C);  // daneben
    static constexpr uint16_t kNone = rgb565(0xA7, 0x74, 0xD8);   // nicht abgegeben
    static constexpr uint16_t kWarn = rgb565(0xFF, 0xD5, 0x4A);   // technical: battery, signal

    HubBoard _board;
    GFXcanvas16* _frame = nullptr;
    uint16_t* _last = nullptr;
    U8G2_FOR_ADAFRUIT_GFX _text;
    bool _ok = false;
    Page _page = Page::Auto;
    uint8_t _brightness = 0;
    uint16_t _devicePage = 0;
    uint32_t _lastDraw = 0;
    uint32_t _lastInput = 0;
    bool _touchDown = false;
    bool _bootDown = false;
    uint32_t _bootAt = 0;

    // ── Input ────────────────────────────────────────────────────────────────
    void _input(uint32_t now) {
        uint16_t x, y;
        bool down = _board.readTouch(x, y);
        if (down && !_touchDown) _tap(x, y, now);
        _touchDown = down;

        // BOOT: short press = next page, long press = back to Auto.
        bool boot = _board.bootKeyDown();
        if (boot && !_bootDown) _bootAt = now;
        if (!boot && _bootDown) {
            _lastInput = now;
            if (now - _bootAt >= kLongPressMs)
                _page = Page::Auto;
            else
                _page = (Page)(((uint8_t)_page + 1) % 3);
            _devicePage = 0;
        }
        _bootDown = boot;
    }

    void _tap(uint16_t x, uint16_t y, uint32_t now) {
        _lastInput = now;
        if (y >= kTabY) {
            uint8_t tab = x / (W / 4);
            if (tab == 3) {
                _brightness = (uint8_t)((_brightness + 1) % 3);
                _board.setBacklight(kBrightness[_brightness]);
            } else {
                _page = (Page)tab;
                _devicePage = 0;
            }
        } else if (_page == Page::Devices && y >= kContentY) {
            _devicePage++;  // wraps in _drawDevices
        }
    }

    // ── Text helpers ─────────────────────────────────────────────────────────
    void _font(const uint8_t* f, uint16_t color) {
        _text.setFont(f);
        _text.setFontMode(1);  // setFont() switches back to a solid background
        _text.setForegroundColor(color);
    }
    int16_t _width(const char* s) { return _text.getUTF8Width(s); }
    void _at(int16_t x, int16_t y, const char* s) { _text.drawUTF8(x, y, s); }
    void _right(int16_t xr, int16_t y, const char* s) { _at(xr - _width(s), y, s); }
    void _center(int16_t xc, int16_t y, const char* s) { _at(xc - _width(s) / 2, y, s); }

    static void _mmss(char* out, size_t len, uint32_t ms) {
        uint32_t s = ms / 1000;
        if (s >= 3600)
            snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                     (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
        else
            snprintf(out, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }

    static const char* _typeName(game::RoundType t, bool rainbow) {
        switch (t) {
            case game::RoundType::Race:
                return "Schnellrunde";
            case game::RoundType::Choice:
                return "Auswahl";
            case game::RoundType::Scale:
                return rainbow ? "Farbskala" : "Skala";
            case game::RoundType::Move:
                return "Bewegung";
            default:
                return "";
        }
    }

    // ── Frame ────────────────────────────────────────────────────────────────
    void _draw(const HubView& v) {
        _frame->fillScreen(kBg);
        _drawHeader(v);
        switch (_page) {
            case Page::Devices:
                _drawDevices(v);
                break;
            case Page::Radio:
                _drawRadio(v);
                break;
            default:
                _drawAuto(v);
                break;
        }
        _drawTabs();
    }

    void _drawHeader(const HubView& v) {
        char buf[48];
        _frame->fillRect(0, 0, W, kHeaderH, kPanel);
        _frame->drawFastHLine(0, kHeaderH - 1, W, kLine);
        // Row 1: session, console, channel.
        _font(u8g2_font_helvB12_tf, kText);
        if (v.session)
            snprintf(buf, sizeof(buf), "Sitzung %u", v.sessionId);
        else
            snprintf(buf, sizeof(buf), "Keine Sitzung");
        _at(kPad, 21, buf);
        _font(u8g2_font_helvR10_tf, v.console ? kText : kWarn);
        const char* con = v.console ? "Konsole" : "ohne Konsole";
        int16_t conX = W - kPad - _width(con);
        _frame->fillCircle(conX - 9, 16, 4, v.console ? kText : kWarn);
        _at(conX, 21, con);
        // Row 2: members, login, low battery, channel.
        if (!v.session) return;
        int16_t x = kPad;
        _font(u8g2_font_helvR10_tf, kText);
        snprintf(buf, sizeof(buf), "%u dabei", v.present);
        _at(x, 45, buf);
        x += _width(buf) + 12;
        uint16_t offline = v.members - v.present;
        if (offline) {
            _font(u8g2_font_helvR10_tf, kDim);
            snprintf(buf, sizeof(buf), "%u offline", offline);
            _at(x, 45, buf);
            x += _width(buf) + 12;
        }
        if (v.lowBattery) {
            _font(u8g2_font_helvR10_tf, kWarn);
            snprintf(buf, sizeof(buf), "%u Akku schwach", v.lowBattery);
            _at(x, 45, buf);
        }
        _font(u8g2_font_helvR10_tf, kDim);
        snprintf(buf, sizeof(buf), "Kanal %u", v.channel);
        _right(W - kPad, 45, buf);
    }

    void _drawTabs() {
        static const char* kNames[4] = {"Live", "Geräte", "Funk", "Licht"};
        _frame->fillRect(0, kTabY, W, kTabH, kPanel);
        _frame->drawFastHLine(0, kTabY, W, kLine);
        int16_t tw = W / 4;
        for (uint8_t i = 0; i < 4; i++) {
            bool active = i < 3 && (uint8_t)_page == i;
            if (active) {
                _frame->fillRect(i * tw, kTabY + 1, tw, kTabH - 1, kLine);
                _frame->fillRect(i * tw, kTabY + 1, tw, 4, kText);
            }
            if (i) _frame->drawFastVLine(i * tw, kTabY + 8, kTabH - 16, kLine);
            char buf[16];
            if (i == 3)
                snprintf(buf, sizeof(buf), "Licht %u%%", kBrightness[_brightness]);
            else
                snprintf(buf, sizeof(buf), "%s", kNames[i]);
            _font(active ? u8g2_font_helvB12_tf : u8g2_font_helvR12_tf, active ? kText : kDim);
            _center(i * tw + tw / 2, kTabY + 31, buf);
        }
    }

    // ── Auto: the page follows the session ───────────────────────────────────
    void _drawAuto(const HubView& v) {
        if (!v.session) return _drawNoSession(v);
        bool round = v.type != game::RoundType::Idle && v.type != game::RoundType::Move;
        if (round && (v.phase != game::Phase::Closed || v.opened)) return _drawRound(v);
        _drawOverview(v);
    }

    void _drawNoSession(const HubView& v) {
        _font(u8g2_font_helvB24_tf, kText);
        _center(W / 2, 200, "Bereit");
        _font(u8g2_font_helvR14_tf, kDim);
        _center(W / 2, 240, v.console ? "Konsole verbunden," : "Konsole am Laptop starten");
        _center(W / 2, 264, v.console ? "noch keine Sitzung" : "und den Hub anstecken");
    }

    void _drawOverview(const HubView& v) {
        char buf[32];
        int16_t y = kContentY + 16;
        if (v.login) {
            _frame->fillRoundRect(kPad, y, W - 2 * kPad, 34, 6, kText);
            _font(u8g2_font_helvB14_tf, kBg);
            _center(W / 2, y + 24, "Anmeldung offen");
            y += 46;
        } else {
            _font(u8g2_font_helvR12_tf, kDim);
            _at(kPad, y + 14, "Anmeldung geschlossen");
            y += 26;
        }
        _font(u8g2_font_logisoso42_tn, kText);
        snprintf(buf, sizeof(buf), "%u", v.present);
        _at(kPad, y + 46, buf);
        int16_t nx = kPad + _width(buf) + 10;
        _font(u8g2_font_helvR14_tf, kText);
        _at(nx, y + 22, "dabei");
        _font(u8g2_font_helvR12_tf, kDim);
        char dur[16];
        _mmss(dur, sizeof(dur), v.sessionMs);
        snprintf(buf, sizeof(buf), "seit %s", dur);
        _at(nx, y + 44, buf);
        y += 64;
        _drawGrid(v, y, kTabY - 30 - y, false);
        _font(u8g2_font_helvR10_tf, kDim);
        _at(kPad, kTabY - 10, "grau = offline,  gelber Rahmen = Akku schwach");
    }

    // Member numbers as tiles. Offline grey, low battery orange frame,
    // answered (in a round) green.
    void _drawGrid(const HubView& v, int16_t y, int16_t h, bool showAnswered) {
        const int16_t tw = 40, th = 26, gap = 2;
        const int16_t cols = (W - 2 * kPad + gap) / (tw + gap);
        const int16_t rows = (h + gap) / (th + gap);
        int16_t shown = 0;
        _font(u8g2_font_helvB10_tf, kText);
        for (uint16_t i = 0; i < v.listedCount && shown < cols * rows; i++) {
            const HubView::Member& m = v.listed[i];
            if (showAnswered && !m.present) continue;
            if (shown == cols * rows - 1 && v.listedCount - i > 1) {
                char more[12];
                snprintf(more, sizeof(more), "+%u", v.listedCount - i);
                _font(u8g2_font_helvB10_tf, kDim);
                _center(kPad + (shown % cols) * (tw + gap) + tw / 2,
                        y + (shown / cols) * (th + gap) + 18, more);
                return;
            }
            int16_t x0 = kPad + (shown % cols) * (tw + gap);
            int16_t y0 = y + (shown / cols) * (th + gap);
            uint16_t fill = !m.present ? kBg : (showAnswered && m.answered) ? kLine : kPanel;
            _frame->fillRoundRect(x0, y0, tw, th, 4, fill);
            bool low =
                m.present && m.battery != game::kNoBattery && m.battery < HubView::kLowBattery;
            _frame->drawRoundRect(x0, y0, tw, th, 4, low ? kWarn : kLine);
            char num[6];
            snprintf(num, sizeof(num), "%u", m.n);
            _font(u8g2_font_helvB10_tf, m.present ? kText : kDim);
            _center(x0 + tw / 2, y0 + 18, num);
            shown++;
        }
    }

    // ── Round: ring, "x von y", answer cross ─────────────────────────────────
    // Shared answer layout (gestaltung-buzzer/SYSTEM.md, decisions v7): the
    // same grid in every state. Ring left (time and status), "x von y
    // abgegeben" right, below it the answer cross in key positions with a
    // fifth field in the middle for the centre key's current function.
    static constexpr int16_t kRingX = 70, kRingY = 152, kRingOut = 58, kRingIn = 48;
    static constexpr int16_t kCrossX = 160, kCrossY = 323, kKey = 66, kGrid = 72;

    enum class RingState : uint8_t { Running, Closed, Resolved };

    // Segment i (0 = top, clockwise) of 8, 45 deg each with a 14 deg gap,
    // filled from 8 quads so the pieces stay apart without anti-aliasing.
    void _segment(uint8_t i, uint16_t color) {
        const float d2r = 3.14159265f / 180.0f;
        float a0 = (i * 45.0f + 7.0f - 90.0f) * d2r, a1 = (i * 45.0f + 38.0f - 90.0f) * d2r;
        const uint8_t steps = 8;
        for (uint8_t s = 0; s < steps; s++) {
            float b0 = a0 + (a1 - a0) * s / steps, b1 = a0 + (a1 - a0) * (s + 1) / steps;
            int16_t ox0 = kRingX + lroundf(kRingOut * cosf(b0)),
                    oy0 = kRingY + lroundf(kRingOut * sinf(b0));
            int16_t ox1 = kRingX + lroundf(kRingOut * cosf(b1)),
                    oy1 = kRingY + lroundf(kRingOut * sinf(b1));
            int16_t ix0 = kRingX + lroundf(kRingIn * cosf(b0)),
                    iy0 = kRingY + lroundf(kRingIn * sinf(b0));
            int16_t ix1 = kRingX + lroundf(kRingIn * cosf(b1)),
                    iy1 = kRingY + lroundf(kRingIn * sinf(b1));
            _frame->fillTriangle(ox0, oy0, ox1, oy1, ix0, iy0, color);
            _frame->fillTriangle(ix0, iy0, ox1, oy1, ix1, iy1, color);
        }
    }

    void _lock(int16_t cx, int16_t cy, uint16_t color) {
        for (int8_t r = 7; r >= 5; r--) _frame->drawCircle(cx, cy - 6, r, color);
        _frame->fillRect(cx - 4, cy - 6, 9, 6, kBg);
        _frame->fillRect(cx - 7, cy - 6, 3, 6, color);
        _frame->fillRect(cx + 5, cy - 6, 3, 6, color);
        _frame->fillRoundRect(cx - 10, cy, 21, 15, 3, color);
    }

    // Check mark on a round plaque, diameter 20.
    void _check(int16_t cx, int16_t cy, uint16_t plaque, uint16_t mark) {
        _frame->fillCircle(cx, cy, 10, plaque);
        for (int8_t o = -1; o <= 1; o++) {
            _frame->drawLine(cx - 5, cy + o, cx - 1, cy + 4 + o, mark);
            _frame->drawLine(cx - 1, cy + 4 + o, cx + 6, cy - 4 + o, mark);
        }
    }

    void _drawRing(const HubView& v, RingState st, uint16_t right) {
        char buf[16];
        if (st == RingState::Running) {
            uint8_t lit = 8;
            if (v.countdownMs) {
                lit = (uint8_t)((8UL * v.remainingMs + v.countdownMs - 1) / v.countdownMs);
                if (lit > 8) lit = 8;
            }
            for (uint8_t i = 0; i < 8; i++) _segment(i, i < lit ? kText : kRingOff);
            if (v.countdownMs) {
                _mmss(buf, sizeof(buf), v.remainingMs + 999);
                _font(u8g2_font_helvB18_tf, kText);
                _center(kRingX, kRingY + 1, buf);
                _font(u8g2_font_helvR12_tf, kText);
                _center(kRingX, kRingY + 23, "noch");
            } else {
                _font(u8g2_font_helvB18_tf, kText);
                _center(kRingX, kRingY + 8, "-");
            }
            return;
        }
        if (st == RingState::Closed) {
            for (uint8_t i = 0; i < 8; i++) _segment(i, kGrey);
            _lock(kRingX, kRingY - 8, kGrey);
            _font(u8g2_font_helvR12_tf, kGrey);
            _center(kRingX, kRingY + 26, "zu");
            return;
        }
        uint8_t n = 0;
        if (v.present) {
            n = (uint8_t)((8UL * right + v.present / 2) / v.present);
            if (right > 0 && n == 0) n = 1;
            if (right < v.present && n == 8) n = 7;
        }
        for (uint8_t i = 0; i < 8; i++) _segment(i, i < n ? kGreen : kGrey);
        snprintf(buf, sizeof(buf), "%u", right);
        _font(u8g2_font_helvB18_tf, kText);
        _center(kRingX, kRingY + 1, buf);
        _font(u8g2_font_helvB12_tf, kGreen);
        _center(kRingX, kRingY + 23, "richtig");
    }

    // One answer key, centre cx/cy. dir: 0 up, 1 right, 2 down, 3 left.
    enum class KeyLook : uint8_t { Open, Counted, Right, Other };
    void _key(int16_t cx, int16_t cy, uint8_t dir, KeyLook look, int32_t number) {
        int16_t x0 = cx - kKey / 2, y0 = cy - kKey / 2;
        uint16_t fill = look == KeyLook::Right ? kGreen : look == KeyLook::Other ? kGrey : kText;
        _frame->fillRoundRect(x0, y0, kKey, kKey, 6, fill);
        // Equilateral triangle, side 16, tip 5 px from the outer edge.
        const int16_t s = 16, h = 14, tip = 5;
        int16_t tx, ty, bx0, by0, bx1, by1;
        switch (dir) {
            case 0:
                tx = cx, ty = y0 + tip, bx0 = cx - s / 2, by0 = ty + h, bx1 = cx + s / 2,
                by1 = ty + h;
                break;
            case 1:
                tx = x0 + kKey - 1 - tip, ty = cy, bx0 = tx - h, by0 = cy - s / 2, bx1 = tx - h,
                by1 = cy + s / 2;
                break;
            case 2:
                tx = cx, ty = y0 + kKey - 1 - tip, bx0 = cx - s / 2, by0 = ty - h, bx1 = cx + s / 2,
                by1 = ty - h;
                break;
            default:
                tx = x0 + tip, ty = cy, bx0 = tx + h, by0 = cy - s / 2, bx1 = tx + h,
                by1 = cy + s / 2;
                break;
        }
        _frame->fillTriangle(tx, ty, bx0, by0, bx1, by1, kBg);
        if (number < 0) return;
        // Content area: the outer edge keeps 18 px for the triangle.
        int16_t ccx = cx + (dir == 1 ? -9 : dir == 3 ? 9 : 0);
        int16_t ccy = cy + (dir == 0 ? 9 : dir == 2 ? -9 : 0);
        char buf[12];
        snprintf(buf, sizeof(buf), "%ld", (long)number);
        _font(number >= 10 && (dir == 1 || dir == 3) && look == KeyLook::Right
                  ? u8g2_font_helvB12_tf
                  : u8g2_font_helvB18_tf,
              kBg);
        int16_t tw = _width(buf);
        if (look == KeyLook::Right) {
            int16_t gw = 20 + 3 + tw, gx = ccx - gw / 2;
            _check(gx + 10, ccy, kBg, kGreen);
            _at(gx + 23, ccy + 9, buf);
        } else {
            _at(ccx - tw / 2, ccy + 9, buf);
        }
    }

    // The middle field: no answer, a small word for the centre key's function.
    void _middle(const char* word, int32_t number) {
        int16_t x0 = kCrossX - kKey / 2, y0 = kCrossY - kKey / 2;
        _frame->drawRoundRect(x0, y0, kKey, kKey, 6, kRingOff);
        _frame->drawRoundRect(x0 + 1, y0 + 1, kKey - 2, kKey - 2, 5, kRingOff);
        if (number >= 0) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%ld", (long)number);
            _font(u8g2_font_helvB12_tf, kDim);
            _center(kCrossX, kCrossY + 2, buf);
        }
        if (word && *word) {
            _font(u8g2_font_helvR10_tf, kDim);
            _center(kCrossX, kCrossY + (number >= 0 ? 20 : 5), word);
        }
    }

    void _drawCross(const HubView& v, bool closed, bool resolved) {
        static const int8_t kDx[4] = {0, 1, 0, -1}, kDy[4] = {-1, 0, 1, 0};
        static const game::Key kKeys[4] = {game::Key::Up, game::Key::Right, game::Key::Down,
                                           game::Key::Left};
        bool anyDir = false;
        for (uint8_t d = 0; d < 4; d++)
            if (v.keyMask & (1u << (uint8_t)kKeys[d])) anyDir = true;
        for (uint8_t d = 0; d < 4; d++) {
            if (anyDir && !(v.keyMask & (1u << (uint8_t)kKeys[d]))) continue;
            uint8_t value = (uint8_t)(d + 1);  // choice values: 1 up, 2 right, 3 down, 4 left
            KeyLook look = !closed              ? KeyLook::Open
                           : !resolved          ? KeyLook::Counted
                           : v.correct == value ? KeyLook::Right
                                                : KeyLook::Other;
            _key(kCrossX + kDx[d] * kGrid, kCrossY + kDy[d] * kGrid, d, look,
                 closed ? (int32_t)v.counts[value] : -1);
        }
        bool enterAnswer = (v.keyMask & (1u << (uint8_t)game::Key::Enter)) != 0;
        _middle(closed ? "" : "lösen", closed && enterAnswer ? (int32_t)v.counts[5] : -1);
    }

    // Category line in place of the old status pill: round type and number.
    void _drawCategory(const HubView& v) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s · Runde %u", _typeName(v.type, v.rainbow), v.round);
        _font(u8g2_font_helvR12_tf, kDim);
        _at(kPad, kContentY + 22, buf);
    }

    void _drawRound(const HubView& v) {
        char buf[40];
        const bool closed = v.phase == game::Phase::Closed;
        const bool choice = v.type == game::RoundType::Choice;
        const bool resolved = closed && choice && v.correct >= 1 && v.correct <= 5;
        _drawCategory(v);
        if (v.phase == game::Phase::Announced) {
            _font(u8g2_font_helvB18_tf, kText);
            _center(W / 2, kContentY + 150, "Gleich geht es los");
            return;
        }
        uint16_t right = resolved ? v.counts[v.correct] : 0;
        _drawRing(v,
                  !closed    ? RingState::Running
                  : resolved ? RingState::Resolved
                             : RingState::Closed,
                  right);
        // Right of the ring, the same in every state.
        snprintf(buf, sizeof(buf), "%u von %u", v.answered, v.present);
        _font(u8g2_font_helvB18_tf, kText);
        _at(140, 134, buf);
        _font(u8g2_font_helvR12_tf, kText);
        _at(140, 158, v.type == game::RoundType::Race ? "gedrückt" : "abgegeben");
        if (resolved) {
            uint16_t wrong = v.valueCount > right ? (uint16_t)(v.valueCount - right) : 0;
            uint16_t none = v.present > v.answered ? (uint16_t)(v.present - v.answered) : 0;
            snprintf(buf, sizeof(buf), "%u", wrong);
            _font(u8g2_font_helvB12_tf, kNeben);
            _right(168, 184, buf);
            _font(u8g2_font_helvR10_tf, kText);
            _at(174, 184, "daneben");
            if (none) {
                snprintf(buf, sizeof(buf), "%u", none);
                _font(u8g2_font_helvB12_tf, kNone);
                _right(168, 206, buf);
                _font(u8g2_font_helvR10_tf, kText);
                _at(174, 206, "nicht abgegeben");
            }
        }
        // Below: the cross (choice), or what the round type needs.
        const int16_t below = 222;
        switch (v.type) {
            case game::RoundType::Choice:
                _drawCross(v, closed, resolved);
                break;
            case game::RoundType::Scale:
                if (closed)
                    _answersScale(v, kPad, below, W - 2 * kPad, kTabY - 8 - below);
                else
                    _drawMissing(v, below);
                break;
            case game::RoundType::Race:
                if (closed)
                    _answersRace(v, kPad, below, W - 2 * kPad);
                else if (v.answering) {
                    _frame->fillRoundRect(kPad, below, W - 2 * kPad, 40, 6, kPanel);
                    _font(u8g2_font_helvB14_tf, kText);
                    snprintf(buf, sizeof(buf), "Nr. %u antwortet", v.answering);
                    _center(W / 2, below + 27, buf);
                    _drawMissing(v, below + 52);
                } else {
                    _drawMissing(v, below);
                }
                break;
            default:
                break;
        }
    }

    // Device numbers still without a value (open scale or race).
    void _drawMissing(const HubView& v, int16_t y) {
        if (v.present <= v.answered) return;
        _font(u8g2_font_helvR10_tf, kDim);
        _at(kPad, y + 12, "fehlt noch:");
        y += 18;
        int16_t x = kPad;
        _font(u8g2_font_helvB10_tf, kText);
        for (uint16_t i = 0; i < v.listedCount; i++) {
            const HubView::Member& m = v.listed[i];
            if (!m.present || m.answered) continue;
            char num[6];
            snprintf(num, sizeof(num), "%u", m.n);
            int16_t w = _width(num) + 12;
            if (x + w > W - kPad) {
                x = kPad;
                y += 22;
            }
            if (y + 20 > kTabY - 4) {
                _font(u8g2_font_helvR10_tf, kDim);
                _at(x, y + 15, "...");
                return;
            }
            _frame->drawRoundRect(x, y, w - 4, 20, 4, kLine);
            _at(x + 4, y + 15, num);
            x += w;
        }
    }

    // Scale result: average and distribution in zone or rainbow colours.
    void _answersScale(const HubView& v, int16_t x, int16_t y, int16_t w, int16_t h) {
        char buf[24];
        uint8_t lo = v.min ? v.min : 1, hi = v.max > lo ? v.max : lo;
        if (hi > HubView::kMaxValue) hi = HubView::kMaxValue;
        uint8_t steps = (uint8_t)(hi - lo + 1);
        _font(u8g2_font_helvR12_tf, kDim);
        _at(x, y + 14, "Durchschnitt");
        if (v.valueCount) {
            uint32_t tenth = (v.valueSum * 10 + v.valueCount / 2) / v.valueCount;
            snprintf(buf, sizeof(buf), "%lu,%lu", (unsigned long)(tenth / 10),
                     (unsigned long)(tenth % 10));
        } else {
            snprintf(buf, sizeof(buf), "-");
        }
        _font(u8g2_font_helvB18_tf, kText);
        _at(x + 110, y + 16, buf);
        int16_t gy = y + 40, gh = h - 40 - 20;
        uint16_t most = 1;
        for (uint8_t s = lo; s <= hi; s++)
            if (v.counts[s] > most) most = v.counts[s];
        int16_t bw = w / steps;
        for (uint8_t s = lo; s <= hi; s++) {
            uint16_t col = kGrey;
            if (v.rainbow && s >= 1 && s <= game::kRainbowSteps) col = rgb(game::kRainbow[s - 1]);
            int16_t bh = (int16_t)((int32_t)(gh - 16) * v.counts[s] / most);
            int16_t bx = x + (s - lo) * bw;
            _frame->fillRect(bx + 1, gy + gh - bh, bw - 2, bh, col);
            _frame->drawFastHLine(bx, gy + gh, bw, kLine);
            if (steps <= 12) {
                snprintf(buf, sizeof(buf), "%u", v.counts[s]);
                _font(u8g2_font_helvB10_tf, kText);
                _center(bx + bw / 2, gy + gh - bh - 4, buf);
            }
            if (steps <= 12 || s == lo || s == hi) {
                snprintf(buf, sizeof(buf), "%u", s);
                _font(u8g2_font_helvR10_tf, kDim);
                _center(bx + bw / 2, gy + gh + 15, buf);
            }
        }
    }

    // Race result: the fastest number; right in green, wrong in the
    // "daneben" orange.
    void _answersRace(const HubView& v, int16_t x, int16_t y, int16_t w) {
        char buf[24];
        if (!v.winner) {
            _font(u8g2_font_helvR14_tf, kDim);
            _center(x + w / 2, y + 60, "Kein Sieger");
            return;
        }
        uint16_t col = v.winnerWrong ? kNeben : kGreen;
        _frame->fillRoundRect(x, y, w, 140, 10, kPanel);
        _frame->drawRoundRect(x, y, w, 140, 10, col);
        _frame->drawRoundRect(x + 1, y + 1, w - 2, 138, 9, col);
        _font(u8g2_font_helvR14_tf, kText);
        _center(x + w / 2, y + 30, "Schnellste Nummer");
        _font(u8g2_font_logisoso42_tn, kText);
        snprintf(buf, sizeof(buf), "%u", v.winner);
        _center(x + w / 2, y + 92, buf);
        _font(u8g2_font_helvB14_tf, col);
        _center(x + w / 2, y + 124, v.winnerWrong ? "daneben" : "richtig");
    }

    // ── Devices ──────────────────────────────────────────────────────────────
    void _drawDevices(const HubView& v) {
        char buf[32];
        const int16_t rowH = 24, perCol = (kContentH - 52) / rowH, perPage = perCol * 2;
        uint16_t pages = v.listedCount ? (uint16_t)((v.listedCount + perPage - 1) / perPage) : 1;
        if (_devicePage >= pages) _devicePage = 0;
        _font(u8g2_font_helvB14_tf, kText);
        _at(kPad, kContentY + 28, "Geräte");
        _font(u8g2_font_helvR10_tf, kDim);
        snprintf(buf, sizeof(buf), "Seite %u/%u (antippen)", _devicePage + 1, pages);
        _right(W - kPad, kContentY + 28, buf);
        _at(kPad, kContentY + 44, "Nr.   Akku (+ lädt)   Signal");
        if (!v.listedCount) {
            _font(u8g2_font_helvR14_tf, kDim);
            _center(W / 2, kContentY + 140, v.session ? "Noch keine Geräte" : "Keine Sitzung");
            return;
        }
        const int16_t colW = (W - 2 * kPad) / 2;
        for (int16_t i = 0; i < perPage; i++) {
            uint16_t idx = (uint16_t)(_devicePage * perPage + i);
            if (idx >= v.listedCount) break;
            const HubView::Member& m = v.listed[idx];
            int16_t cx = kPad + (i / perCol) * colW;
            int16_t cy = kContentY + 50 + (i % perCol) * rowH;
            _font(u8g2_font_helvB10_tf, m.present ? kText : kDim);
            snprintf(buf, sizeof(buf), "%u", m.n);
            _at(cx, cy + 16, buf);
            bool low = m.battery != game::kNoBattery && m.battery < HubView::kLowBattery;
            _font(u8g2_font_helvR10_tf, !m.present ? kDim : low ? kWarn : kText);
            if (m.battery == game::kNoBattery)
                snprintf(buf, sizeof(buf), "-");
            else
                snprintf(buf, sizeof(buf), "%u%%%s", m.battery, m.charging ? "+" : "");
            _at(cx + 40, cy + 16, buf);
            _font(u8g2_font_helvR10_tf, m.present && m.rssi < HubView::kWeakRssi ? kWarn : kDim);
            if (m.present)
                snprintf(buf, sizeof(buf), "%d dBm", m.rssi);
            else
                snprintf(buf, sizeof(buf), "offline");
            _right(cx + colW - 10, cy + 16, buf);
        }
    }

    // ── Radio ────────────────────────────────────────────────────────────────
    void _drawRadio(const HubView& v) {
        char buf[32];
        _font(u8g2_font_helvB14_tf, kText);
        _at(kPad, kContentY + 28, "Funk");
        struct Row {
            const char* label;
            char value[24];
            uint16_t color;
        } rows[6];
        uint8_t n = 0;
        auto add = [&](const char* label, uint16_t color, const char* fmt, auto... args) {
            rows[n].label = label;
            rows[n].color = color;
            snprintf(rows[n].value, sizeof(rows[n].value), fmt, args...);
            n++;
        };
        if (!v.session) {
            _font(u8g2_font_helvR14_tf, kDim);
            _center(W / 2, kContentY + 140, "Keine Sitzung");
            return;
        }
        add("Kanal", v.offChannel ? kWarn : kText, v.offChannel ? "%u (Wechsel)" : "%u", v.channel);
        add("Empfangen", kText, "%lu Frames", (unsigned long)v.rxFrames);
        add("Gesendet", kText, "%lu Frames", (unsigned long)v.txFrames);
        if (v.present)
            add("Schwächstes Signal", v.weakestRssi < HubView::kWeakRssi ? kWarn : kText, "%d dBm",
                v.weakestRssi);
        else
            add("Schwächstes Signal", kDim, "%s", "-");
        add("Schwaches Signal", v.weakCount ? kWarn : kText, "%u Geräte", v.weakCount);
        add("Offline", v.members > v.present ? kWarn : kText, "%u Geräte",
            (unsigned)(v.members - v.present));
        for (uint8_t i = 0; i < n; i++) {
            int16_t ry = kContentY + 50 + i * 44;
            _frame->fillRoundRect(kPad, ry, W - 2 * kPad, 38, 6, kPanel);
            _font(u8g2_font_helvR12_tf, kDim);
            _at(kPad + 10, ry + 25, rows[i].label);
            _font(u8g2_font_helvB12_tf, rows[i].color);
            _right(W - kPad - 10, ry + 25, rows[i].value);
        }
        (void)buf;
    }

    // ── Output ───────────────────────────────────────────────────────────────
    // Sends the band of rows that changed since the last frame.
    void _flush() {
        const uint16_t* cur = _frame->getBuffer();
        int32_t first = -1, last = -1;
        for (uint16_t y = 0; y < HubBoard::kHeight; y++) {
            const size_t off = (size_t)y * W;
            if (memcmp(cur + off, _last + off, W * 2) != 0) {
                if (first < 0) first = y;
                last = y;
            }
        }
        if (first < 0) return;
        _board.pushRows(cur, (uint16_t)first, (uint16_t)last);
        memcpy(_last + (size_t)first * W, cur + (size_t)first * W,
               (size_t)(last - first + 1) * W * 2);
    }
};

}  // namespace display
