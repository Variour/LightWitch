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
    static constexpr uint16_t kBg = rgb565(10, 10, 14);
    static constexpr uint16_t kPanel = rgb565(32, 34, 42);
    static constexpr uint16_t kLine = rgb565(60, 62, 72);
    static constexpr uint16_t kText = rgb565(240, 240, 240);
    static constexpr uint16_t kDim = rgb565(150, 152, 160);
    static constexpr uint16_t kGreen = rgb565(0x2F, 0xFB, 0x95);
    static constexpr uint16_t kWrong = rgb565(200, 62, 0);  // 128/40/0 lifted for the screen
    static constexpr uint16_t kOrange = rgb565(255, 140, 0);
    static constexpr uint16_t kBlue = rgb565(60, 120, 255);

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
        _font(u8g2_font_helvR10_tf, v.console ? kGreen : kOrange);
        const char* con = v.console ? "Konsole" : "ohne Konsole";
        int16_t conX = W - kPad - _width(con);
        _frame->fillCircle(conX - 9, 16, 4, v.console ? kGreen : kOrange);
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
            _font(u8g2_font_helvR10_tf, kOrange);
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
                _frame->fillRect(i * tw, kTabY + 1, tw, 4, kBlue);
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
        if (v.type != game::RoundType::Idle && v.phase != game::Phase::Closed) return _drawOpen(v);
        if (v.type != game::RoundType::Idle && v.phase == game::Phase::Closed && v.opened)
            return renderAnswers(v, kPad, kContentY, W - 2 * kPad, kContentH);
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
            _frame->fillRoundRect(kPad, y, W - 2 * kPad, 34, 6, kBlue);
            _font(u8g2_font_helvB14_tf, kText);
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
        _at(kPad, kTabY - 10, "grau = offline,  orange Rahmen = Akku schwach");
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
            uint16_t fill = !m.present                     ? kBg
                            : (showAnswered && m.answered) ? rgb(0, 110, 60)
                                                           : kPanel;
            _frame->fillRoundRect(x0, y0, tw, th, 4, fill);
            bool low =
                m.present && m.battery != game::kNoBattery && m.battery < HubView::kLowBattery;
            _frame->drawRoundRect(x0, y0, tw, th, 4, low ? kOrange : kLine);
            char num[6];
            snprintf(num, sizeof(num), "%u", m.n);
            _font(u8g2_font_helvB10_tf, m.present ? kText : kDim);
            _center(x0 + tw / 2, y0 + 18, num);
            shown++;
        }
    }

    // ── Round announced or open ──────────────────────────────────────────────
    void _drawOpen(const HubView& v) {
        char buf[40];
        int16_t y = kContentY + 30;
        _font(u8g2_font_helvB14_tf, kText);
        snprintf(buf, sizeof(buf), "%s", _typeName(v.type, v.rainbow));
        _at(kPad, y, buf);
        _font(u8g2_font_helvR12_tf, kDim);
        snprintf(buf, sizeof(buf), "Runde %u", v.round);
        _right(W - kPad, y, buf);

        if (v.phase == game::Phase::Announced) {
            _font(u8g2_font_helvB18_tf, kText);
            _center(W / 2, y + 120, "Gleich geht es los");
            return;
        }
        // x of y, large.
        y += 24;
        _font(u8g2_font_logisoso58_tn, kText);
        snprintf(buf, sizeof(buf), "%u", v.answered);
        int16_t wA = _width(buf);
        char tot[8];
        snprintf(tot, sizeof(tot), "%u", v.present);
        _font(u8g2_font_logisoso32_tn, kDim);
        int16_t wT = _width(tot);
        _font(u8g2_font_helvR14_tf, kDim);
        int16_t wV = _width(" von ");
        int16_t x = (W - (wA + wV + wT)) / 2;
        _font(u8g2_font_logisoso58_tn, v.present && v.answered >= v.present ? kGreen : kText);
        _at(x, y + 62, buf);
        _font(u8g2_font_helvR14_tf, kDim);
        _at(x + wA, y + 62, " von ");
        _font(u8g2_font_logisoso32_tn, kDim);
        _at(x + wA + wV, y + 62, tot);
        _font(u8g2_font_helvR12_tf, kDim);
        _center(W / 2, y + 88,
                v.type == game::RoundType::Race ? "haben gedrückt" : "haben geantwortet");

        // Time since open.
        y += 130;
        char t[16];
        _mmss(t, sizeof(t), v.sinceOpenMs);
        _font(u8g2_font_helvR12_tf, kDim);
        _at(kPad, y, "läuft seit");
        _font(u8g2_font_helvB18_tf, kText);
        _center(W / 2, y, t);

        // Race: the device with the turn.
        y += 20;
        if (v.type == game::RoundType::Race && v.answering) {
            _frame->fillRoundRect(kPad, y, W - 2 * kPad, 40, 6, kPanel);
            _font(u8g2_font_helvB14_tf, kText);
            snprintf(buf, sizeof(buf), "Nr. %u antwortet", v.answering);
            _center(W / 2, y + 27, buf);
            y += 50;
        }
        // Missing devices.
        if (v.present > v.answered) {
            _font(u8g2_font_helvR10_tf, kDim);
            _at(kPad, y + 14, "fehlt noch:");
            _drawMissing(v, y + 20);
        }
    }

    void _drawMissing(const HubView& v, int16_t y) {
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
                _at(x, y, "...");
                return;
            }
            _frame->drawRoundRect(x, y, w - 4, 20, 4, kLine);
            _at(x + 4, y + 15, num);
            x += w;
        }
    }

    // ── Result (closed round) ────────────────────────────────────────────────
    // Swappable: the shared answer layout for slides, devices and hub (thread
    // "Einheitliches Antwort-Layout") replaces this function. Draws into the
    // rectangle x, y, w, h only.
    void renderAnswers(const HubView& v, int16_t x, int16_t y, int16_t w, int16_t h) {
        char buf[40];
        _font(u8g2_font_helvB14_tf, kText);
        _at(x, y + 30, _typeName(v.type, v.rainbow));
        _font(u8g2_font_helvR12_tf, kDim);
        snprintf(buf, sizeof(buf), "Runde %u beendet", v.round);
        _right(x + w, y + 30, buf);
        int16_t top = y + 50;
        switch (v.type) {
            case game::RoundType::Choice:
                _answersChoice(v, x, top, w, y + h - top);
                break;
            case game::RoundType::Scale:
                _answersScale(v, x, top, w, y + h - top);
                break;
            case game::RoundType::Race:
                _answersRace(v, x, top, w);
                break;
            default:
                _font(u8g2_font_helvR14_tf, kText);
                snprintf(buf, sizeof(buf), "%u von %u dabei", v.present, v.members);
                _center(x + w / 2, top + 80, buf);
                break;
        }
    }

    void _answersChoice(const HubView& v, int16_t x, int16_t y, int16_t w, int16_t h) {
        static const char* kKey[5] = {"oben", "rechts", "unten", "links", "Mitte"};
        char buf[24];
        uint16_t total = 0, most = 1;
        for (uint8_t k = 1; k <= 5; k++) {
            total += v.counts[k];
            if (v.counts[k] > most) most = v.counts[k];
        }
        const int16_t rowH = 50, labelW = 64, numW = 74;
        for (uint8_t k = 1; k <= 5; k++) {
            int16_t ry = y + (k - 1) * (rowH + 4);
            bool right = v.correct == k;
            uint16_t col =
                v.keyColor[k - 1] ? rgb(game::colorRgb((game::Color)v.keyColor[k - 1])) : kDim;
            // Right answer: the row itself is marked (light green field).
            _frame->fillRoundRect(x, ry, w, rowH, 6, right ? rgb(0, 90, 50) : kPanel);
            if (right) _frame->drawRoundRect(x, ry, w, rowH, 6, kGreen);
            _frame->fillCircle(x + 14, ry + rowH / 2, 6, col);
            _font(u8g2_font_helvB12_tf, kText);
            _at(x + 26, ry + (right ? 24 : 31), kKey[k - 1]);
            if (right) {
                _font(u8g2_font_helvB10_tf, kGreen);
                _at(x + 26, ry + 40, "richtig");
            }
            int16_t bx = x + labelW + 24, bw = w - labelW - 24 - numW;
            int16_t len = (int16_t)((int32_t)bw * v.counts[k] / most);
            _frame->fillRect(bx, ry + 14, len, rowH - 28, col);
            _font(u8g2_font_helvB14_tf, kText);
            snprintf(buf, sizeof(buf), "%u", v.counts[k]);
            _right(x + w - 40, ry + 32, buf);
            _font(u8g2_font_helvR10_tf, kDim);
            snprintf(buf, sizeof(buf), "%u%%", total ? (unsigned)(v.counts[k] * 100 / total) : 0u);
            _right(x + w - 6, ry + 32, buf);
        }
        int16_t fy = y + 5 * (rowH + 4) + 24;
        if (v.correct >= 1 && v.correct <= 5 && total) {
            _font(u8g2_font_helvB14_tf, kGreen);
            snprintf(buf, sizeof(buf), "%u%% richtig",
                     (unsigned)(v.counts[v.correct] * 100 / total));
            _at(x, fy, buf);
        }
        _font(u8g2_font_helvR12_tf, kDim);
        snprintf(buf, sizeof(buf), "%u von %u", total, v.present);
        _right(x + w, fy, buf);
        (void)h;
    }

    void _answersScale(const HubView& v, int16_t x, int16_t y, int16_t w, int16_t h) {
        char buf[24];
        uint8_t lo = v.min ? v.min : 1, hi = v.max > lo ? v.max : lo;
        if (hi > HubView::kMaxValue) hi = HubView::kMaxValue;
        uint8_t steps = (uint8_t)(hi - lo + 1);
        // Average, large.
        _font(u8g2_font_helvR12_tf, kDim);
        _at(x, y + 18, "Durchschnitt");
        if (v.valueCount) {
            uint32_t tenth = (v.valueSum * 10 + v.valueCount / 2) / v.valueCount;
            snprintf(buf, sizeof(buf), "%lu,%lu", (unsigned long)(tenth / 10),
                     (unsigned long)(tenth % 10));
        } else {
            snprintf(buf, sizeof(buf), "-");
        }
        _font(u8g2_font_logisoso42_tf, kText);
        _at(x, y + 70, buf);
        _font(u8g2_font_helvR12_tf, kDim);
        snprintf(buf, sizeof(buf), "%u von %u", v.valueCount, v.present);
        _right(x + w, y + 70, buf);
        // Distribution.
        int16_t gy = y + 110, gh = h - 110 - 24;
        uint16_t most = 1;
        for (uint8_t s = lo; s <= hi; s++)
            if (v.counts[s] > most) most = v.counts[s];
        int16_t bw = w / steps;
        for (uint8_t s = lo; s <= hi; s++) {
            uint16_t col = kDim;
            if (v.rainbow && s >= 1 && s <= game::kRainbowSteps)
                col = rgb(game::kRainbow[s - 1]);
            else if (v.redMax && s <= v.redMax)
                col = kWrong;
            else if (v.greenMin && s >= v.greenMin)
                col = kGreen;
            else if (v.redMax || v.greenMin)
                col = rgb(255, 170, 0);
            int16_t bh = (int16_t)((int32_t)gh * v.counts[s] / most);
            int16_t bx = x + (s - lo) * bw;
            _frame->fillRect(bx + 1, gy + gh - bh, bw - 2, bh, col);
            if (steps <= 12) {
                snprintf(buf, sizeof(buf), "%u", v.counts[s]);
                _font(u8g2_font_helvB10_tf, kText);
                _center(bx + bw / 2, gy + gh - bh - 4, buf);
            }
            _frame->drawFastHLine(bx, gy + gh, bw, kLine);
            if (steps <= 12 || s == lo || s == hi) {
                snprintf(buf, sizeof(buf), "%u", s);
                _font(u8g2_font_helvR10_tf, kDim);
                _center(bx + bw / 2, gy + gh + 16, buf);
            }
        }
    }

    void _answersRace(const HubView& v, int16_t x, int16_t y, int16_t w) {
        char buf[24];
        if (!v.winner) {
            _font(u8g2_font_helvR14_tf, kDim);
            _center(x + w / 2, y + 100, "Kein Sieger");
            return;
        }
        uint16_t col = v.winnerWrong ? kWrong : kGreen;
        _frame->fillRoundRect(x, y + 20, w, 150, 10,
                              v.winnerWrong ? rgb(70, 22, 0) : rgb(0, 90, 50));
        _frame->drawRoundRect(x, y + 20, w, 150, 10, col);
        _font(u8g2_font_helvR14_tf, kText);
        _center(x + w / 2, y + 52, "Schnellste Nummer");
        _font(u8g2_font_logisoso58_tn, kText);
        snprintf(buf, sizeof(buf), "%u", v.winner);
        _center(x + w / 2, y + 130, buf);
        _font(u8g2_font_helvB14_tf, col);
        _center(x + w / 2, y + 160, v.winnerWrong ? "Antwort daneben" : "Antwort richtig");
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
            _font(u8g2_font_helvR10_tf, !m.present ? kDim : low ? kOrange : kText);
            if (m.battery == game::kNoBattery)
                snprintf(buf, sizeof(buf), "-");
            else
                snprintf(buf, sizeof(buf), "%u%%%s", m.battery, m.charging ? "+" : "");
            _at(cx + 40, cy + 16, buf);
            _font(u8g2_font_helvR10_tf, m.present && m.rssi < HubView::kWeakRssi ? kOrange : kDim);
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
        add("Kanal", v.offChannel ? kOrange : kText, v.offChannel ? "%u (Wechsel)" : "%u",
            v.channel);
        add("Empfangen", kText, "%lu Frames", (unsigned long)v.rxFrames);
        add("Gesendet", kText, "%lu Frames", (unsigned long)v.txFrames);
        if (v.present)
            add("Schwächstes Signal", v.weakestRssi < HubView::kWeakRssi ? kOrange : kText,
                "%d dBm", v.weakestRssi);
        else
            add("Schwächstes Signal", kDim, "%s", "-");
        add("Schwaches Signal", v.weakCount ? kOrange : kText, "%u Geräte", v.weakCount);
        add("Offline", v.members > v.present ? kOrange : kText, "%u Geräte",
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
