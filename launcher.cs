// SrmodLauncher: installs the mod, launches the game and edits srmod.ini (the mod reloads it live).
// Built with the .NET Framework csc.exe that ships with Windows (C# 5 syntax).
using System;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

static class Ini {
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern int GetPrivateProfileString(string s, string k, string d, StringBuilder r, int n, string f);
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern bool WritePrivateProfileString(string s, string k, string v, string f);
    public static string File;
    public static string Get(string s, string k, string d) {
        var b = new StringBuilder(1024); GetPrivateProfileString(s, k, d, b, b.Capacity, File); return b.ToString();
    }
    public static int GetI(string s, string k, int d) { int v; return int.TryParse(Get(s, k, ""), out v) ? v : d; }
    public static float GetF(string s, string k, float d) {
        float v; return float.TryParse(Get(s, k, ""), NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : d;
    }
    public static void Set(string s, string k, object v) {
        string str = v is float ? ((float)v).ToString("0.####", CultureInfo.InvariantCulture) : v.ToString();
        WritePrivateProfileString(s, k, str, File);
    }
}

class Widget {
    public string Id, Label, Sample; public bool On; public float X, Y; public int Anchor, Size; public Color Color;
    public string Sec { get { return "widget." + Id; } }
}

class LauncherForm : Form {
    // Must match the defaults in mod.cpp LoadSettings().
    static readonly string[] Ids = { "coords", "speed", "total_rta", "total_igt", "map_rta", "map_igt", "pb", "loads", "deaths", "category", "map" };
    static readonly string[] Labels = { "", "Speed ", "RTA ", "IGT ", "Map RTA ", "Map IGT ", "PB ", "Loads ", "Deaths ", "", "" };
    static readonly string[] Samples = { "X 1234.5  Y -678.9  Z 12.0", "320 u/s (max 455)", "1:23:45.67", "1:20:02.10", "4:05.33", "3:58.90", "4:01.00  -0:02.10", "12 (3:43.57)", "3", "Any%", "trainyard" };
    static readonly string[] Keys = { "toggle", "start_stop", "reset", "category", "save_pos", "teleport" };
    static readonly string[] KeyTitles = { "Показать/скрыть оверлей", "Старт/финиш рана", "Сброс", "Сменить категорию", "Сохранить позицию", "Телепорт на позицию" };
    static readonly Keys[] KeyDefaults = { System.Windows.Forms.Keys.F6, System.Windows.Forms.Keys.F7, System.Windows.Forms.Keys.F8, System.Windows.Forms.Keys.F10, System.Windows.Forms.Keys.NumPad7, System.Windows.Forms.Keys.NumPad9 };
    // Map file name (as the mod reads it) and display name, pairs.
    static readonly string[] Missions = {
        "trainyard", "Вокзал", "farm", "Укреплённая ферма", "caverns", "Пещеры", "church", "Церковь",
        "tavern", "Пивная", "hospital", "Госпиталь", "cannery", "Консервный завод", "digsite", "Раскоп",
        "airfield_east", "Аэродром — восток", "airfield_west", "Аэродром — запад", "castle", "Замок",
        "castle_top", "Вершина замка", "zeppelin", "Цеппелин", "blacksun", "Чёрное Солнце",
        "downtown", "Деловой центр", "downtown_radio", "Радиостанция", "downtown_west", "Запад делового центра",
        "mte", "Исторический центр — восток", "mte_para_hq", "База исследований СС", "mte_ss_hq", "Штаб-квартира СС",
        "mtw", "Исторический центр — запад", "mtw_off", "Квартира офицера", "mtw_ware", "Склад" };
    static readonly string[] Anchors = { "Левый верх", "Правый верх", "Левый низ", "Правый низ", "Центр" };

    string dir = AppDomain.CurrentDomain.BaseDirectory;
    Widget[] widgets = new Widget[Ids.Length];
    CheckedListBox list = new CheckedListBox();
    Panel preview = new DoubleBufferedPanel();
    TextBox labelBox = new TextBox();
    NumericUpDown sizeBox = new NumericUpDown();
    ComboBox anchorBox = new ComboBox();
    Button colorBtn = new Button();
    Label status = new Label();
    bool loading, allowCheck;
    int dragIdx = -1; Point dragLast;

    class DoubleBufferedPanel : Panel { public DoubleBufferedPanel() { DoubleBuffered = true; } }

    string P(string f) { return Path.Combine(dir, f); }
    Widget Sel { get { return list.SelectedIndex >= 0 ? widgets[list.SelectedIndex] : null; } }

    public LauncherForm() {
        Ini.File = P("srmod.ini");
        Text = "Wolfenstein Speedrun Mod"; Width = 1100; Height = 720; StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9.5f);

        for (int i = 0; i < Ids.Length; i++) {
            var w = new Widget { Id = Ids[i], Sample = Samples[i] };
            w.On = Ini.GetI(w.Sec, "enabled", Ids[i] != "map" ? 1 : 0) != 0;
            w.X = Ini.GetF(w.Sec, "x", 0.01f); w.Y = Ini.GetF(w.Sec, "y", 0.02f + 0.03f * i);
            w.Anchor = Ini.GetI(w.Sec, "anchor", 0); w.Size = Ini.GetI(w.Sec, "size", 22);
            w.Label = Ini.Get(w.Sec, "label", Labels[i]);
            uint c; w.Color = uint.TryParse(Ini.Get(w.Sec, "color", "FFFFFFFF"), NumberStyles.HexNumber, null, out c) ? Color.FromArgb((int)c) : Color.White;
            widgets[i] = w;
        }

        // Top bar: category, language, play, install.
        var top = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 46, Padding = new Padding(6) };
        var cat = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110 };
        cat.Items.AddRange(new object[] { "Any%", "Cheat%" });
        cat.SelectedIndex = Ini.Get("general", "category", "any") == "cheat" ? 1 : 0;
        cat.SelectedIndexChanged += delegate { Ini.Set("general", "category", cat.SelectedIndex == 1 ? "cheat" : "any"); };
        var lang = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 100 };
        lang.Items.AddRange(new object[] { "english", "russian", "french", "german", "italian", "spanish", "polish" });
        lang.SelectedItem = Ini.Get("launcher", "language", "english");
        if (lang.SelectedIndex < 0) lang.SelectedIndex = 0;
        lang.SelectedIndexChanged += delegate { Ini.Set("launcher", "language", lang.SelectedItem); };
        var play = new Button { Text = "▶ Играть", Width = 110, Height = 30, BackColor = Color.FromArgb(60, 140, 60), ForeColor = Color.White };
        play.Click += delegate { Play((string)lang.SelectedItem); };
        var inst = new Button { Text = "Установить/обновить мод", AutoSize = true, Height = 30 };
        inst.Click += delegate { Install(); };
        var uninst = new Button { Text = "Удалить мод", AutoSize = true, Height = 30 };
        uninst.Click += delegate { Uninstall(); };
        var resetPb = new Button { Text = "Сбросить PB", AutoSize = true, Height = 30 };
        resetPb.Click += delegate {
            if (MessageBox.Show("Удалить все сохранённые PB по картам?", Text, MessageBoxButtons.YesNo) == DialogResult.Yes) File.Delete(P("srmod_pb.ini"));
        };
        top.Controls.AddRange(new Control[] { new Label { Text = "Категория:", AutoSize = true, Margin = new Padding(3, 8, 0, 0) }, cat,
            new Label { Text = "Язык:", AutoSize = true, Margin = new Padding(8, 8, 0, 0) }, lang, play, inst, uninst, resetPb });

        status.Dock = DockStyle.Bottom; status.Height = 24; status.Padding = new Padding(6, 4, 0, 0);

        var tabs = new TabControl { Dock = DockStyle.Fill };
        tabs.TabPages.Add(WidgetsTab());
        tabs.TabPages.Add(GeneralTab());
        tabs.TabPages.Add(KeysTab());
        tabs.TabPages.Add(CheatTab());
        Controls.Add(tabs); Controls.Add(top); Controls.Add(status);
        UpdateStatus();
    }

    // ---- Widgets tab: list + properties + draggable 16:9 preview ----
    TabPage WidgetsTab() {
        var page = new TabPage("Оверлей");
        var left = new Panel { Dock = DockStyle.Left, Width = 260, Padding = new Padding(6) };
        list.Dock = DockStyle.Top; list.Height = 260;
        foreach (var w in widgets) list.Items.Add(w.Id, w.On);
        // Clicking the name only selects; only the checkbox glyph toggles the widget.
        list.ItemCheck += (s, e) => {
            if (!allowCheck) { e.NewValue = e.CurrentValue; return; }
            widgets[e.Index].On = e.NewValue == CheckState.Checked; SaveWidget(widgets[e.Index]);
        };
        list.MouseDown += (s, e) => {
            int i = list.IndexFromPoint(e.Location);
            if (i < 0 || e.X > 18) return;
            allowCheck = true; list.SetItemChecked(i, !list.GetItemChecked(i)); allowCheck = false;
        };
        list.SelectedIndexChanged += delegate { LoadProps(); };

        var props = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(0, 8, 0, 0) };
        sizeBox.Minimum = 8; sizeBox.Maximum = 120;
        anchorBox.DropDownStyle = ComboBoxStyle.DropDownList; anchorBox.Items.AddRange(Anchors);
        labelBox.TextChanged += delegate { if (!loading && Sel != null) { Sel.Label = labelBox.Text; SaveWidget(Sel); } };
        sizeBox.ValueChanged += delegate { if (!loading && Sel != null) { Sel.Size = (int)sizeBox.Value; SaveWidget(Sel); } };
        anchorBox.SelectedIndexChanged += delegate { if (!loading && Sel != null) { Sel.Anchor = anchorBox.SelectedIndex; SaveWidget(Sel); } };
        colorBtn.Text = "Цвет…";
        colorBtn.Click += delegate {
            if (Sel == null) return;
            var d = new ColorDialog { Color = Sel.Color, FullOpen = true };
            if (d.ShowDialog() == DialogResult.OK) { Sel.Color = Color.FromArgb(255, d.Color); SaveWidget(Sel); LoadProps(); }
        };
        AddRow(props, "Подпись", labelBox); AddRow(props, "Размер (px @1080p)", sizeBox);
        AddRow(props, "Привязка", anchorBox); AddRow(props, "", colorBtn);
        props.Controls.Add(new Label { Text = "Тащи виджет мышью в превью.\nПозиция хранится в долях экрана,\nподходит под любое разрешение.", AutoSize = true, ForeColor = Color.Gray }, 0, 4);
        props.SetColumnSpan(props.GetControlFromPosition(0, 4), 2);
        left.Controls.Add(props); left.Controls.Add(list);

        preview.Dock = DockStyle.Fill; preview.BackColor = Color.FromArgb(40, 44, 52);
        preview.Paint += PaintPreview;
        preview.Resize += delegate { preview.Invalidate(); };
        preview.MouseDown += PreviewDown;
        preview.MouseMove += PreviewMove;
        preview.MouseUp += delegate { dragIdx = -1; };
        page.Controls.Add(preview); page.Controls.Add(left);
        list.SelectedIndex = 0;
        return page;
    }

    static void AddRow(TableLayoutPanel t, string label, Control c) {
        int r = t.RowCount++;
        t.Controls.Add(new Label { Text = label, AutoSize = true, Margin = new Padding(0, 6, 4, 0) }, 0, r);
        c.Width = 130; t.Controls.Add(c, 1, r);
    }

    void LoadProps() {
        var w = Sel; if (w == null) return;
        loading = true;
        labelBox.Text = w.Label; sizeBox.Value = Math.Max(8, Math.Min(120, w.Size));
        anchorBox.SelectedIndex = Math.Max(0, Math.Min(4, w.Anchor)); colorBtn.BackColor = w.Color;
        colorBtn.ForeColor = w.Color.GetBrightness() > 0.5 ? Color.Black : Color.White;
        loading = false;
        preview.Invalidate();
    }

    void SaveWidget(Widget w) {
        Ini.Set(w.Sec, "enabled", w.On ? 1 : 0); Ini.Set(w.Sec, "x", w.X); Ini.Set(w.Sec, "y", w.Y);
        Ini.Set(w.Sec, "anchor", w.Anchor); Ini.Set(w.Sec, "size", w.Size);
        Ini.Set(w.Sec, "color", w.Color.ToArgb().ToString("X8"));
        Ini.Set(w.Sec, "label", "\"" + w.Label + "\"");  // quotes keep leading/trailing spaces
        preview.Invalidate();
    }

    Rectangle Screen16x9() {
        int W = preview.ClientSize.Width - 20, H = preview.ClientSize.Height - 20;
        if (W * 9 > H * 16) W = H * 16 / 9; else H = W * 9 / 16;
        return new Rectangle((preview.ClientSize.Width - W) / 2, (preview.ClientSize.Height - H) / 2, W, H);
    }

    Rectangle WidgetRect(Graphics g, Widget w, Rectangle scr, out Font f) {
        f = new Font(Ini.Get("general", "font", "Consolas"), Math.Max(4f, w.Size * scr.Height / 1080f), FontStyle.Bold, GraphicsUnit.Pixel);
        var sz = g.MeasureString(w.Label + w.Sample, f).ToSize();
        int x = scr.X + (int)(w.X * scr.Width), y = scr.Y + (int)(w.Y * scr.Height);
        if (w.Anchor == 1 || w.Anchor == 3) x -= sz.Width;
        if (w.Anchor == 2 || w.Anchor == 3) y -= sz.Height;
        if (w.Anchor == 4) { x -= sz.Width / 2; y -= sz.Height / 2; }
        return new Rectangle(x, y, sz.Width, sz.Height);
    }

    void PaintPreview(object s, PaintEventArgs e) {
        var g = e.Graphics; var scr = Screen16x9();
        using (var b = new System.Drawing.Drawing2D.LinearGradientBrush(scr, Color.FromArgb(70, 80, 70), Color.FromArgb(25, 28, 25), 90f)) g.FillRectangle(b, scr);
        g.DrawRectangle(Pens.Gray, scr);
        for (int i = 0; i < widgets.Length; i++) {
            var w = widgets[i]; if (!w.On) continue;
            Font f; var r = WidgetRect(g, w, scr, out f);
            if (i == list.SelectedIndex) g.DrawRectangle(Pens.Yellow, r);
            g.DrawString(w.Label + w.Sample, f, Brushes.Black, r.X + 2, r.Y + 2);
            using (var br = new SolidBrush(w.Color)) g.DrawString(w.Label + w.Sample, f, br, r.X, r.Y);
            f.Dispose();
        }
    }

    void PreviewDown(object s, MouseEventArgs e) {
        var scr = Screen16x9();
        using (var g = preview.CreateGraphics())
            for (int i = widgets.Length - 1; i >= 0; i--) {
                if (!widgets[i].On) continue;
                Font f; var r = WidgetRect(g, widgets[i], scr, out f); f.Dispose();
                if (r.Contains(e.Location)) { dragIdx = i; dragLast = e.Location; list.SelectedIndex = i; return; }
            }
    }

    void PreviewMove(object s, MouseEventArgs e) {
        if (dragIdx < 0) return;
        var scr = Screen16x9(); var w = widgets[dragIdx];
        w.X = Math.Max(0f, Math.Min(1f, w.X + (e.X - dragLast.X) / (float)scr.Width));
        w.Y = Math.Max(0f, Math.Min(1f, w.Y + (e.Y - dragLast.Y) / (float)scr.Height));
        dragLast = e.Location;
        SaveWidget(w);
    }

    // ---- General / keys / cheat tabs ----
    TabPage GeneralTab() {
        var page = new TabPage("Общие");
        var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(12), AutoScroll = true };
        var dec = new NumericUpDown { Minimum = 0, Maximum = 3, Value = Math.Max(0, Math.Min(3, Ini.GetI("general", "decimals", 2))) };
        dec.ValueChanged += delegate { Ini.Set("general", "decimals", (int)dec.Value); };
        AddRow(t, "Знаков после точки (0–3)", dec);
        AddCheck(t, "Всегда показывать часы", "general", "hours_always", 0);
        AddCheck(t, "Тень под текстом", "general", "shadow", 1);
        AddCheck(t, "Автостарт рана при загрузке стартовой карты", "general", "auto_start", 1);
        var start = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        for (int i = 0; i < Missions.Length; i += 2) start.Items.Add(Missions[i + 1] + "  (" + Missions[i] + ")");
        int cur = Array.IndexOf(Missions, Ini.Get("general", "start_map", "trainyard"));
        start.SelectedIndex = cur >= 0 && cur % 2 == 0 ? cur / 2 : Array.IndexOf(Missions, "trainyard") / 2;
        start.SelectedIndexChanged += delegate { Ini.Set("general", "start_map", Missions[start.SelectedIndex * 2]); };
        AddRow(t, "Стартовая миссия", start); start.Width = 320;
        var font = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220 };
        foreach (var fam in FontFamily.Families) font.Items.Add(fam.Name);
        font.SelectedItem = Ini.Get("general", "font", "Consolas");
        font.SelectedIndexChanged += delegate { Ini.Set("general", "font", "\"" + font.SelectedItem + "\""); preview.Invalidate(); };
        AddRow(t, "Шрифт", font); font.Width = 220;
        return page.With(t);
    }

    TabPage KeysTab() {
        var page = new TabPage("Хоткеи");
        var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(12) };
        for (int i = 0; i < Keys.Length; i++) {
            string key = Keys[i];
            var b = new Button { Width = 160, Text = ((Keys)Ini.GetI("keys", key, (int)KeyDefaults[i])).ToString() };
            b.Click += delegate { b.Text = "Нажми клавишу…"; b.Focus(); };
            b.PreviewKeyDown += (s, e) => e.IsInputKey = true;
            b.KeyDown += (s, e) => {
                if (b.Text != "Нажми клавишу…") return;
                Ini.Set("keys", key, (int)e.KeyCode); b.Text = e.KeyCode.ToString(); e.Handled = true;
            };
            AddRow(t, KeyTitles[i], b);
        }
        return page.With(t);
    }

    TabPage CheatTab() {
        var page = new TabPage("Cheat%");
        var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(12) };
        AddCheck(t, "Бессмертие (god) всё время", "cheat", "god", 1);
        AddText(t, "Команды после загрузки карты (через ;)", "cheat", "commands", "give all;giveAllPowerUpgrades;momoney", 420);
        AddText(t, "Команда телепорта (%f = x y z)", "cheat", "teleport_cmd", "script $player1.setOrigin('%.3f %.3f %.3f');", 420);
        t.Controls.Add(new Label { AutoSize = true, ForeColor = Color.Gray, Text =
            "Полезные команды игры: give all, giveAllPowerUpgrades, givePowers, momoney, giveGold,\n" +
            "giveAllIntel, notarget, undying, killActiveEnemies, gotoMission." }, 0, t.RowCount++);
        t.SetColumnSpan(t.GetControlFromPosition(0, t.RowCount - 1), 2);
        return page.With(t);
    }

    static void AddCheck(TableLayoutPanel t, string title, string sec, string key, int def) {
        var c = new CheckBox { Text = title, AutoSize = true, Checked = Ini.GetI(sec, key, def) != 0 };
        c.CheckedChanged += delegate { Ini.Set(sec, key, c.Checked ? 1 : 0); };
        t.Controls.Add(c, 0, t.RowCount++); t.SetColumnSpan(c, 2);
    }

    static void AddText(TableLayoutPanel t, string title, string sec, string key, string def, int width = 160) {
        var b = new TextBox { Text = Ini.Get(sec, key, def) };
        b.TextChanged += delegate { Ini.Set(sec, key, "\"" + b.Text + "\""); };
        AddRow(t, title, b); b.Width = width;
    }

    // ---- Install / launch ----
    bool Installed { get { return File.Exists(P("binkw32_orig.dll")); } }

    void UpdateStatus() {
        status.Text = !File.Exists(P("Wolf2.exe")) ? "Лаунчер должен лежать в папке SP рядом с Wolf2.exe" :
                      Installed ? "Мод установлен. Настройки применяются в игре сразу." : "Мод не установлен.";
    }

    bool Install() {
        try {
            if (!File.Exists(P("srmod.dll"))) throw new Exception("Нет srmod.dll рядом с лаунчером.");
            if (!Installed) File.Move(P("binkw32.dll"), P("binkw32_orig.dll"));
            File.Copy(P("srmod.dll"), P("binkw32.dll"), true);
            UpdateStatus(); return true;
        } catch (Exception ex) { MessageBox.Show("Не удалось установить: " + ex.Message, Text); return false; }
    }

    void Uninstall() {
        try {
            if (!Installed) return;
            File.Delete(P("binkw32.dll")); File.Move(P("binkw32_orig.dll"), P("binkw32.dll"));
        } catch (Exception ex) { MessageBox.Show("Не удалось удалить: " + ex.Message, Text); }
        UpdateStatus();
    }

    void Play(string lang) {
        if (Process.GetProcessesByName("Wolf2").Length > 0) { MessageBox.Show("Игра уже запущена.", Text); return; }
        if (!Install()) return;
        Process.Start(new ProcessStartInfo(P("Wolf2.exe"), "+set com_allowconsole 1 +set com_SingleDeclFile 0 +set sys_lang \"" + lang + "\"") { WorkingDirectory = dir });
    }

    [STAThread]
    static void Main() { Application.EnableVisualStyles(); Application.Run(new LauncherForm()); }
}

static class Ext {
    public static TabPage With(this TabPage p, Control c) { p.Controls.Add(c); return p; }
}
