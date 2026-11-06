#include "menu.h"
#include <algorithm>
#include <cmath>

MenuItem MenuItem::action(std::string l, std::function<void()> f, std::string h) {
  MenuItem m;
  m.kind = Action;
  m.label = std::move(l);
  m.onActivate = std::move(f);
  m.help = std::move(h);
  return m;
}
MenuItem MenuItem::submenu(std::string l, std::function<void()> f, std::string h) {
  MenuItem m = action(std::move(l), std::move(f), std::move(h));
  m.kind = Submenu;
  return m;
}
MenuItem MenuItem::toggle(std::string l, std::function<int()> g, std::function<void(int)> s, std::string h) {
  MenuItem m;
  m.kind = Toggle;
  m.label = std::move(l);
  m.get = std::move(g);
  m.set = std::move(s);
  m.help = std::move(h);
  return m;
}
MenuItem MenuItem::choice(std::string l, std::vector<std::string> opts, std::function<int()> g,
                          std::function<void(int)> s, std::string h) {
  MenuItem m;
  m.kind = Choice;
  m.label = std::move(l);
  auto o = std::make_shared<std::vector<std::string>>(std::move(opts));
  m.options = [o]() { return *o; };
  m.get = std::move(g);
  m.set = std::move(s);
  m.help = std::move(h);
  return m;
}
MenuItem MenuItem::slider(std::string l, int mn, int mx, int st, std::function<int()> g, std::function<void(int)> s,
                          std::string suffix, std::string h) {
  MenuItem m;
  m.kind = Slider;
  m.label = std::move(l);
  m.minV = mn;
  m.maxV = mx;
  m.step = st;
  m.get = std::move(g);
  m.set = std::move(s);
  m.suffix = std::move(suffix);
  m.help = std::move(h);
  m.wrap = false;
  return m;
}
MenuItem MenuItem::info(std::string l, std::string h) {
  MenuItem m;
  m.kind = Label;
  m.label = std::move(l);
  m.help = std::move(h);
  return m;
}
MenuItem MenuItem::separator(std::string l) {
  MenuItem m;
  m.kind = Separator;
  m.label = std::move(l);
  return m;
}

std::wstring Menu::widen(const std::string& s) { return widenUtf8(s); }

namespace {
// The palette (as in the Mickey & Donald Collection's menus).
ColorF rgb(uint32_t c, float a = 1.0f) {
  return colorF(((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f, a);
}
ColorF withA(ColorF c, float a) { return colorF(c.r, c.g, c.b, c.a * a); }
ColorF mix(ColorF x, ColorF y, float t) {
  return colorF(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t);
}
const uint32_t kPanel = 0x12151C, kCrumb = 0x8A93A6, kSub = 0xB7BFCE, kText = 0xD4DAE6, kDim = 0x5A6070,
               kValue = 0xA9B3C6, kOff = 0x3A404E, kKnob = 0xF4F6FA, kHint = 0xC9D0DC, kKeys = 0x7C8598,
               kGold = 0xF2C14E;
constexpr float kLine = 1.33f;  // Segoe UI line height per pixel of font size

// Each style's colours (the panel and what's on it).
struct Look {
  uint32_t panel, crumb, sub, text, value, dim, off, knob, hint, keys, rowSel, toastBg, toastText;
  float panelAlpha;
};
const Look kLooks[Menu::kStyleCount] = {
    // Storybook: a cream card with a dark brown outline
    {0xFFF8E4, 0x9A7A58, 0x6B4E32, 0x3A1D0E, 0x6B4E32, 0xBFAE94, 0xD9CBAE, 0xFFFFFF, 0x5A4028, 0x9A7A58, 0x3A1D0E,
     0xFFF8E4, 0x3A1D0E, 0.97f},
    // Retro: black, white text, gold for what's chosen
    {0x000000, 0x7C7C7C, 0xBCBCBC, 0xFCFCFC, 0xFCFCFC, 0x5C5C5C, 0x3C3C3C, 0xFCFCFC, 0xFCFCFC, 0x7C7C7C, 0xFCBC3C,
     0x000000, 0xFCFCFC, 0.94f},
    // Themed Dark and Classic: the dark panel
    {kPanel, kCrumb, kSub, kText, kValue, kDim, kOff, kKnob, kHint, kKeys, 0xFFFFFF, 0x181C26, 0xFFFFFF, 0.918f},
    {kPanel, kCrumb, kSub, kText, kValue, kDim, kOff, kKnob, kHint, kKeys, 0xFFFFFF, 0x181C26, 0xFFFFFF, 0.918f},
};
constexpr uint32_t kInk = 0x3A1D0E;    // (Storybook's outlines)
constexpr uint32_t kRetroGold = 0xFCBC3C, kBrick = 0xC84C0C;

// The pixel font: 5x7, rows top to bottom, bit 4 the left column. Upper case
// (lower case is drawn as upper, as the games' own text is).
struct PixelChar {
  wchar_t c;
  uint8_t rows[7];
};
const PixelChar kPixelFont[] = {
    {L' ', {0, 0, 0, 0, 0, 0, 0}},        {L'!', {4, 4, 4, 4, 4, 0, 4}},          {L'"', {10, 10, 0, 0, 0, 0, 0}},
    {L'#', {10, 31, 10, 10, 10, 31, 10}}, {L'%', {24, 25, 2, 4, 8, 19, 3}},       {L'&', {12, 18, 20, 8, 21, 18, 13}},
    {L'\'', {4, 4, 0, 0, 0, 0, 0}},       {L'(', {2, 4, 8, 8, 8, 4, 2}},          {L')', {8, 4, 2, 2, 2, 4, 8}},
    {L'*', {0, 4, 21, 14, 21, 4, 0}},     {L'+', {0, 4, 4, 31, 4, 4, 0}},         {L',', {0, 0, 0, 0, 12, 4, 8}},
    {L'-', {0, 0, 0, 31, 0, 0, 0}},       {L'.', {0, 0, 0, 0, 0, 12, 12}},        {L'/', {1, 1, 2, 4, 8, 16, 16}},
    {L'0', {14, 17, 19, 21, 25, 17, 14}}, {L'1', {4, 12, 4, 4, 4, 4, 14}},        {L'2', {14, 17, 1, 2, 4, 8, 31}},
    {L'3', {30, 1, 1, 14, 1, 1, 30}},     {L'4', {2, 6, 10, 18, 31, 2, 2}},       {L'5', {31, 16, 30, 1, 1, 17, 14}},
    {L'6', {6, 8, 16, 30, 17, 17, 14}},   {L'7', {31, 1, 2, 4, 8, 8, 8}},         {L'8', {14, 17, 17, 14, 17, 17, 14}},
    {L'9', {14, 17, 17, 15, 1, 2, 12}},   {L':', {0, 12, 12, 0, 12, 12, 0}},      {L';', {0, 12, 12, 0, 12, 4, 8}},
    {L'<', {2, 4, 8, 16, 8, 4, 2}},       {L'=', {0, 0, 31, 0, 31, 0, 0}},        {L'>', {8, 4, 2, 1, 2, 4, 8}},
    {L'?', {14, 17, 1, 2, 4, 0, 4}},      {L'@', {14, 17, 23, 21, 23, 16, 14}},   {L'A', {14, 17, 17, 31, 17, 17, 17}},
    {L'B', {30, 17, 17, 30, 17, 17, 30}}, {L'C', {14, 17, 16, 16, 16, 17, 14}},   {L'D', {30, 17, 17, 17, 17, 17, 30}},
    {L'E', {31, 16, 16, 30, 16, 16, 31}}, {L'F', {31, 16, 16, 30, 16, 16, 16}},   {L'G', {14, 17, 16, 23, 17, 17, 15}},
    {L'H', {17, 17, 17, 31, 17, 17, 17}}, {L'I', {14, 4, 4, 4, 4, 4, 14}},        {L'J', {7, 2, 2, 2, 2, 18, 12}},
    {L'K', {17, 18, 20, 24, 20, 18, 17}}, {L'L', {16, 16, 16, 16, 16, 16, 31}},   {L'M', {17, 27, 21, 21, 17, 17, 17}},
    {L'N', {17, 17, 25, 21, 19, 17, 17}}, {L'O', {14, 17, 17, 17, 17, 17, 14}},   {L'P', {30, 17, 17, 30, 16, 16, 16}},
    {L'Q', {14, 17, 17, 17, 21, 18, 13}}, {L'R', {30, 17, 17, 30, 20, 18, 17}},   {L'S', {15, 16, 16, 14, 1, 1, 30}},
    {L'T', {31, 4, 4, 4, 4, 4, 4}},       {L'U', {17, 17, 17, 17, 17, 17, 14}},   {L'V', {17, 17, 17, 17, 17, 10, 4}},
    {L'W', {17, 17, 17, 21, 21, 21, 10}}, {L'X', {17, 17, 10, 4, 10, 17, 17}},    {L'Y', {17, 17, 10, 4, 4, 4, 4}},
    {L'Z', {31, 1, 2, 4, 8, 16, 31}},     {L'[', {14, 8, 8, 8, 8, 8, 14}},        {L']', {14, 2, 2, 2, 2, 2, 14}},
    {L'_', {0, 0, 0, 0, 0, 0, 31}},       {0x2191, {4, 14, 21, 4, 4, 4, 4}},      {0x2193, {4, 4, 4, 4, 21, 14, 4}},
    {0x2190, {0, 4, 8, 31, 8, 4, 0}},     {0x2192, {0, 4, 2, 31, 2, 4, 0}},       {0x25C0, {2, 6, 14, 30, 14, 6, 2}},
    {0x25B6, {8, 12, 14, 15, 14, 12, 8}}, {0x2713, {0, 1, 1, 2, 18, 12, 4}},      {0x2022, {0, 0, 14, 14, 14, 0, 0}},
    {0x266B, {15, 9, 9, 9, 27, 27, 0}},   {0x00D7, {0, 17, 10, 4, 10, 17, 0}},
};

const PixelChar* pixelChar(wchar_t c) {
  if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
  if (c == 0xE9 || c == 0xC9) c = L'E';  // (é)
  if (c == 0x2019) c = L'\'';
  if (c == 0x2013 || c == 0x2014) c = L'-';
  for (const PixelChar& p : kPixelFont)
    if (p.c == c) return &p;
  return nullptr;
}

// What the pixel font writes for characters it has no picture of.
std::wstring pixelString(const std::wstring& s) {
  std::wstring o;
  for (wchar_t c : s) {
    if (c == 0x2026) o += L"...";
    else if (c == 0xAB) o += L"<<";
    else if (c == 0xBB) o += L">>";
    else if (c == 0x24B6) o += L"(A)";
    else if (c == 0x24B7) o += L"(B)";
    else if (c == 0x24CE) o += L"(Y)";
    else if (c == 0x24CD) o += L"(X)";
    else if (pixelChar(c)) o += c;
    else o += L'?';
  }
  return o;
}
}  // namespace

float Menu::pixelMeasure(const std::wstring& s, float size) {
  const std::wstring t = pixelString(s);
  return t.empty() ? 0.0f : (t.size() * 6 - 1) * (size / 9.0f);
}

void Menu::pixelText(const std::wstring& s, RectF r, float size, ColorF c, TextAlign align, ParaAlign palign) {
  const std::wstring t = pixelString(s);
  const float cell = size / 9.0f;  // (7 rows and the line's gaps in a font size)
  const float w = pixelMeasure(s, size), h = 7 * cell;
  float x = align == kAlignTrailing ? r.right - w : align == kAlignCenter ? (r.left + r.right - w) * 0.5f : r.left;
  float y = palign == kParaNear ? r.top : palign == kParaFar ? r.bottom - h : (r.top + r.bottom - h) * 0.5f;
  for (wchar_t ch : t) {
    const PixelChar* g = pixelChar(ch);
    if (g && x + 5 * cell <= r.right + cell * 0.5f && x >= r.left - cell * 0.5f) {
      for (int row = 0; row < 7; row++) {
        const uint8_t bits = g->rows[row];
        for (int col = 0; col < 5;) {
          if (!((bits >> (4 - col)) & 1)) {
            col++;
            continue;
          }
          int run = col;
          while (run < 5 && ((bits >> (4 - run)) & 1)) run++;
          cv_->fillRect(rectF(std::round(x + col * cell), std::round(y + row * cell), std::round(x + run * cell),
                              std::round(y + (row + 1) * cell)),
                        c);
          col = run;
        }
      }
    }
    x += 6 * cell;
  }
}

void Menu::displayText(const std::wstring& s, RectF r, float size, ColorF fill, ColorF edge, float edgeW,
                       TextAlign align) {
  if (!cv_) return;
  if (style == kRetro) {
    pixelText(s, r, size, fill, align, kParaCenter);
    return;
  }
  if (edgeW > 0) {  // (an outline: the text drawn around it first)
    for (int k = 0; k < 8; k++) {
      const float ang = k * 3.14159265f / 4;
      cv_->titleText(s, rectF(r.left + std::cos(ang) * edgeW, r.top + std::sin(ang) * edgeW,
                              r.right + std::cos(ang) * edgeW, r.bottom + std::sin(ang) * edgeW),
                     size, edge, align);
    }
    cv_->titleText(s, rectF(r.left, r.top + edgeW * 1.4f, r.right, r.bottom + edgeW * 1.4f), size, edge, align);
  }
  cv_->titleText(s, r, size, fill, align);
}

float Menu::displayMeasure(const std::wstring& s, float size) {
  if (!cv_ || s.empty()) return 0.0f;
  return style == kRetro ? pixelMeasure(s, size) : cv_->measureTitle(s, size);
}

void Menu::drawBricks(RectF r, ColorF brick, ColorF mortar, float bh, float a) {
  cv_->fillRect(r, withA(mortar, a));
  const float bw = bh * 2.0f, gap = std::max(1.0f, bh * 0.12f);
  int rowN = 0;
  for (float y = r.top; y < r.bottom; y += bh, rowN++) {
    const float off = (rowN & 1) ? bw * 0.5f : 0.0f;
    for (float x = r.left - off; x < r.right; x += bw) {
      RectF b = rectF(std::max(r.left, x + gap * 0.5f), y + gap * 0.5f, std::min(r.right, x + bw - gap * 0.5f),
                      std::min(r.bottom, y + bh - gap * 0.5f));
      if (b.right > b.left && b.bottom > b.top) cv_->fillRect(b, withA(brick, a));
    }
  }
}

// Themed Dark: behind the title, the game's pattern (Super Mario Bros.' bricks,
// the Lost Levels' in its own tint, SMB2's starry night, SMB3's checkers; a
// starry sky for the collection).
void Menu::drawHeaderArt(RectF r, float a) {
  const float h = r.bottom - r.top;
  cv_->pushClip(r);
  switch (gameArt_) {
    case 1: drawBricks(r, rgb(0xB4432B), rgb(0x5E1E10), h / 4, a); break;
    case 2: drawBricks(r, rgb(0x2E8B57), rgb(0x123D24), h / 4, a); break;
    case 4: {
      cv_->fillRect(r, rgb(0x14100C, a));
      const float c = h / 3;
      for (int row = 0; row < 3; row++)
        for (int col = 0; col * c < r.right - r.left; col++)
          if ((row + col) & 1) cv_->fillRect(rectF(r.left + col * c, r.top + row * c, r.left + (col + 1) * c, r.top + (row + 1) * c),
                                             rgb(0xE07A12, 0.85f * a));
      break;
    }
    default: {  // (night: SMB2's dream, or the collection's sky)
      cv_->fillGradient(r, gameArt_ == 3 ? rgb(0x2A1B5C, a) : rgb(0x10224A, a), gameArt_ == 3 ? rgb(0x5B3A9E, a) : rgb(0x2B4C8C, a));
      uint32_t seed = 7;
      for (int k = 0; k < 28; k++) {
        seed = seed * 1103515245u + 12345u;
        const float sx = r.left + (seed >> 8) % 1000 / 1000.0f * (r.right - r.left);
        seed = seed * 1103515245u + 12345u;
        const float sy = r.top + (seed >> 8) % 1000 / 1000.0f * h;
        const float sr = h * (0.025f + ((seed >> 4) % 3) * 0.012f);
        drawStar(sx, sy, sr, rgb(0xFFF1A8, 0.8f * a), rgb(0xFFF1A8, 0), 0.0f);
      }
      break;
    }
  }
  // (a shade along the bottom, the panel's colour)
  cv_->fillGradient(rectF(r.left, r.bottom - h * 0.35f, r.right, r.bottom), rgb(kPanel, 0), rgb(kPanel, 0.65f * a));
  cv_->popClip();
}

void Menu::text(const std::wstring& s, RectF r, float size, bool bold, ColorF c,
                TextAlign align, ParaAlign palign) {
  text(s, r, size, bold ? kWeightSemiBold : kWeightNormal, c, align, palign);
}

void Menu::text(const std::wstring& s, RectF r, float size, int weight, ColorF c,
                TextAlign align, ParaAlign palign) {
  if (!cv_) return;
  if (style == kRetro) pixelText(s, r, size * 0.78f, c, align, palign);  // (pixels read larger: a little smaller)
  else cv_->text(s, r, size, style == kStorybook && weight < kWeightSemiBold ? kWeightSemiBold : weight, c, align, palign);
}

float Menu::measure(const std::wstring& s, float size, bool bold) {
  return measure(s, size, bold ? kWeightSemiBold : kWeightNormal);
}

float Menu::measure(const std::wstring& s, float size, int weight) {
  if (!cv_ || s.empty()) return 0.0f;
  if (style == kRetro) return pixelMeasure(s, size * 0.78f);
  return cv_->measure(s, size, style == kStorybook && weight < kWeightSemiBold ? kWeightSemiBold : weight);
}

void Menu::fillRound(RectF r, float radius, ColorF c) { roundRect(r, radius, c); }

void Menu::outlineRound(RectF r, float radius, float width, ColorF c) {
  if (cv_) cv_->strokeRound(r, radius, width, c);
}

void Menu::roundRect(RectF r, float radius, ColorF c) {
  if (cv_) cv_->fillRound(r, radius, c);
}

void Menu::push(std::shared_ptr<MenuPage> p) {
  if (p->rebuild) p->rebuild(*p);
  if (p->sel >= (int)p->items.size()) p->sel = 0;
  stack_.push_back(p);
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const Toast& t) { return t.hint; }),
                toasts_.end());
  if (!p->items.empty() && !selectable(p->items[p->sel])) moveSel(1);
}

void Menu::pop() {
  if (stack_.empty()) return;
  if (stack_.back()->onLeave) stack_.back()->onLeave();
  stack_.pop_back();
  refresh();
}

void Menu::closeAll() {
  while (!stack_.empty()) pop();
}

void Menu::refresh() {
  MenuPage* p = top();
  if (p && p->rebuild) {
    int sel = p->sel;
    p->rebuild(*p);
    p->sel = std::min(sel, (int)p->items.size() - 1);
    if (p->sel < 0) p->sel = 0;
    if (!p->items.empty() && !selectable(p->items[p->sel])) moveSel(1);
  }
}

void Menu::confirm(const std::string& question, std::function<void()> yes, const std::string& detail) {
  auto p = std::make_shared<MenuPage>();
  p->title = question;
  p->subtitle = detail;
  p->items.push_back(MenuItem::action("Yes", [this, yes]() {
    auto y = yes;  // (popping destroys this item)
    pop();
    y();
  }));
  p->items.push_back(MenuItem::action("No", [this]() { pop(); }));
  p->sel = 1;
  push(p);
}

bool Menu::selectable(const MenuItem& it) const {
  if (it.kind == MenuItem::Label || it.kind == MenuItem::Separator) return false;
  if (it.enabled && !it.enabled()) return false;
  return true;
}

void Menu::moveSel(int dir) {
  MenuPage* p = top();
  if (!p || p->items.empty()) return;
  int n = (int)p->items.size();
  int s = p->sel;
  for (int i = 0; i < n; i++) {
    s = (s + dir + n) % n;
    if (selectable(p->items[s])) {
      p->sel = s;
      return;
    }
  }
}

// Callbacks are copied before they run: an action may close the menu or
// rebuild the page, which destroys the item (and the original std::function).
void Menu::activate(MenuItem& it) {
  if (it.onActivate) {  // an explicit action wins (e.g. play the chosen number)
    auto f = it.onActivate;
    f();
    return;
  }
  switch (it.kind) {
    case MenuItem::Toggle:
      if (it.set) {
        auto set = it.set;
        set(it.get() ? 0 : 1);
      }
      break;
    case MenuItem::Choice:
      adjust(it, 1);
      break;
    default: break;
  }
}

void Menu::adjust(MenuItem& it, int dir) {
  if (!it.get || !it.set) return;
  auto set = it.set;  // see activate()
  int v = it.get();
  if (it.kind == MenuItem::Toggle) {
    set(v ? 0 : 1);
  } else if (it.kind == MenuItem::Choice) {
    int n = (int)it.options().size();
    if (n <= 0) return;
    v += dir;
    if (it.wrap) v = (v + n) % n;
    else v = std::clamp(v, 0, n - 1);
    set(v);
  } else if (it.kind == MenuItem::Slider) {
    set(std::clamp(v + dir * it.step, it.minV, it.maxV));
  }
}

void Menu::tickToasts(float dt) {
  time_ += dt;
  for (auto& t : toasts_) t.time -= dt;
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const Toast& t) { return t.time <= 0; }),
                toasts_.end());
  if (!banners_.empty()) {
    banners_.front().t += dt;
    if (banners_.front().t > 0.35f + 4.0f + 0.5f) banners_.erase(banners_.begin());
  }
}

void Menu::update(const MenuInput& in, float dt) {
  if (stack_.empty() || capturing) return;
  // Keep the page (and the closures its items hold) alive until we return,
  // even if an action closes the menu.
  std::shared_ptr<MenuPage> keep = stack_.back();
  MenuPage* p = keep.get();

  // Auto-repeat for held directions
  int dir = in.up ? -1 : in.down ? 1 : in.left ? -2 : in.right ? 2 : 0;
  bool fire = false;
  if (dir != repeatDir_) {
    repeatDir_ = dir;
    repeatTimer_ = 0.38f;
    fire = dir != 0;
  } else if (dir != 0) {
    repeatTimer_ -= dt;
    if (repeatTimer_ <= 0) {
      repeatTimer_ = 0.07f;
      fire = true;
    }
  }
  MenuInput e;
  if (fire) {
    e.up = dir == -1;
    e.down = dir == 1;
    e.left = dir == -2;
    e.right = dir == 2;
  }
  e.confirm = in.confirm;
  e.back = in.back;
  e.tabLeft = in.tabLeft;
  e.tabRight = in.tabRight;

  if (e.up || e.down) p->freeScroll = false;
  if (p->inputCustom && p->inputCustom(*this, e)) return;
  if (e.back) {
    pop();
    return;
  }
  if (p->items.empty()) return;
  if (e.up) moveSel(-1);
  if (e.down) moveSel(1);
  MenuItem& it = p->items[p->sel];
  if (!selectable(it)) return;
  if (e.left || e.right) {
    adjust(it, e.left ? -1 : 1);
    return;  // the setter may have rebuilt the page; `it` is no longer valid
  }
  if (e.confirm) activate(it);
}

void Menu::notify(const std::string& text, float seconds, bool hint) {
  if (!toasts_.empty() && toasts_.back().text == text) {
    toasts_.back().time = toasts_.back().total = seconds;
    return;
  }
  toasts_.push_back({text, seconds, seconds, hint});
  if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
}

void Menu::achievement(const std::string& name, const std::string& desc) { banners_.push_back({name, desc, 0.0f}); }

// In play: top right, below SMB1's HUD rows (scanlines 16-31, so from about
// 15.5% of the height) and the FPS counter, stacking down. With the menu open
// the top holds titles, so toasts sit bottom centre over the key hints.
void Menu::drawToasts(int w, int h, float s) {
  bool bottom = open();
  float size = bottom ? 19 * s : 24 * s;
  float lh = size * kLine, th = lh * 1.5f;
  float y = bottom ? h - th - h * 0.012f : std::max(64 * s, h * 0.155f);
  for (size_t i = 0; i < toasts_.size(); i++) {
    const Toast& t = toasts_[bottom ? toasts_.size() - 1 - i : i];  // newest nearest the edge
    float a = std::min(1.0f, t.time * 3.0f) * std::min(1.0f, (t.total - t.time) * 6.0f + 0.2f);
    std::wstring ws = widen(t.text);
    float tw = std::min(measure(ws, size, false), w * 0.8f);
    float bw = tw + h * 0.05f;
    RectF r = bottom ? rectF((w - bw) * 0.5f, y, (w + bw) * 0.5f, y + th)
                           : rectF(w - bw - 16 * s, y, w - 16 * s, y + th);
    const Look& L = kLooks[style];
    const float rr = style == kRetro ? 0.0f : th * 0.3f;
    roundRect(r, rr, rgb(L.toastBg, 0.94f * a));
    if (style == kStorybook) outlineRound(r, rr, std::max(2.0f, h / 360.0f), rgb(kInk, a));
    if (style == kRetro) cv_->strokeRect(r, std::max(2.0f, h / 360.0f), rgb(L.toastText, a));
    roundRect(rectF(r.left, r.top + th * 0.2f, r.left + std::max(3.0f, h / 300.0f), r.top + th * 0.8f), 2.0f,
              withA(accent_, a));
    text(ws, rectF(r.left + h * 0.025f, r.top, r.right - h * 0.02f, r.bottom), size, false, rgb(L.toastText, a));
    y += bottom ? -th * 1.15f : th * 1.15f;
  }
}

// "Achievement unlocked": a small card (a star, the name and what it was for)
// slides down at the top, stays a few seconds and fades.
void Menu::drawBanner(int w, int h, float s) {
  if (banners_.empty()) return;
  const Banner& b = banners_.front();
  const float kIn = 0.35f, kStay = 4.0f;
  float a = b.t < kIn ? b.t / kIn : b.t > kIn + kStay ? std::max(0.0f, 1.0f - (b.t - kIn - kStay) / 0.5f) : 1.0f;
  float slide = b.t < kIn ? 1.0f - (1.0f - a) * (1.0f - a) : 1.0f;
  const std::wstring head = L"Achievement unlocked", name = widen(b.name), desc = widen(b.desc);
  float ss = 19 * s, sb = 26 * s;
  float ls = ss * kLine, lb = sb * kLine, pad = ls * 0.55f;
  float textH = ls + lb + (desc.empty() ? 0.0f : ls);
  float icon = textH * 0.78f;
  float maxTw = w * 0.86f - icon - pad * 3;
  float nameS = std::min(1.0f, maxTw / std::max(1.0f, measure(name, sb, true)));
  float descS = std::min(1.0f, maxTw / std::max(1.0f, measure(desc, ss, false)));
  float tw = std::max({measure(head, ss, false), measure(name, sb, true) * nameS, measure(desc, ss, false) * descS});
  float bw = icon + tw + pad * 3, bh = textH + pad * 1.6f;
  RectF r = rectF((w - bw) * 0.5f, h * 0.025f - (1 - slide) * (bh + h * 0.025f), 0, 0);
  r.right = r.left + bw;
  r.bottom = r.top + bh;
  const bool story = style == kStorybook, retro = style == kRetro;
  roundRect(r, retro ? 0.0f : bh * 0.2f, rgb(story ? 0xFFF8E4 : retro ? 0x000000 : 0x141824, 0.95f * a));
  if (retro) cv_->strokeRect(r, std::max(2.0f, h / 300.0f), rgb(kRetroGold, a));
  else outlineRound(r, bh * 0.2f, std::max(story ? 3.0f : 1.5f, h / (story ? 300.0f : 480.0f)), rgb(story ? kInk : kGold, a));
  RectF ic = rectF(r.left + pad * 1.1f, r.top + (bh - icon) * 0.5f, 0, 0);
  ic.right = ic.left + icon;
  ic.bottom = ic.top + icon;
  roundRect(ic, icon * 0.5f, rgb(0x2B2617, a));
  cv_->strokeEllipse((ic.left + ic.right) * 0.5f, (ic.top + ic.bottom) * 0.5f, icon * 0.5f, icon * 0.5f,
                     std::max(1.5f, h / 540.0f), rgb(kGold, a));
  drawStar((ic.left + ic.right) * 0.5f, (ic.top + ic.bottom) * 0.5f, icon * 0.34f, rgb(0xFFD83D, a),
           rgb(0xB8860B, a), std::max(1.0f, h / 900.0f));
  float tx = ic.right + pad, y = r.top + pad * 0.8f;
  text(head, rectF(tx, y, r.right, y + ls), ss, false, rgb(story ? 0xB8860B : kGold, a), kAlignLeading,
       kParaNear);
  y += ls;
  text(name, rectF(tx, y, r.right, y + lb), sb * nameS, true, rgb(story ? kInk : 0xFFFFFF, a), kAlignLeading,
       kParaNear);
  y += lb;
  if (!desc.empty())
    text(desc, rectF(tx, y, r.right, y + ls), ss * descS, false, rgb(story ? 0x6B4E32 : kSub, a), kAlignLeading,
         kParaNear);
}

// A five-pointed star (point up) of outer radius r, drawn as a shape so it sits
// exactly in the middle: its box, not its circumcircle, is centred on (cx, cy)
// (the lower points reach only cos 36° = 0.809 r below the centre).
void Menu::drawStar(float cx, float cy, float r, ColorF fill, ColorF edge, float edgeW) {
  const float kPi = 3.14159265f, inner = r * 0.42f, oy = cy + r * (1.0f - 0.809f) * 0.5f;
  PointF pts[10];
  for (int i = 0; i < 10; i++) {
    float ang = -kPi / 2 + i * kPi / 5, rr = i % 2 ? inner : r;
    pts[i] = {cx + rr * std::cos(ang), oy + rr * std::sin(ang)};
  }
  cv_->fillPolygon(pts, 10, fill);
  cv_->strokePolygon(pts, 10, edgeW, edge);
}

void Menu::drawOsd(int w, int h, const std::string& fps, const std::string& status) {
  if (!cv_) return;
  float s = std::max(h / 1080.0f, 0.55f);
  auto card = [&](RectF r, const std::string& t, ColorF c) {
    roundRect(r, (r.bottom - r.top) * 0.3f, rgb(0x181C26, 0.85f));
    text(widen(t), r, 20 * s, true, c, kAlignCenter);
  };
  if (!status.empty()) {  // (as wide as its text: online play's lines are longer)
    const float tw = std::max(220 * s, measure(widen(status), 20 * s, true) + 36 * s);
    card(rectF(16 * s, 16 * s, 16 * s + tw, 52 * s), status, rgb(0xFFD866));
  }
  if (!fps.empty()) card(rectF(w - 196 * s, 16 * s, w - 16 * s, 52 * s), fps, rgb(0x9BE59B));
  drawToasts(w, h, s);
  drawBanner(w, h, s);
}

void Menu::draw(int w, int h, float anim) {
  MenuPage* p = top();
  if (!p || !cv_) return;
  scale_ = std::max(h / 1080.0f, 0.55f);
  const float s = scale_, W = (float)w, H = (float)h;
  const float a = std::clamp(anim, 0.0f, 1.0f);
  const Look& L = kLooks[std::clamp(style, 0, kStyleCount - 1)];
  const bool story = style == kStorybook, retro = style == kRetro, themed = style == kThemedDark;
  const float sizeTitle = (story ? 56 : retro ? 44 : themed ? 54 : 48) * s, sizeBody = 26 * s, sizeSmall = 19 * s;
  const float lhTitle = sizeTitle * kLine, lhBody = sizeBody * kLine, lhSmall = sizeSmall * kLine;
  const float ink = std::max(2.0f, H / 300.0f);  // (Storybook's outline width)

  // Dim the game
  if (story) cv_->fillRect(rectF(0, 0, W, H), colorF(0.10f, 0.22f, 0.50f, 0.55f * a));  // (a sky-blue wash)
  else cv_->fillRect(rectF(0, 0, W, H), colorF(0.02f, 0.027f, 0.047f, (retro ? 0.8f : 0.6f) * a));

  // The panel
  const bool wide = p->wide || p->listFraction < 0.999f;
  float pw = std::min({W * (wide ? 0.8f : 0.62f), H * (wide ? 1.5f : 1.15f), W - 32 * s});
  float ph = H * 0.84f;
  RectF panel = rectF((W - pw) * 0.5f, (H - ph) * 0.5f + (1 - a) * 24 * s, 0, 0);
  panel.right = panel.left + pw;
  panel.bottom = panel.top + ph;
  const float rad = retro ? 0.0f : story ? H * 0.03f : H * 0.018f, pad = H * 0.035f;
  if (story) {
    // a cream card, a thick dark outline and a solid shadow below right
    roundRect(rectF(panel.left + ink * 2.5f, panel.top + ink * 3.5f, panel.right + ink * 2.5f, panel.bottom + ink * 3.5f),
              rad, rgb(kInk, 0.75f * a));
    roundRect(panel, rad, rgb(L.panel, L.panelAlpha * a));
    outlineRound(panel, rad, ink * 1.6f, rgb(kInk, a));
  } else if (retro) {
    cv_->fillRect(panel, rgb(L.panel, L.panelAlpha * a));
  } else {
    roundRect(rectF(panel.left + rad * 0.3f, panel.top + rad * 0.5f, panel.right + rad * 0.3f,
                    panel.bottom + rad * 0.5f),
              rad, rgb(0, 0.5f * a));
    roundRect(panel, rad, rgb(L.panel, L.panelAlpha * a));
    outlineRound(panel, rad, std::max(1.5f, H / 540.0f), withA(mix(accent_, rgb(kPanel), 0.35f), a));
  }

  // Long titles and hints shrink to fit the panel instead of running out of it.
  auto fit = [&](const std::wstring& t, float size, bool bold, float room) {
    return size * std::min(1.0f, room / std::max(1.0f, measure(t, size, bold)));
  };
  const float tx = panel.left + pad, inner = pw - pad * 2;
  float y = panel.top + pad * 0.8f;
  if (retro) {  // brick bars along the top and the bottom
    const float bb = std::max(6.0f, H * 0.022f);
    drawBricks(rectF(panel.left, panel.top, panel.right, panel.top + bb * 2), rgb(kBrick), rgb(0x000000), bb, a);
    drawBricks(rectF(panel.left, panel.bottom - bb * 2, panel.right, panel.bottom), rgb(kBrick), rgb(0x000000), bb, a);
    y += bb * 1.6f;
  }
  float headTop = y;
  if (themed) {  // the game's pattern behind the breadcrumb and the title
    // (a banner inset in the panel, under the breadcrumb and the title)
    const float hb = (stack_.size() > 1 ? lhSmall : 0.0f) + lhTitle + pad * 0.6f, m = pad * 0.3f;
    const RectF band = rectF(panel.left + m, panel.top + m, panel.right - m, panel.top + m + hb);
    drawHeaderArt(band, a);
    cv_->strokeRect(band, std::max(1.0f, H / 720.0f), withA(mix(accent_, rgb(kPanel), 0.35f), a));
    headTop = band.bottom;
  }
  if (stack_.size() > 1) {  // breadcrumb: the pages this one was opened from
    std::wstring crumb;
    for (size_t i = 0; i + 1 < stack_.size(); i++) crumb += widen(stack_[i]->title) + L"  »  ";
    text(crumb, rectF(tx, y, panel.right - pad, y + lhSmall), fit(crumb, sizeSmall, false, inner), false,
         rgb(themed ? 0xE8ECF4 : L.crumb, a));
    y += lhSmall;
  }
  std::wstring title = widen(p->title);
  float titleRoom = inner;
  closeHit_ = RectF{};
  if (touchUi) {
    // (top right: back, or out of the menu)
    const float cs = lhTitle * 0.9f;
    closeHit_ = rectF(panel.right - pad - cs, y + (lhTitle - cs) * 0.5f, panel.right - pad, y + (lhTitle + cs) * 0.5f);
    if (story) {
      roundRect(closeHit_, cs * 0.5f, rgb(0xFFD400, a));
      outlineRound(closeHit_, cs * 0.5f, ink, rgb(kInk, a));
    } else {
      roundRect(closeHit_, retro ? 0.0f : cs * 0.5f, rgb(retro ? 0x3C3C3C : 0x262B38, a));
    }
    text(stack_.size() > 1 ? L"\x2190" : L"\x00D7", closeHit_, sizeBody, true, rgb(story ? kInk : kText, a), kAlignCenter);
    titleRoom -= cs + pad * 0.5f;
  }
  if (style == kClassic) {
    text(title, rectF(tx, y, panel.right - pad, y + lhTitle), fit(title, sizeTitle, true, titleRoom), true,
         withA(accent_, a));
  } else {
    // the title in the display face: Storybook's in the game's colour, outlined;
    // Themed Dark's white over the game's pattern; Retro's gold pixels
    const std::wstring& t = title;
    const float ts = sizeTitle * std::min(1.0f, titleRoom / std::max(1.0f, displayMeasure(t, sizeTitle)));
    if (story)
      displayText(t, rectF(tx, y, panel.right - pad, y + lhTitle), ts, withA(accent_, a),
                  rgb(kInk, a), ink * 1.2f);
    else if (themed)
      displayText(t, rectF(tx, y, panel.right - pad, y + lhTitle), ts, rgb(0xFFFFFF, a), rgb(0x0B0D12, 0.85f * a), ink);
    else
      displayText(t, rectF(tx, y, panel.right - pad, y + lhTitle), ts, rgb(kRetroGold, a), rgb(0, 0), 0.0f);
  }
  y += lhTitle;
  if (themed) y = std::max(y, headTop);
  if (!p->subtitle.empty()) {
    std::wstring sub = widen(p->subtitle);
    text(sub, rectF(tx, y - lhSmall * 0.15f, panel.right - pad, y + lhSmall * 0.85f),
         fit(sub, sizeSmall, false, inner), false, rgb(L.sub, a));
    y += lhSmall;
  }
  y += pad * 0.35f;
  if (story) cv_->fillRect(rectF(tx, y, panel.right - pad, y + std::max(2.0f, H / 400.0f)), rgb(kInk, 0.25f * a));
  else if (!retro && !themed)
    cv_->fillRect(rectF(tx, y, panel.right - pad, y + std::max(1.0f, H / 720.0f)), withA(accent_, 0.35f * a));
  y += pad * 0.45f;

  // The list (beside a custom panel on pages that have one)
  // (the footer: the selected row's description, wrapped onto up to three lines
  // at a readable size rather than shrunk onto one, then the keys)
  const float sizeHelp = 22 * s, lhHelp = sizeHelp * kLine;
  const float row = lhBody * 1.32f, footerH = lhHelp * 3.1f + lhSmall * 1.75f + (retro ? H * 0.04f : 0.0f);
  const float listTop = y, listBottom = panel.bottom - footerH - pad * 0.5f;
  const float listW = inner * p->listFraction;
  const float vx = tx + listW;  // values end here
  const int visible = std::max(1, (int)((listBottom - listTop) / row));
  const int n = (int)p->items.size();
  // Keep the selection in view (smooth scroll); on the last selectable row,
  // also bring the read-only rows after it (e.g. Options' Hotkeys) into view.
  float target = p->scroll;
  if (n > 0 && !p->freeScroll) {
    if (p->sel < target + 1) target = (float)std::max(0, p->sel - 1);
    if (p->sel > target + visible - 2) target = (float)(p->sel - visible + 2);
    int last = -1;
    for (int i = n - 1; i >= 0 && last < 0; i--)
      if (selectable(p->items[i])) last = i;
    if (p->sel == last) target = std::max(target, std::min((float)p->sel, (float)(n - visible)));
  }
  target = std::clamp(target, 0.0f, (float)std::max(0, n - visible));
  p->scroll += (target - p->scroll) * 0.35f;
  if (std::fabs(target - p->scroll) < 0.01f) p->scroll = target;

  cv_->pushClip(rectF(panel.left, listTop - row * 0.1f, panel.right, listBottom + row * 0.1f));
  rowHits_.clear();
  rowH_ = row;
  int first = (int)std::floor(p->scroll);
  for (int i = first; i < n && i < first + visible + 1; i++) {
    float ry = listTop + (i - p->scroll) * row;
    if (ry < listTop - row * 0.5f || ry + row > listBottom + row * 0.5f) continue;
    MenuItem& it = p->items[i];
    if (ry + row * 0.5f >= listTop && ry + row * 0.5f <= listBottom)
      rowHits_.push_back({rectF(panel.left + pad * 0.5f, ry, vx + pad * 0.5f, ry + row), i});
    const bool en = !it.enabled || it.enabled();
    const bool sel = i == p->sel && selectable(it);
    if (it.kind == MenuItem::Separator) {
      if (!it.label.empty()) {
        const RectF hr = rectF(tx, ry + row - lhSmall * 1.25f, vx, ry + row - lhSmall * 0.05f);
        if (story) displayText(widen(it.label), hr, sizeSmall * 1.25f, withA(accent_, a), rgb(0, 0), 0.0f);
        else if (retro) text(widen(it.label), hr, sizeSmall, true, rgb(kRetroGold, a));
        else text(widen(it.label), hr, sizeSmall, true, withA(accent_, 0.9f * a));
      }
      continue;
    }
    // the selection: Storybook's yellow pill (outlined) with a star; Retro's
    // gold arrow; Themed Dark's tinted bar with a star; Classic's tinted bar
    float cursor = 0;  // (room the cursor takes before the label)
    if (sel) {
      const RectF hr = rectF(panel.left + pad * 0.5f, ry + row * 0.06f, vx + pad * 0.5f, ry + row * 0.94f);
      const float bh = hr.bottom - hr.top;
      if (story) {
        roundRect(hr, bh * 0.5f, rgb(0xFFD400, a));
        outlineRound(hr, bh * 0.5f, ink, rgb(kInk, a));
        const float bob = std::sin(time_ * 5.0f) * bh * 0.04f;
        drawStar(hr.left + bh * 0.55f, (hr.top + hr.bottom) * 0.5f + bob, bh * 0.3f, rgb(0xFFFFFF, a), rgb(kInk, a),
                 ink * 0.7f);
        cursor = bh * 0.75f;
      } else if (retro) {
        const float cs = sizeBody * 0.45f, cx = tx + std::sin(time_ * 6.0f) * cs * 0.12f, cy = ry + row * 0.5f;
        const PointF tri[3] = {{cx, cy - cs * 0.7f}, {cx + cs, cy}, {cx, cy + cs * 0.7f}};
        cv_->fillPolygon(tri, 3, rgb(kRetroGold, a));
        cursor = cs * 1.8f;
      } else {
        float pulse = 0.22f + 0.06f * std::sin(time_ * 4.0f);
        roundRect(hr, row * 0.2f, withA(accent_, pulse * a));
        if (themed) {
          drawStar(hr.left + bh * 0.5f, (hr.top + hr.bottom) * 0.5f, bh * 0.26f, rgb(0xFFD83D, a), rgb(0xB8860B, a),
                   std::max(1.0f, H / 900.0f));
          cursor = bh * 0.6f;
        } else {
          roundRect(rectF(hr.left, hr.top + bh * 0.18f, hr.left + std::max(3.0f, H / 300.0f), hr.top + bh * 0.82f),
                    2.0f, withA(accent_, a));
        }
      }
    }
    ColorF col = rgb(!en ? L.dim : sel ? L.rowSel : L.text, a);
    ColorF vcol = rgb(!en ? L.dim : sel ? L.rowSel : L.value, a);
    const float lx = tx + (sel ? std::max(cursor, story || retro || themed ? 0.0f : pad * 0.35f) : retro ? sizeBody * 0.81f : 0);
    const RectF rr = rectF(lx, ry, vx, ry + row);
    std::wstring label = widen(it.label);
    // the value on the right, and what the label may use
    std::wstring val;
    float valRoom = 0;  // space the value's control takes
    switch (it.kind) {
      case MenuItem::Toggle: {
        bool on = it.get && it.get();
        float sh = lhBody * 0.62f, sw = sh * 1.9f;
        RectF sr = rectF(vx - sw, ry + (row - sh) * 0.5f, vx, ry + (row + sh) * 0.5f);
        if (retro) {
          // a ? block when on, a used block when off
          const float bs = sh * 1.15f;
          RectF b = rectF(vx - bs, ry + (row - bs) * 0.5f, vx, ry + (row + bs) * 0.5f);
          cv_->fillRect(b, rgb(on ? kRetroGold : 0x7A3C10, (en ? 1.0f : 0.4f) * a));
          cv_->strokeRect(b, std::max(2.0f, bs * 0.08f), rgb(on ? kBrick : 0x3A1A08, a));
          if (on) pixelText(L"?", b, bs * 0.75f, rgb(kBrick, a), kAlignCenter, kParaCenter);
          sw = bs;
        } else {
          const bool lit = on;
          const ColorF track = lit ? (story ? rgb(0x43B047, (en ? 1.0f : 0.4f) * a) : withA(accent_, (en ? 1.0f : 0.4f) * a))
                                   : rgb(L.off, a);
          roundRect(sr, sh * 0.5f, track);
          if (story) outlineRound(sr, sh * 0.5f, ink * 0.8f, rgb(kInk, a));
          float kx = on ? sr.right - sh : sr.left;
          const RectF knob = rectF(kx + sh * 0.12f, sr.top + sh * 0.12f, kx + sh * 0.88f, sr.top + sh * 0.88f);
          roundRect(knob, sh * 0.38f, rgb(L.knob, a));
          if (story) outlineRound(knob, sh * 0.38f, ink * 0.6f, rgb(kInk, a));
        }
        valRoom = sw + sh * 0.7f;
        if (it.valueText) {  // a note beside the switch (what the setting needs, ...)
          std::wstring note = widen(it.valueText());
          float right = vx - sw - sh * 0.7f, left = lx + measure(label, sizeBody, sel) + sh;
          float room = std::max(0.0f, right - left);
          float w0 = measure(note, sizeBody, false);
          if (!note.empty() && room > w0 * 0.35f)
            text(note, rectF(left, ry, right, ry + row), sizeBody * std::min(1.0f, room / std::max(1.0f, w0)), false,
                 vcol, kAlignTrailing);
          valRoom = std::max(valRoom, vx - left);
        }
        break;
      }
      case MenuItem::Choice: {
        if (it.valueText) val = widen(it.valueText());
        else if (it.get && it.options) {
          auto o = it.options();
          int v = it.get();
          val = widen((v >= 0 && v < (int)o.size()) ? o[v] : "?");
        }
        if (sel) val = L"◀  " + val + L"  ▶";
        break;
      }
      case MenuItem::Slider: {
        int c = it.get ? it.get() : 0;
        std::wstring txt = it.valueText ? widen(it.valueText()) : std::to_wstring(c) + widen(it.suffix);
        float bw = pw * 0.22f, bh = std::max(3.0f, H / 200.0f);
        float numW = measure(L"000 ms", sizeBody, false) + pad * 0.45f;  // (the value, clear of the knob)
        float f = it.maxV > it.minV ? (float)(c - it.minV) / (it.maxV - it.minV) : 0;
        f = std::clamp(f, 0.0f, 1.0f);
        if (retro) {
          // ten blocks, lit up to the value
          const float gap = bh, cell = (bw - gap * 9) / 10, ch = cell * 0.9f;
          const int lit = (int)std::lround(f * 10);
          for (int k = 0; k < 10; k++) {
            const float bx = vx - numW - bw + k * (cell + gap);
            cv_->fillRect(rectF(bx, ry + (row - ch) * 0.5f, bx + cell, ry + (row + ch) * 0.5f),
                          rgb(k < lit ? (en ? kRetroGold : 0x7C7C7C) : 0x3C3C3C, a));
          }
        } else {
          const float th = story ? bh * 2.2f : bh;
          RectF br = rectF(vx - bw - numW, ry + row * 0.5f - th * 0.5f, vx - numW, ry + row * 0.5f + th * 0.5f);
          roundRect(br, th * 0.5f, rgb(L.off, a));
          roundRect(rectF(br.left, br.top, br.left + bw * f, br.bottom), th * 0.5f,
                    en ? withA(accent_, a) : rgb(L.dim, a));
          if (story) outlineRound(br, th * 0.5f, ink * 0.7f, rgb(kInk, a));
          float kx = br.left + bw * f;
          const float kr = story ? th * 0.95f : bh * 1.4f;
          const RectF knob = rectF(kx - kr, ry + row * 0.5f - kr, kx + kr, ry + row * 0.5f + kr);
          roundRect(knob, kr, rgb(L.knob, a));
          if (story) outlineRound(knob, kr, ink * 0.7f, rgb(kInk, a));
        }
        text(txt, rr, sizeBody, false, vcol, kAlignTrailing);
        valRoom = bw + numW + bh * 2;
        break;
      }
      case MenuItem::Bind:
        if (capturing && sel) {
          val = L"Press…";
          vcol = retro ? rgb(kRetroGold, a) : story ? rgb(0xC0281E, a) : withA(accent_, a);
        } else if (it.valueText) {
          val = widen(it.valueText());
        }
        break;
      default:
        if (it.valueText) val = widen(it.valueText());
        break;
    }
    if (retro && sel) col = vcol = rgb(kRetroGold, a);
    float labelRoom = vx - lx - valRoom - (val.empty() ? 0.0f : measure(val, sizeBody, false) + pad * 0.5f);
    float labelSize = fit(label, sizeBody, sel, std::max(labelRoom, (vx - lx) * 0.3f));
    text(label, rectF(lx, ry, vx, ry + row), labelSize, sel, col);
    if (!val.empty()) {
      float room = std::max(vx - lx - measure(label, labelSize, sel) - pad * 0.5f, (vx - lx) * 0.25f);
      text(val, rr, fit(val, sizeBody, false, room), false, vcol, kAlignTrailing);
    }
  }
  cv_->popClip();

  // scroll indicators
  if (p->scroll > 0.05f)
    text(L"↑", rectF(tx, listTop - lhSmall * 1.05f, tx + listW, listTop), sizeSmall, false, rgb(L.crumb, a),
         kAlignCenter);
  if (p->scroll + visible < n - 0.05f)
    text(L"↓", rectF(tx, listBottom - lhSmall * 0.3f, tx + listW, listBottom + lhSmall * 0.7f), sizeSmall,
         false, rgb(L.crumb, a), kAlignCenter);

  if (p->drawCustom) {
    RectF cr = p->listFraction < 0.999f ? rectF(vx + pad, listTop, panel.right - pad, listBottom)
                                              : rectF(tx, listTop, panel.right - pad, listBottom);
    p->drawCustom(*this, cv_, cr);
  }

  // Footer: what the selected row does, and the keys
  float fy = panel.bottom - footerH;
  if (story) cv_->fillRect(rectF(tx, fy, panel.right - pad, fy + std::max(2.0f, H / 400.0f)), rgb(kInk, 0.25f * a));
  else if (!retro)
    cv_->fillRect(rectF(tx, fy, panel.right - pad, fy + std::max(1.0f, H / 720.0f)), colorF(1, 1, 1, 0.19f * a));
  std::wstring help;
  if (capturing) help = widen(captureText);
  else if (n > 0 && p->sel < n && selectable(p->items[p->sel])) help = widen(p->items[p->sel].help);
  if (!help.empty()) {
    // words onto lines no wider than the panel; a fourth line's worth goes on the third, shrunk to fit
    std::vector<std::wstring> lines(1);
    size_t at = 0;
    while (at < help.size()) {
      size_t sp = help.find(L' ', at);
      std::wstring word = help.substr(at, sp == std::wstring::npos ? std::wstring::npos : sp - at);
      at = sp == std::wstring::npos ? help.size() : sp + 1;
      if (word.empty()) continue;
      std::wstring tryLine = lines.back().empty() ? word : lines.back() + L" " + word;
      if (lines.size() < 3 && !lines.back().empty() && measure(tryLine, sizeHelp, false) > inner) lines.push_back(word);
      else lines.back() = tryLine;
    }
    const ColorF hc = capturing ? (retro ? rgb(kRetroGold, a) : withA(accent_, a)) : rgb(L.hint, a);
    float ly = fy + lhHelp * 0.2f;
    for (const std::wstring& l : lines) {
      text(l, rectF(tx, ly, panel.right - pad, ly + lhHelp), fit(l, sizeHelp, false, inner), false, hc);
      ly += lhHelp * 0.95f;
    }
  }
  std::wstring keys = !keysHint.empty() ? keysHint
                     : confirmRight     ? L"↑↓ Select      ←→ Change      Enter / Ⓑ Confirm      Esc / Ⓐ Back"
                                        : L"↑↓ Select      ←→ Change      Enter / Ⓐ Confirm      Esc / Ⓑ Back";
  const float ky = panel.bottom - lhSmall * 1.45f - (retro ? H * 0.04f : 0.0f);
  text(keys, rectF(tx, ky, panel.right - pad, ky + lhSmall), fit(keys, sizeSmall, false, inner), false, rgb(L.keys, a));
}

// A tap: the close button backs out; a row is chosen and used (a slider
// moves the way of the half tapped).
void Menu::tap(float x, float y) {
  if (stack_.empty() || capturing) return;
  std::shared_ptr<MenuPage> keep = stack_.back();
  MenuPage* p = keep.get();
  auto in = [&](const RectF& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
  if (closeHit_.right > closeHit_.left && in(closeHit_)) {
    pop();
    return;
  }
  for (const RowHit& h : rowHits_) {
    if (!in(h.r) || h.index >= (int)p->items.size()) continue;
    MenuItem& it = p->items[h.index];
    if (!selectable(it)) return;
    p->sel = h.index;
    if (it.kind == MenuItem::Slider) adjust(it, x < (h.r.left + h.r.right) * 0.5f ? -1 : 1);
    else if (it.kind == MenuItem::Choice && !it.onActivate && x < h.r.left + (h.r.right - h.r.left) * 0.5f)
      adjust(it, -1);
    else activate(it);
    return;
  }
}

void Menu::drag(float dy) {
  MenuPage* p = top();
  if (!p || dy == 0) return;
  p->freeScroll = true;
  p->scroll = std::max(0.0f, std::min(p->scroll - dy / std::max(rowH_, 1.0f), (float)std::max(0, (int)p->items.size() - 1)));
}
