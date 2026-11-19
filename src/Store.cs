// 数据：便签（每张一个 json）、回收站、全局设置、旧数据迁移
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows.Media;

namespace DeskNote
{
    enum WinMode { Desktop = 0, Topmost = 1, Normal = 2 }

    // ---------------------------------------------------------------------
    // JSON：读用系统自带的 JavaScriptSerializer，写自己拼（要缩进、要保持字段顺序）
    // ---------------------------------------------------------------------
    class JObj : List<KeyValuePair<string, object>>
    {
        public JObj Set(string k, object v) { Add(new KeyValuePair<string, object>(k, v)); return this; }
    }

    static class Json
    {
        public static IDictionary<string, object> Parse(string s)
        {
            var o = new System.Web.Script.Serialization.JavaScriptSerializer { MaxJsonLength = int.MaxValue }.DeserializeObject(s);
            var d = o as IDictionary<string, object>;
            if (d == null) throw new FormatException("顶层不是对象");
            return d;
        }

        public static IDictionary<string, object> Obj(IDictionary<string, object> d, string k)
        {
            object o; return d != null && d.TryGetValue(k, out o) ? o as IDictionary<string, object> : null;
        }
        public static IEnumerable Arr(IDictionary<string, object> d, string k)
        {
            object o;
            if (d != null && d.TryGetValue(k, out o) && o is IEnumerable && !(o is string)) return (IEnumerable)o;
            return new object[0];
        }
        public static string Str(IDictionary<string, object> d, string k, string def)
        {
            object o; return d != null && d.TryGetValue(k, out o) && o is string ? (string)o : def;
        }
        public static double Num(IDictionary<string, object> d, string k, double def)
        {
            object o;
            if (d == null || !d.TryGetValue(k, out o) || o == null || o is string || o is bool) return def;
            try { return Convert.ToDouble(o, CultureInfo.InvariantCulture); } catch { return def; }
        }
        public static bool Bool(IDictionary<string, object> d, string k, bool def)
        {
            object o; return d != null && d.TryGetValue(k, out o) && o is bool ? (bool)o : def;
        }

        public static string Write(object v)
        {
            var sb = new StringBuilder();
            WriteValue(sb, v, 0);
            sb.Append("\r\n");
            return sb.ToString();
        }

        // 字段都是简单值、且不超过 3 个的对象写成一行（比如每个条目）
        static bool IsInline(JObj o)
        {
            if (o.Count > 3) return false;
            foreach (var kv in o) if (kv.Value is JObj || kv.Value is IList) return false;
            return true;
        }

        static void WriteValue(StringBuilder sb, object v, int depth)
        {
            string pad = new string(' ', depth * 2), pad1 = new string(' ', depth * 2 + 2);
            if (v == null) sb.Append("null");
            else if (v is string) sb.Append(Str((string)v));
            else if (v is bool) sb.Append((bool)v ? "true" : "false");
            else if (v is double || v is int || v is long) sb.Append(Convert.ToDouble(v).ToString("R", CultureInfo.InvariantCulture));
            else if (v is JObj)
            {
                var o = (JObj)v;
                if (o.Count == 0) { sb.Append("{}"); return; }
                if (IsInline(o))
                {
                    sb.Append("{ ");
                    for (int i = 0; i < o.Count; i++)
                    {
                        if (i > 0) sb.Append(", ");
                        sb.Append(Str(o[i].Key)).Append(": ");
                        WriteValue(sb, o[i].Value, depth + 1);
                    }
                    sb.Append(" }");
                    return;
                }
                sb.Append("{\r\n");
                for (int i = 0; i < o.Count; i++)
                {
                    sb.Append(pad1).Append(Str(o[i].Key)).Append(": ");
                    WriteValue(sb, o[i].Value, depth + 1);
                    sb.Append(i < o.Count - 1 ? ",\r\n" : "\r\n");
                }
                sb.Append(pad).Append('}');
            }
            else if (v is IList)
            {
                var l = (IList)v;
                if (l.Count == 0) { sb.Append("[]"); return; }
                sb.Append("[\r\n");
                for (int i = 0; i < l.Count; i++)
                {
                    sb.Append(pad1);
                    WriteValue(sb, l[i], depth + 1);
                    sb.Append(i < l.Count - 1 ? ",\r\n" : "\r\n");
                }
                sb.Append(pad).Append(']');
            }
            else sb.Append(Str(v.ToString()));
        }

        public static string Str(string s)
        {
            var sb = new StringBuilder("\"");
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else sb.Append(c);
                        break;
                }
            }
            return sb.Append('"').ToString();
        }

        public static string ColorStr(Color c) { return string.Format("#{0:X2}{1:X2}{2:X2}", c.R, c.G, c.B); }
        public static Color ParseColor(string s, Color def)
        {
            try { return s == null ? def : (Color)ColorConverter.ConvertFromString(s); } catch { return def; }
        }
    }

    // ---------------------------------------------------------------------
    // 便签
    // ---------------------------------------------------------------------
    class NoteItem
    {
        public string Text = "";
        public bool Done;
        public DateTime? Remind;   // 提醒时间（本地时间，精确到分钟）
        public bool Reminded;      // 已经提醒过
        public const string TimeFmt = "yyyy-MM-dd HH:mm";

        public NoteItem Clone() { return new NoteItem { Text = Text, Done = Done, Remind = Remind, Reminded = Reminded }; }
        public JObj ToJson()
        {
            var o = new JObj().Set("text", Text).Set("done", Done);
            if (Remind.HasValue) o.Set("remind", Remind.Value.ToString(TimeFmt, CultureInfo.InvariantCulture)).Set("reminded", Reminded);
            return o;
        }
        public static NoteItem From(object o)
        {
            var d = o as IDictionary<string, object>;
            if (d != null)
            {
                var it = new NoteItem { Text = Json.Str(d, "text", "").Replace("\r\n", "\n"), Done = Json.Bool(d, "done", false) };
                DateTime t;
                if (DateTime.TryParseExact(Json.Str(d, "remind", ""), TimeFmt, CultureInfo.InvariantCulture, DateTimeStyles.None, out t))
                {
                    it.Remind = t;
                    it.Reminded = Json.Bool(d, "reminded", false);
                }
                return it;
            }
            var s = o as string; // 也接受直接写字符串
            return s != null ? new NoteItem { Text = s.Replace("\r\n", "\n") } : null;
        }
    }

    class NoteData
    {
        public static readonly Color DefaultTextColor = Color.FromRgb(0xA9, 0xB1, 0xB7);
        public static readonly Color DefaultBgColor = Color.FromRgb(0x34, 0x42, 0x4D);

        public string Id;
        public string Title = "ToDo";
        public List<NoteItem> Items = new List<NoteItem>();
        // 外观
        public double Opacity = 0.85, FontSize = 14.5;
        public int FontWeight = 400;
        public Color TextColor = DefaultTextColor, BgColor = DefaultBgColor;
        public bool Shadow, Checkboxes;
        // 窗口：位置用物理像素（多屏不同缩放时才准），大小用 WPF 单位
        public WinMode Mode = WinMode.Desktop;
        public bool Collapsed;
        public double? X, Y;
        public double Width = 340, Height = 420;

        public static string NewId() { return DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N").Substring(0, 6); }

        // 新便签沿用这张的外观和窗口模式
        public NoteData CloneStyle()
        {
            return new NoteData
            {
                Id = NewId(), Title = "新便签", Opacity = Opacity, FontSize = FontSize, FontWeight = FontWeight,
                TextColor = TextColor, BgColor = BgColor, Shadow = Shadow, Checkboxes = Checkboxes,
                Mode = Mode, Width = Width, Height = Height
            };
        }

        public JObj ToJson()
        {
            var items = new List<object>();
            foreach (var it in Items) items.Add(it.ToJson());
            var style = new JObj()
                .Set("opacity", Opacity).Set("fontSize", FontSize).Set("fontWeight", FontWeight)
                .Set("textColor", Json.ColorStr(TextColor)).Set("bgColor", Json.ColorStr(BgColor))
                .Set("shadow", Shadow).Set("checkboxes", Checkboxes);
            var win = new JObj()
                .Set("mode", Mode == WinMode.Topmost ? "topmost" : Mode == WinMode.Normal ? "normal" : "desktop")
                .Set("collapsed", Collapsed)
                .Set("x", X.HasValue ? (object)Math.Round(X.Value) : null).Set("y", Y.HasValue ? (object)Math.Round(Y.Value) : null)
                .Set("width", Math.Round(Width, 1)).Set("height", Math.Round(Height, 1));
            return new JObj().Set("title", Title).Set("items", items).Set("style", style).Set("window", win);
        }

        // 只含内容和外观（不含窗口位置），用于撤销和比较是否改动
        public string ContentJson()
        {
            var j = ToJson();
            j.RemoveAll(kv => kv.Key == "window");
            return Json.Write(j);
        }

        // 用另一份数据覆盖内容和外观（窗口位置、模式不动）
        public void CopyContentFrom(NoteData s)
        {
            Title = s.Title; Items = s.Items;
            Opacity = s.Opacity; FontSize = s.FontSize; FontWeight = s.FontWeight;
            TextColor = s.TextColor; BgColor = s.BgColor; Shadow = s.Shadow; Checkboxes = s.Checkboxes;
        }

        public static NoteData FromContentJson(string json, string id) { return From(Json.Parse(json), id); }

        public static NoteData From(IDictionary<string, object> d, string id)
        {
            var n = new NoteData { Id = id };
            var t = Json.Str(d, "title", "").Trim();
            if (t.Length > 0) n.Title = t;
            foreach (var o in Json.Arr(d, "items"))
            {
                var it = NoteItem.From(o);
                if (it != null && it.Text.Trim().Length > 0) n.Items.Add(it);
            }
            var s = Json.Obj(d, "style");
            n.Opacity = Math.Max(0, Math.Min(1, Json.Num(s, "opacity", n.Opacity)));
            n.FontSize = Math.Max(8, Math.Min(60, Json.Num(s, "fontSize", n.FontSize)));
            n.FontWeight = (int)Math.Max(100, Math.Min(900, Json.Num(s, "fontWeight", n.FontWeight)));
            n.TextColor = Json.ParseColor(Json.Str(s, "textColor", null), n.TextColor);
            n.BgColor = Json.ParseColor(Json.Str(s, "bgColor", null), n.BgColor);
            n.Shadow = Json.Bool(s, "shadow", false);
            n.Checkboxes = Json.Bool(s, "checkboxes", false);
            var w = Json.Obj(d, "window");
            var mode = Json.Str(w, "mode", "desktop");
            n.Mode = mode == "topmost" ? WinMode.Topmost : mode == "normal" ? WinMode.Normal : WinMode.Desktop;
            n.Collapsed = Json.Bool(w, "collapsed", false);
            double x = Json.Num(w, "x", double.NaN), y = Json.Num(w, "y", double.NaN);
            if (!double.IsNaN(x) && !double.IsNaN(y)) { n.X = x; n.Y = y; }
            n.Width = Math.Max(160, Json.Num(w, "width", n.Width));
            n.Height = Math.Max(120, Json.Num(w, "height", n.Height));
            return n;
        }
    }

    // ---------------------------------------------------------------------
    // 回收站：删掉的条目和整张便签，不自动清理
    // ---------------------------------------------------------------------
    class TrashEntry
    {
        public string Kind = "item";       // item | note
        public DateTime DeletedAt = DateTime.Now;
        public string NoteId, NoteTitle;    // 条目来自哪张便签
        public int Index;                   // 条目原来的位置
        public NoteItem Item;
        public NoteData Note;               // 整张便签

        public JObj ToJson()
        {
            var o = new JObj().Set("kind", Kind).Set("deletedAt", DeletedAt.ToString("yyyy-MM-dd HH:mm:ss"));
            if (Kind == "note") o.Set("noteId", Note.Id).Set("note", Note.ToJson());
            else o.Set("noteId", NoteId).Set("noteTitle", NoteTitle).Set("index", Index).Set("item", Item.ToJson());
            return o;
        }

        public static TrashEntry From(IDictionary<string, object> d)
        {
            var e = new TrashEntry { Kind = Json.Str(d, "kind", "item") };
            DateTime t;
            if (DateTime.TryParse(Json.Str(d, "deletedAt", ""), CultureInfo.InvariantCulture, DateTimeStyles.None, out t)) e.DeletedAt = t;
            if (e.Kind == "note")
            {
                var nd = Json.Obj(d, "note");
                if (nd == null) return null;
                e.Note = NoteData.From(nd, Json.Str(d, "noteId", NoteData.NewId()));
            }
            else
            {
                object o;
                e.Item = d.TryGetValue("item", out o) ? NoteItem.From(o) : null;
                if (e.Item == null) return null;
                e.NoteId = Json.Str(d, "noteId", null);
                e.NoteTitle = Json.Str(d, "noteTitle", "");
                e.Index = (int)Json.Num(d, "index", 0);
            }
            return e;
        }
    }

    // ---------------------------------------------------------------------
    // 全局设置
    // ---------------------------------------------------------------------
    class GlobalSettings
    {
        public bool HotkeyEnabled = true;
        public string Hotkey = "Ctrl+Alt+N";
        // 首次运行时是否已处理过开机自启（全新安装默认打开；老用户不动）
        public bool AutoStartInit;
        // 同步到 gopan
        public bool SyncEnabled;
        public string SyncServer = "", SyncFolder = "/DeskNote", SyncKey = "";
    }

    // ---------------------------------------------------------------------
    // 文件读写
    // ---------------------------------------------------------------------
    static class Store
    {
        public static string Dir, NotesDir, TrashFile, SettingsFile;
        // 自己最后写入每个便签文件的内容，用来区分外部修改
        static readonly Dictionary<string, string> lastWritten = new Dictionary<string, string>();
        static readonly HashSet<string> deleted = new HashSet<string>();

        public static void Init(string dir)
        {
            Dir = dir;
            NotesDir = Path.Combine(Dir, "notes");
            TrashFile = Path.Combine(Dir, "trash.json");
            SettingsFile = Path.Combine(Dir, "settings.json");
            Directory.CreateDirectory(NotesDir);
        }

        public static void Log(string msg)
        {
            try
            {
                Directory.CreateDirectory(Dir);
                File.AppendAllText(Path.Combine(Dir, "error.log"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss ") + msg + "\r\n");
            }
            catch { }
        }

        // 先写临时文件再替换，写到一半断电也不会把原文件弄坏
        public static void WriteAtomic(string path, string content)
        {
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, content, new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(tmp, path, null);
            else File.Move(tmp, path);
        }

        public static string NotePath(string id) { return Path.Combine(NotesDir, id + ".json"); }

        public static List<NoteData> LoadNotes()
        {
            var list = new List<NoteData>();
            foreach (var f in Directory.GetFiles(NotesDir, "*.json"))
            {
                string id = Path.GetFileNameWithoutExtension(f);
                try
                {
                    string content = File.ReadAllText(f, Encoding.UTF8);
                    list.Add(NoteData.From(Json.Parse(content), id));
                    lastWritten[id] = content;
                }
                catch (Exception ex) { Log("读取便签失败 " + f + "：" + ex.Message); }
            }
            list.Sort((a, b) => string.CompareOrdinal(a.Id, b.Id));
            return list;
        }

        public static void SaveNote(NoteData n)
        {
            try
            {
                // 已删除（移进回收站）的便签不再写回，防止关窗时保存位置把文件又写出来
                if (deleted.Contains(n.Id)) return;
                string content = Json.Write(n.ToJson());
                string last;
                if (lastWritten.TryGetValue(n.Id, out last) && last == content) return;
                lastWritten[n.Id] = content;
                WriteAtomic(NotePath(n.Id), content);
            }
            catch (Exception ex)
            {
                Log("保存便签失败：" + ex);
                System.Windows.MessageBox.Show("保存失败：" + ex.Message, "DeskNote");
            }
        }

        public static void DeleteNoteFile(string id)
        {
            deleted.Add(id);
            try { File.Delete(NotePath(id)); lastWritten.Remove(id); }
            catch (Exception ex) { Log("删除便签文件失败：" + ex.Message); }
        }

        // 外部程序改了便签文件：返回新内容；是自己写的或读不了就返回 null
        public static NoteData ReadIfChangedExternally(string id)
        {
            string path = NotePath(id), content = null;
            for (int i = 0; i < 5 && content == null; i++)
            {
                try { content = File.ReadAllText(path, Encoding.UTF8); }
                catch (FileNotFoundException) { return null; }
                catch (IOException) { System.Threading.Thread.Sleep(80); } // 对方可能还没写完
                catch (Exception) { return null; }
            }
            string last;
            if (content == null || (lastWritten.TryGetValue(id, out last) && last == content)) return null;
            try
            {
                var n = NoteData.From(Json.Parse(content), id);
                lastWritten[id] = content;
                return n;
            }
            catch (Exception ex) { Log(id + ".json 格式有误，暂不重新读入：" + ex.Message); return null; }
        }

        public static bool IsKnown(string id) { return lastWritten.ContainsKey(id); }

        // 从回收站恢复时解除删除标记
        public static void Revive(string id) { deleted.Remove(id); }
        public static bool IsDeleted(string id) { return deleted.Contains(id); }

        // ---- 回收站 ----
        public static List<TrashEntry> LoadTrash()
        {
            var list = new List<TrashEntry>();
            try
            {
                if (!File.Exists(TrashFile)) return list;
                var d = Json.Parse(File.ReadAllText(TrashFile, Encoding.UTF8));
                foreach (var o in Json.Arr(d, "entries"))
                {
                    var e = o as IDictionary<string, object>;
                    var te = e != null ? TrashEntry.From(e) : null;
                    if (te != null) list.Add(te);
                }
            }
            catch (Exception ex) { Log("读取回收站失败：" + ex.Message); }
            return list;
        }

        public static void SaveTrash(List<TrashEntry> list)
        {
            try
            {
                var arr = new List<object>();
                foreach (var e in list) arr.Add(e.ToJson());
                WriteAtomic(TrashFile, Json.Write(new JObj().Set("entries", arr)));
            }
            catch (Exception ex) { Log("保存回收站失败：" + ex); }
        }

        // ---- 全局设置 ----
        public static GlobalSettings LoadSettings()
        {
            var s = new GlobalSettings();
            try
            {
                if (!File.Exists(SettingsFile)) return s;
                var d = Json.Parse(File.ReadAllText(SettingsFile, Encoding.UTF8));
                s.HotkeyEnabled = Json.Bool(d, "hotkeyEnabled", s.HotkeyEnabled);
                s.Hotkey = Json.Str(d, "hotkey", s.Hotkey);
                s.AutoStartInit = Json.Bool(d, "autoStartInit", false);
                s.SyncEnabled = Json.Bool(d, "syncEnabled", false);
                s.SyncServer = Json.Str(d, "syncServer", "");
                s.SyncFolder = Json.Str(d, "syncFolder", s.SyncFolder);
                s.SyncKey = Unprotect(Json.Str(d, "syncKey", ""));
            }
            catch (Exception ex) { Log("读取设置失败：" + ex.Message); }
            return s;
        }

        public static void SaveSettings(GlobalSettings s)
        {
            try
            {
                WriteAtomic(SettingsFile, Json.Write(new JObj()
                    .Set("hotkeyEnabled", s.HotkeyEnabled).Set("hotkey", s.Hotkey).Set("autoStartInit", s.AutoStartInit)
                    .Set("syncEnabled", s.SyncEnabled).Set("syncServer", s.SyncServer).Set("syncFolder", s.SyncFolder)
                    .Set("syncKey", Protect(s.SyncKey))));
            }
            catch (Exception ex) { Log("保存设置失败：" + ex.Message); }
        }

        // 密钥用 Windows 的 DPAPI 加密保存：只有当前 Windows 用户能解开，文件被拷走也读不出来
        static string Protect(string plain)
        {
            if (string.IsNullOrEmpty(plain)) return "";
            var b = System.Security.Cryptography.ProtectedData.Protect(Encoding.UTF8.GetBytes(plain), null,
                System.Security.Cryptography.DataProtectionScope.CurrentUser);
            return "dpapi:" + Convert.ToBase64String(b);
        }

        static string Unprotect(string s)
        {
            if (string.IsNullOrEmpty(s) || !s.StartsWith("dpapi:")) return "";
            try
            {
                var b = System.Security.Cryptography.ProtectedData.Unprotect(Convert.FromBase64String(s.Substring(6)), null,
                    System.Security.Cryptography.DataProtectionScope.CurrentUser);
                return Encoding.UTF8.GetString(b);
            }
            catch (Exception ex) { Log("解不开保存的同步密钥（换了电脑或用户？）：" + ex.Message); return ""; }
        }

        // ---- 每日备份：backups\yyyy-MM-dd\ 里放一份当天第一次检查时的全部数据，保留最近 KeepBackups 天 ----
        public const int KeepBackups = 14;
        public static string BackupsDir { get { return Path.Combine(Dir, "backups"); } }

        // 今天已经备份过就什么也不做；返回新建的备份目录或 null
        public static string DailyBackup()
        {
            try
            {
                string target = Path.Combine(BackupsDir, DateTime.Now.ToString("yyyy-MM-dd"));
                if (Directory.Exists(target)) return null;
                string tmp = target + ".partial";
                if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
                Directory.CreateDirectory(Path.Combine(tmp, "notes"));
                foreach (var f in Directory.GetFiles(NotesDir, "*.json"))
                    File.Copy(f, Path.Combine(tmp, "notes", Path.GetFileName(f)));
                if (File.Exists(TrashFile)) File.Copy(TrashFile, Path.Combine(tmp, "trash.json"));
                if (File.Exists(SettingsFile)) File.Copy(SettingsFile, Path.Combine(tmp, "settings.json"));
                Directory.Move(tmp, target); // 复制完整了才改成正式名字

                var dirs = new List<string>();
                foreach (var dd in Directory.GetDirectories(BackupsDir))
                {
                    DateTime t;
                    if (DateTime.TryParseExact(Path.GetFileName(dd), "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out t)) dirs.Add(dd);
                }
                dirs.Sort(StringComparer.Ordinal);
                for (int i = 0; i < dirs.Count - KeepBackups; i++) Directory.Delete(dirs[i], true);
                return target;
            }
            catch (Exception ex) { Log("每日备份失败：" + ex.Message); return null; }
        }

        // ---- 从上一版（单张便签：note.json + settings.txt）迁移 ----
        public static void MigrateLegacy()
        {
            string oldNote = Path.Combine(Dir, "note.json"), oldSettings = Path.Combine(Dir, "settings.txt");
            if (!File.Exists(oldNote) || Directory.GetFiles(NotesDir, "*.json").Length > 0) return;
            try
            {
                var n = NoteData.From(Json.Parse(File.ReadAllText(oldNote, Encoding.UTF8)), NoteData.NewId());
                if (File.Exists(oldSettings))
                {
                    // 上一版按主屏缩放比例保存 WPF 单位的位置，换算成物理像素
                    double scale = 1;
                    using (var g = System.Drawing.Graphics.FromHwnd(IntPtr.Zero)) scale = g.DpiX / 96.0;
                    double left = double.NaN, top = double.NaN;
                    foreach (var line in File.ReadAllLines(oldSettings))
                    {
                        var kv = line.Split(new[] { '=' }, 2);
                        if (kv.Length != 2) continue;
                        double v;
                        bool isNum = double.TryParse(kv[1], NumberStyles.Float, CultureInfo.InvariantCulture, out v);
                        switch (kv[0])
                        {
                            case "Left": if (isNum) left = v; break;
                            case "Top": if (isNum) top = v; break;
                            case "Width": if (isNum) n.Width = Math.Max(160, v); break;
                            case "Height": if (isNum) n.Height = Math.Max(120, v); break;
                            case "Opacity": if (isNum) n.Opacity = Math.Max(0, Math.Min(1, v)); break;
                            case "FontSize": if (isNum) n.FontSize = v; break;
                            case "FontWeight": if (isNum) n.FontWeight = (int)v; break;
                            case "TextShadow": if (isNum) n.Shadow = v != 0; break;
                            case "Mode": if (isNum && v >= 0 && v <= 2) n.Mode = (WinMode)(int)v; break;
                            case "Collapsed": if (isNum) n.Collapsed = v != 0; break;
                            case "TextColor": n.TextColor = Json.ParseColor(kv[1], n.TextColor); break;
                            case "BgColor": n.BgColor = Json.ParseColor(kv[1], n.BgColor); break;
                        }
                    }
                    if (!double.IsNaN(left) && !double.IsNaN(top)) { n.X = left * scale; n.Y = top * scale; }
                }
                SaveNote(n);
                File.Move(oldNote, oldNote + ".old");
                if (File.Exists(oldSettings)) File.Move(oldSettings, oldSettings + ".old");
            }
            catch (Exception ex) { Log("迁移旧数据失败：" + ex); }
        }
    }
}
