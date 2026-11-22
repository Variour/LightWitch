#include "Lamp.h"

#include <LittleFS.h>
#include <math.h>

#include "../logging/Logger.h"

namespace lamp {

bool Lamp::_loaded = false;
char Lamp::_name[32] = {};
Output Lamp::_outputs[MAX_LIGHTS];
uint8_t Lamp::_outputCount = 0;
float Lamp::_dens[F_COUNT][K_COUNT] = {};
float Lamp::_famArea[F_COUNT] = {};
uint8_t Lamp::_masks[8] = {};
uint8_t Lamp::_maskCount = 0;
float Lamp::_zMin = 0;
float Lamp::_zMax = 0;
float Lamp::_rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
bool Lamp::_tilted = false;

float levelToDensity(uint8_t b) {
    if (b == 0) return 0.0f;
    // SCALE_MIN * (SCALE_MAX / SCALE_MIN)^((b - 1) / 254) as repeated
    // multiplication by one step: avoids pulling powf into the image.
    static constexpr float STEP = 1.0331927f;  // 4000^(1/254)
    float m = SCALE_MIN;
    for (uint8_t i = 1; i < b; i++) m *= STEP;
    return m;
}

void familyWeights(uint8_t family, float r, float g, float b, float* w) {
    for (uint8_t k = 0; k < K_COUNT; k++) w[k] = 0.0f;
    if (family == F_RGB) {
        w[K_R] = r;
        w[K_G] = g;
        w[K_B] = b;
    } else if (family == F_WHITE) {
        w[K_A] = r > g ? r - g : 0.0f;
        w[K_WW] = g > b ? g - b : 0.0f;
        w[K_CW] = b;
    } else {
        w[K_A] = r;
    }
}

static uint8_t _kindFromString(const char* s, bool& ok) {
    ok = true;
    if (!s || !*s) return K_NONE;
    if (!strcmp(s, "r")) return K_R;
    if (!strcmp(s, "g")) return K_G;
    if (!strcmp(s, "b")) return K_B;
    if (!strcmp(s, "ww")) return K_WW;
    if (!strcmp(s, "cw")) return K_CW;
    if (!strcmp(s, "a")) return K_A;
    ok = false;
    return K_NONE;
}

static uint8_t _familyOfKind(uint8_t k) {
    if (k == K_R || k == K_G || k == K_B) return F_RGB;
    if (k == K_WW || k == K_CW) return F_WHITE;
    return F_AMBER;
}

// Centre of "at": one point [x, y] or a list of points.
static bool _centre(JsonVariant at, float& x, float& y) {
    JsonArray a = at.as<JsonArray>();
    if (a.isNull() || a.size() == 0) return false;
    if (a[0].is<JsonArray>()) {
        float sx = 0, sy = 0;
        uint8_t n = 0;
        for (JsonArray p : a) {
            if (p.size() < 2) return false;
            sx += p[0].as<float>();
            sy += p[1].as<float>();
            n++;
        }
        x = sx / n;
        y = sy / n;
        return true;
    }
    if (a.size() < 2) return false;
    x = a[0].as<float>();
    y = a[1].as<float>();
    return true;
}

static bool _vec3(JsonVariant v, float* out) {
    JsonArray a = v.as<JsonArray>();
    if (a.isNull() || a.size() != 3) return false;
    for (uint8_t i = 0; i < 3; i++) out[i] = a[i].as<float>();
    return true;
}

static int16_t _toPos(float mm) {
    float v = roundf(mm * 10.0f);
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)v;
}

bool Lamp::parse(JsonDocument& doc, Output* outs, uint8_t& outCount, char* nameOut, size_t nameLen,
                 const char*& err) {
    err = nullptr;
    outCount = 0;
    if (strcmp(doc["format"] | "", "lightwitch-lamp") != 0 || (doc["version"] | 0) != 1) {
        err = "not a lightwitch-lamp v1 file";
        return false;
    }
    if (nameOut) strlcpy(nameOut, doc["name"] | "", nameLen);
    JsonObject types = doc["types"].as<JsonObject>();
    JsonArray outputs = doc["outputs"].as<JsonArray>();
    if (types.isNull() || outputs.isNull() || outputs.size() == 0) {
        err = "types or outputs missing";
        return false;
    }
    if (outputs.size() > MAX_LIGHTS) {
        err = "too many outputs";
        return false;
    }
    uint32_t totalPixels = 0;
    for (JsonObject o : outputs) {
        Output& out = outs[outCount];
        out = Output();
        strlcpy(out.id, o["id"] | "", sizeof(out.id));
        out.light = o["light"].isNull() ? 0xFF : (uint8_t)(o["light"].as<int>());
        if (out.light != 0xFF) {
            if (out.light >= MAX_LIGHTS || !Config::get().lights[out.light].exists) {
                err = "light does not exist";
                return false;
            }
            for (uint8_t j = 0; j < outCount; j++)
                if (outs[j].light == out.light) {
                    err = "two outputs on one light";
                    return false;
                }
        }
        JsonArray mods = o["modules"].as<JsonArray>();
        if (mods.isNull()) {
            err = "output without modules";
            return false;
        }
        for (JsonObject m : mods) {
            JsonObject t = types[m["t"] | ""].as<JsonObject>();
            if (t.isNull()) {
                err = "unknown module type";
                return false;
            }
            float area = (t["area"] | 0.0f) / 10000.0f;  // mm² -> dm²
            JsonArray pixels = t["pixels"].as<JsonArray>();
            float p[3], n[3], u[3];
            if (area <= 0 || pixels.isNull() || !_vec3(m["p"], p) || !_vec3(m["n"], n) ||
                !_vec3(m["u"], u)) {
                err = "bad module or type";
                return false;
            }
            // Normalise n, make u orthogonal to it (Gram-Schmidt), r = u x n.
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl < 1e-6f) {
                err = "bad module normal";
                return false;
            }
            for (float& v : n) v /= nl;
            float un = u[0] * n[0] + u[1] * n[1] + u[2] * n[2];
            for (uint8_t i = 0; i < 3; i++) u[i] -= un * n[i];
            float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            if (ul < 1e-6f) {
                err = "module u parallel to n";
                return false;
            }
            for (float& v : u) v /= ul;
            float r[3] = {u[1] * n[2] - u[2] * n[1], u[2] * n[0] - u[0] * n[2],
                          u[0] * n[1] - u[1] * n[0]};

            // Pass 1: kinds, positions, pixel families of this module.
            size_t base = out.ch.size();
            uint8_t famPixels[F_COUNT] = {0, 0, 0};
            std::vector<uint8_t> pixFam;
            std::vector<float> lx;
            std::vector<float> ly;
            for (JsonObject px : pixels) {
                JsonArray ch = px["ch"].as<JsonArray>();
                if (ch.size() != 3) {
                    err = "pixel needs 3 channels";
                    return false;
                }
                uint8_t fam = 0xFF;
                for (JsonObject c : ch) {
                    bool ok;
                    Channel cc;
                    cc.kind = _kindFromString(c["k"] | "", ok);
                    if (!ok) {
                        err = "unknown channel kind";
                        return false;
                    }
                    cc.lm = c["lm"] | 0.0f;
                    float x = 0, y = 0;
                    if (cc.kind != K_NONE && !_centre(c["at"], x, y)) {
                        err = "channel without position";
                        return false;
                    }
                    lx.push_back(x);
                    ly.push_back(y);
                    cc.pos[0] = _toPos(p[0] + x * r[0] + y * u[0]);
                    cc.pos[1] = _toPos(p[1] + x * r[1] + y * u[1]);
                    cc.pos[2] = _toPos(p[2] + x * r[2] + y * u[2]);
                    if (cc.kind != K_NONE) {
                        uint8_t f = _familyOfKind(cc.kind);
                        if (fam == 0xFF || f < fam) fam = f;  // rgb < white < amber
                    }
                    out.ch.push_back(cc);
                }
                pixFam.push_back(fam);
                if (fam != 0xFF) famPixels[fam]++;
                if (++totalPixels > MAX_PIXELS) {
                    err = "too many pixels";
                    return false;
                }
            }
            // Pass 2: emitters (channels of a pixel at one position) and their area.
            uint8_t mask = 0;
            for (uint8_t f = 0; f < F_COUNT; f++)
                if (famPixels[f]) mask |= 1 << f;
            for (size_t pi = 0; pi < pixFam.size(); pi++) {
                Channel* c = &out.ch[base + pi * 3];
                const float* x = &lx[pi * 3];
                const float* y = &ly[pi * 3];
                uint8_t emitters = 0;
                for (uint8_t i = 0; i < 3; i++) {
                    c[i].family = pixFam[pi] == 0xFF ? F_AMBER : pixFam[pi];
                    c[i].modMask = mask;
                    bool same = false;
                    for (uint8_t j = 0; j < i; j++)
                        if (c[j].kind != K_NONE && fabsf(x[i] - x[j]) < 0.01f &&
                            fabsf(y[i] - y[j]) < 0.01f)
                            same = true;
                    c[i].firstOfEmitter = c[i].kind == K_NONE || !same;
                    if (c[i].kind != K_NONE && !same) emitters++;
                }
                // Emitters of one pixel are contiguous in practice; a channel
                // sharing a position with an earlier, non-adjacent one still
                // gets its own entry with the emitter area below.
                float a = (pixFam[pi] == 0xFF || emitters == 0)
                              ? 0.0f
                              : area / famPixels[pixFam[pi]] / emitters;
                for (uint8_t i = 0; i < 3; i++) c[i].area = c[i].kind == K_NONE ? 0.0f : a;
            }
            out.modules++;
        }
        out.pixels = out.ch.size() / 3;
        outCount++;
    }
    return true;
}

void Lamp::load() {
    _loaded = false;
    File f = LittleFS.open(FILE_PATH, "r");
    if (!f) return;
    if (f.size() > MAX_FILE_BYTES) {
        Logger::e("[lamp] %s too large (%u B)", FILE_PATH, (unsigned)f.size());
        f.close();
        return;
    }
    JsonDocument doc;
    DeserializationError de = deserializeJson(doc, f);
    f.close();
    if (de) {
        Logger::e("[lamp] %s: %s", FILE_PATH, de.c_str());
        return;
    }
    const char* err;
    if (!parse(doc, _outputs, _outputCount, _name, sizeof(_name), err)) {
        Logger::e("[lamp] %s rejected: %s", FILE_PATH, err);
        return;
    }
    _computeCeilings();
    _loaded = true;
    for (uint8_t i = 0; i < _outputCount; i++)
        Logger::i("[lamp] \"%s\" output %s -> light %d: %u modules, %u pixels", _name,
                  _outputs[i].id, _outputs[i].light == 0xFF ? -1 : _outputs[i].light,
                  _outputs[i].modules, _outputs[i].pixels);
}

const Output* Lamp::outputForLight(uint8_t light) {
    if (!_loaded) return nullptr;
    for (uint8_t i = 0; i < _outputCount; i++)
        if (_outputs[i].light == light) return &_outputs[i];
    return nullptr;
}

void Lamp::_computeCeilings() {
    for (uint8_t f = 0; f < F_COUNT; f++) {
        _famArea[f] = 0;
        for (uint8_t k = 0; k < K_COUNT; k++) _dens[f][k] = 0;
    }
    _maskCount = 0;
    for (uint8_t o = 0; o < _outputCount; o++) {
        if (_outputs[o].light == 0xFF) continue;  // not driven by this device
        for (const Channel& c : _outputs[o].ch) {
            if (c.kind == K_NONE || c.area <= 0) continue;
            bool known = false;
            for (uint8_t i = 0; i < _maskCount; i++) known |= _masks[i] == c.modMask;
            if (!known && _maskCount < 8) _masks[_maskCount++] = c.modMask;
            if (c.firstOfEmitter) _famArea[c.family] += c.area;
            float d = c.lm / c.area;
            float& cur = _dens[c.family][c.kind];
            if (cur == 0 || d < cur) cur = d;
        }
    }
    _computeHeight();
}

void Lamp::toUpFrame(const float* in, float* out) {
    for (uint8_t r = 0; r < 3; r++)
        out[r] = _rot[3 * r] * in[0] + _rot[3 * r + 1] * in[1] + _rot[3 * r + 2] * in[2];
}

// Rotation taking the unit vector u onto +z (Rodrigues, axis u x z).
void Lamp::setUp(const float* up) {
    float n = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (n < 1e-3f) return;
    float ux = up[0] / n, uy = up[1] / n, uz = up[2] / n;
    if (uz < -0.9999f) {
        const float flip[9] = {1, 0, 0, 0, -1, 0, 0, 0, -1};
        memcpy(_rot, flip, sizeof(_rot));
    } else {
        // v = u x z = (uy, -ux, 0); R = I + [v]x + [v]x^2 / (1 + uz)
        float vx = uy, vy = -ux, k = 1.0f / (1.0f + uz);
        float r[9] = {1 - vy * vy * k,
                      vx * vy * k,
                      vy,
                      vx * vy * k,
                      1 - vx * vx * k,
                      -vx,
                      -vy,
                      vx,
                      1 - (vx * vx + vy * vy) * k};
        memcpy(_rot, r, sizeof(_rot));
    }
    _tilted = uz < 0.9999f;
    _computeHeight();
}

void Lamp::_computeHeight() {
    bool first = true;
    for (uint8_t o = 0; o < _outputCount; o++) {
        if (_outputs[o].light == 0xFF) continue;
        for (const Channel& c : _outputs[o].ch) {
            if (c.kind == K_NONE || c.area <= 0) continue;
            float p[3] = {c.pos[0] / 10.0f, c.pos[1] / 10.0f, c.pos[2] / 10.0f}, q[3];
            toUpFrame(p, q);
            if (first || q[2] < _zMin) _zMin = q[2];
            if (first || q[2] > _zMax) _zMax = q[2];
            first = false;
        }
    }
}

float Lamp::_ceilingFor(uint8_t family, const float* w) {
    float sum = 0;
    for (uint8_t k = 0; k < K_COUNT; k++) sum += w[k] * _dens[family][k];
    float ceil = 0;
    for (uint8_t k = 1; k < K_COUNT; k++) {
        if (w[k] <= 0 || _dens[family][k] <= 0) continue;
        float c = sum / w[k];
        if (ceil == 0 || c < ceil) ceil = c;
    }
    return ceil;
}

float Lamp::ceiling(uint8_t family) {
    float w[K_COUNT];
    for (uint8_t k = 0; k < K_COUNT; k++) w[k] = _dens[family][k] > 0 ? 1.0f : 0.0f;
    return _ceilingFor(family, w);
}

// Amber family for deep warm tones (glow), RGB for colours white chips
// cannot mix (not ordered r >= g >= b, or fully saturated), white otherwise.
uint8_t Lamp::renderFamily(uint8_t mask, float r, float g, float b) {
    bool hasRgb = mask & (1 << F_RGB), hasWhite = mask & (1 << F_WHITE),
         hasAmber = mask & (1 << F_AMBER);
    if (hasAmber && g <= 0.45f && b <= 0.15f) return F_AMBER;
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    bool warmOrdered = r >= g && g >= b;
    if (hasRgb && (!warmOrdered || 1.0f - mn >= 0.95f)) return F_RGB;
    if (hasWhite) return F_WHITE;
    if (hasRgb) return F_RGB;
    return F_AMBER;
}

float Lamp::colourCeiling(float r, float g, float b) {
    float ceil = 0;
    float w[K_COUNT];
    for (uint8_t i = 0; i < _maskCount; i++) {
        uint8_t f = renderFamily(_masks[i], r, g, b);
        familyWeights(f, r, g, b, w);
        float c = _ceilingFor(f, w);
        if (c > 0 && (ceil == 0 || c < ceil)) ceil = c;
    }
    return ceil;
}

void Lamp::emitterDuty(const Channel* ch, uint8_t n, uint8_t r, uint8_t g, uint8_t b, float M,
                       uint8_t* out) {
    for (uint8_t i = 0; i < n; i++) out[i] = 0;
    uint8_t m = r > g ? r : g;
    if (b > m) m = b;
    float area = ch[0].area;
    if (m == 0 || M <= 0 || area <= 0) return;
    float cr = (float)r / m, cg = (float)g / m, cb = (float)b / m;
    if (renderFamily(ch[0].modMask, cr, cg, cb) != ch[0].family) return;
    float w[K_COUNT];
    familyWeights(ch[0].family, cr, cg, cb, w);
    float flux = 0, wmax = 0;
    for (uint8_t i = 0; i < n; i++) {
        flux += w[ch[i].kind] * ch[i].lm;
        if (w[ch[i].kind] > wmax) wmax = w[ch[i].kind];
    }
    if (flux <= 0) return;
    if (mode() != Mode::WithPeaks) {
        float c = colourCeiling(cr, cg, cb);
        if (c > 0 && M > c) M = c;
    }
    // Duty of channel i = w_i * k; clip the emitter as a whole so the hue stays.
    float k = (float)m / 255.0f * M * area / flux;
    if (k * wmax > 1.0f) k = 1.0f / wmax;
    for (uint8_t i = 0; i < n; i++) out[i] = (uint8_t)lroundf(w[ch[i].kind] * k * 255.0f);
}

}  // namespace lamp
