import zipfile
import cadquery as cq
def load(p): return cq.importers.importStep(p).val()
def mesh(s):
    v, t = s.tessellate(0.02, 0.1)
    return [(a.x, a.y, a.z) for a in v], t
shell = load("pager-case-v3-shell.step").rotate((0,0,0),(1,0,0),180)        # screen side down
lid = load("pager-case-v3-lid.step")                                        # outside face down
cap = load("pager-case-v3-button-cap.step").rotate((0,0,0),(0,1,0),-90)     # flange down
items = [("Pager shell v3", shell, 128, 104), ("Pager lid v3", lid, 128, 148)]
for i, x in enumerate((112, 128, 144)):
    items.append((f"Button cap {i+1}", cap, x, 176))
parts = []
for name, shape, cx, cy in items:
    bb = shape.BoundingBox()
    shape = shape.translate((cx-(bb.xmin+bb.xmax)/2, cy-(bb.ymin+bb.ymax)/2, -bb.zmin))
    bb = shape.BoundingBox()
    print(f"{name}: {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f}")
    parts.append((name, *mesh(shape)))
objs, its = [], []
for i, (name, verts, tris) in enumerate(parts, 1):
    v = "".join(f'<vertex x="{x:.4f}" y="{y:.4f}" z="{z:.4f}"/>' for x, y, z in verts)
    t = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in tris)
    objs.append(f'<object id="{i}" type="model" name="{name}"><mesh><vertices>{v}</vertices><triangles>{t}</triangles></mesh></object>')
    its.append(f'<item objectid="{i}"/>')
model = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
         f'<metadata name="Title">Pager case - draft 3</metadata><resources>{"".join(objs)}</resources><build>{"".join(its)}</build></model>')
with zipfile.ZipFile("pager-case-v3-print.3mf", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
    z.writestr("_rels/.rels", '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
    z.writestr("3D/3dmodel.model", model)
