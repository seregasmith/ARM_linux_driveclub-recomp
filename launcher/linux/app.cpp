// dc-launcher: the Linux launcher of the DriveClub runtime (docs/LAUNCHER_PLAN.md). Made for a handheld in Steam's
// Game Mode: the gamepad comes first (Steam Input presents the controls as one); mouse, touch and keyboard work too.
// It edits driveclub.conf, prepares the game (run.sh --prepare-only) and replaces itself with run.sh --play.
//
//   dc-launcher --run-sh FILE --data DIR --patches-xml FILE --bin DIR [--window | --fullscreen]
// Tests: --script FILE (actions, one per frame: up down left right a b x y l1 r1 l2 r2 start, "text STRING",
// "shot NAME", "wait N"), --shots DIR (where "shot" writes NAME.bmp), --no-exec (print the game command instead of
// starting it), --selftest (draw every page off-screen, exit 0 when nothing failed), --size WxH (the window).
#include "conf.h"
#include "game.h"
#include "model.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// DejaVu Sans (Latin, Cyrillic, the arrows and marks used here), embedded (gpu/third_party/fonts, Bitstream Vera
// license), as the in-game overlay embeds it.
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden dc_font_ttf\n"
    ".global dc_font_ttf\n"
    "dc_font_ttf:\n"
    ".incbin \"" DC_FONT_PATH "\"\n"
    ".hidden dc_font_ttf_end\n"
    ".global dc_font_ttf_end\n"
    "dc_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char dc_font_ttf[];
extern "C" const unsigned char dc_font_ttf_end[];

namespace {

// ------------------------------------------------------------------------------------------
// look (the Windows launcher's colours)
// ------------------------------------------------------------------------------------------
constexpr ImU32 kBg = IM_COL32(16, 18, 22, 255), kCard = IM_COL32(24, 27, 33, 255), kLine = IM_COL32(38, 42, 51, 255),
                kText = IM_COL32(232, 234, 237, 255), kMuted = IM_COL32(139, 147, 161, 255),
                kTrack = IM_COL32(33, 37, 45, 255), kFocus = IM_COL32(40, 45, 56, 255),
                kAccent = IM_COL32(224, 64, 38, 255), kAccentHi = IM_COL32(240, 84, 58, 255),
                kOff = IM_COL32(63, 69, 82, 255), kWhite = IM_COL32(245, 246, 248, 255),
                kOk = IM_COL32(90, 200, 120, 255), kWarn = IM_COL32(240, 170, 60, 255), kBad = IM_COL32(240, 96, 84, 255),
                kShade = IM_COL32(8, 9, 12, 200);

ImU32 Alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(float(c >> 24) * a) << 24); }

ImFont* g_font = nullptr;
ImDrawList* g_dl = nullptr;
float S = 1.f;  // scale: window height / 1080
float W = 1920, H = 1080;
bool g_click = false;  // the left button (or a tap) went up this frame, inside the window
ImVec2 g_mouse;

// Whole pixel sizes and positions: crisper text on small screens.
float Px(float size) { return std::max(8.f, std::round(size * S)); }
ImVec2 TextSize(float size, const std::string& t, float wrap = 0) {
    return g_font->CalcTextSizeA(Px(size), FLT_MAX, wrap, t.c_str());
}
void Text(float size, ImVec2 p, ImU32 c, const std::string& t, float wrap = 0) {
    g_dl->AddText(g_font, Px(size), ImVec2(std::round(p.x), std::round(p.y)), c, t.c_str(), nullptr, wrap);
}
// Heavier text (titles): drawn twice, a pixel apart.
void Bold(float size, ImVec2 p, ImU32 c, const std::string& t) {
    Text(size, p, c, t);
    Text(size, ImVec2(p.x + std::max(1.f, S), p.y), c, t);
}
void TextRight(float size, ImVec2 right_mid, ImU32 c, const std::string& t) {
    const ImVec2 sz = TextSize(size, t);
    Text(size, ImVec2(right_mid.x - sz.x, right_mid.y - sz.y / 2), c, t);
}
void TextCenter(float size, ImVec2 a, ImVec2 b, ImU32 c, const std::string& t, bool bold = false) {
    const ImVec2 sz = TextSize(size, t);
    const ImVec2 p((a.x + b.x - sz.x) / 2, (a.y + b.y - sz.y) / 2);
    bold ? Bold(size, p, c, t) : Text(size, p, c, t);
}
// Shortened to fit `w` pixels: paths lose their middle, other text its end.
std::string Fit(float size, const std::string& t, float w, bool middle) {
    if (TextSize(size, t).x <= w) return t;
    auto cp_start = [&](size_t i) { while (i > 0 && (static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) --i; return i; };
    if (!middle) {
        for (size_t n = t.size(); n > 0; n = cp_start(n - 1)) {
            const std::string s = t.substr(0, n) + "…";
            if (TextSize(size, s).x <= w) return s;
        }
        return "…";
    }
    size_t keep = t.size();
    while (keep > 4) {
        keep -= 2;
        const size_t head = cp_start(keep / 3), tail = cp_start(t.size() - (keep - keep / 3));
        const std::string s = t.substr(0, head) + "…" + t.substr(tail);
        if (TextSize(size, s).x <= w) return s;
    }
    return "…";
}
bool Hover(ImVec2 a, ImVec2 b) { return g_mouse.x >= a.x && g_mouse.x < b.x && g_mouse.y >= a.y && g_mouse.y < b.y; }
bool Clicked(ImVec2 a, ImVec2 b) { return g_click && Hover(a, b); }
void Box(ImVec2 a, ImVec2 b, ImU32 c, float r = 10) { g_dl->AddRectFilled(a, b, c, r * S); }
void Frame(ImVec2 a, ImVec2 b, ImU32 c, float r = 10, float t = 2) { g_dl->AddRect(a, b, c, r * S, 0, t * S); }

// ------------------------------------------------------------------------------------------
// input: gamepad, keyboard, mouse; repeats for held directions
// ------------------------------------------------------------------------------------------
enum Act { kNone, kUp, kDown, kLeft, kRight, kAccept, kBack, kTabPrev, kTabNext, kPageUp, kPageDown, kStart, kX, kY, kActCount };
// The face buttons' letters on this controller (Xbox layout by default; SDL tells others apart).
std::string g_btn_a = "A", g_btn_b = "B", g_btn_x = "X", g_btn_y = "Y";
bool g_have_pad = false;

void Glyph(ImVec2 c, const std::string& label, float size = 34) {
    ImU32 col = kOff;
    if (label == "A") col = IM_COL32(96, 176, 64, 255);
    else if (label == "B") col = IM_COL32(214, 70, 60, 255);
    else if (label == "X") col = IM_COL32(56, 120, 220, 255);
    else if (label == "Y") col = IM_COL32(230, 180, 40, 255);
    const float r = size * S / 2;
    if (label.size() == 1) {
        g_dl->AddCircleFilled(c, r, col);
        TextCenter(size * 0.62f, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), kWhite, label, true);
    } else {
        const float w = TextSize(size * 0.5f, label).x + 18 * S;
        Box(ImVec2(c.x - w / 2, c.y - r * 0.8f), ImVec2(c.x + w / 2, c.y + r * 0.8f), kOff, 6);
        TextCenter(size * 0.5f, ImVec2(c.x - w / 2, c.y - r), ImVec2(c.x + w / 2, c.y + r), kWhite, label, true);
    }
}
float GlyphWidth(const std::string& label, float size = 34) {
    return label.size() == 1 ? size * S : TextSize(size * 0.5f, label).x + 18 * S;
}

// ------------------------------------------------------------------------------------------
// child processes: run.sh --prepare-only and the tools, their output read line by line
// ------------------------------------------------------------------------------------------
class Proc {
public:
    ~Proc() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
        }
        if (fd_ >= 0) close(fd_);
    }
    bool Start(const std::vector<std::string>& argv) {
        int p[2];
        if (pipe(p) != 0) return false;
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            dup2(p[1], 1);
            dup2(p[1], 2);
            close(p[0]);
            close(p[1]);
            std::vector<char*> args;
            for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
            args.push_back(nullptr);
            execvp(args[0], args.data());
            std::fprintf(stderr, "cannot start %s: %s\n", args[0], std::strerror(errno));
            _exit(127);
        }
        close(p[1]);
        fd_ = p[0];
        fcntl(fd_, F_SETFL, O_NONBLOCK);
        return true;
    }
    // Reads what arrived; false once the program has ended and everything was read.
    bool Poll() {
        if (fd_ >= 0) {
            char buf[4096];
            for (;;) {
                const ssize_t n = read(fd_, buf, sizeof(buf));
                if (n > 0) {
                    partial_.append(buf, size_t(n));
                    size_t nl;
                    while ((nl = partial_.find('\n')) != std::string::npos) {
                        lines.push_back(partial_.substr(0, nl));
                        partial_.erase(0, nl + 1);
                    }
                    continue;
                }
                if (n == 0) {
                    if (!partial_.empty()) lines.push_back(partial_);
                    partial_.clear();
                    close(fd_);
                    fd_ = -1;
                }
                break;
            }
        }
        if (fd_ < 0 && pid_ > 0) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                pid_ = -1;
            }
        }
        return pid_ > 0 || fd_ >= 0;
    }
    std::vector<std::string> lines;
    int exit_code = -1;

private:
    pid_t pid_ = -1;
    int fd_ = -1;
    std::string partial_;
};

// ------------------------------------------------------------------------------------------
// the app and its layers (the main view, lists, keyboard, folders, progress, text, questions)
// ------------------------------------------------------------------------------------------
struct App;
struct Layer {
    virtual ~Layer() = default;
    virtual void Act(Act a) = 0;
    virtual void Draw(bool top) = 0;
    virtual void TextInput(const std::string&) {}
    // Help bar: the text above the hints, its colour, and the hints themselves.
    virtual std::string Help(ImU32& color) { color = kMuted; return ""; }
    virtual std::vector<std::pair<std::string, std::string>> Hints() = 0; // glyph, label
    virtual bool WantsText() const { return false; }
    bool closed = false;
};

struct App {
    fs::path run_sh, data, conf_path, patch_xml, bin;
    Conf conf;
    Model model;
    int page = kTabPlay;
    int focus[kPageCount] = {};
    float scroll[kPageCount] = {};
    std::vector<std::unique_ptr<Layer>> layers;
    bool quit = false, no_exec = false, dirty = false, exec_game = false;
    std::string toast;
    double toast_until = 0;
    std::vector<FoundDump> found; // the last search's result
    bool searched = false;
    FolderCheck folders;

    void Save() {
        model.Store(conf);
        const std::string problem = conf.Save(conf_path);
        if (!problem.empty()) Toast("Settings not saved: " + problem);
        dirty = false;
    }
    void Toast(const std::string& text, double seconds = 4) {
        toast = text;
        toast_until = double(SDL_GetTicks()) / 1000.0 + seconds;
    }
    void CheckFolders() {
        const int u = model.Find("update"), b = model.Find("base");
        folders = ::CheckFolders(model.rows[u].text, model.rows[b].text);
    }
    // Lossless Scaling frame generation: run.sh --lsfg-status finds lsfg-vk and Lossless.dll as the game's start does.
    // Its first line says what is missing (or "Ready: ..."); the rest is what it found and where it looked.
    std::vector<std::string> lsfg_lines;
    bool lsfg_ready = false;
    void CheckLsfg();
    void Push(Layer* layer) { layers.emplace_back(layer); }
    void Command(const std::string& id);
    void Play();
    void OpenFolders(int target, bool search);
};

// ---- the main view: tabs and their rows ------------------------------------------------------
struct MainLayer : Layer {
    App& app;
    explicit MainLayer(App& a) : app(a) {}
    float row_top_cache = 0;

    std::vector<int> Rows() const { return app.model.Page(app.page); }
    bool Focusable(int row) const { return app.model.rows[row].kind != Kind::Info; }
    int Focused() const {
        const auto rows = Rows();
        if (rows.empty()) return -1;
        return rows[std::clamp(app.focus[app.page], 0, int(rows.size()) - 1)];
    }
    void Move(int dir, int count = 1) {
        const auto rows = Rows();
        int i = app.focus[app.page];
        for (int n = 0; n < count; ++n) {
            int j = i + dir;
            while (j >= 0 && j < int(rows.size()) && !Focusable(rows[j])) j += dir;
            if (j < 0 || j >= int(rows.size())) break;
            i = j;
        }
        app.focus[app.page] = i;
    }
    void SetPage(int page) {
        app.page = page;
        const auto rows = Rows();
        if (!rows.empty() && !Focusable(rows[std::clamp(app.focus[page], 0, int(rows.size()) - 1)])) Move(1);
    }
    void Act(::Act a) override;
    void Draw(bool top) override;
    std::string Help(ImU32& color) override;
    std::vector<std::pair<std::string, std::string>> Hints() override;
    void Activate(int row);
};

// ---- a list to choose from (long choices: language, output resolution) ------------------------
struct ListLayer : Layer {
    std::string title;
    std::vector<std::string> items;
    int focus = 0, chosen = 0;
    float scroll = 0;
    std::function<void(int)> done;
    ListLayer(std::string t, std::vector<std::string> i, int sel, std::function<void(int)> d)
        : title(std::move(t)), items(std::move(i)), focus(sel), chosen(sel), done(std::move(d)) {}
    void Act(::Act a) override {
        const int n = int(items.size());
        if (a == kUp) focus = std::max(0, focus - 1);
        else if (a == kDown) focus = std::min(n - 1, focus + 1);
        else if (a == kPageUp) focus = std::max(0, focus - 8);
        else if (a == kPageDown) focus = std::min(n - 1, focus + 8);
        else if (a == kAccept) { closed = true; done(focus); }
        else if (a == kBack) closed = true;
    }
    void Draw(bool top) override {
        Box(ImVec2(0, 0), ImVec2(W, H), kShade, 0);
        const float item_h = 64 * S, pw = std::min(W - 80 * S, 900 * S);
        const float ph = std::min(H - 240 * S, item_h * float(items.size()) + 120 * S);
        const ImVec2 a((W - pw) / 2, (H - 170 * S - ph) / 2 + 20 * S), b(a.x + pw, a.y + ph);
        Box(a, b, kCard, 14);
        Frame(a, b, kLine, 14);
        Bold(30, ImVec2(a.x + 32 * S, a.y + 26 * S), kWhite, title);
        const ImVec2 la(a.x + 16 * S, a.y + 90 * S), lb(b.x - 16 * S, b.y - 16 * S);
        const float view = lb.y - la.y, want = float(focus) * item_h;
        if (want < scroll) scroll = want;
        if (want + item_h > scroll + view) scroll = want + item_h - view;
        g_dl->PushClipRect(la, lb, true);
        for (int i = 0; i < int(items.size()); ++i) {
            const ImVec2 ia(la.x, la.y + float(i) * item_h - scroll), ib(lb.x, ia.y + item_h - 6 * S);
            if (ib.y < la.y || ia.y > lb.y) continue;
            if (i == focus) {
                Box(ia, ib, kFocus, 8);
                Box(ia, ImVec2(ia.x + 6 * S, ib.y), kAccent, 3);
            }
            Text(28, ImVec2(ia.x + 28 * S, ia.y + (item_h - 6 * S - TextSize(28, "Ag").y) / 2), i == focus ? kWhite : kText,
                 items[size_t(i)]);
            if (i == chosen) TextRight(28, ImVec2(ib.x - 24 * S, (ia.y + ib.y) / 2), kAccent, "✓");
            if (top && Clicked(ia, ib) && g_mouse.y >= la.y && g_mouse.y < lb.y) {
                focus = i;
                Act(kAccept);
            }
        }
        g_dl->PopClipRect();
        if (top && g_click && !Hover(a, b)) closed = true;
    }
    std::vector<std::pair<std::string, std::string>> Hints() override { return {{g_btn_a, "Choose"}, {g_btn_b, "Back"}}; }
};

// ---- a question with two answers -----------------------------------------------------------------
struct ConfirmLayer : Layer {
    std::string title, text, yes, no;
    std::function<void()> on_yes;
    int focus = 1; // No first: a stray press changes nothing
    ConfirmLayer(std::string t, std::string x, std::string y, std::string n, std::function<void()> f)
        : title(std::move(t)), text(std::move(x)), yes(std::move(y)), no(std::move(n)), on_yes(std::move(f)) {}
    void Act(::Act a) override {
        if (a == kLeft) focus = 0;
        else if (a == kRight) focus = 1;
        else if (a == kAccept) { closed = true; if (focus == 0) on_yes(); }
        else if (a == kBack) closed = true;
    }
    void Draw(bool top) override {
        Box(ImVec2(0, 0), ImVec2(W, H), kShade, 0);
        const float pw = std::min(W - 80 * S, 860 * S);
        const float th = TextSize(26, text, pw - 64 * S).y;
        const float ph = 230 * S + th;
        const ImVec2 a((W - pw) / 2, (H - 170 * S - ph) / 2), b(a.x + pw, a.y + ph);
        Box(a, b, kCard, 14);
        Frame(a, b, kLine, 14);
        Bold(32, ImVec2(a.x + 32 * S, a.y + 28 * S), kWhite, title);
        Text(26, ImVec2(a.x + 32 * S, a.y + 84 * S), kText, text, pw - 64 * S);
        const float bw = (pw - 96 * S) / 2, by = b.y - 100 * S;
        for (int i = 0; i < 2; ++i) {
            const ImVec2 ba(a.x + 32 * S + float(i) * (bw + 32 * S), by), bb(ba.x + bw, by + 68 * S);
            Box(ba, bb, focus == i ? (i == 0 ? kAccent : kFocus) : kTrack, 10);
            if (focus == i) Frame(ba, bb, i == 0 ? kAccentHi : kMuted, 10);
            TextCenter(28, ba, bb, kWhite, i == 0 ? yes : no, focus == i);
            if (top && Clicked(ba, bb)) { focus = i; Act(kAccept); }
        }
    }
    std::vector<std::pair<std::string, std::string>> Hints() override {
        return {{"◀▶", "Choose"}, {g_btn_a, "OK"}, {g_btn_b, "Cancel"}};
    }
};

// ---- the on-screen keyboard -------------------------------------------------------------------------
struct KeyboardLayer : Layer {
    std::string title, text;
    size_t max_len;
    bool name_only; // environment variable names: A-Z, 0-9, _
    std::function<void(const std::string&)> done;
    int layout = 0; // 0 Latin, 1 Cyrillic
    bool shift = false;
    int r = 1, c = 0;
    static constexpr int kRows = 5;
    KeyboardLayer(std::string t, std::string initial, size_t max, bool names, std::function<void(const std::string&)> d)
        : title(std::move(t)), text(std::move(initial)), max_len(max), name_only(names), done(std::move(d)) {
        if (name_only) shift = true;
    }
    std::vector<std::string> Keys(int row) const {
        static const char* const lat[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@"};
        static const char* const cyr[4] = {"1234567890", "йцукенгшщзх", "фывапролджэ", "ячсмитьбюъё"};
        std::vector<std::string> out;
        if (row == 4) return {"⇧ Shift", "Space", "⌫", layout ? "ABC" : "АБВ", "Done"};
        const char* s = (layout && !name_only ? cyr : lat)[row];
        for (size_t i = 0; s[i];) {
            size_t n = 1;
            while ((static_cast<unsigned char>(s[i + n]) & 0xC0) == 0x80) ++n;
            std::string k(s + i, n);
            if (shift) k = Upper(k);
            out.push_back(k);
            i += n;
        }
        return out;
    }
    static std::string Upper(const std::string& k) {
        if (k.size() == 1) return std::string(1, char(std::toupper(static_cast<unsigned char>(k[0]))));
        // Cyrillic: а-п (D0 B0..BF) -> А-П (D0 90..9F); р-я (D1 80..8F) -> Р-Я (D0 A0..AF); ё (D1 91) -> Ё (D0 81)
        const unsigned char a = static_cast<unsigned char>(k[0]), b = static_cast<unsigned char>(k[1]);
        if (a == 0xD0 && b >= 0xB0) return std::string{char(0xD0), char(b - 0x20)};
        if (a == 0xD1 && b < 0x90) return std::string{char(0xD0), char(b + 0x20)};
        if (a == 0xD1 && b == 0x91) return std::string{char(0xD0), char(0x81)};
        return k;
    }
    size_t Length() const {
        size_t n = 0;
        for (unsigned char ch : text) n += (ch & 0xC0) != 0x80;
        return n;
    }
    void Type(const std::string& k) {
        for (size_t i = 0; i < k.size();) {
            size_t n = 1;
            while (i + n < k.size() && (static_cast<unsigned char>(k[i + n]) & 0xC0) == 0x80) ++n;
            std::string ch = k.substr(i, n);
            i += n;
            if (name_only) {
                if (ch.size() != 1) continue;
                const char u = char(std::toupper(static_cast<unsigned char>(ch[0])));
                if (!std::isalnum(static_cast<unsigned char>(u)) && u != '_') continue;
                ch = std::string(1, u);
            } else if (static_cast<unsigned char>(ch[0]) < 0x20) {
                continue;
            }
            if (Length() < max_len) text += ch;
        }
    }
    void Erase() {
        if (text.empty()) return;
        size_t i = text.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
        text.erase(i);
    }
    void Press() {
        const auto keys = Keys(r);
        const std::string& k = keys[size_t(std::min(c, int(keys.size()) - 1))];
        if (r < 4) {
            Type(k);
            if (shift && !name_only) shift = false;
            return;
        }
        switch (c) {
        case 0: shift = !shift; break;
        case 1: Type(" "); break;
        case 2: Erase(); break;
        case 3: if (!name_only) layout ^= 1; break;
        default: closed = true; done(text); break;
        }
    }
    void Act(::Act a) override {
        auto width = [&](int row) { return int(Keys(row).size()); };
        if (a == kUp || a == kDown) {
            const int from = width(r);
            const float pos = (float(c) + 0.5f) / float(from);
            r = std::clamp(r + (a == kDown ? 1 : -1), 0, kRows - 1);
            c = std::clamp(int(pos * float(width(r))), 0, width(r) - 1);
        } else if (a == kLeft) c = (c + width(r) - 1) % width(r);
        else if (a == kRight) c = (c + 1) % width(r);
        else if (a == kAccept) Press();
        else if (a == kX) Erase();
        else if (a == kY) { if (!name_only) shift = !shift; }
        else if (a == kTabPrev || a == kTabNext) { if (!name_only) layout ^= 1; }
        else if (a == kStart) { closed = true; done(text); }
        else if (a == kBack) closed = true;
    }
    void TextInput(const std::string& t) override { Type(t); }
    bool WantsText() const override { return true; }
    void Draw(bool top) override {
        Box(ImVec2(0, 0), ImVec2(W, H), kShade, 0);
        const float pw = std::min(W - 60 * S, 1300 * S), key_h = 76 * S, gap = 10 * S;
        const float ph = 200 * S + float(kRows) * (key_h + gap);
        const ImVec2 a((W - pw) / 2, (H - 170 * S - ph) / 2), b(a.x + pw, a.y + ph);
        Box(a, b, kCard, 14);
        Frame(a, b, kLine, 14);
        Bold(28, ImVec2(a.x + 32 * S, a.y + 22 * S), kMuted, title);
        const ImVec2 fa(a.x + 32 * S, a.y + 70 * S), fb(b.x - 32 * S, a.y + 140 * S);
        Box(fa, fb, kTrack, 8);
        const std::string shown = Fit(34, text, fb.x - fa.x - 60 * S, false);
        Text(34, ImVec2(fa.x + 20 * S, fa.y + (fb.y - fa.y - TextSize(34, "Ag").y) / 2), kWhite, shown);
        const float cx = fa.x + 20 * S + TextSize(34, shown).x + 2 * S;
        if ((SDL_GetTicks() / 500) % 2 == 0) Box(ImVec2(cx, fa.y + 14 * S), ImVec2(cx + 3 * S, fb.y - 14 * S), kAccent, 1);
        float y = fb.y + 24 * S;
        for (int row = 0; row < kRows; ++row) {
            const auto keys = Keys(row);
            const float kw = (pw - 64 * S - gap * float(keys.size() - 1)) / float(keys.size());
            for (int k = 0; k < int(keys.size()); ++k) {
                const ImVec2 ka(a.x + 32 * S + float(k) * (kw + gap), y), kb(ka.x + kw, y + key_h);
                const bool f = row == r && k == c;
                const bool lit = row == 4 && ((k == 0 && shift) || k == 4);
                Box(ka, kb, f ? kAccent : lit ? kOff : kTrack, 8);
                TextCenter(row == 4 ? 26 : 32, ka, kb, kWhite, keys[size_t(k)], f);
                if (top && Clicked(ka, kb)) { r = row; c = k; Press(); }
            }
            y += key_h + gap;
        }
    }
    std::vector<std::pair<std::string, std::string>> Hints() override {
        if (name_only) return {{g_btn_a, "Type"}, {g_btn_x, "Delete"}, {"START", "Done"}, {g_btn_b, "Cancel"}};
        return {{g_btn_a, "Type"}, {g_btn_x, "Delete"}, {g_btn_y, "Shift"}, {"L1/R1", "АБВ/ABC"}, {"START", "Done"}, {g_btn_b, "Cancel"}};
    }
};

// ---- text: a tool's output or a log -------------------------------------------------------------------
struct TextLayer : Layer {
    std::string title;
    std::vector<std::string> lines;
    std::unique_ptr<Proc> proc;
    float scroll = 0;
    bool follow = true; // keep the end in view while a program writes
    TextLayer(std::string t, std::vector<std::string> l) : title(std::move(t)), lines(std::move(l)) {}
    // A program's output as it runs.
    static TextLayer* Running(std::string t, const std::vector<std::string>& argv) {
        auto* layer = new TextLayer(std::move(t), std::vector<std::string>{});
        layer->proc = std::make_unique<Proc>();
        if (!layer->proc->Start(argv)) layer->lines.push_back("Could not start " + argv[0] + ".");
        return layer;
    }
    void Act(::Act a) override {
        const float line = 34 * S;
        if (a == kUp) { scroll -= line * 3; follow = false; }
        else if (a == kDown) scroll += line * 3;
        else if (a == kPageUp) { scroll -= line * 15; follow = false; }
        else if (a == kPageDown) scroll += line * 15;
        else if (a == kBack || a == kAccept) closed = true;
    }
    void Draw(bool) override {
        if (proc) {
            proc->Poll();
            lines = proc->lines;
            if (proc->exit_code >= 0 && (lines.empty() || lines.back().rfind("(exit", 0) != 0)) {
                lines.push_back("");
                lines.push_back("(exit code " + std::to_string(proc->exit_code) + ")");
                proc->lines = lines;
            }
        }
        Box(ImVec2(0, 0), ImVec2(W, H), kBg, 0);
        Bold(36, ImVec2(64 * S, 40 * S), kWhite, title);
        if (proc && proc->exit_code < 0) Text(26, ImVec2(64 * S, 92 * S), kWarn, "Running…");
        const ImVec2 a(64 * S, 140 * S), b(W - 64 * S, H - 180 * S);
        Box(a, b, kCard, 10);
        const float line = 34 * S, view = b.y - a.y - 32 * S;
        const float total = line * float(lines.size());
        const float max_scroll = std::max(0.f, total - view);
        if (follow && proc) scroll = max_scroll;
        scroll = std::clamp(scroll + (g_mouse.y > a.y ? -ImGui::GetIO().MouseWheel * line * 3 : 0), 0.f, max_scroll);
        if (scroll >= max_scroll) follow = true;
        g_dl->PushClipRect(ImVec2(a.x + 16 * S, a.y + 16 * S), ImVec2(b.x - 16 * S, b.y - 16 * S), true);
        for (size_t i = 0; i < lines.size(); ++i) {
            const float y = a.y + 16 * S + float(i) * line - scroll;
            if (y + line < a.y || y > b.y) continue;
            const std::string& l = lines[i];
            ImU32 c = kText;
            if (l.find("FAIL") != std::string::npos || l.find("rror") != std::string::npos) c = kBad;
            else if (l.find(" ok") != std::string::npos || l.find("passed") != std::string::npos) c = kOk;
            Text(24, ImVec2(a.x + 24 * S, y), c, Fit(24, l, b.x - a.x - 48 * S, false));
        }
        g_dl->PopClipRect();
    }
    std::vector<std::pair<std::string, std::string>> Hints() override {
        return {{"▲▼", "Scroll"}, {"L2/R2", "Page"}, {g_btn_b, "Back"}};
    }
};

// ---- preparing the game, then starting it ----------------------------------------------------------------
struct ProgressLayer : Layer {
    App& app;
    Proc proc;
    bool failed = false;
    float frac = 0;
    std::string step;
    explicit ProgressLayer(App& a) : app(a) {
        proc.Start({"bash", app.run_sh.string(), "--prepare-only"});
    }
    void Act(::Act a) override {
        if (failed && (a == kBack || a == kAccept)) closed = true;
    }
    void Draw(bool) override {
        if (!failed) {
            if (!proc.Poll()) {
                if (proc.exit_code == 0) {
                    app.exec_game = true;
                } else {
                    failed = true;
                }
            }
            for (const auto& l : proc.lines) {
                // dc-prepare: "[n/6] what"
                if (l.size() > 5 && l[0] == '[' && l[2] == '/' && l[4] == ']') {
                    frac = float(l[1] - '0') / float(l[3] - '0');
                    step = l.substr(6);
                }
            }
        }
        Box(ImVec2(0, 0), ImVec2(W, H), kBg, 0);
        const float cw = std::min(W - 128 * S, 1300 * S), x = (W - cw) / 2;
        if (!failed) {
            Bold(40, ImVec2(x, H * 0.3f), kWhite, app.exec_game ? "Starting DriveClub…" : "Preparing DriveClub…");
            Text(26, ImVec2(x, H * 0.3f + 64 * S), kMuted,
                 step.empty() ? "Applying your settings." : step + "  (the first start converts the game: up to a minute)");
            const ImVec2 a(x, H * 0.3f + 120 * S), b(x + cw, a.y + 14 * S);
            Box(a, b, kTrack, 7);
            const float t = float(SDL_GetTicks() % 1500) / 1500.f;
            if (frac > 0) Box(a, ImVec2(a.x + cw * frac, b.y), kAccent, 7);
            else Box(ImVec2(a.x + cw * t * 0.8f, a.y), ImVec2(a.x + cw * (t * 0.8f + 0.2f), b.y), kAccent, 7);
        } else {
            Bold(40, ImVec2(x, 90 * S), kBad, "The game could not be prepared");
            Text(26, ImVec2(x, 150 * S), kMuted, "What run.sh reported (the whole output is below; check the game folders first):");
            const ImVec2 a(x, 200 * S), b(x + cw, H - 190 * S);
            Box(a, b, kCard, 10);
            const float line = 32 * S;
            const int fit = int((b.y - a.y - 32 * S) / line);
            const int first = std::max(0, int(proc.lines.size()) - fit);
            for (int i = first; i < int(proc.lines.size()); ++i) {
                const std::string& l = proc.lines[size_t(i)];
                Text(23, ImVec2(a.x + 24 * S, a.y + 16 * S + float(i - first) * line),
                     l.find("DriveClub:") == 0 || l.find("rror") != std::string::npos ? kBad : kText,
                     Fit(23, l, cw - 48 * S, false));
            }
        }
    }
    std::vector<std::pair<std::string, std::string>> Hints() override {
        if (failed) return {{g_btn_b, "Back"}};
        return {};
    }
};

// ---- the game folders: the search and a folder browser -------------------------------------------------
struct FolderLayer : Layer {
    App& app;
    int target; // browse: 0 the update, 1 the base game
    bool browsing;
    // search
    std::thread worker;
    std::atomic<bool> stop{false}, finished{false};
    std::atomic<int> visited{0};
    std::vector<FoundDump> result;
    // browse
    fs::path dir;
    std::vector<fs::path> history; // B goes back through these
    struct Entry { std::string label, path, mark; int kind; bool use; };
    std::vector<Entry> entries;
    int focus = 0;
    float scroll = 0;
    std::string message;

    FolderLayer(App& a, int t, bool search) : app(a), target(t), browsing(!search) {
        if (search) StartSearch();
        else {
            const std::string& cur = app.model.rows[app.model.Find(t == 0 ? "update" : "base")].text;
            std::error_code ec;
            fs::path start = cur.empty() ? fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/") : fs::path(cur).parent_path();
            if (!fs::is_directory(start, ec)) start = std::getenv("HOME") ? std::getenv("HOME") : "/";
            Open(start);
        }
    }
    ~FolderLayer() override {
        stop = true;
        if (worker.joinable()) worker.join();
    }
    void StartSearch() {
        browsing = false;
        finished = false;
        visited = 0;
        focus = 0;
        worker = std::thread([this] {
            result = FindDumps(stop, visited);
            finished = true;
        });
    }
    void Assign(int which, const std::string& path) {
        app.model.rows[app.model.Find(which == 0 ? "update" : "base")].text = path;
        app.CheckFolders();
        app.dirty = true;
    }
    void SearchDone() {
        worker.join();
        app.found = result;
        app.searched = true;
        std::string u, b;
        if (PickDumps(result, u, b)) {
            Assign(0, u);
            Assign(1, b);
        }
        BuildResults();
    }
    // Search results as a list: the dumps found, then browsing and Done.
    void BuildResults() {
        entries.clear();
        for (const auto& f : app.found) {
            const char* what = f.kind == FoundDump::Merged ? "1.28 (one folder)" : f.kind == FoundDump::Update ? "Update 1.28" : "Base game";
            entries.push_back({f.path, f.path, std::string(what) + " · " + (f.title_id == "CUSA00003" ? "EU" : "US"),
                               f.kind == FoundDump::Merged ? 3 : f.kind == FoundDump::Update ? 1 : 2, true});
        }
        entries.push_back({"Browse for the update folder…", "@browse0", "", 0, false});
        entries.push_back({"Browse for the base game folder…", "@browse1", "", 0, false});
        entries.push_back({"Search again", "@search", "", 0, false});
        entries.push_back({app.folders.update_ok && app.folders.base_ok ? "Use these folders" : "Done", "@done", "", 0, false});
        focus = app.folders.update_ok && app.folders.base_ok ? int(entries.size()) - 1 : 0;
        scroll = 0;
    }
    void Open(const fs::path& d) {
        browsing = true;
        dir = d;
        entries.clear();
        message.clear();
        const int here = DumpKind(dir);
        if (here) entries.push_back({"Use this folder", dir.string(), here == 3 ? "update + base" : here == 2 ? "base game" : "update 1.28", here, true});
        if (dir.has_parent_path() && dir != dir.root_path()) entries.push_back({"▲  Up", dir.parent_path().string(), "", 0, false});
        if (const char* home = std::getenv("HOME")) entries.push_back({"⌂  Home", home, "", 0, false});
        std::error_code ec;
        for (const char* media : {"/run/media", "/media", "/mnt"}) {
            if (fs::is_directory(media, ec) && !fs::is_empty(media, ec)) entries.push_back({std::string("▣  ") + media + "  (SD card, USB)", media, "", 0, false});
        }
        std::vector<Entry> subs;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.' || !it->is_directory(e2)) continue;
            const int k = DumpKind(it->path());
            subs.push_back({name, it->path().string(), k == 3 ? "update + base" : k == 2 ? "base game" : k == 1 ? "update 1.28" : "", k, false});
        }
        std::sort(subs.begin(), subs.end(), [](const Entry& x, const Entry& y) {
            if ((x.kind != 0) != (y.kind != 0)) return x.kind != 0;
            return x.label < y.label;
        });
        entries.insert(entries.end(), subs.begin(), subs.end());
        focus = 0;
        scroll = 0;
    }
    void Use(const Entry& e) {
        if (browsing) {
            Assign(target, e.path);
            if (e.kind == 3) Assign(1 - target, e.path); // one folder serves as both
            const std::string& problem = target == 0 ? app.folders.update_problem : app.folders.base_problem;
            if (!problem.empty()) { message = problem; return; }
            if (app.searched) BuildResults(), browsing = false;
            else closed = true;
            return;
        }
        if (e.kind == 3) { Assign(0, e.path); Assign(1, e.path); }
        else Assign(e.kind == 1 ? 0 : 1, e.path);
        BuildResults();
    }
    void Accept() {
        if (entries.empty()) return;
        const Entry e = entries[size_t(focus)];
        if (!browsing) {
            if (e.path == "@done") closed = true;
            else if (e.path == "@search") StartSearch();
            else if (e.path == "@browse0" || e.path == "@browse1") {
                target = e.path.back() - '0';
                history.clear();
                const std::string& cur = app.model.rows[app.model.Find(target == 0 ? "update" : "base")].text;
                std::error_code ec;
                fs::path start = !cur.empty() && fs::is_directory(fs::path(cur).parent_path(), ec) ? fs::path(cur).parent_path()
                                                                                             : fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/");
                Open(start);
            } else Use(e);
            return;
        }
        if (e.use || e.kind != 0) Use(e);
        else {
            history.push_back(dir);
            Open(e.path);
        }
    }
    void Act(::Act a) override {
        if (!browsing && !finished) {
            if (a == kBack) closed = true;
            return;
        }
        const int n = int(entries.size());
        if (a == kUp) focus = std::max(0, focus - 1);
        else if (a == kDown) focus = std::min(n - 1, focus + 1);
        else if (a == kPageUp) focus = std::max(0, focus - 8);
        else if (a == kPageDown) focus = std::min(n - 1, focus + 8);
        else if (a == kAccept) Accept();
        else if (a == kY && browsing && n) { Entry e = entries[size_t(focus)]; if (e.label.rfind("▲", 0) != 0 && e.label.rfind("⌂", 0) != 0 && e.label.rfind("▣", 0) != 0) Use(e); }
        else if (a == kBack) {
            if (browsing && !history.empty()) {
                const fs::path back = history.back();
                history.pop_back();
                Open(back);
            } else if (browsing && app.searched) {
                browsing = false;
                BuildResults();
            } else closed = true;
        }
    }
    void Draw(bool top) override {
        if (!browsing && !finished && worker.joinable()) {
            // still searching
        } else if (!browsing && finished && worker.joinable()) {
            SearchDone();
        }
        Box(ImVec2(0, 0), ImVec2(W, H), kBg, 0);
        const float cw = std::min(W - 128 * S, 1500 * S), x = (W - cw) / 2;
        Bold(38, ImVec2(x, 36 * S), kWhite, browsing ? (target == 0 ? "Choose the update folder (version 1.28)" : "Choose the base game folder") : "Game folders");
        // The two folders as chosen now.
        float y = 100 * S;
        for (int i = 0; i < 2; ++i) {
            const std::string& p = app.model.rows[app.model.Find(i == 0 ? "update" : "base")].text;
            const bool ok = i == 0 ? app.folders.update_ok : app.folders.base_ok;
            Text(24, ImVec2(x, y), kMuted, i == 0 ? "Update 1.28" : "Base game");
            Text(24, ImVec2(x + 190 * S, y), ok ? kOk : p.empty() ? kMuted : kBad, ok ? "✓" : p.empty() ? "–" : "✗");
            Text(24, ImVec2(x + 224 * S, y), p.empty() ? kMuted : kText, p.empty() ? "not chosen" : Fit(24, p, cw - 240 * S, true));
            y += 36 * S;
        }
        y += 14 * S;
        if (browsing) {
            Text(24, ImVec2(x, y), kAccent, Fit(24, dir.string(), cw, true));
            y += 44 * S;
        }
        if (!browsing && !finished) {
            Bold(30, ImVec2(x, y + 40 * S), kText, "Searching for DriveClub…");
            Text(24, ImVec2(x, y + 90 * S), kMuted,
                 "Home, the SD card and USB drives: " + std::to_string(visited.load()) + " folders looked at.");
            return;
        }
        if (!browsing && app.found.empty()) {
            Text(24, ImVec2(x, y), kWarn, "No DriveClub dumps were found. Browse for the folders, or copy them to the SD card and search again.");
            y += 44 * S;
        } else if (!browsing) {
            Text(24, ImVec2(x, y), kMuted, "Found on this device: choose one for each field, or Use these folders when both are ✓.");
            y += 44 * S;
        }
        if (!message.empty()) {
            Text(24, ImVec2(x, y), kBad, message, cw);
            y += TextSize(24, message, cw).y + 14 * S;
        }
        const ImVec2 la(x, y), lb(x + cw, H - 180 * S);
        const float item_h = 66 * S, view = lb.y - la.y, want = float(focus) * item_h;
        if (want < scroll) scroll = want;
        if (want + item_h > scroll + view) scroll = want + item_h - view;
        scroll = std::clamp(scroll - (Hover(la, lb) ? ImGui::GetIO().MouseWheel * item_h * 2 : 0), 0.f,
                            std::max(0.f, float(entries.size()) * item_h - view));
        g_dl->PushClipRect(la, lb, true);
        for (int i = 0; i < int(entries.size()); ++i) {
            const Entry& e = entries[size_t(i)];
            const ImVec2 ia(la.x, la.y + float(i) * item_h - scroll), ib(lb.x, ia.y + item_h - 6 * S);
            if (ib.y < la.y || ia.y > lb.y) continue;
            Box(ia, ib, i == focus ? kFocus : kCard, 8);
            if (i == focus) Box(ia, ImVec2(ia.x + 6 * S, ib.y), kAccent, 3);
            const float ty = ia.y + (item_h - 6 * S - TextSize(27, "Ag").y) / 2;
            const float mark_w = e.mark.empty() ? 0 : TextSize(23, e.mark).x + 40 * S;
            Text(27, ImVec2(ia.x + 28 * S, ty), e.path.rfind("@", 0) == 0 ? kAccentHi : kText,
                 Fit(27, e.label, ib.x - ia.x - 56 * S - mark_w, browsing ? false : true));
            if (!e.mark.empty()) {
                const ImVec2 sz = TextSize(23, e.mark);
                const ImVec2 ma(ib.x - sz.x - 40 * S, (ia.y + ib.y - sz.y) / 2 - 6 * S), mb(ib.x - 16 * S, ma.y + sz.y + 12 * S);
                Box(ma, mb, IM_COL32(40, 80, 55, 255), 6);
                Text(23, ImVec2(ma.x + 12 * S, ma.y + 6 * S), kOk, e.mark);
            }
            if (top && Clicked(ia, ib) && g_mouse.y >= la.y && g_mouse.y < lb.y) { focus = i; Accept(); }
        }
        g_dl->PopClipRect();
    }
    std::string Help(ImU32& color) override {
        color = kMuted;
        if (!browsing) return "";
        return "Open folders with A. Folders marked green hold DriveClub: choose one with A (or Y on any folder).";
    }
    std::vector<std::pair<std::string, std::string>> Hints() override {
        if (!browsing && !finished) return {{g_btn_b, "Cancel"}};
        if (browsing) return {{g_btn_a, "Open / choose"}, {g_btn_y, "Use this one"}, {"L2/R2", "Page"}, {g_btn_b, "Back"}};
        return {{g_btn_a, "Choose"}, {g_btn_b, "Close"}};
    }
};

// ---- environment variables (the env. lines) ----------------------------------------------------------------
struct EnvLayer : Layer {
    App& app;
    int focus = 0;
    float scroll = 0;
    explicit EnvLayer(App& a) : app(a) {}
    // The lines the FEX rows do not handle.
    std::vector<std::pair<std::string, std::string>> Vars() const {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [k, v] : app.conf.Items()) {
            if (k.rfind("env.", 0) == 0 && app.model.Find(k) < 0) out.push_back({k.substr(4), v});
        }
        return out;
    }
    void Act(::Act a) override {
        const auto vars = Vars();
        const int n = int(vars.size()) + 1;
        focus = std::clamp(focus, 0, n - 1);
        if (a == kUp) focus = std::max(0, focus - 1);
        else if (a == kDown) focus = std::min(n - 1, focus + 1);
        else if (a == kBack) closed = true;
        else if (a == kY && focus < int(vars.size())) {
            app.conf.Erase("env." + vars[size_t(focus)].first);
            app.dirty = true;
        } else if (a == kAccept) {
            if (focus < int(vars.size())) {
                const std::string key = vars[size_t(focus)].first;
                app.Push(new KeyboardLayer(key + " =", vars[size_t(focus)].second, 200, false, [this, key](const std::string& v) {
                    app.conf.Set("env." + key, v);
                    app.dirty = true;
                }));
            } else {
                app.Push(new KeyboardLayer("New variable: its name (for example BB_BENCHMARK)", "", 64, true,
                                           [this](const std::string& name) {
                                               if (name.empty()) return;
                                               app.Push(new KeyboardLayer(name + " =", "", 200, false, [this, name](const std::string& v) {
                                                   app.conf.Set("env." + name, v);
                                                   app.dirty = true;
                                               }));
                                           }));
            }
        }
    }
    void Draw(bool top) override {
        Box(ImVec2(0, 0), ImVec2(W, H), kBg, 0);
        const float cw = std::min(W - 128 * S, 1500 * S), x = (W - cw) / 2;
        Bold(38, ImVec2(x, 36 * S), kWhite, "Environment variables");
        Text(24, ImVec2(x, 96 * S), kMuted, "Set for the runtime and FEX at every start (driveclub.conf's env. lines). Leave them alone unless you know what they do.", cw);
        const auto vars = Vars();
        const ImVec2 la(x, 170 * S), lb(x + cw, H - 180 * S);
        const float item_h = 70 * S;
        for (int i = 0; i <= int(vars.size()); ++i) {
            const ImVec2 ia(la.x, la.y + float(i) * item_h - scroll), ib(lb.x, ia.y + item_h - 6 * S);
            Box(ia, ib, i == focus ? kFocus : kCard, 8);
            if (i == focus) Box(ia, ImVec2(ia.x + 6 * S, ib.y), kAccent, 3);
            const float ty = ia.y + (item_h - 6 * S - TextSize(27, "Ag").y) / 2;
            if (i < int(vars.size())) {
                Text(27, ImVec2(ia.x + 28 * S, ty), kText, vars[size_t(i)].first);
                TextRight(27, ImVec2(ib.x - 28 * S, (ia.y + ib.y) / 2), kAccentHi, Fit(27, vars[size_t(i)].second, cw * 0.45f, false));
            } else {
                Text(27, ImVec2(ia.x + 28 * S, ty), kAccentHi, "+  Add a variable");
            }
            if (top && Clicked(ia, ib)) { focus = i; Act(kAccept); }
        }
    }
    std::vector<std::pair<std::string, std::string>> Hints() override {
        if (focus < int(Vars().size())) return {{g_btn_a, "Edit"}, {g_btn_y, "Remove"}, {g_btn_b, "Back"}};
        return {{g_btn_a, "Add"}, {g_btn_b, "Back"}};
    }
};

// ---- the main view ------------------------------------------------------------------------------------------
void MainLayer::Activate(int row) {
    Model& m = app.model;
    Row& r = m.rows[row];
    if (!m.Locked(row).empty()) return;
    switch (r.kind) {
    case Kind::Cycle:
    case Kind::Toggle:
        if (m.Step(row, 1)) app.dirty = true;
        break;
    case Kind::List: {
        std::vector<std::string> items;
        for (const auto& c : r.choices) items.push_back(c.label);
        app.Push(new ListLayer(r.label, items, r.sel, [this, row](int i) {
            app.model.SetChoice(row, i);
            app.dirty = true;
        }));
        break;
    }
    case Kind::Text:
        app.Push(new KeyboardLayer(r.label, r.text, 16, false, [this, row](const std::string& t) {
            app.model.SetText(row, t.empty() ? app.model.rows[row].text_def : t);
            app.dirty = true;
        }));
        break;
    case Kind::Folder:
        app.OpenFolders(r.id == "update" ? 0 : 1, false);
        break;
    case Kind::Button:
        app.Command(r.id);
        break;
    default:
        break;
    }
}

void MainLayer::Act(::Act a) {
    const int row = Focused();
    switch (a) {
    case kUp: Move(-1); break;
    case kDown: Move(1); break;
    case kPageUp: Move(-1, 6); break;
    case kPageDown: Move(1, 6); break;
    case kLeft:
    case kRight:
        if (row >= 0 && app.model.Step(row, a == kLeft ? -1 : 1)) app.dirty = true;
        break;
    case kAccept:
        if (row >= 0) Activate(row);
        break;
    case kY:
        if (row >= 0 && app.model.IsSetting(row) && app.model.Locked(row).empty() && !app.model.IsDefault(row)) {
            app.model.Reset(row);
            app.dirty = true;
        }
        break;
    case kTabPrev:
    case kTabNext: {
        int t = app.page >= kTabCount ? kTabAdvanced : app.page;
        t = (t + (a == kTabNext ? 1 : kTabCount - 1)) % kTabCount;
        SetPage(t);
        break;
    }
    case kBack:
        if (app.page >= kTabCount) SetPage(kTabAdvanced);
        else app.Push(new ConfirmLayer("Quit to Steam?", "Your settings are saved. The game does not start.", "Quit", "Stay",
                                       [this] { app.quit = true; }));
        break;
    case kStart: app.Play(); break;
    default: break;
    }
}

std::string MainLayer::Help(ImU32& color) {
    const int row = Focused();
    color = kMuted;
    if (row < 0) return "";
    const Model& m = app.model;
    const Row& r = m.rows[row];
    const std::string locked = m.Locked(row);
    if (!locked.empty()) {
        color = kWarn;
        return locked + "  " + r.hint;
    }
    if (r.id == "lsfg" && r.sel == 1 && !app.lsfg_ready && !app.lsfg_lines.empty()) {
        color = kWarn;
        return app.lsfg_lines[0] + " Until then the game starts without it.";
    }
    if (r.kind == Kind::Folder) {
        const std::string& problem = r.id == "update" ? app.folders.update_problem : app.folders.base_problem;
        if (!problem.empty()) {
            color = kBad;
            return problem;
        }
    }
    return r.hint;
}

std::vector<std::pair<std::string, std::string>> MainLayer::Hints() {
    std::vector<std::pair<std::string, std::string>> h;
    const int row = Focused();
    if (row >= 0) {
        const Model& m = app.model;
        const Row& r = m.rows[row];
        const bool locked = !m.Locked(row).empty();
        if (!locked) {
            switch (r.kind) {
            case Kind::Cycle: h.push_back({"◀▶", "Change"}); break;
            case Kind::Slider: h.push_back({"◀▶", "Adjust"}); break;
            case Kind::Toggle: h.push_back({g_btn_a, "On / off"}); break;
            case Kind::List: h.push_back({g_btn_a, "Choose"}); break;
            case Kind::Text: h.push_back({g_btn_a, "Edit"}); break;
            case Kind::Folder: h.push_back({g_btn_a, "Change"}); break;
            case Kind::Button: h.push_back({g_btn_a, r.id == "@play" ? "Play" : "Open"}); break;
            default: break;
            }
            if (m.IsSetting(row) && !m.IsDefault(row)) h.push_back({g_btn_y, "Default"});
        }
    }
    h.push_back({"L1/R1", "Tabs"});
    if (row < 0 || app.model.rows[row].id != "@play") h.push_back({"START", "Play"});
    h.push_back({g_btn_b, app.page >= kTabCount ? "Back" : "Quit"});
    return h;
}

void MainLayer::Draw(bool top) {
    Model& m = app.model;
    Box(ImVec2(0, 0), ImVec2(W, H), kBg, 0);
    // Header: the name and the tabs.
    const float margin = 56 * S;
    Bold(40, ImVec2(margin, 34 * S), kWhite, "DRIVECLUB");
    const float title_w = TextSize(40, "DRIVECLUB").x + 4 * S;
    float tx = std::max(margin + title_w + 70 * S, W * 0.24f);
    const float tab_y = 30 * S, tab_h = 64 * S;
    tx += GlyphWidth("L1", 40) / 2;
    Glyph(ImVec2(tx, tab_y + tab_h / 2), "L1", 40);
    tx += GlyphWidth("L1", 40) / 2 + 16 * S;
    const int current = app.page >= kTabCount ? kTabAdvanced : app.page;
    for (int t = 0; t < kTabCount; ++t) {
        const float w = TextSize(28, kTabName[t]).x + 44 * S;
        const ImVec2 a(tx, tab_y), b(tx + w, tab_y + tab_h);
        if (t == current) {
            Box(a, b, kFocus, 10);
            Box(ImVec2(a.x + 14 * S, b.y - 6 * S), ImVec2(b.x - 14 * S, b.y - 1 * S), kAccent, 2);
        }
        TextCenter(28, a, b, t == current ? kWhite : kMuted, kTabName[t], t == current);
        if (top && Clicked(a, b)) SetPage(t);
        tx += w + 6 * S;
    }
    tx += 10 * S + GlyphWidth("R1", 40) / 2;
    Glyph(ImVec2(tx, tab_y + tab_h / 2), "R1", 40);
    Box(ImVec2(0, 116 * S), ImVec2(W, 117 * S), kLine, 0);

    // Rows.
    const std::vector<int> rows = Rows();
    const float cw = std::min(W - 2 * margin, 1400 * S), x0 = (W - cw) / 2;
    const ImVec2 ca(0, 118 * S), cb(W, H - 168 * S);
    // Layout: positions of each row (with the section headers) in content space.
    std::vector<float> top_y(rows.size()), height(rows.size());
    std::vector<bool> header(rows.size());
    float y = 24 * S;
    if (app.page == kPagePatches) y += 70 * S;
    std::string section;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = m.rows[size_t(rows[i])];
        header[i] = !r.section.empty() && r.section != section;
        section = r.section;
        if (header[i]) y += (i ? 28 : 0) * S + 46 * S;
        top_y[i] = y;
        height[i] = (r.primary ? 150 : 76) * S;
        y += height[i] + 8 * S;
    }
    const float content_h = y + 24 * S, view_h = cb.y - ca.y;
    const int fi = std::clamp(app.focus[app.page], 0, std::max(0, int(rows.size()) - 1));
    float& scroll = app.scroll[app.page];
    if (!rows.empty()) {
        const float want_top = top_y[size_t(fi)] - (header[size_t(fi)] ? 60 * S : 16 * S);
        const float want_bottom = top_y[size_t(fi)] + height[size_t(fi)] + 16 * S;
        if (want_top < scroll) scroll = want_top;
        if (want_bottom > scroll + view_h) scroll = want_bottom - view_h;
    }
    scroll = std::clamp(scroll - (top && Hover(ca, cb) ? ImGui::GetIO().MouseWheel * 120 * S : 0), 0.f,
                        std::max(0.f, content_h - view_h));
    g_dl->PushClipRect(ca, cb, true);
    const float oy = ca.y - scroll;
    if (app.page == kPagePatches) {
        Bold(32, ImVec2(x0, oy + 24 * S), kWhite, "All community patches");
        Text(23, ImVec2(x0, oy + 66 * S), kMuted, "Advanced › All community patches. The frame rate, resolution and memory patches follow the Graphics tab.");
    }
    section.clear();
    for (size_t i = 0; i < rows.size(); ++i) {
        const int ri = rows[i];
        const Row& r = m.rows[size_t(ri)];
        const bool focused = int(i) == fi;
        const bool locked = !m.Locked(ri).empty();
        if (header[i]) {
            std::string up = r.section;
            for (auto& ch : up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            Text(21, ImVec2(x0 + 4 * S, oy + top_y[i] - 38 * S), kMuted, up);
        }
        const ImVec2 a(x0, oy + top_y[i]), b(x0 + cw, oy + top_y[i] + height[i]);
        if (b.y < ca.y || a.y > cb.y) continue;
        const float alpha = locked ? 0.45f : 1.f;
        // Play: the big button, with a line of the main choices under its label.
        if (r.primary) {
            const bool ready = app.folders.update_ok && app.folders.base_ok;
            Box(a, b, focused ? kAccentHi : kAccent, 14);
            if (focused) Frame(ImVec2(a.x - 4 * S, a.y - 4 * S), ImVec2(b.x + 4 * S, b.y + 4 * S), kWhite, 16, 3);
            TextCenter(46, a, ImVec2(b.x, a.y + 100 * S), kWhite, "▶  PLAY", true);
            TextCenter(23, ImVec2(a.x, a.y + 84 * S), ImVec2(b.x, b.y - 10 * S), Alpha(kWhite, 0.85f),
                       ready ? Fit(23, m.Summary(), cw - 60 * S, false) : "Choose the game folders first (below)");
            if (top && Clicked(a, b) && Hover(ca, cb)) { app.focus[app.page] = int(i); Activate(ri); }
            continue;
        }
        Box(a, b, focused ? kFocus : kCard, 10);
        if (focused) {
            Box(a, ImVec2(a.x + 6 * S, b.y), kAccent, 3);
            Frame(a, b, Alpha(kMuted, 0.5f), 10, 1.5f);
        }
        const float mid = (a.y + b.y) / 2;
        const float lh = TextSize(28, "Ag").y;
        // Label (a dot before it when the setting is not at its default).
        float lx = a.x + 32 * S;
        if (m.IsSetting(ri) && !m.IsDefault(ri)) g_dl->AddCircleFilled(ImVec2(a.x + 19 * S, mid), 5 * S, Alpha(kAccent, alpha));
        const ImU32 label_col = Alpha(r.kind == Kind::Button && r.id != "@quit" ? kAccentHi : focused ? kWhite : kText, alpha);
        const float value_w = cw * 0.44f;
        Text(28, ImVec2(lx, mid - lh / 2), label_col, Fit(28, r.label, cw - value_w - 64 * S, false));
        // Value.
        const float vr = b.x - 32 * S;
        const std::string value = m.ValueText(ri);
        bool row_clicked = top && Clicked(a, b) && Hover(ca, cb);
        switch (r.kind) {
        case Kind::Cycle: {
            const ImU32 arrow = Alpha(focused ? kAccentHi : kOff, alpha);
            const float aw = TextSize(26, "▶").x;
            Text(26, ImVec2(vr - aw, mid - TextSize(26, "▶").y / 2), arrow, "▶");
            const float vw = std::min(TextSize(28, value).x, value_w - 2 * aw - 40 * S);
            const float vx = vr - aw - 22 * S - vw;
            Text(28, ImVec2(vx, mid - lh / 2), Alpha(focused ? kWhite : kText, alpha), Fit(28, value, value_w - 2 * aw - 40 * S, false));
            Text(26, ImVec2(vx - 22 * S - aw, mid - TextSize(26, "◀").y / 2), arrow, "◀");
            if (row_clicked && !locked) {
                app.focus[app.page] = int(i);
                if (app.model.Step(ri, g_mouse.x < vx - 10 * S ? -1 : 1)) app.dirty = true;
                row_clicked = false;
            }
            break;
        }
        case Kind::List:
            TextRight(28, ImVec2(vr - 34 * S, mid), Alpha(focused ? kWhite : kText, alpha), Fit(28, value, value_w - 40 * S, false));
            TextRight(28, ImVec2(vr, mid), Alpha(focused ? kAccentHi : kOff, alpha), "›");
            break;
        case Kind::Toggle: {
            const float tw = 74 * S, th = 38 * S;
            const ImVec2 ta(vr - tw, mid - th / 2), tb(vr, mid + th / 2);
            Box(ta, tb, Alpha(r.sel ? kAccent : kOff, alpha), th / 2 / S);
            const float kx = r.sel ? tb.x - th / 2 : ta.x + th / 2;
            g_dl->AddCircleFilled(ImVec2(kx, mid), th / 2 - 5 * S, Alpha(kWhite, alpha));
            TextRight(24, ImVec2(ta.x - 18 * S, mid), Alpha(kMuted, alpha), r.sel ? "On" : "Off");
            break;
        }
        case Kind::Slider: {
            const float bw = value_w - 110 * S;
            const ImVec2 sa(vr - bw, mid - 4 * S), sb(vr, mid + 4 * S);
            Box(sa, sb, Alpha(kOff, alpha), 4);
            const float t = r.max > r.min ? float(r.sel - r.min) / float(r.max - r.min) : 0;
            Box(sa, ImVec2(sa.x + bw * t, sb.y), Alpha(kAccent, alpha), 4);
            g_dl->AddCircleFilled(ImVec2(sa.x + bw * t, mid), (focused ? 14 : 11) * S, Alpha(kWhite, alpha));
            TextRight(28, ImVec2(sa.x - 26 * S, mid), Alpha(focused ? kWhite : kText, alpha), value);
            if (row_clicked && !locked && g_mouse.x >= sa.x - 10 * S) {
                app.focus[app.page] = int(i);
                const float f = std::clamp((g_mouse.x - sa.x) / bw, 0.f, 1.f);
                Row& rw = app.model.rows[size_t(ri)];
                const int v = rw.min + int(std::lround(f * float(rw.max - rw.min) / float(rw.step))) * rw.step;
                app.model.Step(ri, 0);
                rw.sel = std::clamp(v, rw.min, rw.max);
                app.dirty = true;
                row_clicked = false;
            }
            break;
        }
        case Kind::Text:
            TextRight(28, ImVec2(vr - 40 * S, mid), focused ? kWhite : kText, Fit(28, value, value_w - 50 * S, false));
            TextRight(26, ImVec2(vr, mid), focused ? kAccentHi : kOff, "✎");
            break;
        case Kind::Folder: {
            const bool ok = r.id == "update" ? app.folders.update_ok : app.folders.base_ok;
            const ImU32 c = ok ? kOk : value.empty() ? kWarn : kBad;
            TextRight(28, ImVec2(vr, mid), c, ok ? "✓" : "✗");
            TextRight(24, ImVec2(vr - 40 * S, mid), value.empty() ? kWarn : focused ? kWhite : kText,
                      value.empty() ? "not chosen" : Fit(24, value, value_w + 80 * S, true));
            break;
        }
        case Kind::Button: {
            std::string extra;
            if (r.id == "@patches") {
                const int n = m.ChangedPatches();
                extra = n ? std::to_string(n) + " changed" : "as in DriveClub.xml";
            } else if (r.id == "@lsfg_check") {
                extra = app.lsfg_lines.empty() ? "" : app.lsfg_ready ? "ready" : "not ready";
                if (!extra.empty()) {
                    TextRight(24, ImVec2(vr - 34 * S, mid), app.lsfg_ready ? kOk : kWarn, extra);
                    extra.clear();
                }
            } else if (r.id == "@env") {
                int n = 0;
                for (const auto& [k, v] : app.conf.Items()) n += k.rfind("env.", 0) == 0 && m.Find(k) < 0;
                extra = n ? std::to_string(n) + (n == 1 ? " variable" : " variables") : "none";
            }
            if (!extra.empty()) TextRight(24, ImVec2(vr - 34 * S, mid), kMuted, extra);
            if (r.id != "@quit") TextRight(28, ImVec2(vr, mid), focused ? kAccentHi : kOff, "›");
            break;
        }
        case Kind::Info:
            TextRight(24, ImVec2(vr, mid), kMuted, value);
            break;
        }
        if (row_clicked && Focusable(ri)) {
            const bool again = app.focus[app.page] == int(i);
            app.focus[app.page] = int(i);
            if (again || r.kind == Kind::Toggle || r.kind == Kind::Button || r.kind == Kind::List || r.kind == Kind::Text ||
                r.kind == Kind::Folder) {
                Activate(ri);
            }
        }
    }
    g_dl->PopClipRect();
    // Scroll bar.
    if (content_h > view_h) {
        const float bh = view_h * view_h / content_h, by = ca.y + (view_h - bh) * scroll / (content_h - view_h);
        Box(ImVec2(W - 14 * S, by + 6 * S), ImVec2(W - 8 * S, by + bh - 6 * S), kOff, 3);
    }
}

// ---- app commands ------------------------------------------------------------------------------------------------
void App::OpenFolders(int target, bool search) { Push(new FolderLayer(*this, target, search)); }

void App::CheckLsfg() {
    lsfg_lines.clear();
    lsfg_ready = false;
    if (run_sh.empty()) return;
    // Saved first: run.sh reads the settings file (the environment variables of the Advanced tab among them).
    if (dirty) Save();
    Proc proc;
    if (!proc.Start({"bash", run_sh.string(), "--lsfg-status"})) return;
    const Uint64 until = SDL_GetTicks() + 5000; // a few folders and files: well below a second
    while (proc.Poll() && SDL_GetTicks() < until) SDL_Delay(5);
    lsfg_lines = proc.lines;
    lsfg_ready = proc.exit_code == 0 && !lsfg_lines.empty() && lsfg_lines[0].rfind("Ready", 0) == 0;
    if (lsfg_lines.empty()) lsfg_lines.push_back("run.sh could not check for lsfg-vk (exit code " + std::to_string(proc.exit_code) + ").");
}

void App::Command(const std::string& id) {
    if (id == "@play") Play();
    else if (id == "@quit") quit = true;
    else if (id == "@find") OpenFolders(0, true);
    else if (id == "@patches") {
        page = kPagePatches;
        focus[kPagePatches] = 0;
        scroll[kPagePatches] = 0;
    } else if (id == "@env") Push(new EnvLayer(*this));
    else if (id == "@lsfg_check") {
        CheckLsfg();
        Push(new TextLayer("Lossless Scaling frame generation", lsfg_lines));
    }
    else if (id == "@cpu_selftest") Push(TextLayer::Running("CPU self-test", {(bin / "dc-cpu-selftest").string()}));
    else if (id == "@vulkan_info") {
        const char* vi = std::getenv("BB_VULKANINFO");
        Push(TextLayer::Running("Vulkan info", {vi && *vi ? vi : "vulkaninfo", "--summary"}));
    } else if (id == "@log") {
        fs::path newest;
        fs::file_time_type when{};
        std::error_code ec;
        for (fs::directory_iterator it(data / "logs", ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".log" && (newest.empty() || it->last_write_time() > when)) {
                newest = it->path();
                when = it->last_write_time();
            }
        }
        std::vector<std::string> lines;
        if (newest.empty()) lines.push_back("No log yet: the game writes one at every start.");
        else {
            std::ifstream in(newest, std::ios::binary);
            for (std::string l; std::getline(in, l);) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                lines.push_back(l);
            }
            if (lines.size() > 2000) lines.erase(lines.begin(), lines.end() - 2000);
        }
        auto* t = new TextLayer(newest.empty() ? "Latest log" : "Latest log: " + newest.filename().string(), lines);
        t->scroll = 1e9f;
        Push(t);
    } else if (id == "@reprepare") {
        std::error_code ec;
        fs::remove(data / "prep" / "stamp.txt", ec);
        Toast("The game image is converted again at the next start.");
    } else if (id == "@reset") {
        Push(new ConfirmLayer("Reset all settings?",
                              "Every setting goes back to its default: the game as it starts without the launcher. Your "
                              "game folders are kept; environment variables and patch choices are removed.",
                              "Reset", "Cancel", [this] {
                                  Conf fresh;
                                  for (const char* k : {"update", "base"}) {
                                      if (conf.Has(k)) fresh.Set(k, conf.Get(k));
                                  }
                                  conf = fresh;
                                  model.Load(conf);
                                  dirty = true;
                                  Toast("All settings are back to their defaults.");
                              }));
    }
}

void App::Play() {
    CheckFolders();
    if (!folders.update_ok || !folders.base_ok) {
        Toast("Choose the game folders first.");
        OpenFolders(folders.update_ok ? 1 : 0, !searched);
        return;
    }
    Save();
    Push(new ProgressLayer(*this));
}

// ------------------------------------------------------------------------------------------
// main loop
// ------------------------------------------------------------------------------------------
struct Held {
    Act act = kNone;
    Uint64 since = 0, next = 0;
};

Act ButtonAct(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return kUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return kDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return kLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return kRight;
    case SDL_GAMEPAD_BUTTON_SOUTH: return kAccept;
    case SDL_GAMEPAD_BUTTON_EAST: return kBack;
    case SDL_GAMEPAD_BUTTON_WEST: return kX;
    case SDL_GAMEPAD_BUTTON_NORTH: return kY;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return kTabPrev;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return kTabNext;
    case SDL_GAMEPAD_BUTTON_START: return kStart;
    default: return kNone;
    }
}

Act KeyAct(SDL_Keycode k, SDL_Keymod mod, bool text_mode) {
    switch (k) {
    case SDLK_UP: return kUp;
    case SDLK_DOWN: return kDown;
    case SDLK_LEFT: return kLeft;
    case SDLK_RIGHT: return kRight;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return text_mode ? kStart : kAccept;
    case SDLK_SPACE: return text_mode ? kNone : kAccept;
    case SDLK_ESCAPE: return kBack;
    case SDLK_BACKSPACE: return text_mode ? kX : kBack;
    case SDLK_TAB: return (mod & SDL_KMOD_SHIFT) ? kTabPrev : kTabNext;
    case SDLK_PAGEUP: return kPageUp;
    case SDLK_PAGEDOWN: return kPageDown;
    case SDLK_F5: return kStart;
    case SDLK_DELETE: return text_mode ? kX : kY;
    default: return kNone;
    }
}

bool Repeats(Act a) { return a == kUp || a == kDown || a == kLeft || a == kRight || a == kPageUp || a == kPageDown || a == kX; }

void ReadButtonLabels(SDL_Gamepad* pad) {
    auto label = [&](SDL_GamepadButton b, const char* fallback) -> std::string {
        switch (SDL_GetGamepadButtonLabel(pad, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "✕";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "○";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "□";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "△";
        default: return fallback;
        }
    };
    g_btn_a = label(SDL_GAMEPAD_BUTTON_SOUTH, "A");
    g_btn_b = label(SDL_GAMEPAD_BUTTON_EAST, "B");
    g_btn_x = label(SDL_GAMEPAD_BUTTON_WEST, "X");
    g_btn_y = label(SDL_GAMEPAD_BUTTON_NORTH, "Y");
}

void DrawHelpBar(App& app, Layer& layer) {
    const float top = H - 168 * S;
    Box(ImVec2(0, top), ImVec2(W, H), kCard, 0);
    Box(ImVec2(0, top), ImVec2(W, top + 1 * S), kLine, 0);
    const float margin = 56 * S, cw = W - 2 * margin;
    ImU32 color = kMuted;
    std::string help = layer.Help(color);
    const double now = double(SDL_GetTicks()) / 1000.0;
    if (!app.toast.empty() && now < app.toast_until) {
        help = app.toast;
        color = kWarn;
    }
    if (!help.empty()) {
        // Two lines at most.
        std::string shown = help;
        while (TextSize(25, shown, cw).y > TextSize(25, "A\nA").y + 2 && shown.size() > 8) shown = Fit(25, shown, TextSize(25, shown).x * 0.9f, false);
        Text(25, ImVec2(margin, top + 18 * S), color, shown, cw);
    }
    // Hints, right-aligned on the bottom line; each can be clicked.
    float x = W - margin;
    const float y = H - 44 * S;
    const auto hints = layer.Hints();
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        const float lw = TextSize(24, it->second).x, gw = GlyphWidth(it->first);
        const float left = x - lw - 12 * S - gw;
        Text(24, ImVec2(x - lw, y - TextSize(24, "Ag").y / 2), kText, it->second);
        if (it->first == "◀▶" || it->first == "▲▼") TextCenter(26, ImVec2(left, y - 20 * S), ImVec2(left + gw, y + 20 * S), kText, it->first);
        else Glyph(ImVec2(left + gw / 2, y), it->first);
        if (Clicked(ImVec2(left, y - 24 * S), ImVec2(x, y + 24 * S))) {
            const std::string& g = it->first;
            Act a = g == g_btn_a ? kAccept : g == g_btn_b ? kBack : g == g_btn_x ? kX : g == g_btn_y ? kY : g == "START" ? kStart
                    : g == "L1/R1" ? kTabNext : kNone;
            if (a != kNone) layer.Act(a);
            g_click = false;
        }
        x = left - 40 * S;
    }
}

[[noreturn]] void ExecGame(App& app) {
    app.Save();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
    setenv("DC_NO_LAUNCHER", "1", 1);
    const std::string script = app.run_sh.string();
    if (app.no_exec) {
        std::printf("exec bash %s --play\n", script.c_str());
        std::exit(0);
    }
    execlp("bash", "bash", script.c_str(), "--play", static_cast<char*>(nullptr));
    std::fprintf(stderr, "dc-launcher: cannot start bash %s --play: %s\n", script.c_str(), std::strerror(errno));
    std::exit(1);
}

int Run(int argc, char** argv) {
    App app;
    fs::path script_path, shots;
    bool selftest = false;
    int window_mode = 0; // 0 automatic, 1 window, 2 fullscreen
    int win_w = 1280, win_h = 800;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--run-sh") app.run_sh = next();
        else if (a == "--data") app.data = next();
        else if (a == "--patches-xml") app.patch_xml = next();
        else if (a == "--bin") app.bin = next();
        else if (a == "--script") script_path = next();
        else if (a == "--shots") shots = next();
        else if (a == "--no-exec") app.no_exec = true;
        else if (a == "--selftest") selftest = true;
        else if (a == "--window") window_mode = 1;
        else if (a == "--fullscreen") window_mode = 2;
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &win_w, &win_h); // the window's size (tests)
        else {
            std::fprintf(stderr, "usage: dc-launcher --run-sh FILE --data DIR --patches-xml FILE --bin DIR [--window|--fullscreen]\n");
            return a == "-h" || a == "--help" ? 0 : 1;
        }
    }
    if (app.data.empty()) {
        const char* d = std::getenv("DC_DATA_DIR");
        const char* home = std::getenv("HOME");
        app.data = d && *d ? fs::path(d) : fs::path(home ? home : ".") / ".local/share/driveclub-recomp";
    }
    app.conf_path = app.data / "driveclub.conf";
    // run.sh (Play, the lsfg-vk check) reads the same settings file.
    setenv("DC_DATA_DIR", app.data.c_str(), 1);
    if (!app.conf.Load(app.conf_path)) std::fprintf(stderr, "dc-launcher: cannot read %s\n", app.conf_path.c_str());

    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0"); // the launcher has its own; Steam's would cover it
    SDL_SetHint(SDL_HINT_APP_NAME, "DriveClub");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan,opengles2,opengl,software");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "dc-launcher: SDL: %s\n", SDL_GetError());
        return 2;
    }
    // Game Mode (gamescope) and the Steam Deck UI: fullscreen; a desktop: a window.
    const bool gamescope = std::getenv("GAMESCOPE_WAYLAND_DISPLAY") || std::getenv("SteamGamepadUI") || std::getenv("SteamDeck");
    const bool full = window_mode == 2 || (window_mode == 0 && gamescope);
    SDL_Window* window = SDL_CreateWindow("DriveClub", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (full ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window) {
        std::fprintf(stderr, "dc-launcher: window: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "dc-launcher: renderer: %s\n", SDL_GetError());
        return 2;
    }
    SDL_SetRenderVSync(renderer, 1);
    std::printf("dc-launcher: %s renderer\n", SDL_GetRendererName(renderer));
    int display_hz = 0;
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) display_hz = int(std::lround(mode->refresh_rate));

    app.model.Build(ReadPatches(app.patch_xml), display_hz);
    app.model.Load(app.conf);
    app.CheckFolders();
    app.CheckLsfg();
    auto* main_layer = new MainLayer(app);
    app.layers.emplace_back(main_layer);
    // First start (or the folders are gone): look for them.
    if (!app.folders.update_ok || !app.folders.base_ok) app.OpenFolders(0, true);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    g_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(dc_font_ttf), int(dc_font_ttf_end - dc_font_ttf), 0.f, &fc);

    std::vector<std::string> script;
    if (!script_path.empty()) {
        std::ifstream in(script_path);
        for (std::string l; std::getline(in, l);) {
            if (!l.empty() && l[0] != '#') script.push_back(l);
        }
    }
    if (selftest) {
        // Every tab and the full patch list, then the pop-ups: drawn without errors.
        for (int t = 0; t < kPageCount; ++t) script.push_back("page " + std::to_string(t));
        script.push_back("wait 2");
    }
    size_t script_at = 0;
    int script_wait = 3;

    SDL_Gamepad* pad = nullptr;
    Held held;
    bool text_on = false;
    Uint64 last_input = SDL_GetTicks();
    while (!app.quit) {
        Layer* layer = app.layers.back().get();
        std::vector<Act> acts;
        g_click = false;
        // Events (wait a little when nothing moves: a launcher should not keep a handheld busy).
        SDL_Event e;
        const bool busy = held.act != kNone || app.layers.size() > 1 || !script.empty() || SDL_GetTicks() - last_input < 1000;
        bool got = SDL_WaitEventTimeout(&e, busy ? 16 : 250);
        while (got) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            switch (e.type) {
            case SDL_EVENT_QUIT: app.quit = true; break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) {
                    pad = SDL_OpenGamepad(e.gdevice.which);
                    if (pad) { ReadButtonLabels(pad); g_have_pad = true; }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                    SDL_CloseGamepad(pad);
                    pad = nullptr;
                    int n = 0;
                    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
                        if (n > 0) pad = SDL_OpenGamepad(ids[0]);
                        SDL_free(ids);
                    }
                    if (pad) ReadButtonLabels(pad);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                const Act a = ButtonAct(e.gbutton.button);
                if (a != kNone) {
                    acts.push_back(a);
                    if (Repeats(a)) held = {a, SDL_GetTicks(), SDL_GetTicks() + 380};
                }
                SDL_HideCursor();
                last_input = SDL_GetTicks();
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (ButtonAct(e.gbutton.button) == held.act) held.act = kNone;
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                // The left stick as a D-pad, the triggers as page up / down.
                static Act stick_x = kNone, stick_y = kNone, trig_l = kNone, trig_r = kNone;
                const float v = float(e.gaxis.value) / 32767.f;
                auto edge = [&](Act& state, Act now) {
                    if (now == state) return;
                    if (state == held.act) held.act = kNone;
                    state = now;
                    if (now != kNone) {
                        acts.push_back(now);
                        held = {now, SDL_GetTicks(), SDL_GetTicks() + 380};
                        last_input = SDL_GetTicks();
                    }
                };
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) edge(stick_x, v < -0.6f ? kLeft : v > 0.6f ? kRight : std::fabs(v) < 0.4f ? kNone : stick_x);
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) edge(stick_y, v < -0.6f ? kUp : v > 0.6f ? kDown : std::fabs(v) < 0.4f ? kNone : stick_y);
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) edge(trig_l, v > 0.6f ? kPageUp : v < 0.3f ? kNone : trig_l);
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) edge(trig_r, v > 0.6f ? kPageDown : v < 0.3f ? kNone : trig_r);
                break;
            }
            case SDL_EVENT_KEY_DOWN: {
                if (e.key.repeat) break;
                const Act a = KeyAct(e.key.key, e.key.mod, layer->WantsText());
                if (a != kNone) {
                    acts.push_back(a);
                    if (Repeats(a)) held = {a, SDL_GetTicks(), SDL_GetTicks() + 380};
                }
                last_input = SDL_GetTicks();
                break;
            }
            case SDL_EVENT_KEY_UP:
                if (KeyAct(e.key.key, e.key.mod, layer->WantsText()) == held.act) held.act = kNone;
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (layer->WantsText()) layer->TextInput(e.text.text);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                SDL_ShowCursor();
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) g_click = true;
                last_input = SDL_GetTicks();
                break;
            default: break;
            }
            got = SDL_PollEvent(&e);
        }
        // Held directions repeat, faster after a second.
        const Uint64 now = SDL_GetTicks();
        if (held.act != kNone && now >= held.next) {
            acts.push_back(held.act);
            held.next = now + (now - held.since > 1200 ? 35 : 85);
        }
        // Test script: one step per frame.
        if (script_at >= script.size() && script_wait > 0) --script_wait;
        else if (script_at < script.size() && --script_wait <= 0) {
            script_wait = 2;
            const std::string& l = script[script_at++];
            static const std::pair<const char*, Act> names[] = {{"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight},
                                                               {"a", kAccept}, {"b", kBack}, {"x", kX}, {"y", kY}, {"l1", kTabPrev},
                                                               {"r1", kTabNext}, {"l2", kPageUp}, {"r2", kPageDown}, {"start", kStart}};
            for (const auto& [n, a] : names) {
                if (l == n) acts.push_back(a);
            }
            if (l.rfind("text ", 0) == 0) layer->TextInput(l.substr(5));
            if (l.rfind("wait ", 0) == 0) script_wait = std::atoi(l.c_str() + 5) + 1;
            if (l.rfind("page ", 0) == 0) {
                while (app.layers.size() > 1) app.layers.pop_back();
                static_cast<MainLayer*>(app.layers[0].get())->SetPage(std::atoi(l.c_str() + 5));
            }
            if (l.rfind("shot ", 0) == 0) {
                // Taken after this frame is drawn (below).
            }
        }
        for (Act a : acts) {
            layer = app.layers.back().get();
            layer->Act(a);
            while (app.layers.size() > 1 && app.layers.back()->closed) app.layers.pop_back();
        }
        layer = app.layers.back().get();
        if (layer->WantsText() != text_on) {
            text_on = layer->WantsText();
            text_on ? SDL_StartTextInput(window) : SDL_StopTextInput(window);
        }

        // Draw.
        int pw = 0, ph = 0;
        SDL_GetRenderOutputSize(renderer, &pw, &ph);
        W = float(pw);
        H = float(ph);
        S = std::max(0.45f, std::min(H / 1080.f, W / 1500.f));
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        int ww = 0, wh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        const ImVec2 mp = ImGui::GetIO().MousePos;
        g_mouse = ImVec2(mp.x * (ww ? float(pw) / float(ww) : 1.f), mp.y * (wh ? float(ph) / float(wh) : 1.f));
        g_dl = ImGui::GetBackgroundDrawList();
        // Every layer from the bottom up; only the top one takes clicks. Full-screen layers hide what is below.
        size_t first = 0;
        for (size_t i = 0; i < app.layers.size(); ++i) {
            Layer* l = app.layers[i].get();
            if (dynamic_cast<TextLayer*>(l) || dynamic_cast<FolderLayer*>(l) || dynamic_cast<ProgressLayer*>(l) || dynamic_cast<EnvLayer*>(l)) first = i;
        }
        const size_t count = app.layers.size();
        for (size_t i = first; i < count && i < app.layers.size(); ++i) app.layers[i]->Draw(i + 1 == count);
        DrawHelpBar(app, *app.layers.back());
        while (app.layers.size() > 1 && app.layers.back()->closed) app.layers.pop_back();
        if (app.layers.back().get() != layer) app.CheckFolders();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 16, 18, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (script_at > 0 && script_at <= script.size() && script[script_at - 1].rfind("shot ", 0) == 0 && script_wait == 2 && !shots.empty()) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                fs::create_directories(shots);
                SDL_SaveBMP(s, (shots / (script[script_at - 1].substr(5) + ".bmp")).c_str());
                SDL_DestroySurface(s);
            }
        }
        SDL_RenderPresent(renderer);
        if (app.dirty) app.Save();
        if (app.exec_game) ExecGame(app);
        if (!script.empty() && script_at >= script.size() && script_wait <= 0) app.quit = true;
    }
    if (app.dirty) app.Save();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (pad) SDL_CloseGamepad(pad);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (selftest) std::printf("dc-launcher: selftest ok\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) { return Run(argc, argv); }
