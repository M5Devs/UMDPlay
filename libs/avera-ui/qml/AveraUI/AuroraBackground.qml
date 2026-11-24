import QtQuick

// The deep-oceanic canvas: base navy, two atmospheric light leaks and the
// faint 45-degree specular stripe texture from the Stitch export. Painted once
// per size change on a Canvas — cheaper and far more portable than stacked
// blurred rectangles.
Item {
    id: root
    anchors.fill: parent

    Canvas {
        id: paint
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const w = width, h = height;

            ctx.fillStyle = Theme.canvas;
            ctx.fillRect(0, 0, w, h);

            let g = ctx.createRadialGradient(w * 0.5, h * 0.12, 0,
                                             w * 0.5, h * 0.12, Math.max(w, h) * 0.62);
            g.addColorStop(0.0, "#3D0053DB");
            g.addColorStop(0.55, "#140053DB");
            g.addColorStop(1.0, "#000053DB");
            ctx.fillStyle = g;
            ctx.fillRect(0, 0, w, h);

            g = ctx.createRadialGradient(w * 0.82, h * 0.86, 0,
                                         w * 0.82, h * 0.86, Math.max(w, h) * 0.5);
            g.addColorStop(0.0, "#2200D2FF");
            g.addColorStop(1.0, "#0000D2FF");
            ctx.fillStyle = g;
            ctx.fillRect(0, 0, w, h);

            g = ctx.createRadialGradient(w * 0.1, h * 0.95, 0,
                                         w * 0.1, h * 0.95, Math.max(w, h) * 0.4);
            g.addColorStop(0.0, "#182A344D");
            g.addColorStop(1.0, "#002A344D");
            ctx.fillStyle = g;
            ctx.fillRect(0, 0, w, h);

            // Specular diagonal texture (1px white every 16px at 45 degrees).
            ctx.strokeStyle = "#0DFFFFFF";
            ctx.lineWidth = 1;
            ctx.beginPath();
            const step = 16;
            for (let x = -h; x < w + h; x += step) {
                ctx.moveTo(x, 0);
                ctx.lineTo(x + h, h);
            }
            ctx.stroke();
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
}
