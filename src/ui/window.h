// 窗口和输入接口（跨平台）：界面代码只通过它开窗口、收事件。
// 坐标：窗口里的都是 DIP（客户区左上角为原点）；屏幕上的是物理像素。
// Windows 实现在 win/window_win.cpp。
#pragma once
#include "gfx.h"

struct Window;     // 一个顶层窗口（平台定义）
struct TextField;  // 窗口里的一个原生单行输入框（输入法、读屏靠系统控件才可靠）

// ---- 按键：数值沿用 Windows 的虚拟键码（字母、数字就是大写字符本身）----
enum Key
{
    KeyBack = 0x08, KeyTab = 0x09, KeyEnter = 0x0D, KeyShift = 0x10, KeyCtrl = 0x11, KeyAlt = 0x12, KeyPause = 0x13,
    KeyEscape = 0x1B, KeySpace = 0x20, KeyPageUp = 0x21, KeyPageDown = 0x22, KeyEnd = 0x23, KeyHome = 0x24,
    KeyLeft = 0x25, KeyUp = 0x26, KeyRight = 0x27, KeyDown = 0x28, KeyInsert = 0x2D, KeyDelete = 0x2E,
    KeyLWin = 0x5B, KeyRWin = 0x5C, KeyApps = 0x5D, KeyF1 = 0x70, KeyF12 = 0x7B, KeyF24 = 0x87,
    KeyLShift = 0xA0, KeyRShift = 0xA1, KeyLCtrl = 0xA2, KeyRCtrl = 0xA3, KeyLAlt = 0xA4, KeyRAlt = 0xA5,
    KeySemicolon = 0xBA, KeyEqual = 0xBB, KeyComma = 0xBC, KeyMinus = 0xBD, KeyPeriod = 0xBE, KeySlash = 0xBF,
    KeyBackquote = 0xC0, KeyLBracket = 0xDB, KeyBackslash = 0xDC, KeyRBracket = 0xDD, KeyQuote = 0xDE,
};
enum { ModShift = 1, ModCtrl = 2, ModAlt = 4, ModWin = 8 }; // macOS 外壳把 Cmd 报成 ModCtrl
enum { MouseLeft = 1, MouseRight = 2, MouseMiddle = 3 };

enum EvType
{
    EvMouseMove,    // x, y
    EvMouseLeave,
    EvMouseDown,    // button, clicks（1 单击，2 双击）, x, y, mods
    EvMouseUp,      // button, x, y, mods
    EvWheel,        // wheel（向上为正，一格 = 1）, x, y
    EvKeyDown,      // key, mods；返回 true 表示处理了（平台就不再做对话框导航等默认处理）
    EvKeyUp,        // key, mods
    EvChar,         // ch
    EvTimer,        // timer
    EvResize,       // 客户区大小变了（Wnd::ClientSize 取新大小）
    EvScale,        // 换到了缩放比例不同的屏幕（Wnd::Scale 取新比例；窗口大小平台已经调好）
    EvTheme,        // 系统深浅色或强调色变了（平台已经调好标题栏）
    EvActivate,     // button：1 窗口变成活动窗口，0 不再是
    EvFocus,        // button：1 窗口本身得到键盘焦点，0 失去（焦点去了输入框也算失去）
    EvClose,        // 用户点了关闭（key = 0）或按了 Esc（key = KeyEscape）；返回 true 表示自己处理（不处理就销毁窗口）
    EvDestroy,      // 窗口没了：之后不能再用这个 Window*
    EvFieldChange,  // field 里的文字变了
    EvFieldFocus,   // field 得到焦点
    EvFieldBlur,    // field 失去焦点
    EvSubmit,       // 在输入框里按了回车（field 是当时有焦点的输入框，可能为空）
};

struct Event
{
    int type = 0;
    float x = 0, y = 0;
    int button = 0, clicks = 0;
    int key = 0, mods = 0;
    u32 ch = 0;
    float wheel = 0;
    int timer = 0;
    TextField* field = nullptr;
};

// 窗口内容和事件由界面代码实现
struct WinHandler
{
    virtual void Paint(Window* w, Painter* p, float width, float height) = 0;
    virtual bool OnEvent(Window* w, const Event& e) = 0;
};

struct WinDesc
{
    Str title;
    float minW = 0, minH = 0;     // 可以拖小到多小（客户区，DIP；0 = 不限）
    bool resizable = true;
    bool minimizable = true;
    bool topmost = false;
    bool appIcon = true;          // 标题栏、任务栏用程序图标
    void* owner = nullptr;        // 主人窗口（平台句柄）：不单独出现在任务栏，总在主人前面
};

struct PointI { int x, y; };
struct RectI { int left, top, right, bottom; };

enum CursorShape { CursorArrow, CursorHand, CursorIBeam, CursorMove, CursorSizeWE, CursorSizeNS, CursorSizeNWSE, CursorSizeNESW };

namespace Wnd
{
    // ---- 窗口 ----
    // 普通的对话框窗口（带标题栏，内容不透明）；创建后还没显示，也还没定大小，排好版后调 PlaceCentered
    Window* NewDialog(const WinDesc& d, WinHandler* h);
    // 客户区定为 w × h（DIP），放在鼠标所在屏幕的中间；高度最多占工作区的 maxFrac 再减 marginDip
    void PlaceCentered(Window* w, float width, float height, float maxFrac, float marginDip);
    // 模态：主人窗口先禁用，等 *done 变成 true 后销毁对话框
    void RunModal(Window* w, bool* done);
    void Destroy(Window* w);                  // 立刻销毁（会收到 EvDestroy）
    void Show(Window* w);                     // 显示并带到前台
    bool IsVisible(Window* w);
    void Invalidate(Window* w);               // 稍后重画
    void Update(Window* w);                   // 有要重画的就马上画（滚动时跟手）
    void Focus(Window* w);                    // 键盘焦点给窗口本身（输入框失去焦点）
    float Scale(Window* w);                   // 物理像素 / DIP
    void ClientSize(Window* w, float* width, float* height);
    RectI ScreenBounds(Window* w);            // 整个窗口在屏幕上的位置（物理像素）
    PointI ToScreen(Window* w, float x, float y); // 客户区 DIP -> 屏幕像素
    bool CursorPos(Window* w, float* x, float* y); // 鼠标在客户区里的位置（DIP）；返回是否在客户区内
    void SetTimer(Window* w, int id, int ms);
    void KillTimer(Window* w, int id);
    void Capture(Window* w, bool on);         // 按下后鼠标移出窗口也继续收事件
    void SetCursor(CursorShape c);
    void WantAllKeys(Window* w, bool on);     // 录快捷键时：Tab、Esc 也当普通按键发过来
    void* Native(Window* w);                  // 平台的窗口句柄（只给平台代码用）
    WinHandler* Handler(Window* w);

    // ---- 弹出窗口：无边框、整窗透明（自己画全部像素）、不抢焦点、总在最前（菜单、悬停预览）----
    Window* NewPopup(WinHandler* h, void* owner, int x, int y); // 建在屏幕 (x, y) 处（决定用哪块屏的缩放）；先不显示
    void SetBounds(Window* w, int x, int y, int width, int height); // 屏幕像素
    void ShowNoActivate(Window* w);
    // 马上画：画布 pw × ph 物理像素，EndPaint 时贴到窗口上（窗口大小跟着变）；BeginPaint 失败返回空，EndPaint 照样要调
    Painter* BeginPaint(Window* w, int pw, int ph);
    void EndPaint(Window* w);
    RectI WorkAreaAt(int x, int y);           // 屏幕上 (x, y) 所在那块屏的工作区（去掉任务栏），屏幕像素

    // ---- 弹出会话（菜单）：期间所有鼠标、键盘都交给它；点到外面、别的程序到了前台就结束 ----
    struct PopupLoop
    {
        bool done = false;
        bool keepOnForegroundChange = false; // 调界面用
        bool dragging = false;               // 拖滑块时别把鼠标捕获抢回去
        virtual void OnMouse(int type, int sx, int sy) = 0; // EvMouseMove / EvMouseDown / EvMouseUp，屏幕像素
        virtual void OnKey(int key) = 0;
        virtual bool Inside(int sx, int sy) = 0;           // 这一点在不在弹出的窗口上
    };
    // 先把 owner 带到前台（菜单的命令要回到它身上），然后一直跑到 loop->done
    void RunPopupLoop(Window* root, void* owner, PopupLoop* loop);

    // ---- 原生输入框 ----
    enum { FieldCenter = 1, FieldPassword = 2, FieldNoIme = 4 };
    TextField* CreateField(Window* w, const Str& text, int flags);
    void FieldSetBounds(TextField* f, RectF r); // 文字区域（客户区 DIP）：左右照 r，高度按字体行高、在 r 里垂直居中
    void FieldSetText(TextField* f, const Str& text);
    Str FieldText(TextField* f);
    void FieldFocus(TextField* f);
    bool FieldHasFocus(TextField* f);
    void FieldSelectAll(TextField* f);
    void FieldSetCue(TextField* f, const Str& cue); // 空着时显示的灰色提示
    // 字体（FontNote / FontUi）、字号（DIP）、粗细、文字颜色、平时和有焦点时的底色（不透明）；主题变了要重新设
    void FieldStyle(TextField* f, int font, float fontSize, int weight, u32 textColor, u32 bgRest, u32 bgFocus);
    void FieldSetMaxLen(TextField* f, int n);

    // ---- 交给系统 ----
    void OpenUrl(const Str& url);      // 默认浏览器打开
    void OpenFolder(const Str& path);  // 文件管理器打开

    // ---- 系统对话框 ----
    bool Confirm(Window* owner, const Str& title, const Str& text); // 确定 / 取消（警告样式）
}
