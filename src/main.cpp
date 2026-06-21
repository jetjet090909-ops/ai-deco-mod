#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <cocos2d.h>
#include <matjson.hpp>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unordered_map>
#include <algorithm>

using namespace geode::prelude;
using namespace cocos2d;

// ═══════════════════════════════════════════════════════════════════
//  BASE64
// ═══════════════════════════════════════════════════════════════════

static const std::string B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64Encode(const unsigned char* d, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned b = d[i] << 16;
        if (i+1 < len) b |= d[i+1] << 8;
        if (i+2 < len) b |= d[i+2];
        out += B64[(b>>18)&63]; out += B64[(b>>12)&63];
        out += (i+1 < len) ? B64[(b>>6)&63] : '=';
        out += (i+2 < len) ? B64[b&63]      : '=';
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREENSHOT
// ═══════════════════════════════════════════════════════════════════

struct Snap { std::string b64; bool ok = false; };

static Snap captureEditor() {
    auto* dir = CCDirector::sharedDirector();
    auto sz = dir->getWinSize();
    auto* rt = CCRenderTexture::create(
        (int)sz.width, (int)sz.height, kCCTexture2DPixelFormat_RGBA8888);
    if (!rt) return {};
    rt->begin();
    dir->getRunningScene()->visit();
    rt->end();
    auto* img = rt->newCCImage(false);
    if (!img) return {};
    auto path = Mod::get()->getSaveDir() / "gd_ai_snap.png";
    bool saved = img->saveToFile(path.string().c_str(), false);
    CC_SAFE_RELEASE(img);
    if (!saved) return {};
    auto raw = file::readBinary(path);
    if (!raw) return {};
    auto data = raw.unwrap();
    Snap s; s.b64 = b64Encode(data.data(), data.size()); s.ok = !s.b64.empty();
    return s;
}

// ═══════════════════════════════════════════════════════════════════
//  CONSTANTS
// ═══════════════════════════════════════════════════════════════════

static constexpr float GD_UNITS_PER_SEC = 311.f;  // Speed 1x
static constexpr int   CONFIDENCE_WARN  = 70;

// Object IDs safe for all players (always owned)
static const std::vector<int> SAFE_PALETTE = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    211, 467, 1031, 1755, 1756,
    1329, 1330, 1331, 1332, 1334,
    899, 1006
};

// Style presets — detailed prompts for consistent results
static const std::vector<std::pair<std::string, std::string>> PRESETS = {
    {"HELL",   "hellfire demonic inferno - deep reds and burning oranges, pitch black bg, lava glow effects, bone and spike shapes, everything feels scorched and alive with fire"},
    {"SPACE",  "deep cosmic void - near-black bg, cyan and violet nebula glows, dense star particle fields, hovering crystal formations, dramatic lens flares and light streaks"},
    {"OCEAN",  "abyssal underwater depth - deep teal and midnight blue, bioluminescent glowing particles, soft coral and seaweed silhouettes, shimmering light rays filtering from above"},
    {"NEON",   "cyberpunk dystopia nightscape - absolute black bg, hot pink and electric blue neon line outlines, sharp geometric grid patterns, glitch pulse effects at intervals"},
    {"NATURE", "enchanted ancient forest - deep emerald greens and earthy browns, firefly glow particles, twisted vine and root silhouettes, warm dappled golden canopy light"},
};

// ═══════════════════════════════════════════════════════════════════
//  GEMINI URL
// ═══════════════════════════════════════════════════════════════════

static const std::string GEMINI_URL =
    "https://generativelanguage.googleapis.com/v1beta/models/"
    "gemini-2.0-flash:generateContent";

// ─── Build per-pass system prompt ────────────────────────────────

static std::string buildPassPrompt(int pass, bool ownedOnly,
                                   float secStart, float secEnd,
                                   float bpm) {
    float beatLen = GD_UNITS_PER_SEC * 60.f / bpm;

    std::string passStr;
    switch (pass) {
        case 0:
            passStr =
                "═══ PASS 1 — BACKGROUND LAYER ═══\n"
                "Place ONLY large background objects. z_layer MUST be -3.\n"
                "Scale range: 1.5–4.0. Object count: 15–25.\n"
                "These are massive atmospheric shapes far behind gameplay.\n"
                "Sparse placement — these set the mood, not the detail.";
            break;
        case 1:
            passStr =
                "═══ PASS 2 — MIDGROUND LAYER ═══\n"
                "Place ONLY midground detail objects. z_layer MUST be -1.\n"
                "Scale range: 0.8–2.0. Object count: 20–30.\n"
                "Fill space between background and gameplay with layered interest.\n"
                "Denser than pass 1. Avoid covering pass 1 objects entirely.";
            break;
        case 2:
            passStr = std::string(
                "═══ PASS 3 — FOREGROUND + TRIGGERS ═══\n"
                "Place small foreground accent objects AND all color/pulse triggers.\n"
                "z_layer for deco: 1 or 3. Scale: 0.3–1.2. Count: 10–20 deco objects.\n"
                "ALSO generate 4–8 color triggers and 3–5 pulse triggers.\n"
                "BPM = ") + std::to_string((int)bpm) + ". GD speed = ~311 units/sec.\n" +
                "Beat interval in units = " + std::to_string((int)beatLen) + ".\n" +
                "Snap trigger X positions to nearest beat grid (" +
                std::to_string((int)beatLen) + " unit intervals).";
            break;
        default:
            passStr = "Place decoration objects.";
    }

    std::string secStr = "";
    if (secStart >= 0.f && secEnd > secStart) {
