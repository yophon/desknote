// 便签窗口：撤销/重做、条目里的链接、提醒
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;

namespace DeskNote
{
    partial class NoteWindow
    {
        // =====================================================================
        // 撤销 / 重做：每一步记下改动前的整张便签内容（不含窗口位置）。
        // 一步里删进回收站的条目也记下来，撤销时从回收站拿掉，重做时放回去
        // =====================================================================
        class UndoStep
        {
            public string State;
            public List<TrashEntry> Trashed = new List<TrashEntry>();
        }

        const int MaxUndo = 50;
        readonly List<UndoStep> undoStack = new List<UndoStep>();
        readonly List<UndoStep> redoStack = new List<UndoStep>();
        UndoStep pendingStep;
        int changeDepth;

        // 一次操作开始（可以嵌套，最外层结束时才记一步）
        void BeginChange()
        {
            if (changeDepth++ == 0) pendingStep = new UndoStep { State = d.ContentJson() };
        }

        void EndChange()
        {
            if (changeDepth == 0) return;
            if (--changeDepth > 0) return;
            var p = pendingStep;
            pendingStep = null;
            if (p != null && (p.Trashed.Count > 0 || p.State != d.ContentJson()))
            {
                undoStack.Add(p);
                if (undoStack.Count > MaxUndo) undoStack.RemoveAt(0);
                redoStack.Clear();
            }
            // 操作期间别处改了这张便签：以外部内容为准（别处的改动丢了就找不回来）
            if (pendingReload != null && !Editing && dragIndex < 0)
            {
                var pr = pendingReload;
                pendingReload = null;
                ApplyExternal(pr);
                Render();
            }
        }

        void ClearUndo()
        {
            undoStack.Clear();
            redoStack.Clear();
        }

        public bool CanUndo { get { return undoStack.Count > 0; } }

        public void Undo()
        {
            CommitEdit(true);
            if (undoStack.Count == 0) return;
            var s = undoStack[undoStack.Count - 1];
            undoStack.RemoveAt(undoStack.Count - 1);
            redoStack.Add(new UndoStep { State = d.ContentJson(), Trashed = s.Trashed });
            ApplyState(s.State);
            app.UntrashEntries(s.Trashed);
        }

        public void Redo()
        {
            CommitEdit(true);
            if (redoStack.Count == 0) return;
            var s = redoStack[redoStack.Count - 1];
            redoStack.RemoveAt(redoStack.Count - 1);
            undoStack.Add(new UndoStep { State = d.ContentJson(), Trashed = s.Trashed });
            ApplyState(s.State);
            app.RetrashEntries(s.Trashed);
        }

        void ApplyState(string json)
        {
            pendingStep = null; // 从菜单里点的撤销：菜单关闭时别再把“撤销”本身记成一步
            d.CopyContentFrom(NoteData.FromContentJson(json, d.Id));
            ApplyOpacity();
            ApplyShadow();
            Save();
            Render();
        }

        void OnPreviewKey(object s, KeyEventArgs e)
        {
            if (Editing) return; // 编辑框里的 Ctrl+Z 交给编辑框自己
            var m = Keyboard.Modifiers;
            Key k = e.Key == Key.System ? e.SystemKey : e.Key;
            if (m == ModifierKeys.Control && k == Key.Z) { Undo(); e.Handled = true; }
            else if ((m == ModifierKeys.Control && k == Key.Y) || (m == (ModifierKeys.Control | ModifierKeys.Shift) && k == Key.Z)) { Redo(); e.Handled = true; }
        }

        // =====================================================================
        // 链接：条目里的网址加下划线，按住 Ctrl 单击用浏览器打开
        // （不按 Ctrl 时照常拖动窗口、双击编辑）
        // =====================================================================
        static readonly Regex UrlRx = new Regex(@"(https?://|www\.)[^\s<>""'，。；：、！？（）【】《》「」]+", RegexOptions.IgnoreCase);

        public static List<KeyValuePair<string, string>> SplitLinks(string text)
        {
            // 返回 (文字, 链接或 null) 片段
            var parts = new List<KeyValuePair<string, string>>();
            int pos = 0;
            foreach (Match m in UrlRx.Matches(text))
            {
                if (m.Index < pos) continue;
                string url = m.Value.TrimEnd('.', ',', ';', ':', '!', '?', ')', ']', '}');
                if (url.Length < 8) continue;
                if (m.Index > pos) parts.Add(new KeyValuePair<string, string>(text.Substring(pos, m.Index - pos), null));
                string target = url.StartsWith("www.", StringComparison.OrdinalIgnoreCase) ? "http://" + url : url;
                parts.Add(new KeyValuePair<string, string>(url, target));
                pos = m.Index + url.Length;
            }
            if (pos < text.Length) parts.Add(new KeyValuePair<string, string>(text.Substring(pos), null));
            return parts;
        }

        void AddTextWithLinks(TextBlock tb, string text)
        {
            foreach (var p in SplitLinks(text))
            {
                if (p.Value == null) { tb.Inlines.Add(new Run(p.Key)); continue; }
                tb.Inlines.Add(new Run(p.Key)
                {
                    TextDecorations = TextDecorations.Underline, Tag = p.Value,
                    ToolTip = "按住 Ctrl 单击打开\n" + p.Value
                });
            }
        }

        bool TryOpenLink(MouseButtonEventArgs e)
        {
            if ((Keyboard.Modifiers & ModifierKeys.Control) == 0) return false;
            var run = e.OriginalSource as Run;
            if (run == null)
            {
                var tb = e.OriginalSource as TextBlock;
                if (tb != null)
                {
                    var tp = tb.GetPositionFromPoint(e.GetPosition(tb), false);
                    if (tp != null) run = tp.Parent as Run;
                }
            }
            if (run == null || !(run.Tag is string)) return false;
            try { Process.Start((string)run.Tag); }
            catch (Exception ex) { Store.Log("打开链接失败：" + ex.Message); }
            return true;
        }

        // =====================================================================
        // 提醒
        // =====================================================================
        static readonly Brush DueBrush = new SolidColorBrush(Color.FromRgb(0xE5, 0x5B, 0x5B));

        public static string FormatWhen(DateTime t)
        {
            var today = DateTime.Today;
            if (t.Date == today) return t.ToString("HH:mm");
            if (t.Date == today.AddDays(1)) return "明天 " + t.ToString("HH:mm");
            if (t.Year == today.Year) return t.ToString("M月d日 HH:mm");
            return t.ToString("yyyy-M-d HH:mm");
        }

        Inline ReminderTag(NoteItem it)
        {
            var t = it.Remind.Value;
            bool due = it.Reminded && !it.Done;
            return new Run("  ⏰ " + FormatWhen(t))
            {
                FontSize = Math.Round(d.FontSize * 0.85, 1),
                Foreground = due ? DueBrush : DimBrush,
                ToolTip = (due ? "提醒时间已到：" : "提醒：") + t.ToString("yyyy-MM-dd HH:mm")
            };
        }

        void EditReminder(int index)
        {
            if (index < 0 || index >= d.Items.Count) return;
            var it = d.Items[index];
            var dlg = new ReminderDialog(it.Text, it.Remind) { Icon = app.WindowIcon };
            if (dlg.ShowDialog() != true) { SendToBottom(); return; }
            if (index >= d.Items.Count || d.Items[index] != it) return; // 对话框开着时条目被别处改了
            BeginChange();
            it.Remind = dlg.Result;
            it.Reminded = false;
            Save();
            EndChange();
            Render();
            SendToBottom();
        }

        // 程序检查到这张便签有提醒到点了（Reminded 已经标好）
        public void OnRemindersFired()
        {
            Save();
            if (Editing) return; // 重绘会打断正在输入的内容，等编辑结束自然会刷新
            Render();
        }
    }
}
