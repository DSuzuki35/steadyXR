using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace SteadyXR
{
    public static class LayerEditorTheme
    {
        [DllImport("user32.dll")]
        public static extern bool SetProcessDPIAware();
        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);
        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        private static extern int SetWindowTheme(IntPtr window, string app, string classes);

        public static void ThemeNative(Control control)
        {
            int dark = SystemInformation.HighContrast ? 0 : 1;
            if (control is Form)
                DwmSetWindowAttribute(control.Handle, 20, ref dark, sizeof(int));
            SetWindowTheme(control.Handle, dark == 1 ? "DarkMode_Explorer" : null, null);
            if (control is DataGridView)
                foreach (Control child in control.Controls)
                    if (child is ScrollBar) ThemeNative(child);
        }

        private static GraphicsPath Rounded(Rectangle r, int diameter)
        {
            var path = new GraphicsPath();
            path.AddArc(r.Left, r.Top, diameter, diameter, 180, 90);
            path.AddArc(r.Right - diameter, r.Top, diameter, diameter, 270, 90);
            path.AddArc(r.Right - diameter, r.Bottom - diameter, diameter, diameter, 0, 90);
            path.AddArc(r.Left, r.Bottom - diameter, diameter, diameter, 90, 90);
            path.CloseFigure();
            return path;
        }

        public static void StyleCard(Panel panel)
        {
            panel.Paint += delegate(object sender, PaintEventArgs e) {
                var bounds = panel.ClientRectangle;
                bounds.Width--; bounds.Height--;
                using (var path = Rounded(bounds, 12))
                using (var edge = new Pen(SystemInformation.HighContrast ? SystemColors.WindowText : Color.FromArgb(53, 53, 60)))
                using (var inside = new SolidBrush(panel.BackColor)) {
                    e.Graphics.Clear(panel.Parent.BackColor);
                    e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                    e.Graphics.FillPath(inside, path);
                    e.Graphics.DrawPath(edge, path);
                }
            };
        }

        public static void StyleButton(ButtonBase button, bool primary)
        {
            if (SystemInformation.HighContrast) return;
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderSize = 0;
            button.Paint += delegate(object sender, PaintEventArgs e) {
                var radio = button as RadioButton;
                bool selected = radio != null && radio.Checked;
                bool hover = button.Enabled && button.ClientRectangle.Contains(button.PointToClient(Cursor.Position));
                bool focus = button.Focused;
                var fill = Color.FromArgb(42, 45, 50);
                var edge = Color.FromArgb(70, 72, 80);
                var ink = button.Enabled ? Color.FromArgb(244, 246, 248) : Color.FromArgb(143, 149, 158);
                var blue = Color.FromArgb(94, 158, 255);
                if (radio != null) {
                    fill = selected ? Color.FromArgb(34, 49, 71) : hover ? Color.FromArgb(43, 44, 49) : button.Parent.BackColor;
                    edge = selected ? Color.FromArgb(53, 74, 105) : fill;
                    if (selected && button.Enabled) ink = Color.FromArgb(182, 211, 255);
                } else if (primary && button.Enabled) {
                    fill = hover ? Color.FromArgb(61, 124, 222) : Color.FromArgb(49, 108, 207);
                    edge = Color.FromArgb(76, 133, 224);
                } else if (hover) fill = Color.FromArgb(53, 56, 63);
                var bounds = button.ClientRectangle;
                bounds.Width--; bounds.Height--;
                using (var path = Rounded(bounds, 10))
                using (var pen = new Pen(focus ? blue : edge))
                using (var brush = new SolidBrush(fill)) {
                    e.Graphics.Clear(button.Parent.BackColor);
                    e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                    e.Graphics.FillPath(brush, path);
                    e.Graphics.DrawPath(pen, path);
                }
                var flags = TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine;
                if (radio != null) {
                    if (selected) using (var marker = new SolidBrush(blue))
                        e.Graphics.FillRectangle(marker, 1, bounds.Height / 4, 3, bounds.Height / 2);
                    bounds.X += 15;
                    bounds.Width -= 18;
                    flags |= TextFormatFlags.Left;
                } else flags |= TextFormatFlags.HorizontalCenter;
                TextRenderer.DrawText(e.Graphics, button.Text, button.Font, bounds, ink, flags);
            };
        }
    }
}
