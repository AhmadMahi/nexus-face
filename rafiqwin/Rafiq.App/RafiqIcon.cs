using System.Drawing.Drawing2D;

namespace Rafiq.App;

/// <summary>
/// The tray face: a small robot head whose eyes carry the state.
///
/// Drawn rather than shipped as artwork, because the eyes have to change
/// colour and the head has to follow a light or dark taskbar.
/// </summary>
public static class RafiqIcon
{
    public enum Face { Linked, Adrift, Unset }

    public static Icon Make(Face f, bool lightTaskbar, int px)
    {
        var bmp = new Bitmap(px, px);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            float s = px / 18f;
            var ink = lightTaskbar ? Color.FromArgb(230, 20, 24, 30)
                                   : Color.FromArgb(235, 240, 244, 250);
            var eye = f switch
            {
                Face.Linked => Color.FromArgb(255, 52, 199, 89),
                Face.Adrift => Color.FromArgb(255, 235, 72, 62),
                _           => Color.FromArgb(150, 150, 155, 165)
            };
            using var pen = new Pen(ink, Math.Max(1f, 1.3f * s)) { StartCap = LineCap.Round, EndCap = LineCap.Round };
            using var inkB = new SolidBrush(ink);
            using var eyeB = new SolidBrush(eye);

            float Y(float v) => (18 - v) * s;      // drawn counting up, painted counting down

            g.DrawLine(pen, 9 * s, Y(14.2f), 9 * s, Y(16.0f));
            g.FillEllipse(inkB, 8.0f * s, Y(17.1f), 2.1f * s, 2.1f * s);

            var head = new RectangleF(2.6f * s, Y(14.2f), 12.8f * s, 11f * s);
            using (var path = Rounded(head, 3.2f * s)) g.DrawPath(pen, path);

            g.DrawLine(pen, 1.0f * s, Y(9), 2.6f * s, Y(9));
            g.DrawLine(pen, 15.4f * s, Y(9), 17.0f * s, Y(9));

            g.FillEllipse(eyeB, 5.05f * s, Y(11.0f), 3.0f * s, 3.0f * s);
            g.FillEllipse(eyeB, 9.95f * s, Y(11.0f), 3.0f * s, 3.0f * s);

            g.DrawLine(pen, 6.3f * s, Y(5.7f), 11.7f * s, Y(5.7f));
        }
        var h = bmp.GetHicon();
        try { return (Icon)Icon.FromHandle(h).Clone(); }
        finally { DestroyIcon(h); bmp.Dispose(); }
    }

    public static GraphicsPath Rounded(RectangleF r, float rad)
    {
        var p = new GraphicsPath();
        float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern bool DestroyIcon(IntPtr h);
}
