"""Pager case, draft 3 (after the second test print).

Draft 3: solid window frame with a bevel (no pane recess), a USB-C opening
shaped to the plug, and a tighter bay around the rocker wheel.

Draft 2 notes:

Changes from draft 1:
  * 5 mm thinner: the 750 mAh battery now lies toward the cable end, over the
    USB-C socket, instead of on top of the tall 4-pin connectors.
  * Rocker end rebuilt: solid 1.2 mm corner walls with separate push-button
    caps (the flexing tabs snapped), and an open bay so the wheel stands proud.
  * Screen window masked down to the area that actually shows an image.
Axes: X = length (rocker end is -X), Y = width (USB-C on +Y), Z = thickness
(screen faces +Z, PCB front face is Z = 0).
"""
import cadquery as cq

ZB = -12.5                    # back inside face
YC, YO = 15.9, 17.5           # cavity / outside half width
ZF_IN, ZF_OUT = 1.1, 2.7      # front wall inside / outside
LID_T = 1.5
XE_IN, XE_OUT = -33.1, -34.3  # corner walls beside the rocker (hold the button caps)
XB_IN, XB_OUT = -32.2, -33.6  # recessed wall under the rocker
BAY = 7.2                     # half width of the rocker bay (wheel is 6.6)
BAY_Z = -4.6                  # bottom of the opening (wheel reaches -3.8)
USB_X, USB_Z = 0.6, -3.2      # centre of the USB-C socket
X1, XO1 = 39.6, 42.0          # cable-end cavity / outside
BTN_Y, BTN_Z = 11.15, -3.35   # side-button centres
KEY = (38.5, -14.0)           # key ring hole
SCREW = (35.2, -11.3)


def box(x0, x1, y0, y1, z0, z1):
    return (cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False)
            .translate((x0, y0, z0)))


def slot_y(xc, zc, w, h, y0, y1):
    """A rounded (stadium) hole through a side wall: w along X, h along Z."""
    s = cq.Workplane("XY").slot2D(w, h).extrude(y1 - y0)
    s = s.rotate((0, 0, 0), (1, 0, 0), -90)          # extrusion now runs along +Y
    bb = s.val().BoundingBox()
    return s.translate((xc - (bb.xmin + bb.xmax) / 2, y0 - bb.ymin, zc - (bb.zmin + bb.zmax) / 2))


def cyl(x, y, z0, z1, d):
    return cq.Workplane("XY").circle(d / 2).extrude(z1 - z0).translate((x, y, z0))


bay_cut = box(XE_OUT - 1, XB_OUT, -BAY, BAY, ZB - LID_T - 1, ZF_OUT + 1)

# ---------------------------------------------------------------- shell
shell = box(XE_OUT, XO1, -YO, YO, ZB, ZF_OUT)
shell = shell.edges("|Z").fillet(3.5)
shell = shell.faces(">Z").edges().chamfer(0.8)                      # prints cleaner on the plate than a round
shell = shell.cut(bay_cut)                                         # step the end back at the rocker
shell = shell.cut(box(XB_IN, X1, -YC, YC, ZB, 0.0))                # board, parts, battery
for s in (-1, 1):                                                  # pockets for the side buttons
    y0, y1 = sorted((s * (BAY + 1.0), s * YC))
    shell = shell.cut(box(XE_IN, XB_IN, y0, y1, ZB, 0.0))
    shell = shell.cut(box(XE_OUT - 0.1, XE_IN + 0.1, s * BTN_Y - 1.8, s * BTN_Y + 1.8,
                          BTN_Z - 1.5, BTN_Z + 1.5))               # hole for the button cap
shell = shell.cut(box(XB_OUT - 0.1, XB_IN, -BAY, BAY, BAY_Z, ZF_OUT + 1))  # open bay: rocker stands proud
shell = shell.cut(box(-29.9, 30.7, -14.9, 14.9, 0.0, ZF_IN))       # screen glass pocket
shell = shell.cut(box(30.7, 33.4, -9.0, 8.0, 0.0, ZF_IN))          # screen flex cable
window = box(-27.3, 22.7, -12.6, 12.6, ZF_IN, ZF_OUT).union(
    cq.Workplane("XY").rect(51.0, 26.2).workplane(offset=-0.5).rect(50.0, 25.2).loft()
    .translate((-2.3, 0, ZF_OUT)))
shell = shell.cut(window)                                          # window with a 0.5 mm bevel
shell = shell.cut(slot_y(USB_X, USB_Z, 9.8, 4.0, YC - 0.1, YO + 0.1))      # hole hugs the socket
shell = shell.cut(slot_y(USB_X, USB_Z, 12.6, 6.8, YO - 1.0, YO + 0.1))     # shallow seat for the plug body
# stops so the board cannot slide toward the cable end
shell = shell.union(box(31.95, 33.2, 10.0, YC, -3.0, 0.0))
shell = shell.union(box(31.95, 33.2, -YC, -10.5, -3.0, 0.0))
# solid corner for the key ring and the lid screw
shell = shell.union(box(33.6, X1, -YC, -9.4, ZB, 0.0))
shell = shell.cut(cyl(KEY[0], KEY[1], ZB, ZF_OUT, 4.6))
shell = shell.cut(cyl(SCREW[0], SCREW[1], ZB, ZB + 7.0, 1.7))
# ledge the lid hooks under, and a groove its snap bump clicks into
shell = shell.union(box(XB_IN, XB_IN + 0.7, -5.0, 5.0, ZB, ZB + 0.9))
shell = shell.cut(box(29.5, 36.5, YC, YC + 0.35, ZB + 0.7, ZB + 1.6))

# ------------------------------------------------------------------ lid
lid = box(XE_OUT, XO1, -YO, YO, ZB - LID_T, ZB)
lid = lid.edges("|Z").fillet(3.5)
lid = lid.faces("<Z").edges().fillet(1.0)
lid = lid.cut(bay_cut)
ring = box(-30.5, X1 - 0.25, -YC + 0.25, YC - 0.25, ZB, ZB + 1.6).cut(
    box(-29.5, X1 - 1.25, -YC + 1.25, YC - 1.25, ZB, ZB + 1.6))
ring = ring.cut(box(33.3, X1, -YC, -9.1, ZB, ZB + 1.6))            # clears the solid corner
lid = lid.union(ring)
lid = lid.union(box(30.0, 36.0, YC - 0.25, YC + 0.2, ZB + 0.9, ZB + 1.5))   # snap bump
lid = lid.union(box(XB_IN + 1.0, XB_IN + 2.2, -4.5, 4.5, ZB, ZB + 1.8))     # hook post
lid = lid.union(box(XB_IN + 0.2, XB_IN + 1.0, -4.5, 4.5, ZB + 1.0, ZB + 1.8))  # hook tongue
lid = lid.cut(cyl(KEY[0], KEY[1], ZB - LID_T, ZB, 4.6))
lid = lid.cut(cyl(SCREW[0], SCREW[1], ZB - LID_T, ZB, 2.4))
for x in (15.2, 25.65):                                            # paperclip holes: BOOT, RESET
    lid = lid.cut(cyl(x, -11.9, ZB - LID_T, ZB, 2.0))

# --------------------------------------------------------- button cap
# flange sits inside the wall against the switch; the stem pokes out 1.2 mm
cap = box(0, 0.5, -1.8, 1.8, -2.2, 2.2).union(box(0.5, 0.5 + 1.2 + 1.2, -1.6, 1.6, -1.3, 1.3))

# ------------------------------------------- stand-ins for fit checks
board = box(-31.6, 31.6, -15.6, 15.6, -1.6, 0)
board = board.union(box(-29.6, 30.4, -14.6, 14.6, 0, 0.9))            # glass
board = board.union(box(-34.35, -25.3, -6.6, 6.6, -3.8, -1.0))        # rocker wheel
for s in (-1, 1):
    y0, y1 = sorted((s * 7.4, s * 15.0))
    board = board.union(box(-31.7, -28.9, y0, y1, -5.1, -1.6))        # side-button bodies
    y0, y1 = sorted((s * 9.6, s * 12.7))
    board = board.union(box(-32.4, -30.9, y0, y1, -4.1, -2.6))        # their plungers
board = board.union(box(-3.9, 5.1, 8.3, 15.6, -4.8, -0.6))            # USB-C
board = board.union(box(-2.1, 9.9, -15.6, -9.5, -9.75, -1.6))         # GPIO header
board = board.union(box(-22.8, -7.7, 7.6, 15.6, -9.4, -1.6))          # UART header
board = board.union(box(8.1, 12.1, 10.1, 15.3, -4.5, -1.6))           # battery socket
for x0, x1 in ((12.9, 17.5), (23.4, 27.9)):
    board = board.union(box(x0, x1, -14.2, -9.6, -8.1, -1.6))         # BOOT / RESET
battery = box(-7.2, 37.8, -7.6, 14.4, -12.0, -5.1)                    # 692245, 6.9 x 22 x 45
caps = None
for s in (-1, 1):
    c = cap.mirror("YZ").translate((XE_IN + 0.55, s * BTN_Y, BTN_Z))
    caps = c if caps is None else caps.union(c)

for name, part in (("shell", shell), ("lid", lid)):
    for other, label in ((board, "board"), (battery, "battery"), (caps, "button caps")):
        clash = part.intersect(other).val()
        vol = clash.Volume() if clash.isValid() else 0
        print(f"{name} vs {label}: overlap {vol:.3f} mm3")
for other, label in ((board, "board"), (battery, "battery")):
    print(f"caps vs {label}: overlap {caps.intersect(other).val().Volume():.3f} mm3")
print(f"shell vs lid: overlap {shell.intersect(lid).val().Volume():.3f} mm3")
bb = shell.union(lid).val().BoundingBox()
print(f"outside size: {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm")
print(f"plastic: shell {shell.val().Volume()/1000:.1f} cm3, lid {lid.val().Volume()/1000:.1f} cm3")

for name, part in (("shell", shell), ("lid", lid), ("button-cap", cap)):
    cq.exporters.export(part, f"pager-case-v3-{name}.step")
    cq.exporters.export(part, f"pager-case-v3-{name}.stl", tolerance=0.02, angularTolerance=0.1)

view = (cq.Assembly().add(shell, name="shell").add(caps.translate((-5, 0, 0)), name="caps")
        .add(lid.translate((0, 0, -12)), name="lid")).toCompound()
opts = {"width": 900, "height": 700, "showAxes": False, "showHidden": False,
        "projectionDir": (-0.9, -0.75, 0.6), "strokeWidth": 0.25}
cq.exporters.export(cq.Workplane(obj=view), "preview-v3-front.svg", opt=opts)
opts["projectionDir"] = (-0.35, 0.9, 0.3)
cq.exporters.export(cq.Workplane(obj=view), "preview-v3-usb.svg", opt=opts)
opts["projectionDir"] = (-0.9, 0.3, 0.75)
cq.exporters.export(cq.Workplane(obj=view), "preview-v3-rocker.svg", opt=opts)
