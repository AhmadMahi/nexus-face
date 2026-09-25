using System.Drawing.Drawing2D;

namespace Rafiq.App;

/// <summary>The two looks, so nothing picks its own colours.</summary>
public sealed class Skin
{
    public Color Bg, Card, CardHot, CardOn, Text, Dim, Accent, Line;
    public bool Light;

    public static Skin For(bool light) => light
        ? new Skin {
            Light = true,
            Bg = Color.FromArgb(246, 246, 248), Card = Color.FromArgb(234, 234, 238),
            CardHot = Color.FromArgb(224, 224, 230), CardOn = Color.FromArgb(206, 226, 252),
            Text = Color.FromArgb(24, 24, 28), Dim = Color.FromArgb(118, 118, 128),
            Accent = Color.FromArgb(10, 100, 220), Line = Color.FromArgb(214, 214, 220) }
        : new Skin {
            Light = false,
            Bg = Color.FromArgb(28, 28, 32), Card = Color.FromArgb(44, 44, 50),
            CardHot = Color.FromArgb(56, 56, 63), CardOn = Color.FromArgb(28, 62, 104),
            Text = Color.FromArgb(240, 240, 245), Dim = Color.FromArgb(150, 150, 160),
            Accent = Color.FromArgb(82, 160, 255), Line = Color.FromArgb(60, 60, 68) };
}

/// <summary>
/// One of the twelve. Drawn rather than composed from controls: a dozen
/// nested panels flicker on resize and fight the rounded corners.
/// </summary>
public sealed class TileButton : Control
{
    public string Glyph = "", Title = "", Detail = "";
    public bool On, Available = true;
    public Skin Skin = Skin.For(true);
    bool _hot;

    public TileButton()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint
               | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        Cursor = Cursors.Hand;
    }

    protected override void OnMouseEnter(EventArgs e) { _hot = true;  Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hot = false; Invalidate(); base.OnMouseLeave(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
        g.Clear(Skin.Bg);

        var r = new RectangleF(0, 0, Width - 1, Height - 1);
        using var path = RafiqIcon.Rounded(r, Dpi(11));
        using (var b = new SolidBrush(On ? Skin.CardOn : (_hot && Available ? Skin.CardHot : Skin.Card)))
            g.FillPath(b, path);
        if (On)
            using (var p = new Pen(Skin.Accent, 1f)) g.DrawPath(p, path);

        var fg = !Available ? Skin.Dim : (On ? Skin.Accent : Skin.Text);
        var dim = !Available ? Skin.Dim : (On ? Skin.Accent : Skin.Dim);

        // The glyph is a character from Segoe's icon face, which every
        // supported Windows has, so nothing needs shipping with the app.
        using var icon = new Font("Segoe MDL2 Assets", Dpi(15), FontStyle.Regular, GraphicsUnit.Pixel);
        using var tf   = new Font("Segoe UI", Dpi(11), FontStyle.Regular, GraphicsUnit.Pixel);
        using var df   = new Font("Segoe UI", Dpi(10), FontStyle.Regular, GraphicsUnit.Pixel);
        using var fgB  = new SolidBrush(fg);
        using var dimB = new SolidBrush(dim);
        var mid = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center,
                                     Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };

        g.DrawString(Glyph, icon, fgB, new RectangleF(0, Dpi(9), Width, Dpi(20)), mid);
        g.DrawString(Title, tf, fgB, new RectangleF(Dpi(2), Dpi(30), Width - Dpi(4), Dpi(15)), mid);
        if (Detail.Length > 0)
            g.DrawString(Detail, df, dimB, new RectangleF(Dpi(2), Dpi(45), Width - Dpi(4), Dpi(14)), mid);
    }

    float Dpi(float v) => v * DeviceDpi / 96f;
}
