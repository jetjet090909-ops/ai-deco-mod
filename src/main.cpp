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

static constexpr float GD_UNITS_PER_SEC = 311.f;  
static constexpr int   CONFIDENCE_WARN  = 70;

static const std::vector<int> SAFE_PALETTE = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    211, 467, 1031, 1755, 1756,
    1329, 1330, 1331, 1332, 1334,
    899, 1006
};

static const std::vector<std::pair<std::string, std::string>> PRESETS = {
    {"HELL",   "hellfire demonic inferno - deep reds and burning oranges, pitch black bg, lava glow effects, bone and spike shapes"},
    {"SPACE",  "deep cosmic void - near-black bg, cyan and violet nebula glows, dense star particle fields"},
    {"OCEAN",  "abyssal underwater depth - deep teal and midnight blue, bioluminescent glowing particles"},
    {"NEON",   "cyberpunk dystopia nightscape - absolute black bg, hot pink and electric blue neon outlines"},
    {"NATURE", "enchanted ancient forest - deep emerald greens and earthy browns, firefly glow particles"},
};

static const std::string GEMINI_URL =
    "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent";

// ─── Build per-pass system prompt ────────────────────────────────

static std::string buildPassPrompt(int pass, bool ownedOnly, float secStart, float secEnd, float bpm) {
    float beatLen = GD_UNITS_PER_SEC * 60.f / bpm;
    std::string passStr;
    switch (pass) {
        case 0:
            passStr = "═══ PASS 1 — BACKGROUND LAYER ═══\nPlace ONLY large background objects. z_layer MUST be -3.\nScale range: 1.5–4.0. Object count: 15–25."; break;
        case 1:
            passStr = "═══ PASS 2 — MIDGROUND LAYER ═══\nPlace ONLY midground detail objects. z_layer MUST be -1.\nScale range: 0.8–2.0. Object count: 20–30."; break;
        case 2:
            passStr = "═══ PASS 3 — FOREGROUND + TRIGGERS ═══\nz_layer for deco: 1 or 3. Scale: 0.3–1.2. Count: 10–20 deco objects.\nBPM = " + std::to_string((int)bpm) + ". Beat interval in units = " + std::to_string((int)beatLen); break;
        default: passStr = "Place decoration objects.";
    }

    std::string secStr = "";
    if (secStart >= 0.f && secEnd > secStart) {
        secStr = "\n\nSECTION CONSTRAINT: ONLY place objects between x=" + std::to_string((int)secStart) + " and x=" + std::to_string((int)secEnd);
    }

    std::string palStr = "";
    if (ownedOnly) {
        palStr = "\n\nOBJECT PALETTE (restricted): Only use IDs: 1,2,3,4,5,6,7,8,9,10,211,467,1031,1755,1756,1329,1330,1331,1332,1334\n";
    }

    return "You are an elite Geometry Dash level decorator. Output raw JSON format only.\n" + passStr + secStr + palStr;
}

struct ChatEntry {
    std::string sender;
    std::string message;
    std::string timestamp;
};

// ═══════════════════════════════════════════════════════════════════
//  AI DECO POPUP
// ═══════════════════════════════════════════════════════════════════

class AIDecoPopup : public FLAlertLayer {
    CCTextInputNode* m_promptInput   = nullptr;
    CCTextInputNode* m_bpmInput      = nullptr;
    CCTextInputNode* m_secStartInput = nullptr;
    CCTextInputNode* m_secEndInput   = nullptr;
    CCLabelBMFont* m_statusLabel   = nullptr;
    CCLayer* m_chatLayer     = nullptr;
    CCMenu* m_actionMenu    = nullptr;

    float m_chatY         = 0.f;
    bool  m_busy          = false;
    bool  m_ownedOnly     = false;
    bool  m_previewMode   = true;
    float m_bpm           = 120.f;
    float m_secStart      = -1.f;
    float m_secEnd        = -1.f;
    std::string m_currentPrompt;
    std::string m_currentApiKey;

    std::vector<GameObject*>              m_previewObjects;
    std::vector<GameObject*>              m_lastPlaced;
    std::vector<std::vector<GameObject*>> m_passObjects;
    std::vector<ChatEntry>                m_chatHistory;

    EventListener<Task<web::WebResponse, web::WebProgress>> m_listener;

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

    void buildUI() {
        auto* popupBg = CCScale9Sprite::create("GJ_square01.png");
        popupBg->setContentSize({PW, PH});
        popupBg->setPosition({PW/2.f, PH/2.f});
        m_mainLayer->addChild(popupBg, -1);

        auto* title = CCLabelBMFont::create("AI Deco Assistant", "goldFont.fnt");
        title->setScale(0.65f);
        title->setPosition({PW/2.f, PH - 18.f});
        m_mainLayer->addChild(title);

        auto* chatBG = CCScale9Sprite::create("square02_001.png");
        chatBG->setContentSize({PW - 20.f, CHAT_H});
        chatBG->setPosition({PW/2.f, CHAT_H/2.f + 130.f});
        chatBG->setOpacity(55);
        m_mainLayer->addChild(chatBG);

        auto* clip = CCClippingNode::create();
        clip->setContentSize({PW - 22.f, CHAT_H - 4.f});
        clip->setPosition({11.f, 132.f});
        auto* stencil = CCLayerColor::create({255,255,255,255});
        stencil->setContentSize({PW - 22.f, CHAT_H - 4.f});
        clip->setStencil(stencil);
        m_mainLayer->addChild(clip, 5);

        m_chatLayer = CCLayer::create();
        m_chatLayer->setPosition({8.f, CHAT_H - 8.f});
        clip->addChild(m_chatLayer);

        auto* presetMenu = CCMenu::create();
        presetMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(presetMenu, 10);

        float gap = (PW - 32.f) / (float)PRESETS.size();
        for (int i = 0; i < (int)PRESETS.size(); i++) {
            auto* spr = ButtonSprite::create(PRESETS[i].first.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.4f);
            spr->setScale(0.65f);
            auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AIDecoPopup::onPreset));
            btn->setTag(i);
            btn->setPosition({16.f + i * gap + gap/2.f, 122.f});
            presetMenu->addChild(btn);
        }

        addSmallLabel("BPM:", 22.f, 104.f);
        addInputBG(70.f, 104.f, 50.f);
        m_bpmInput = makeInput(50.f, 22.f, "120", 70.f, 104.f);

        addSmallLabel("X:", 128.f, 104.f);
        addInputBG(162.f, 104.f, 58.f);
        m_secStartInput = makeInput(54.f, 22.f, "start", 162.f, 104.f);

        addSmallLabel("to", 200.f, 104.f);
        addInputBG(232.f, 104.f, 58.f);
        m_secEndInput = makeInput(54.f, 22.f, "end", 232.f, 104.f);

        auto* selMenu = CCMenu::create();
        selMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(selMenu, 10);

        auto* selSpr = ButtonSprite::create("SEL", "bigFont.fnt", "GJ_button_05.png", 0.5f);
        selSpr->setScale(0.60f);
        auto* selBtn = CCMenuItemSpriteExtra::create(selSpr, this, menu_selector(AIDecoPopup::onUseSelection));
        selBtn->setPosition({285.f, 104.f});
        selMenu->addChild(selBtn);

        auto* inputBG = CCScale9Sprite::create("square02_001.png");
        inputBG->setContentSize({PW - 120.f, 36.f});
        inputBG->setPosition({(PW-120.f)/2.f + 5.f, 76.f});
        m_mainLayer->addChild(inputBG, 5);

        m_promptInput = CCTextInputNode::create(PW - 130.f, 30.f, "Describe your deco vibe...", "chatFont.fnt");
        m_promptInput->setPosition({(PW-120.f)/2.f + 5.f, 76.f});
        m_mainLayer->addChild(m_promptInput, 6);

        m_actionMenu = CCMenu::create();
        m_actionMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(m_actionMenu, 10);
        buildActionButtons(false);

        m_statusLabel = CCLabelBMFont::create("Type a vibe, pick a preset, and hit GO!", "chatFont.fnt");
        m_statusLabel->setScale(0.32f);
        m_statusLabel->setPosition({PW/2.f - 30.f, 14.f});
        m_mainLayer->addChild(m_statusLabel, 5);

        pushChat("AI: Ready! I will screenshot your layout before each pass.", {120,220,255});
    }

    void addSmallLabel(const char* txt, float x, float y) {
        auto* lbl = CCLabelBMFont::create(txt, "chatFont.fnt");
        lbl->setScale(0.37f); lbl->setPosition({x, y});
        m_mainLayer->addChild(lbl, 5);
    }

    void addInputBG(float x, float y, float w) {
        auto* bg = CCScale9Sprite::create("square02_001.png");
        bg->setContentSize({w, 24.f}); bg->setPosition({x, y}); bg->setOpacity(120);
        m_mainLayer->addChild(bg, 5);
    }

    CCTextInputNode* makeInput(float w, float h, const char* ph, float x, float y) {
        auto* inp = CCTextInputNode::create(w, h, ph, "chatFont.fnt");
        inp->setPosition({x, y}); m_mainLayer->addChild(inp, 6); return inp;
    }

    void buildActionButtons(bool previewPending) {
        m_actionMenu->removeAllChildren();
        if (previewPending) {
            auto* cSpr = ButtonSprite::create("CONFIRM", "bigFont.fnt", "GJ_button_01.png", 0.6f);
            auto* cBtn = CCMenuItemSpriteExtra::create(cSpr, this, menu_selector(AIDecoPopup::onConfirmPreview));
            cBtn->setPosition({PW - 70.f, 50.f}); m_actionMenu->addChild(cBtn);

            auto* rSpr = ButtonSprite::create("REJECT", "bigFont.fnt", "GJ_button_06.png", 0.6f);
            auto* rBtn = CCMenuItemSpriteExtra::create(rSpr, this, menu_selector(AIDecoPopup::onRejectPreview));
            rBtn->setPosition({PW - 70.f, 26.f}); m_actionMenu->addChild(rBtn);
        } else {
            auto* goSpr = ButtonSprite::create("GO", "goldFont.fnt", "GJ_button_01.png", 1.f);
            auto* goBtn = CCMenuItemSpriteExtra::create(goSpr, this, menu_selector(AIDecoPopup::onSend));
            goBtn->setPosition({PW - 42.f, 76.f}); m_actionMenu->addChild(goBtn);

            auto* uSpr = ButtonSprite::create("UNDO", "bigFont.fnt", "GJ_button_06.png", 0.45f);
            uSpr->setScale(0.65f);
            auto* uBtn = CCMenuItemSpriteExtra::create(uSpr, this, menu_selector(AIDecoPopup::onUndo));
            uBtn->setPosition({PW - 42.f, 50.f}); m_actionMenu->addChild(uBtn);

            auto* lSpr = ButtonSprite::create("LOG", "bigFont.fnt", "GJ_button_05.png", 0.4f);
            lSpr->setScale(0.60f);
            auto* lBtn = CCMenuItemSpriteExtra::create(lSpr, this, menu_selector(AIDecoPopup::onExportChat));
            lBtn->setPosition({PW - 42.f, 28.f}); m_actionMenu->addChild(lBtn);
        }
    }

    void pushChat(const std::string& msg, ccColor3B col = {230,230,230}, const std::string& sender = "AI") {
        auto* lbl = CCLabelBMFont::create(msg.c_str(), "chatFont.fnt");
        lbl->setScale(0.38f); lbl->setColor(col); lbl->setAnchorPoint({0.f, 1.f});
        lbl->setMaxLineWidth(PW - 52.f); lbl->setPosition({0.f, m_chatY});
        m_chatLayer->addChild(lbl);
        m_chatY -= (lbl->getContentSize().height * 0.38f + 5.f);

        time_t now = time(nullptr);
        char buf[16]; strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&now));
        m_chatHistory.push_back({sender, msg, std::string(buf)});
    }

    void setStatus(const std::string& msg, ccColor3B col = {255,200,60}) {
        m_statusLabel->setString(msg.c_str()); m_statusLabel->setColor(col);
    }

    void onPreset(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)PRESETS.size()) return;
        m_promptInput->setString(PRESETS[idx].second.c_str());
        pushChat("Preset: " + PRESETS[idx].first, {255,200,80}, "YOU");
    }

    void onUseSelection(CCObject*) {
        auto* lel = LevelEditorLayer::get(); if (!lel) return;
        auto* edUI = lel->m_editorUI; if (!edUI || !edUI->m_selectedObjects) return;

        float minX = 1e9f, maxX = -1e9f; bool found = false;
        for (unsigned int i = 0; i < edUI->m_selectedObjects->count(); i++) {
            auto* obj = dynamic_cast<GameObject*>(edUI->m_selectedObjects->objectAtIndex(i));
            if (obj) {
                float ox = obj->getPositionX();
                if (ox < minX) minX = ox; if (ox > maxX) maxX = ox;
                found = true;
            }
        }
        if (!found) return;
        m_secStart = minX - 60.f; m_secEnd = maxX + 60.f;
        m_secStartInput->setString(std::to_string((int)m_secStart).c_str());
        m_secEndInput->setString(std::to_string((int)m_secEnd).c_str());
    }

    void onSend(CCObject*) {
        if (m_busy) return;
        auto apiKey = Mod::get()->getSettingValue<std::string>("api-key");
        if (apiKey.empty()) { setStatus("Missing API Key!", {255,80,80}); return; }

        m_currentPrompt = m_promptInput->getString();
        if (m_currentPrompt.empty()) return;

        m_currentApiKey = apiKey;
        m_passObjects.clear(); m_passObjects.resize(3);
        m_busy = true;
        runPass(0);
    }

    void runPass(int pass) {
        this->setVisible(false);
        this->scheduleOnce([this, pass](float) {
            auto snap = captureEditor();
            this->setVisible(true);
            if (!snap.ok) { m_busy = false; return; }
            sendToGemini(pass, snap.b64);
        }, 0.05f, "pass_snap");
    }

    void sendToGemini(int pass, const std::string& imgB64) {
        std::string sysPrompt = buildPassPrompt(pass, m_ownedOnly, m_secStart, m_secEnd, m_bpm);
        
        auto body = matjson::Object();
        auto contents = matjson::Array();
        auto content = matjson::Object();
        auto parts = matjson::Array();
        
        auto imgPart = matjson::Object();
        auto imgSrc = matjson::Object();
        imgSrc["type"] = "base64";
        imgSrc["media_type"] = "image/png";
        imgSrc["data"] = imgB64;
        imgPart["inline_data"] = imgSrc;
        
        auto txtPart = matjson::Object();
        txtPart["text"] = sysPrompt + "\n\nUser request: " + m_currentPrompt;
        
        parts.push_back(imgPart);
        parts.push_back(txtPart);
        content["parts"] = parts;
        contents.push_back(content);
        body["contents"] = contents;

        auto req = web::WebRequest();
        req.header("Content-Type", "application/json");
        req.bodyString(matjson::Value(body).dump());

        m_listener.listen([this, pass](Task<web::WebResponse, web::WebProgress>::Event* e) {
            if (auto* res = e->getValue()) {
                std::string raw = res->string().unwrap_or("");
                int code = res->code();
                Loader::get()->queueInMainThread([this, pass, raw, code]() {
                    handlePassResponse(pass, raw, code);
                });
            }
        });
        m_listener = req.post(GEMINI_URL + "?key=" + m_currentApiKey).send();
    }

    void handlePassResponse(int pass, const std::string& raw, int code) {
        if (code != 200 || raw.empty()) { m_busy = false; return; }
        
        auto wrapper = matjson::parse(raw);
        if (wrapper.is_error()) { m_busy = false; return; }

        std::string jsonText = "";
        try {
            jsonText = wrapper.unwrap()["candidates"][0]["content"]["parts"][0]["text"].asString().unwrap_or("");
        } catch (...) { m_busy = false; return; }

        auto start = jsonText.find('{');
        auto end = jsonText.rfind('}');
        if (start == std::string::npos || end == std::string::npos) { proceedAfterPass(pass); return; }
        
        auto deco = matjson::parse(jsonText.substr(start, end - start + 1));
        if (!deco.is_error()) {
            applyPassObjects(deco.unwrap(), pass);
        }
        proceedAfterPass(pass);
    }

    void proceedAfterPass(int pass) {
        if (pass < 2) {
            this->scheduleOnce([this, pass](float) { runPass(pass + 1); }, 0.15f, "next_pass");
        } else {
            m_busy = false;
            if (m_previewMode) {
                m_previewObjects.clear();
                for (auto& vec : m_passObjects) {
                    for (auto* obj : vec) {
                        if (obj) { obj->setOpacity(128); m_previewObjects.push_back(obj); }
                    }
                }
                buildActionButtons(true);
            }
        }
    }

    void applyPassObjects(const matjson::Value& data, int pass) {
        auto* editor = LevelEditorLayer::get(); if (!editor) return;
        if (!data.contains("objects")) return;
        
        auto arr = data["objects"].asArray();
        if (!arr.is_ok()) return;

        for (auto& obj : arr.unwrap()) {
            int id = obj["id"].asInt().unwrap_or(211);
            float x = (float)obj["x"].asDouble().unwrap_or(150.0);
            float y = (float)obj["y"].asDouble().unwrap_or(105.0);
            float sc = (float)obj["scale"].asDouble().unwrap_or(1.0);
            int zl = obj["z_layer"].asInt().unwrap_or(-3);

            auto* go = editor->createObject(id, {x, y}, false);
            if (go) {
                go->setRScale(sc);
                // Safe variable casting matching modern v5.7.1 profiles
                go->m_zLayer = static_cast<geode::SeedValueV<ZLayer, 3215>>(zl);
                m_passObjects[pass].push_back(go);
            }
        }
    }

    void onConfirmPreview(CCObject*) {
        for (auto* obj : m_previewObjects) if (obj) obj->setOpacity(255);
        m_lastPlaced = m_previewObjects; m_previewObjects.clear();
        buildActionButtons(false);
    }

    void onRejectPreview(CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (editor) {
            for (auto* obj : m_previewObjects) if (obj) editor->removeObject(obj, true);
        }
        m_previewObjects.clear(); buildActionButtons(false);
    }

    void onUndo(CCObject*) {
        auto* editor = LevelEditorLayer::get();
        if (editor) {
            for (auto* obj : m_lastPlaced) if (obj) editor->removeObject(obj, true);
        }
        m_lastPlaced.clear();
    }

    void onExportChat(CCObject*) {}

public:
    ~AIDecoPopup() { m_listener.getTask().abort(); }
    static AIDecoPopup* create() {
        auto* ret = new AIDecoPopup();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret); return nullptr;
    }
};

// ═══════════════════════════════════════════════════════════════════
//  EDITOR UI HOOK
// ═══════════════════════════════════════════════════════════════════

class $modify(MyEditorUI, EditorUI) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;
        auto wsz = CCDirector::sharedDirector()->getWinSize();

        auto* spr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_starBtnOff_001.png", 0.85f,
            CircleBaseColor::Pink,
            CircleBaseSize::Medium);

        auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MyEditorUI::onOpenAI));
        auto* menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({wsz.width - 48.f, wsz.height - 200.f});
        this->addChild(menu, 100);
        return true;
    }
    void onOpenAI(CCObject*) { AIDecoPopup::create()->show(); }
};
