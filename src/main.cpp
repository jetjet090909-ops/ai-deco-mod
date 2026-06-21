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
        secStr = "\n\nSECTION CONSTRAINT: ONLY place objects between x=" +
            std::to_string((int)secStart) + " and x=" +
            std::to_string((int)secEnd) +
            ". Do NOT place anything outside this x range.\n";
    }

    std::string palStr = "";
    if (ownedOnly) {
        palStr = "\n\nOBJECT PALETTE (restricted): Only use IDs: "
            "1,2,3,4,5,6,7,8,9,10,211,467,1031,1755,1756,1329,1330,1331,1332,1334\n";
    }

    return R"(
You are an elite Geometry Dash level decorator matching the visual artistry of
Tidal Wave, Yatagarasu, Bloodbath, and Cataclysm. You understand depth layering,
color theory, and atmosphere-first decoration philosophy.

You are given a SCREENSHOT of the current GD editor state. Before generating,
carefully analyze:
  - Where the ground line is (the base most blocks are on)
  - Where the gameplay path runs (blocks, spikes, platforms)
  - What empty space is available (above, below, between objects)
  - What was already placed in previous passes (if any)

Your output MUST NOT overlap with gameplay objects.
Keep all decorations at least 30 units away from any gameplay block you see.

)" + passStr + secStr + palStr + R"(

Respond ONLY with raw JSON — absolutely no markdown, no backticks, no explanation text:

{
  "analysis": "one concise sentence describing what you see in the editor",
  "confidence": 85,
  "bg_color": {"r": 5, "g": 0, "b": 15},
  "ground_color": {"r": 10, "g": 0, "b": 30},
  "objects": [
    {
      "id": 211,
      "x": 150, "y": 300,
      "scale": 2.5, "rotation": 0,
      "flip_x": false,
      "z_layer": -3,
      "color_channel": 1
    }
  ],
  "color_channels": [
    {"channel": 1, "r": 255, "g": 50, "b": 200, "opacity": 1.0, "blending": true}
  ],
  "triggers": [
    {"type": "color", "x": 311, "target_channel": 1, "r": 200, "g": 0, "b": 80, "duration": 0.5, "blending": true},
    {"type": "pulse", "x": 622, "target_channel": 2, "r": 150, "g": 50, "b": 255, "fade_in": 0.1, "hold": 0.2, "fade_out": 0.4}
  ]
}

KEY RULES:
  - confidence: your 0–100 certainty about how well you can see the layout
  - z_layer must match the current pass instruction above
  - Best deco IDs: 211(glow sq), 1755(circle), 1756(diamond), 1031(small glow),
    467(triangle), 1329-1334(particles), 1616-1620(lens flares), 1/10(blocks)
  - Use color_channel 1-8 for dynamic color via triggers
  - Vary scales dramatically for depth perception
  - ONLY output the raw JSON object, nothing else
)";
}

// ═══════════════════════════════════════════════════════════════════
//  CHAT ENTRY
// ═══════════════════════════════════════════════════════════════════

struct ChatEntry {
    std::string sender;
    std::string message;
    std::string timestamp;
};

// ═══════════════════════════════════════════════════════════════════
//  AI DECO POPUP
// ═══════════════════════════════════════════════════════════════════

class AIDecoPopup : public FLAlertLayer {

    // ── UI nodes ─────────────────────────────────────────────────
    CCTextInputNode* m_promptInput   = nullptr;
    CCTextInputNode* m_bpmInput      = nullptr;
    CCTextInputNode* m_secStartInput = nullptr;
    CCTextInputNode* m_secEndInput   = nullptr;
    CCLabelBMFont*   m_statusLabel   = nullptr;
    CCLayer*         m_chatLayer     = nullptr;
    CCMenu*          m_actionMenu    = nullptr;

    // ── State ────────────────────────────────────────────────────
    float m_chatY         = 0.f;
    bool  m_busy          = false;
    bool  m_ownedOnly     = false;
    bool  m_previewMode   = true;
    float m_bpm           = 120.f;
    float m_secStart      = -1.f;
    float m_secEnd        = -1.f;
    std::string m_currentPrompt;
    std::string m_currentApiKey;

    // ── Object tracking ──────────────────────────────────────────
    std::vector<GameObject*>              m_previewObjects;
    std::vector<GameObject*>              m_lastPlaced;
    std::vector<std::vector<GameObject*>> m_passObjects;  // [pass][objs]

    // ── Chat history ─────────────────────────────────────────────
    std::vector<ChatEntry> m_chatHistory;

    // ── Network ──────────────────────────────────────────────────
    EventListener<Task<web::WebResponse, web::WebProgress>> m_listener;

    // ── Dimensions ───────────────────────────────────────────────
    static constexpr float PW     = 460.f;
    static constexpr float PH     = 420.f;
    static constexpr float CHAT_H = 190.f;

protected:
    bool init() {
        if (!FLAlertLayer::init(nullptr, "AI Deco Assistant", "Close", nullptr, PW))
            return false;
        m_mainLayer->removeAllChildren();
        buildUI();
        return true;
    }

    // ═════════════════════════════════════════════════════════════
    //  UI BUILD
    // ═════════════════════════════════════════════════════════════

    void buildUI() {
        // Re-add popup background (FLAlertLayer defaults were cleared)
        auto* popupBg = CCScale9Sprite::create("GJ_square01.png");
        popupBg->setContentSize({PW, PH});
        popupBg->setPosition({PW/2.f, PH/2.f});
        m_mainLayer->addChild(popupBg, -1);

        // Title
        auto* title = CCLabelBMFont::create("AI Deco Assistant", "goldFont.fnt");
        title->setScale(0.65f);
        title->setPosition({PW/2.f, PH - 18.f});
        m_mainLayer->addChild(title);

        auto* sub = CCLabelBMFont::create(
            "Gemini 2.0 Flash  |  3-Pass Vision  |  by D.M", "chatFont.fnt");
        sub->setScale(0.30f);
        sub->setColor({130, 100, 255});
        sub->setPosition({PW/2.f, PH - 34.f});
        m_mainLayer->addChild(sub);

        // Chat BG
        auto* chatBG = CCScale9Sprite::create("square02_001.png");
        chatBG->setContentSize({PW - 20.f, CHAT_H});
        chatBG->setPosition({PW/2.f, CHAT_H/2.f + 130.f});
        chatBG->setOpacity(55);
        chatBG->setColor({6, 0, 22});
        m_mainLayer->addChild(chatBG);

        // Clipping node for chat scroll
        auto* clip = CCClippingNode::create();
        clip->setContentSize({PW - 22.f, CHAT_H - 4.f});
        clip->setPosition({11.f, 132.f});
        clip->setAlphaThreshold(0.05f);
        auto* stencil = CCLayerColor::create({255,255,255,255});
        stencil->setContentSize({PW - 22.f, CHAT_H - 4.f});
        clip->setStencil(stencil);
        m_mainLayer->addChild(clip, 5);

        m_chatLayer = CCLayer::create();
        m_chatLayer->setPosition({8.f, CHAT_H - 8.f});
        clip->addChild(m_chatLayer);

        // ── Preset buttons ──────────────────────────────────────
        auto* presetMenu = CCMenu::create();
        presetMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(presetMenu, 10);

        float gap = (PW - 32.f) / (float)PRESETS.size();
        for (int i = 0; i < (int)PRESETS.size(); i++) {
            auto* spr = ButtonSprite::create(
                PRESETS[i].first.c_str(), "bigFont.fnt",
                "GJ_button_04.png", 0.4f);
            spr->setScale(0.65f);
            auto* btn = CCMenuItemSpriteExtra::create(
                spr, this, menu_selector(AIDecoPopup::onPreset));
            btn->setTag(i);
            btn->setPosition({16.f + i * gap + gap/2.f, 122.f});
            presetMenu->addChild(btn);
        }

        // ── BPM + Section row ────────────────────────────────────
        // BPM label + input
        addSmallLabel("BPM:", 22.f, 104.f);
        addInputBG(70.f, 104.f, 50.f);
        m_bpmInput = makeInput(50.f, 22.f, "120", 70.f, 104.f);

        // Section label + inputs
        addSmallLabel("X:", 128.f, 104.f);
        addInputBG(162.f, 104.f, 58.f);
        m_secStartInput = makeInput(54.f, 22.f, "start", 162.f, 104.f);

        addSmallLabel("to", 200.f, 104.f);
        addInputBG(232.f, 104.f, 58.f);
        m_secEndInput = makeInput(54.f, 22.f, "end", 232.f, 104.f);

        // Use selection button
        auto* selMenu = CCMenu::create();
        selMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(selMenu, 10);

        auto* selSpr = ButtonSprite::create("SEL", "bigFont.fnt", "GJ_button_05.png", 0.5f);
        selSpr->setScale(0.60f);
        auto* selBtn = CCMenuItemSpriteExtra::create(
            selSpr, this, menu_selector(AIDecoPopup::onUseSelection));
        selBtn->setPosition({285.f, 104.f});
        selMenu->addChild(selBtn);

        // ── Prompt input ─────────────────────────────────────────
        auto* inputBG = CCScale9Sprite::create("square02_001.png");
        inputBG->setContentSize({PW - 120.f, 36.f});
        inputBG->setPosition({(PW-120.f)/2.f + 5.f, 76.f});
        inputBG->setOpacity(130);
        inputBG->setColor({18, 8, 45});
        m_mainLayer->addChild(inputBG, 5);

        m_promptInput = CCTextInputNode::create(
            PW - 130.f, 30.f, "Describe your deco vibe...", "chatFont.fnt");
        m_promptInput->setPosition({(PW-120.f)/2.f + 5.f, 76.f});
        m_mainLayer->addChild(m_promptInput, 6);

        // ── Action buttons ───────────────────────────────────────
        m_actionMenu = CCMenu::create();
        m_actionMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(m_actionMenu, 10);
        buildActionButtons(false);

        // Status bar
        m_statusLabel = CCLabelBMFont::create(
            "Type a vibe, pick a preset, and hit GO!", "chatFont.fnt");
        m_statusLabel->setScale(0.32f);
        m_statusLabel->setColor({160,160,255});
        m_statusLabel->setPosition({PW/2.f - 30.f, 14.f});
        m_mainLayer->addChild(m_statusLabel, 5);

        // Initial messages
        pushChat("AI: Ready! I will screenshot your layout before each pass.", {120,220,255});
        pushChat("AI: Pick a style preset or describe your own vibe below.", {120,220,255});
        pushChat("AI: Set BPM for beat-aligned triggers. Use SEL for section.", {100,200,220});
    }

    // ── Small UI helpers ──────────────────────────────────────────

    void addSmallLabel(const char* txt, float x, float y) {
        auto* lbl = CCLabelBMFont::create(txt, "chatFont.fnt");
        lbl->setScale(0.37f);
        lbl->setPosition({x, y});
        m_mainLayer->addChild(lbl, 5);
    }

    void addInputBG(float x, float y, float w) {
        auto* bg = CCScale9Sprite::create("square02_001.png");
        bg->setContentSize({w, 24.f});
        bg->setPosition({x, y});
        bg->setOpacity(120);
        bg->setColor({20, 10, 50});
        m_mainLayer->addChild(bg, 5);
    }

    CCTextInputNode* makeInput(float w, float h, const char* ph,
                               float x, float y) {
        auto* inp = CCTextInputNode::create(w, h, ph, "chatFont.fnt");
        inp->setPosition({x, y});
        m_mainLayer->addChild(inp, 6);
        return inp;
    }

    // ─── Action button layout (normal vs preview-pending) ─────────

    void buildActionButtons(bool previewPending) {
        m_actionMenu->removeAllChildren();

        if (previewPending) {
            // CONFIRM
            auto* cSpr = ButtonSprite::create("CONFIRM", "bigFont.fnt", "GJ_button_01.png", 0.6f);
            auto* cBtn = CCMenuItemSpriteExtra::create(
                cSpr, this, menu_selector(AIDecoPopup::onConfirmPreview));
            cBtn->setPosition({PW - 70.f, 50.f});
            m_actionMenu->addChild(cBtn);

            // REJECT
            auto* rSpr = ButtonSprite::create("REJECT", "bigFont.fnt", "GJ_button_06.png", 0.6f);
            auto* rBtn = CCMenuItemSpriteExtra::create(
                rSpr, this, menu_selector(AIDecoPopup::onRejectPreview));
            rBtn->setPosition({PW - 70.f, 26.f});
            m_actionMenu->addChild(rBtn);

        } else {
            // GO
            auto* goSpr = ButtonSprite::create("GO", "goldFont.fnt", "GJ_button_01.png", 1.f);
            auto* goBtn = CCMenuItemSpriteExtra::create(
                goSpr, this, menu_selector(AIDecoPopup::onSend));
            goBtn->setPosition({PW - 42.f, 76.f});
            m_actionMenu->addChild(goBtn);

            // UNDO
            auto* uSpr = ButtonSprite::create("UNDO", "bigFont.fnt", "GJ_button_06.png", 0.45f);
            uSpr->setScale(0.65f);
            auto* uBtn = CCMenuItemSpriteExtra::create(
                uSpr, this, menu_selector(AIDecoPopup::onUndo));
            uBtn->setPosition({PW - 42.f, 50.f});
            m_actionMenu->addChild(uBtn);

            // LOG (export)
            auto* lSpr = ButtonSprite::create("LOG", "bigFont.fnt", "GJ_button_05.png", 0.4f);
            lSpr->setScale(0.60f);
            auto* lBtn = CCMenuItemSpriteExtra::create(
                lSpr, this, menu_selector(AIDecoPopup::onExportChat));
            lBtn->setPosition({PW - 42.f, 28.f});
            m_actionMenu->addChild(lBtn);

            // OWN toggle
            auto* oSpr = ButtonSprite::create(
                m_ownedOnly ? "OWN:ON" : "OWN:OFF", "bigFont.fnt",
                m_ownedOnly ? "GJ_button_01.png" : "GJ_button_04.png", 0.38f);
            oSpr->setScale(0.60f);
            auto* oBtn = CCMenuItemSpriteExtra::create(
                oSpr, this, menu_selector(AIDecoPopup::onToggleOwned));
            oBtn->setPosition({330.f, 28.f});
            m_actionMenu->addChild(oBtn);

            // PREVIEW toggle
            auto* pSpr = ButtonSprite::create(
                m_previewMode ? "PRV:ON" : "PRV:OFF", "bigFont.fnt",
                m_previewMode ? "GJ_button_01.png" : "GJ_button_04.png", 0.38f);
            pSpr->setScale(0.60f);
            auto* pBtn = CCMenuItemSpriteExtra::create(
                pSpr, this, menu_selector(AIDecoPopup::onTogglePreview));
            pBtn->setPosition({385.f, 28.f});
            m_actionMenu->addChild(pBtn);
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  CHAT HELPERS
    // ═════════════════════════════════════════════════════════════

    void pushChat(const std::string& msg, ccColor3B col = {230,230,230},
                  const std::string& sender = "AI") {
        auto* lbl = CCLabelBMFont::create(msg.c_str(), "chatFont.fnt");
        lbl->setScale(0.38f);
        lbl->setColor(col);
        lbl->setAnchorPoint({0.f, 1.f});
        lbl->setMaxLineWidth(PW - 52.f);
        lbl->setPosition({0.f, m_chatY});
        m_chatLayer->addChild(lbl);
        m_chatY -= (lbl->getContentSize().height * 0.38f + 5.f);

        // Record history with timestamp
        time_t now = time(nullptr);
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&now));
        m_chatHistory.push_back({sender, msg, std::string(buf)});
    }

    void setStatus(const std::string& msg, ccColor3B col = {255,200,60}) {
        m_statusLabel->setString(msg.c_str());
        m_statusLabel->setColor(col);
    }

    // ═════════════════════════════════════════════════════════════
    //  PRESET HANDLER
    // ═════════════════════════════════════════════════════════════

    void onPreset(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)PRESETS.size()) return;
        m_promptInput->setString(PRESETS[idx].second.c_str());
        pushChat("Preset: " + PRESETS[idx].first, {255,200,80}, "YOU");
        setStatus("Preset loaded! Hit GO when ready.", {100,255,200});
    }

    // ═════════════════════════════════════════════════════════════
    //  USE SELECTION (reads selected objects' X bounds)
    // ═════════════════════════════════════════════════════════════

    void onUseSelection(CCObject*) {
        auto* lel = LevelEditorLayer::get();
        if (!lel) { setStatus("Open editor first!", {255,80,80}); return; }
        auto* edUI = lel->m_editorUI;
        if (!edUI) { setStatus("Open editor first!", {255,80,80}); return; }

        float minX = 1e9f, maxX = -1e9f;
        bool  found = false;

        if (edUI->m_selectedObjects) {
            for (unsigned int i = 0; i < edUI->m_selectedObjects->count(); i++) {
                auto* obj = dynamic_cast<GameObject*>(
                    edUI->m_selectedObjects->objectAtIndex(i));
                if (obj) {
                    float ox = obj->getPositionX();
                    if (ox < minX) minX = ox;
                    if (ox > maxX) maxX = ox;
                    found = true;
                }
            }
        }

        if (!found) {
            setStatus("Select objects in editor first!", {255,150,50});
            return;
        }

        // Add 60-unit padding on each side
        minX -= 60.f; maxX += 60.f;
        m_secStart = minX; m_secEnd = maxX;
        m_secStartInput->setString(std::to_string((int)minX).c_str());
        m_secEndInput->setString(std::to_string((int)maxX).c_str());
        pushChat(fmt::format("Section: x={} to x={}", (int)minX, (int)maxX),
                 {100,255,200}, "SYS");
        setStatus(fmt::format("Section locked: {} to {}", (int)minX, (int)maxX),
                  {100,255,200});
    }

    // ═════════════════════════════════════════════════════════════
    //  TOGGLES
    // ═════════════════════════════════════════════════════════════

    void onToggleOwned(CCObject*) {
        m_ownedOnly = !m_ownedOnly;
        buildActionButtons(false);
        pushChat(m_ownedOnly ? "Palette: owned objects only (safe mode)"
                             : "Palette: all objects (full mode)",
                 {200,200,100}, "SYS");
    }

    void onTogglePreview(CCObject*) {
        m_previewMode = !m_previewMode;
        buildActionButtons(false);
        pushChat(m_previewMode ? "Preview ON — confirm before finalizing"
                               : "Preview OFF — instant placement",
                 {200,200,100}, "SYS");
    }

    // ═════════════════════════════════════════════════════════════
    //  MAIN SEND — starts 3-pass pipeline
    // ═════════════════════════════════════════════════════════════

    void onSend(CCObject*) {
        if (m_busy) { setStatus("Still thinking...", {255,180,50}); return; }

        auto apiKey = Mod::get()->getSettingValue<std::string>("api-key");
        if (apiKey.empty()) {
            setStatus("Add API key in Mod Settings first!", {255,80,80}); return;
        }

        std::string prompt = m_promptInput->getString();
        if (prompt.empty()) {
            setStatus("Type a vibe first!", {255,150,50}); return;
        }

        if (!LevelEditorLayer::get()) {
            setStatus("Open the editor first!", {255,80,80}); return;
        }

        // Parse BPM
        std::string bpmStr = m_bpmInput->getString();
        if (!bpmStr.empty() && bpmStr != "120") {
            try { m_bpm = std::clamp(std::stof(bpmStr), 40.f, 300.f); }
            catch (...) { m_bpm = 120.f; }
        }

        // Parse section X range
        m_secStart = -1.f; m_secEnd = -1.f;
        try {
            auto ss = m_secStartInput->getString();
            auto se = m_secEndInput->getString();
            if (!ss.empty() && ss != "start") m_secStart = std::stof(ss);
            if (!se.empty() && se != "end")   m_secEnd   = std::stof(se);
        } catch (...) {}

        m_currentPrompt  = prompt;
        m_currentApiKey  = apiKey;
        m_passObjects.clear();
        m_passObjects.resize(3);
        m_busy = true;

        pushChat("You: " + prompt, {255,240,120}, "YOU");
        m_promptInput->setString("");
        pushChat(fmt::format("AI: Starting 3-pass decoration at {:.0f} BPM!", m_bpm),
                 {120,220,255});

        runPass(0);
    }

    // ═════════════════════════════════════════════════════════════
    //  MULTI-PASS RUNNER
    // ═════════════════════════════════════════════════════════════

    void runPass(int pass) {
        static const char* passNames[] = {"Background", "Midground", "Foreground+Triggers"};
        setStatus(fmt::format("Pass {}/3: {} — screenshotting...",
                  pass+1, passNames[pass]), {180,130,255});
        pushChat(fmt::format("AI: Pass {}/3 ({}). Capturing fresh screenshot...",
                  pass+1, passNames[pass]), {160,180,255});

        // Hide popup so it doesn't appear in screenshot
        this->setVisible(false);

        this->scheduleOnce([this, pass](float) {
            auto snap = captureEditor();
            this->setVisible(true);

            if (!snap.ok) {
                setStatus("Screenshot failed. Try again.", {255,80,80});
                pushChat("AI: Screenshot error. Please retry.", {255,100,100});
                m_busy = false;
                return;
            }

            setStatus(fmt::format("Pass {}/3 — asking Gemini Vision...", pass+1),
                      {140,120,255});
            sendToGemini(pass, snap.b64);

        }, 0.05f, fmt::format("pass_snap_{}", pass).c_str());
    }

    // ═════════════════════════════════════════════════════════════
    //  GEMINI API CALL
    // ═════════════════════════════════════════════════════════════

    void sendToGemini(int pass, const std::string& imgB64) {
        std::string sysPrompt = buildPassPrompt(
            pass, m_ownedOnly, m_secStart, m_secEnd, m_bpm);
        std::string fullText  = sysPrompt +
            "\n\nDecoration request: " + m_currentPrompt;

        // Build multimodal content
        auto imgSrc = matjson::Object();
        imgSrc["type"]       = "base64";
        imgSrc["media_type"] = "image/png";
        imgSrc["data"]       = imgB64;
        auto imgPart = matjson::Object();
        imgPart["inline_data"] = imgSrc;

        auto txtPart = matjson::Object();
        txtPart["text"] = fullText;

        auto parts = matjson::Array();
        parts.push_back(imgPart);
        parts.push_back(txtPart);

        auto content = matjson::Object();
        content["parts"] = parts;
        content["role"]  = "user";

        auto contents = matjson::Array();
        contents.push_back(content);

        auto genCfg = matjson::Object();
        genCfg["temperature"]     = 0.75;
        genCfg["maxOutputTokens"] = 8192;

        auto body = matjson::Object();
        body["contents"]         = contents;
        body["generationConfig"] = genCfg;

        auto req = web::WebRequest();
        req.header("Content-Type", "application/json");
        req.bodyString(matjson::Value(body).dump());

        m_listener.listen([this, pass](Task<web::WebResponse, web::WebProgress>::Event* e) {
            if (auto* res = e->getValue()) {
                std::string raw  = res->string().unwrap_or("");
                int         code = res->code();
                Loader::get()->queueInMainThread([this, pass, raw, code]() {
                    handlePassResponse(pass, raw, code);
                });
            }
        });
        m_listener = req.post(GEMINI_URL + "?key=" + m_currentApiKey);
    }

    // ═════════════════════════════════════════════════════════════
    //  RESPONSE HANDLER
    // ═════════════════════════════════════════════════════════════

    void handlePassResponse(int pass, const std::string& raw, int code) {
        if (code != 200 || raw.empty()) {
            setStatus(fmt::format("Pass {} API error {}.", pass+1, code), {255,80,80});
            pushChat(fmt::format("AI: Error on pass {}. Code: {}. Check key.",
                     pass+1, code), {255,100,100});
            m_busy = false;
            return;
        }

        auto wrapper = matjson::parse(raw);
        if (wrapper.is_error()) {
            setStatus("Parse error on response.", {255,80,80});
            m_busy = false;
            return;
        }

        std::string jsonText;
        try {
            jsonText = wrapper.unwrap()["candidates"][0]["content"]["parts"][0]["text"]
                           .asString().unwrap_or("");
        } catch (...) {
            setStatus("Unexpected Gemini response format.", {255,80,80});
            m_busy = false;
            return;
        }

        // Strip any markdown fences
        auto start = jsonText.find('{');
        auto end   = jsonText.rfind('}');
        if (start == std::string::npos || end == std::string::npos || end <= start) {
            pushChat(fmt::format("AI: Pass {} format issue — skipping.", pass+1),
                     {255,150,80});
            proceedAfterPass(pass);
            return;
        }
        jsonText = jsonText.substr(start, end - start + 1);

        auto deco = matjson::parse(jsonText);
        if (deco.is_error()) {
            pushChat("AI: JSON error, skipping this pass.", {255,150,80});
            proceedAfterPass(pass);
            return;
        }

        // ── Confidence check ──────────────────────────────────────
        int confidence = deco.unwrap()["confidence"].asInt().unwrap_or(100);
        if (confidence < CONFIDENCE_WARN) {
            pushChat(fmt::format(
                "AI: Low confidence ({}%) — layout not fully visible. "
                "Results may clip gameplay. Consider retrying.",
                confidence), {255,220,50});
            setStatus(fmt::format("Caution: {}% confidence", confidence), {255,200,50});
        }

        // ── Show analysis ─────────────────────────────────────────
        auto analysis = deco.unwrap()["analysis"].asString().unwrap_or("");
        if (!analysis.empty())
            pushChat(fmt::format("[P{}] {}", pass+1, analysis), {150,255,200});

        // ── Place objects ─────────────────────────────────────────
        int placed = applyPassObjects(deco.unwrap(), pass);
        pushChat(fmt::format("AI: Pass {}/3 done — {} objects.", pass+1, placed),
                 {100,255,180});

        proceedAfterPass(pass);
    }

    void proceedAfterPass(int pass) {
        if (pass < 2) {
            // Brief delay then next pass with fresh screenshot
            this->scheduleOnce([this, pass](float) {
                runPass(pass + 1);
            }, 0.15f, fmt::format("next_pass_{}", pass).c_str());
        } else {
            // All 3 passes complete
            m_busy = false;

            if (m_previewMode) {
                // Collect all objects, set to 50% opacity for preview
                m_previewObjects.clear();
                for (auto& passVec : m_passObjects)
                    for (auto* obj : passVec)
                        if (obj) { obj->setOpacity(128); m_previewObjects.push_back(obj); }

                buildActionButtons(true);
                int total = (int)m_previewObjects.size();
                setStatus(fmt::format("Preview ready! {} objects at 50% opacity.", total),
                          {255,230,80});
                pushChat(fmt::format("AI: All 3 passes done! {} total objects previewed.",
                         total), {255,230,80});
                pushChat("AI: CONFIRM to keep, REJECT to remove all.", {255,230,80});

            } else {
                // Instant — finalize immediately
                m_lastPlaced.clear();
                for (auto& passVec : m_passObjects)
                    for (auto* obj : passVec)
                        if (obj) m_lastPlaced.push_back(obj);

                int total = (int)m_lastPlaced.size();
                setStatus(fmt::format("Done! {} objects placed across 3 layers.", total),
                          {100,255,160});
                pushChat(fmt::format("AI: Complete! {} objects — bg/mid/fg layers done.",
                         total), {100,255,180});
            }

            if (auto* lel = LevelEditorLayer::get()) {
                if (auto* edUI = lel->m_editorUI) edUI->deselectAll();
            }
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  APPLY PASS OBJECTS
    // ═════════════════════════════════════════════════════════════

    int applyPassObjects(const matjson::Value& data, int pass) {
        auto* editor = LevelEditorLayer::get();
        if (!editor) return 0;

        int count = 0;
        float beatLen = GD_UNITS_PER_SEC * 60.f / m_bpm;

        // ── Deco objects ──────────────────────────────────────────
        if (data.contains("objects")) {
            auto arr = data["objects"].asArray();
            if (arr.is_ok()) {
                for (auto& obj : arr.unwrap()) {
                    int   id   = obj["id"].asInt().unwrap_or(211);
                    float x    = (float)obj["x"].asDouble().unwrap_or(150.0);
                    float y    = (float)obj["y"].asDouble().unwrap_or(105.0);
                    float sc   = (float)obj["scale"].asDouble().unwrap_or(1.0);
                    float rot  = (float)obj["rotation"].asDouble().unwrap_or(0.0);
                    bool  flpX = obj["flip_x"].asBool().unwrap_or(false);
                    int   zl   = obj["z_layer"].asInt().unwrap_or(-3);

                    // Clamp values
                    id  = std::clamp(id,  1,    3000);
                    sc  = std::clamp(sc,  0.1f, 5.f);
                    zl  = std::clamp(zl, -5,    5);

                    // Owned palette filter
                    if (m_ownedOnly) {
                        bool safe = false;
                        for (int sid : SAFE_PALETTE)
                            if (sid == id) { safe = true; break; }
                        if (!safe) id = 211;
                    }

                    auto* go = editor->createObject(id, {x, y}, false);
                    if (go) {
                        go->setRScale(sc);
                        go->setRotation(rot);
                        if (flpX) go->flipX(true);
                        go->m_zLayer = (ZLayer)zl;
                        m_passObjects[pass].push_back(go);
                        count++;
                    }
                }
            }
        }

        // ── Triggers (pass 2 only) ────────────────────────────────
        if (pass == 2 && data.contains("triggers")) {
            auto arr = data["triggers"].asArray();
            if (arr.is_ok()) {
                for (auto& trig : arr.unwrap()) {
                    std::string type = trig["type"].asString().unwrap_or("color");
                    float x = (float)trig["x"].asDouble().unwrap_or(311.0);

                    // Snap to beat grid
                    if (beatLen > 0.f)
                        x = std::round(x / beatLen) * beatLen;

                    int trigId = (type == "pulse") ? 1006 : 899;
                    auto* go = editor->createObject(trigId, {x, 105.f}, false);
                    if (go) {
                        m_passObjects[pass].push_back(go);
                        count++;
                    }
                }
            }
        }

        return count;
    }

    // ═════════════════════════════════════════════════════════════
    //  PREVIEW CONFIRM / REJECT
    // ═════════════════════════════════════════════════════════════

    void onConfirmPreview(CCObject*) {
        for (auto* obj : m_previewObjects)
            if (obj) obj->setOpacity(255);

        m_lastPlaced = m_previewObjects;
        m_previewObjects.clear();

        buildActionButtons(false);
        setStatus(fmt::format("Confirmed! {} objects kept.", (int)m_lastPlaced.size()),
                  {100,255,160});
        pushChat(fmt::format("AI: Saved! {} objects confirmed.", (int)m_lastPlaced.size()),
                 {100,255,180});
    }

    void onRejectPreview(CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (editor)
            for (auto* obj : m_previewObjects)
                if (obj) editor->removeObject(obj, true);

        int count = (int)m_previewObjects.size();
        m_previewObjects.clear();

        buildActionButtons(false);
        setStatus("Rejected. Try a new vibe!", {255,150,80});
        pushChat(fmt::format("AI: Removed {} preview objects.", count), {255,150,100});
    }

    // ═════════════════════════════════════════════════════════════
    //  UNDO LAST PLACEMENT
    // ═════════════════════════════════════════════════════════════

    void onUndo(CCObject*) {
        if (m_lastPlaced.empty()) {
            setStatus("Nothing to undo!", {255,150,50}); return;
        }
        auto* editor = LevelEditorLayer::get();
        if (editor)
            for (auto* obj : m_lastPlaced)
                if (obj) editor->removeObject(obj, true);

        int count = (int)m_lastPlaced.size();
        m_lastPlaced.clear();
        setStatus(fmt::format("Undone {} objects.", count), {255,180,80});
        pushChat(fmt::format("AI: Undone. {} objects removed.", count), {255,180,100});
    }

    // ═════════════════════════════════════════════════════════════
    //  EXPORT CHAT LOG
    // ═════════════════════════════════════════════════════════════

    void onExportChat(CCObject*) {
        if (m_chatHistory.empty()) {
            setStatus("No chat to export!", {255,150,50}); return;
        }

        std::string path = CCFileUtils::sharedFileUtils()->getWritablePath()
            + "ai_deco_log.txt";

        std::ofstream f(path);
        if (!f.is_open()) {
            setStatus("Couldn't write log file!", {255,80,80}); return;
        }

        f << "╔══════════════════════════════════╗\n";
        f << "║   AI Deco Assistant  —  Chat Log ║\n";
        f << "║         by D.M                   ║\n";
        f << "╚══════════════════════════════════╝\n\n";

        for (auto& e : m_chatHistory)
            f << "[" << e.timestamp << "] " << e.sender << ": " << e.message << "\n";

        f.close();

        setStatus("Log saved to ai_deco_log.txt!", {100,255,160});
        pushChat("SYS: Log saved to writable GD folder.", {200,200,100}, "SYS");
    }

public:
    static AIDecoPopup* create() {
        auto* ret = new AIDecoPopup();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// ═══════════════════════════════════════════════════════════════════
//  EDITOR UI HOOK — adds the AI button
// ═══════════════════════════════════════════════════════════════════

class $modify(MyEditorUI, EditorUI) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;

        auto wsz = CCDirector::sharedDirector()->getWinSize();

        auto* spr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_starBtnOff_001.png", 0.85f,
            CircleBaseColor::Pink,
            CircleBaseSize::Medium);

        auto* btn  = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(MyEditorUI::onOpenAI));
        auto* menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({wsz.width - 48.f, wsz.height - 200.f});
        this->addChild(menu, 100);

        return true;
    }

    void onOpenAI(CCObject*) { AIDecoPopup::create()->show(); }
};
