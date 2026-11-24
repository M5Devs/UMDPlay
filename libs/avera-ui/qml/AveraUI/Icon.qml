import QtQuick

// Line/fill icon set drawn with Canvas, so the shell needs no icon font and no
// image assets. Every glyph is authored in a 24x24 box and scaled to `size`.
Item {
    id: root

    property string name: "play"
    property real size: 20
    property color color: Theme.onSurface
    property real weight: 1.7          // stroke width in the 24x24 box

    implicitWidth: size
    implicitHeight: size

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onSizeChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const s = width / 24.0;
            ctx.scale(s, s);
            ctx.lineWidth = root.weight;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.strokeStyle = root.color;
            ctx.fillStyle = root.color;

            const tri = (x, y, w, h) => {
                ctx.beginPath();
                ctx.moveTo(x, y);
                ctx.lineTo(x + w, y + h / 2);
                ctx.lineTo(x, y + h);
                ctx.closePath();
                ctx.fill();
            };
            const circle = (cx, cy, r, filled) => {
                ctx.beginPath();
                ctx.arc(cx, cy, r, 0, Math.PI * 2);
                filled ? ctx.fill() : ctx.stroke();
            };
            const line = (x1, y1, x2, y2) => {
                ctx.beginPath();
                ctx.moveTo(x1, y1);
                ctx.lineTo(x2, y2);
                ctx.stroke();
            };
            const rrect = (x, y, w, h, r) => {
                const k = Math.min(r, w / 2, h / 2);
                ctx.beginPath();
                ctx.moveTo(x + k, y);
                ctx.lineTo(x + w - k, y);
                ctx.quadraticCurveTo(x + w, y, x + w, y + k);
                ctx.lineTo(x + w, y + h - k);
                ctx.quadraticCurveTo(x + w, y + h, x + w - k, y + h);
                ctx.lineTo(x + k, y + h);
                ctx.quadraticCurveTo(x, y + h, x, y + h - k);
                ctx.lineTo(x, y + k);
                ctx.quadraticCurveTo(x, y, x + k, y);
                ctx.closePath();
            };

            switch (root.name) {
            case "play":      tri(8, 5, 11, 14); break;
            case "pause":
                rrect(7.5, 5, 3.6, 14, 1.6); ctx.fill();
                rrect(13.4, 5, 3.6, 14, 1.6); ctx.fill();
                break;
            case "stop":
                rrect(6.5, 6.5, 11, 11, 2); ctx.fill();
                break;
            case "skip-prev":
                rrect(6, 5.5, 2.2, 13, 1); ctx.fill();
                tri(18, 5.5, -9.5, 13);
                break;
            case "skip-next":
                rrect(15.8, 5.5, 2.2, 13, 1); ctx.fill();
                tri(6, 5.5, 9.5, 13);
                break;
            case "rewind":
                tri(12.5, 6, -7, 12);
                tri(20, 6, -7, 12);
                break;
            case "forward":
                tri(4, 6, 7, 12);
                tri(11.5, 6, 7, 12);
                break;
            case "fullscreen":
                line(4.5, 9, 4.5, 4.5); line(4.5, 4.5, 9, 4.5);
                line(19.5, 9, 19.5, 4.5); line(19.5, 4.5, 15, 4.5);
                line(4.5, 15, 4.5, 19.5); line(4.5, 19.5, 9, 19.5);
                line(19.5, 15, 19.5, 19.5); line(19.5, 19.5, 15, 19.5);
                break;
            case "volume":
                ctx.beginPath();
                ctx.moveTo(4, 9.5); ctx.lineTo(7.5, 9.5); ctx.lineTo(12, 5.5);
                ctx.lineTo(12, 18.5); ctx.lineTo(7.5, 14.5); ctx.lineTo(4, 14.5);
                ctx.closePath(); ctx.fill();
                ctx.beginPath(); ctx.arc(12, 12, 4.6, -0.9, 0.9); ctx.stroke();
                ctx.beginPath(); ctx.arc(12, 12, 7.6, -0.8, 0.8); ctx.stroke();
                break;
            case "chevron-right": line(9.5, 5.5, 16, 12); line(16, 12, 9.5, 18.5); break;
            case "chevron-left":  line(14.5, 5.5, 8, 12);  line(8, 12, 14.5, 18.5); break;
            case "arrow-right":
                line(4.5, 12, 19, 12); line(13.5, 6.5, 19, 12); line(19, 12, 13.5, 17.5);
                break;
            case "disc":
                circle(12, 12, 8.4, false);
                circle(12, 12, 2.1, true);
                break;
            case "menu-book":
                ctx.beginPath();
                ctx.moveTo(4.5, 6); ctx.lineTo(4.5, 18.5); ctx.lineTo(11.5, 18.5);
                ctx.lineTo(12, 17.6); ctx.lineTo(12.5, 18.5); ctx.lineTo(19.5, 18.5);
                ctx.lineTo(19.5, 6); ctx.stroke();
                line(12, 7.2, 12, 17.4);
                break;
            case "chapters":
                line(4.5, 7.5, 19.5, 7.5);
                line(4.5, 12, 19.5, 12);
                line(4.5, 16.5, 13.5, 16.5);
                break;
            case "options":
                line(4.5, 8, 19.5, 8); line(4.5, 16, 19.5, 16);
                circle(9, 8, 2.1, true);
                circle(15.5, 16, 2.1, true);
                break;
            case "dots":
                circle(6.5, 12, 1.5, true);
                circle(12, 12, 1.5, true);
                circle(17.5, 12, 1.5, true);
                break;
            case "clock":
                circle(12, 12, 8.2, false);
                line(12, 7.4, 12, 12.2); line(12, 12.2, 15.6, 14);
                break;
            case "person":
                circle(12, 8.6, 3.5, true);
                ctx.beginPath();
                ctx.arc(12, 20, 6.4, Math.PI, Math.PI * 2);
                ctx.fill();
                break;
            case "music":
                circle(8, 16.6, 2.6, true);
                circle(17, 14.6, 2.6, true);
                line(10.6, 16.2, 10.6, 6.4);
                line(19.6, 14.2, 19.6, 4.4);
                line(10.6, 6.4, 19.6, 4.4);
                break;
            case "movie":
                rrect(3.5, 7, 17, 11.5, 2); ctx.stroke();
                line(3.5, 10.6, 20.5, 10.6);
                circle(7, 8.8, 0.9, true); circle(12, 8.8, 0.9, true);
                circle(17, 8.8, 0.9, true);
                break;
            case "camera":
                // Body with a raised viewfinder hump and a big lens: readable
                // at 14 px, where a thin outline would read as a dot.
                rrect(2.6, 7.0, 18.8, 12.4, 2.6); ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(7.6, 7.0); ctx.lineTo(9.2, 4.4); ctx.lineTo(14.8, 4.4);
                ctx.lineTo(16.4, 7.0);
                ctx.stroke();
                circle(12, 13.2, 4.0, false);
                circle(12, 13.2, 1.5, true);
                break;
            case "ab":
                // Two loop markers with a repeating arrow between them.
                line(4.0, 6.4, 4.0, 17.6);
                line(4.0, 6.4, 7.4, 6.4);
                line(4.0, 17.6, 7.4, 17.6);
                line(20.0, 6.4, 20.0, 17.6);
                line(20.0, 6.4, 16.6, 6.4);
                line(20.0, 17.6, 16.6, 17.6);
                line(8.4, 12.0, 15.6, 12.0);
                line(15.6, 12.0, 13.4, 9.9);
                line(15.6, 12.0, 13.4, 14.1);
                break;
            case "check":
                line(5, 12.5, 10, 17.5); line(10, 17.5, 19, 6.5);
                break;
            case "search":
                circle(10.5, 10.5, 6.2, false);
                line(15.2, 15.2, 20, 20);
                break;
            default:
                circle(12, 12, 8, false);
                break;
            }
        }
    }
}
