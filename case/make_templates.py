"""1:1 cutting templates for the pager case (US Letter). All numbers in mm,
taken from Elecrow's 3D model of the CrowPanel 2.13" board."""
from reportlab.lib.pagesizes import letter
from reportlab.lib.units import mm
from reportlab.pdfgen import canvas

c = canvas.Canvas("pager-case-templates.pdf", pagesize=letter)
c.setTitle("Pager case cutting templates (print at 100%)")
PW, PH = letter[0] / mm, letter[1] / mm


def text(x, y, s, size=9, bold=False, center=False):
    c.setFont("Helvetica-Bold" if bold else "Helvetica", size)
    (c.drawCentredString if center else c.drawString)(x * mm, y * mm, s)


def rect(x0, y0, x1, y1, dash=False, r=0, width=0.5):
    c.setLineWidth(width)
    c.setDash(2, 2) if dash else c.setDash()
    if r:
        c.roundRect(x0 * mm, y0 * mm, (x1 - x0) * mm, (y1 - y0) * mm, r * mm)
    else:
        c.rect(x0 * mm, y0 * mm, (x1 - x0) * mm, (y1 - y0) * mm)
    c.setDash()


def line(x0, y0, x1, y1, dash=False, width=0.5):
    c.setLineWidth(width)
    c.setDash(2, 2) if dash else c.setDash()
    c.line(x0 * mm, y0 * mm, x1 * mm, y1 * mm)
    c.setDash()


def circle(x, y, d):
    c.setLineWidth(0.5)
    c.circle(x * mm, y * mm, d / 2 * mm)
    line(x - d / 2 - 1, y, x + d / 2 + 1, y, width=0.25)
    line(x, y - d / 2 - 1, x, y + d / 2 + 1, width=0.25)


def scale_check(y):
    text(15, y + 9, "Scale check: print at 100% (\"actual size\"), then measure these with a ruler before cutting.", 9)
    line(15, y, 115, y, width=0.8)
    for i in range(0, 101, 10):
        line(15 + i, y, 15 + i, y + (3 if i % 50 else 5), width=0.6)
    text(15, y - 4, "0", 7); text(63, y - 4, "50 mm", 7); text(110, y - 4, "100 mm", 7)
    line(130, y, 130 + 50.8, y, width=0.8)
    for i in (0, 25.4, 50.8):
        line(130 + i, y, 130 + i, y + 5, width=0.6)
    text(130, y - 4, "0", 7); text(176, y - 4, "2 inches", 7)


# ============================================================ page 1
text(15, PH - 18, "Pager case templates - sheet 1: project box", 15, bold=True)
text(15, PH - 25, "For a plastic box with at least 64 x 32 mm of inside space, and 12 mm of depth (18 mm with the 750 mAh battery behind the board).", 8)
scale_check(PH - 45)

# --- front face
ox, oy = 108, PH - 92            # board centre on the page
def P(X, Y):                      # case coords -> page (rocker end on the right, USB edge at the bottom)
    return ox - X, oy - Y
text(15, PH - 62, "A. Front of the box (seen from outside)", 11, bold=True)
x0, y0 = P(31.6, 15.6); x1, y1 = P(-31.6, -15.6)
rect(x0, y0, x1, y1, dash=True)
x0, y0 = P(29.6, 13.8); x1, y1 = P(-28.8, -13.8)
rect(x0, y0, x1, y1, width=1.0)
text(ox, oy - 1, "CUT OUT: screen window  58.4 x 27.6", 9, center=True)
text(ox, oy - 22, "Dashed line = edge of the circuit board (63.2 x 31.2). Tape this where the board will sit.", 8, center=True)
text(ox + 36, oy + 2, "rocker", 8); text(ox + 36, oy - 2, "end", 8)
text(ox - 50, oy, "cable end", 8)
text(ox, oy - 27, "USB-C is on this (bottom) long edge. Check that against your board before cutting.", 8, center=True)

# --- rocker end wall
text(15, PH - 128, "B. Rocker-end wall (seen from outside, screen side up)", 11, bold=True)
bx, by = 60, PH - 142            # centre line x, reference line y
line(bx - 22, by, bx + 22, by, width=1.0)
text(bx + 24, by - 1, "inside face of the box front", 8)
text(bx + 24, by - 5, "(the screen glass rests against it)", 8)
line(bx, by + 3, bx, by - 12, dash=True, width=0.3)
text(bx, by + 4, "centre of board", 7, center=True)
line(bx - 15.6, by + 2, bx - 15.6, by - 9, dash=True, width=0.3)
line(bx + 15.6, by + 2, bx + 15.6, by - 9, dash=True, width=0.3)
rect(bx - 7.6, by - 5.4, bx + 7.6, by - 1.4, width=1.0)
circle(bx - 11.15, by - 4.25, 4)
circle(bx + 11.15, by - 4.25, 4)
text(15, by - 16, "Rocker slot: 15.2 wide, from 1.4 to 5.4 below the line.   Two 4 mm holes for MENU and EXIT: 11.15 either side of centre,", 8)
text(15, by - 20, "4.25 below the line. The board's end must sit hard against this wall so the rocker pokes through (it sticks out 2.7 mm).", 8)
text(15, by - 24, "Add your box's front wall thickness to measure from the outside instead. The USB-C edge is to the LEFT in this view.", 8)

# --- USB wall
text(15, PH - 178, "C. USB-C wall (the long side, seen from outside, screen side up)", 11, bold=True)
ux, uy = 150, PH - 192           # rocker-end edge of the board, reference line
line(ux - 70, uy, ux + 4, uy, width=1.0)
text(ux - 70, uy + 2, "inside face of the box front", 8)
line(ux, uy + 3, ux, uy - 12, dash=True, width=0.3)
text(ux + 1.5, uy - 8, "rocker-end edge", 7)
text(ux + 1.5, uy - 11, "of the board", 7)
rect(ux - 32.2 - 6.2, uy - 6.4, ux - 32.2 + 6.2, uy - 1.0, width=1.0)
text(ux - 32.2, uy - 4.6, "USB-C", 8, center=True)
text(15, uy - 16, "Slot: 12.4 wide x 5.4 tall, centred 32.2 from the rocker-end edge of the board, from 1.0 to 6.4 below the line.", 8)
text(15, uy - 20, "That is wide enough for a charging plug's plastic body. The socket itself is 8.9 wide.", 8)

text(15, 30, "Also: BOOT and RESET are small push buttons on the back of the board near the cable end. Drill two 2 mm holes in the back of the", 8)
text(15, 26, "box over them if you want paperclip access (RESET is the one you need if the pager ever stops responding).", 8)
text(15, 18, "Not yet checked against a real board. Cut on scrap or cardboard first.", 8, bold=True)
c.showPage()

# ============================================================ page 2
text(15, PH - 18, "Pager case templates - sheet 2: layered wood", 15, bold=True)
text(15, PH - 25, "Cut from 3 mm (1/8 in) basswood or walnut sheet. Stack front to back: 1 x Bezel, 2 x Frame A, Frame B (2 for a slim case with a", 8)
text(15, PH - 29, "narrow battery, 4 with the 750 mAh battery), 1 x Back. Glue bezel + frames; fix the back with small screws or magnets.", 8)
scale_check(PH - 47)

XL, XR = 43.4, -32.1             # outline ends in case X (key ring end, rocker end)
HW = 19.0                         # half width of the outline


def outline(oy, lug_hole=True):
    x0, y0 = ox2 - XL, oy - HW
    x1, y1 = ox2 - XR, oy + HW
    rect(x0, y0, x1, y1, r=3, width=1.0)
    if lug_hole:
        circle(ox2 - 38.4, oy, 5)
    return x0, x1


ox2 = 112
def Q(X, Y, oy):
    return ox2 - X, oy - Y


def cavity(oy, open_end, usb):
    # board pocket 64 x 32, plus room for the screen cable at the far end
    xa, ya = Q(33.4, 16.0, oy)
    xb, yb = Q(-32.1 if open_end else -28.1, -16.0, oy)
    c.setLineWidth(1.0)
    p = c.beginPath()
    if open_end:
        # three-sided pocket, open at the rocker end
        p.moveTo(xb * mm, ya * mm); p.lineTo(xa * mm, ya * mm)
        p.lineTo(xa * mm, yb * mm); p.lineTo(xb * mm, yb * mm)
        c.drawPath(p)
        # the outline is not cut across the open end
        c.setStrokeColorRGB(1, 1, 1)
        line(xb, ya + 0.6, xb, yb - 0.6, width=2.5)
        c.setStrokeColorRGB(0, 0, 0)
    else:
        rect(xa, ya, xb, yb, width=1.0)
    if usb:
        ux0, _ = Q(0.6 + 6.2, 0, oy); ux1, _ = Q(0.6 - 6.2, 0, oy)
        rect(ux0, oy - HW, ux1, oy - 16.0, width=1.0)
        text((ux0 + ux1) / 2, oy - 13.5, "USB-C notch 12.4 wide", 7, center=True)


tops = [PH - 80, PH - 128, PH - 176, PH - 224]
# Bezel
oy = tops[0]
outline(oy)
x0, y0 = Q(29.6, 13.8, oy); x1, y1 = Q(-28.8, -13.8, oy)
rect(x0, y0, x1, y1, width=1.0)
text(ox2, oy - 1, "CUT OUT: window 58.4 x 27.6", 9, center=True)
text(15, oy + HW + 3, "1. Bezel (cut 1) - outside 75.5 x 38", 10, bold=True)
text(ox2 + 34, oy + 1, "rocker", 7); text(ox2 + 34, oy - 2, "end", 7)
# Frame A
oy = tops[1]
outline(oy)
cavity(oy, True, True)
text(ox2, oy - 1, "CUT OUT: pocket 32 wide,", 8, center=True)
text(ox2, oy - 5, "open at the rocker end", 8, center=True)
text(15, oy + HW + 3, "2. Frame A (cut 2) - sits right behind the bezel; rocker, buttons and USB-C come out through it", 10, bold=True)
# Frame B
oy = tops[2]
outline(oy)
cavity(oy, False, False)
text(ox2, oy - 1, "CUT OUT: 61.5 x 32", 9, center=True)
text(15, oy + HW + 3, "3. Frame B (cut 2 for slim, 4 with the 750 mAh battery) - closed at the rocker end", 10, bold=True)
# Back
oy = tops[3]
outline(oy)
for X in (15.2, 25.65):
    x, y = Q(X, -11.9, oy)
    circle(x, y, 2)
for Y in (-12, 12):
    x, y = Q(38.4, Y, oy)
    circle(x, y, 1.5)
x, y = Q(-30.1, 0, oy)
circle(x, y, 1.5)
text(ox2, oy - 1, "Back plate: solid", 9, center=True)
text(15, oy + HW + 3, "4. Back (cut 1) - 2 mm holes: paperclip access to BOOT / RESET.  1.5 mm marks: screw positions", 10, bold=True)

text(15, 20, "All four are drawn from the FRONT with the rocker end on the right. The back's holes are positioned for that view, so keep", 8)
text(15, 16, "this side facing the screen. The 5 mm hole at the left is for the key ring. Sand the bezel's window edges to a slight bevel.", 8)
text(15, 9, "Not yet checked against a real board. Cut a cardboard set first and test the fit.", 8, bold=True)
c.showPage()
c.save()
