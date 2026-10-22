# Анцыбал — основа (тело, одно на весь вид) для Blender 5.x.
# Запуск: вставить в Text Editor и Run Script, или через Blender MCP execute_code.
# Пересоздаёт коллекцию «Antsybal»: тело (low-poly, цвет в атрибуте Col), скелет AntsybalRig с весами,
# кости-сокеты s_* (без деформации) — к ним в UE крепятся сменные детали: волосы, тина, мох.
#
# Облик — по референсу: тощий сутулый болотный гоблин — маленькая голова, большие тёмные глаза,
# острые уши, длинные тонкие руки с когтистыми пальцами, человеческие ноги со стопами и когтями на пальцах.
# Волосы, мох и свисающая тина — сменный слой, не основа.
#
# Тело строится модификатором Skin по графу «костяк + радиусы»: так силуэт правится числами ниже,
# а сетка замкнутая. Потом Subsurf → шум → Decimate: неровные треугольные грани low-poly.
# Цвет — по граням, по ближайшему участку костяка. Веса — тоже по костяку (см. внизу).
#
# Координаты в графе: см, f — вперёд, s — влево, z — вверх. В Blender персонаж смотрит в −Y, лево — +X.

import bpy, bmesh, math, random
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree

COLLECTION = "Antsybal"
HUNCH = math.radians(10)   # сутулость
TARGET_TRIS = 5000         # треугольников в теле после Decimate
VOXEL = 0.35               # см, шаг Remesh — тоньше самых тонких пальцев

COLORS = {  # линейные; альфа — свечение (глаза)
    "skin": (0.055, 0.065, 0.05, 0), "belly": (0.07, 0.078, 0.06, 0), "dark": (0.03, 0.035, 0.028, 0),
    "claw": (0.02, 0.02, 0.018, 0), "pupil": (0.003, 0.003, 0.003, 0),
    "socket": (0.014, 0.016, 0.012, 0),                                   # тёмная запавшая глазница
    "sclera": (0.025, 0.027, 0.02, 0), "iris": (0.2, 0.22, 0.15, 0.3),   # глаз тёмный, радужка бледная, чуть светится
}


def bl(v):  # f,s,z (см) → Blender (м)
    return Vector((v.y, -v.x, v.z)) * 0.01


def fsz(v):  # Blender (м) → f,s,z (см)
    return Vector((-v.y, v.x, v.z)) * 100.0


# ---------------- Костяк ----------------
# Граф — набор цепочек без развилок: у Skin развилки на 4–6 веток (голова, ладонь, плечевой пояс) не сходятся
# и рвут сетку. Каждая цепочка (рука, палец, ухо…) начинается внутри того, к чему крепится.
G_P, G_R, G_E, G_TAG, G_BONE, ROOTS = [], [], [], [], [], []
G_RXY = []  # радиусы сечения (вбок, вглубь); G_R — средний, для весов и цвета
CLAWS = []  # (основание, кончик, радиус, кость) — тоньше вокселя, ставятся отдельными шипами
BONE = "pelvis"  # кость, к которой относятся следующие участки


def node(p, r, parent=None, tag="skin"):
    rxy = r if isinstance(r, tuple) else (r, r)
    G_P.append(Vector(p)); G_R.append((rxy[0] + rxy[1]) / 2); G_RXY.append(rxy)
    if parent is None:
        ROOTS.append(len(G_P) - 1)
    else:
        G_E.append((parent, len(G_P) - 1)); G_TAG.append(tag); G_BONE.append(BONE)
    return len(G_P) - 1


def chain(points, tag="skin"):  # [(точка, радиус, кость участка до неё), …]; первая точка — корень
    global BONE
    prev = node(points[0][0], points[0][1])
    for p, r, bone_name in points[1:]:
        BONE = bone_name
        prev = node(p, r, prev, tag)
    return prev


Sp = Vector((math.sin(HUNCH), 0, math.cos(HUNCH)))   # ось туловища
Bk = Vector((-math.cos(HUNCH), 0, math.sin(HUNCH)))  # из спины наружу
# Пропорции — человеческие (рост ~170, голова ~23 см, плечи ~36 см), телосложение — тощее.
# Радиусы чуть больше «живых»: Subsurf и Remesh стягивают тело.
PEL = Vector((0, 0, 95))
SH = PEL + Sp * 50                   # основание шеи
HEAD = SH + Vector((3, 0, 19))       # голова — над плечами
COLLAR = PEL + Sp * 36               # верх грудной клетки — ниже линии плеч, иначе вместо шеи выходит конус
UPPER_BACK = PEL + Sp * 40 + Bk * 14  # точка на загривке — для сокета мха


def hp(x, y, z):
    return HEAD + Vector((x, y, z))


# Глаза большие, но посажены глубоко: из глазницы выходит только передняя часть яблока (EYE_OUT).
EYE_R, EYE_OUT = 3.6, 1.5


# Туловище: таз → впалый живот → грудная клетка → плечевой пояс → шея → череп → лицо → подбородок.
# Сечение туловища — овал (вбок, вглубь): человек шире, чем толще.
chain([(PEL + Vector((0, 0, -10)), (10, 8), None),   # промежность: торец цепочки — ниже стыка с ногами
       (PEL, (15, 11), "pelvis"),
       (PEL + Sp * 14, (13, 9.5), "spine_01"),
       (PEL + Sp * 26, (16, 11.5), "spine_02"),
       (COLLAR, (15, 10), "spine_03"),
       (PEL + Sp * 44, (9, 7), "spine_03"),
       (SH, 5.5, "spine_03"),
       (SH + Vector((1.5, 0, 7)), 5.2, "neck_01"),
       (hp(-1, 0, 3), 9.8, "head"),
       (hp(3, 0, -2), 8, "head"),
       (hp(4.5, 0, -10), 4.5, "jaw")])
ARM, LEG = {}, {}
for s in (1, -1):
    side = "l" if s > 0 else "r"
    # Острые уши — вбок и вверх.
    chain([(hp(-1, s * 8, 0), 2.2, None), (hp(-3, s * 12.5, 3), 1.4, f"ear_{side}"), (hp(-6, s * 16.5, 7), 0.3, f"ear_{side}")])
    # Руки — длинные и тонкие, в T-позе (под риг): горизонтально, ладонью вниз.
    sho = SH + Vector((-2, s * 18, -7))
    elb = sho + Vector((0, s * 30, 0))
    wr = elb + Vector((0, s * 28, 0))
    palm = wr + Vector((0, s * 5, -0.5))
    chain([(COLLAR + Vector((0, s * 6, 0)), 8, None), (sho, 6, "spine_03"),
           ((sho + elb) / 2, 3.0, f"upperarm_{side}"), (elb, 3.5, f"upperarm_{side}"),
           ((elb + wr) / 2 + Vector((0, 0, 0.5)), 2.6, f"lowerarm_{side}"), (wr, 2.3, f"lowerarm_{side}"),
           (palm, 3, f"hand_{side}")])
    # Четыре пальца в ряд спереди назад, чуть подогнуты вниз, и большой — спереди; все с когтями.
    hand = f"hand_{side}"
    for k in (-1.5, -0.5, 0.5, 1.5):
        b = palm + Vector((k * 2.1, s * 3.5, -0.3))
        bend = b + Vector((k * 0.5, s * 7, -0.8))
        tip = bend + Vector((0, s * 6, -1.5))
        last = chain([(palm + Vector((k * 1.2, s * 1, 0)), 1.3, None), (b, 1.0, hand), (bend, 0.85, hand), (tip, 0.65, hand)], "dark")
        CLAWS.append((tip, tip + Vector((0, s * 3.5, -2)), 0.6, hand))
    tb = palm + Vector((3, s * 1.5, -0.5))
    tbend = tb + Vector((4, s * 3, -1))
    ttip = tbend + Vector((2, s * 2, -1))
    last = chain([(palm + Vector((1.5, 0, 0)), 1.4, None), (tb, 1.1, hand), (tbend, 0.9, hand), (ttip, 0.7, hand)], "dark")
    CLAWS.append((ttip, ttip + Vector((1.5, s * 1.5, -1)), 0.65, hand))
    ARM[s] = (sho, elb, wr, palm)
    # Ноги тонкие, человеческие; стопа длинная, костлявая: пятка и три пальца с когтями.
    hip = PEL + Vector((0, s * 9, -6))
    knee = Vector((3, s * 10.5, 50))
    ankle = Vector((-1, s * 11, 8))
    ball = Vector((11, s * 12, 2.6))
    foot = f"foot_{side}"
    chain([(PEL + Vector((0, s * 4, 5)), 9, None), (hip, 8, "pelvis"),
           ((hip + knee) / 2, 6.2, f"thigh_{side}"), (knee, 4.8, f"thigh_{side}"),
           ((knee + ankle) / 2 + Vector((-1.5, 0, 6)), 4.2, f"calf_{side}"), (ankle, 2.8, f"calf_{side}"),
           (ball, 2.4, foot)])
    chain([(ankle, 2.6, None), (ankle + Vector((-4, 0, -4.5)), 2.4, foot)], "dark")   # пятка
    for t in (-1, 0, 1):
        toe = ball + Vector((4, t * 1.8, -0.2))
        toe_tip = toe + Vector((4, t * 0.6, -0.8))
        last = chain([(ball + Vector((0, t * 1, 0)), 1.5, None), (toe, 1.0, f"ball_{side}"), (toe_tip, 0.6, f"ball_{side}")], "dark")
        CLAWS.append((toe_tip, toe_tip + Vector((2.5, t * 0.2, -1.2)), 0.55, f"ball_{side}"))
    LEG[s] = (hip, knee, ankle, ball)


# ---------------- Сцена ----------------
def reset_collection():
    col = bpy.data.collections.get(COLLECTION)
    if col:
        for o in list(col.objects):
            bpy.data.objects.remove(o, do_unlink=True)
    else:
        col = bpy.data.collections.new(COLLECTION)
        bpy.context.scene.collection.children.link(col)
    return col


col = reset_collection()

# Тело: Skin по цепочкам → Remesh вокселями (сливает цепочки в одну цельную поверхность, без швов на стыках)
# → шум → Decimate до TARGET_TRIS: неровные треугольные грани low-poly.
skel_me = bpy.data.meshes.new("Antsybal_Skeleton")
skel_me.from_pydata([bl(p) for p in G_P], G_E, [])
skel = bpy.data.objects.new("Antsybal_Skeleton", skel_me)
col.objects.link(skel)
skin = skel.modifiers.new("Skin", 'SKIN')
skin.use_smooth_shade = False
if len(skel_me.skin_vertices) == 0:
    skel_me.skin_vertices.new()
for i, (rx, ry) in enumerate(G_RXY):
    skel_me.skin_vertices[0].data[i].radius = (rx * 0.01, ry * 0.01)
for i in ROOTS:
    skel_me.skin_vertices[0].data[i].use_root = True
sub = skel.modifiers.new("Subsurf", 'SUBSURF'); sub.levels = 1; sub.render_levels = 1
remesh = skel.modifiers.new("Merge", 'REMESH'); remesh.mode = 'VOXEL'; remesh.voxel_size = VOXEL * 0.01
tex = bpy.data.textures.get("AntsybalLumps") or bpy.data.textures.new("AntsybalLumps", 'CLOUDS')
tex.noise_scale = 0.08
disp = skel.modifiers.new("Lumps", 'DISPLACE'); disp.texture = tex; disp.strength = 0.006
dec = skel.modifiers.new("LowPoly", 'DECIMATE')
dec.show_viewport = False
dense = len(skel.evaluated_get(bpy.context.evaluated_depsgraph_get()).data.polygons)
dec.show_viewport = True
dec.ratio = min(1.0, TARGET_TRIS / max(2 * dense, 1))  # Remesh даёт четырёхугольники
dg = bpy.context.evaluated_depsgraph_get()
body_me = bpy.data.meshes.new_from_object(skel.evaluated_get(dg))
body_me.name = "Antsybal"
skel.hide_set(True); skel.hide_render = True  # исходник силуэта: правится радиусами выше

# Глаза сажаются по настоящей поверхности лица (Subsurf и Remesh сжимают голову против радиусов костяка):
# луч из головы находит, где выходит кожа, яблоко утапливается так, чтобы снаружи осталось EYE_OUT.
face_tree = BVHTree.FromPolygons([v.co for v in body_me.vertices], [p.vertices for p in body_me.polygons])
EYES = {}
for s in (1, -1):
    look = Vector((1, s * 0.25, 0.05)).normalized()
    hit = face_tree.ray_cast(bl(hp(1, 0, 1.8)), bl(Vector((1, s * 0.62, 0.0))).normalized())[0]
    EYES[s] = (fsz(hit) - look * (EYE_R - EYE_OUT), look, fsz(hit))  # центр яблока, взгляд, точка на коже


def on_face(origin, direction):  # точка на коже лица по лучу из головы
    return fsz(face_tree.ray_cast(bl(origin), bl(direction).normalized())[0])

# Цвет граней — по ближайшему участку костяка (расстояние до поверхности конуса участка).
rng = random.Random(7)
segs = [(G_P[a], G_P[b], G_R[a], G_R[b], G_TAG[k]) for k, (a, b) in enumerate(G_E)]


def tag_at(c):
    best, bd = "skin", 1e9
    for a, b, ra, rb, t in segs:
        ab = b - a
        u = max(0.0, min(1.0, (c - a).dot(ab) / max(ab.length_squared, 1e-6)))
        d = (a + ab * u - c).length - (ra + (rb - ra) * u)
        if d < bd:
            bd, best = d, t
    return best


bm = bmesh.new()
bm.from_mesh(body_me)
col_layer = bm.loops.layers.float_color.new("Col")
bind_layer = bm.verts.layers.int.new("bind_bone")  # детали, жёстко сидящие на одной кости: индекс в BIND + 1
BIND = ["head"]


def paint(face, tag):
    c = COLORS[tag]
    k = 1 + rng.uniform(-0.09, 0.09)  # разброс тона по граням — лепка low-poly
    for loop in face.loops:
        loop[col_layer] = (c[0] * k, c[1] * k, c[2] * k, c[3])
    face.smooth = False


for f in bm.faces:
    c = fsz(f.calc_center_median())
    near_eye = any((c - e).length < EYE_R + 1.3 for e, _, _ in EYES.values())
    paint(f, "socket" if near_eye else tag_at(c))


def add_part(make, tag, bone_name="head"):
    if bone_name not in BIND:
        BIND.append(bone_name)
    before = set(bm.faces)
    verts = make()["verts"]
    for v in verts:
        v[bind_layer] = BIND.index(bone_name) + 1
    for f in set(bm.faces) - before:
        paint(f, tag)


def blob(center, radii, rot, tag, segments=8, rings=6):
    m = Matrix.Translation(bl(center)) @ rot @ Matrix.Diagonal((radii.x * 0.01, radii.y * 0.01, radii.z * 0.01, 1))
    add_part(lambda: bmesh.ops.create_uvsphere(bm, u_segments=segments, v_segments=rings, radius=1.0, matrix=m), tag)


I4 = Matrix.Identity(4)
for s in (1, -1):
    e, look, skin_pt = EYES[s]
    # Яблоко: зрачок и радужка нарисованы на его гранях — по углу от направления взгляда.
    m = Matrix.Translation(bl(e)) @ Matrix.Diagonal((EYE_R * 0.01, EYE_R * 0.01, EYE_R * 0.01, 1))
    before = set(bm.faces)
    for v in bmesh.ops.create_uvsphere(bm, u_segments=16, v_segments=12, radius=1.0, matrix=m)["verts"]:
        v[bind_layer] = BIND.index("head") + 1
    for f in set(bm.faces) - before:
        cos = (fsz(f.calc_center_median()) - e).normalized().dot(look)
        paint(f, "pupil" if cos > 0.965 else "iris" if cos > 0.86 else "sclera")
    # Верхнее веко — скошено к переносице: взгляд исподлобья.
    blob(skin_pt + Vector((-0.8, 0, 3.0)), Vector((2.2, 3.6, 0.9)), Matrix.Rotation(math.radians(-8 * s), 4, 'Y'), "dark", 8, 4)
blob(on_face(hp(1, 0, -1.5), Vector((1, 0, 0))) + Vector((0.5, 0, 0)), Vector((1.6, 1.2, 2)), I4, "dark", 6, 4)  # нос
blob(on_face(hp(1, 0, -6.5), Vector((1, 0, 0))) - Vector((0.3, 0, 0)), Vector((0.5, 3.2, 0.4)), I4, "pupil", 8, 3)  # щель рта


def spike(base, tip, r, tag, bone_name, sides=4):
    d = bl(tip) - bl(base)
    m = Matrix.Translation((bl(base) + bl(tip)) / 2) @ d.to_track_quat('Z', 'Y').to_matrix().to_4x4()
    add_part(lambda: bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=True, segments=sides,
                                           radius1=r * 0.01, radius2=0.0, depth=d.length, matrix=m), tag, bone_name)


for base, tip, r, bone_name in CLAWS:  # коготь начинается внутри кончика пальца
    spike(base - (tip - base).normalized() * 0.8, tip, r, "claw", bone_name)

bm.to_mesh(body_me)
bm.free()
body = bpy.data.objects.new("Antsybal", body_me)
col.objects.link(body)

# ---------------- Скелет (имена — как у Manny) ----------------
arm_data = bpy.data.armatures.new("AntsybalRig")
rig = bpy.data.objects.new("AntsybalRig", arm_data)
col.objects.link(rig)
rig.show_in_front = True
for o in bpy.context.view_layer.objects:
    o.select_set(False)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
eb = arm_data.edit_bones


def bone(name, h, t, parent=None, deform=True):
    b = eb.new(name)
    b.head, b.tail = bl(h), bl(t)
    b.parent = eb[parent] if parent else None
    b.use_deform = deform
    return b


bone("root", Vector((0, 0, 0)), Vector((15, 0, 0)), deform=False)
bone("pelvis", PEL, PEL + Sp * 12, "root")
bone("spine_01", PEL + Sp * 12, PEL + Sp * 24, "pelvis")
bone("spine_02", PEL + Sp * 24, PEL + Sp * 36, "spine_01")
bone("spine_03", PEL + Sp * 36, SH, "spine_02")
bone("neck_01", SH, hp(-1, 0, -8), "spine_03")
bone("head", hp(-1, 0, -8), hp(-1, 0, 12), "neck_01")
bone("jaw", hp(0, 0, -4), hp(4.5, 0, -10), "head")
for s, side in ((1, "l"), (-1, "r")):
    bone(f"ear_{side}", hp(-1, s * 8, 0), hp(-6, s * 16.5, 7), "head")
    sho, elb, wr, palm = ARM[s]
    bone(f"upperarm_{side}", sho, elb, "spine_03")
    bone(f"lowerarm_{side}", elb, wr, f"upperarm_{side}")
    bone(f"hand_{side}", wr, palm + Vector((0, s * 12, -1)), f"lowerarm_{side}")
    hip, knee, ankle, ball = LEG[s]
    bone(f"thigh_{side}", hip, knee, "pelvis")
    bone(f"calf_{side}", knee, ankle, f"thigh_{side}")
    bone(f"foot_{side}", ankle, ball, f"calf_{side}")
    bone(f"ball_{side}", ball, ball + Vector((9, 0, -1)), f"foot_{side}")
    # Сокеты под мох и тину: плечо, предплечье, пояс, голень.
    bone(f"s_shoulder_{side}", sho + Vector((0, 0, 5)), sho + Vector((0, 0, 11)), f"upperarm_{side}", False)
    bone(f"s_forearm_{side}", (elb + wr) / 2, (elb + wr) / 2 + Vector((0, 0, 6)), f"lowerarm_{side}", False)
    bone(f"s_waist_{side}", hip + Vector((0, s * 3, 6)), hip + Vector((0, s * 8, 6)), "pelvis", False)
    bone(f"s_shin_{side}", (knee + ankle) / 2, (knee + ankle) / 2 + Vector((6, 0, 0)), f"calf_{side}", False)
bone("s_crown", hp(-2, 0, 8.5), hp(-2, 0, 14), "head", False)   # волосы с макушки
bone("s_nape", hp(-7, 0, 1), hp(-11, 0, -4), "head", False)     # волосы с затылка
bone("s_upperback", UPPER_BACK, UPPER_BACK + Bk * 6, "spine_03", False)  # мох на загривке
bpy.ops.object.mode_set(mode='OBJECT')

# Веса — по костяку: вершина берёт кость ближайшего участка (по расстоянию до его поверхности) и плавно
# смешивается только с соседними по графу участками — на суставах мягко, а рука не цепляет бедро, к которому прижата.
# Детали головы (глаза, нос, рот) — целиком на head. Heat-веса Blender на тонких пальцах не сходятся.
FALLOFF = 3.0  # см
adjacent = [[j for j, (c, d) in enumerate(G_E) if {c, d} & {a, b}] for (a, b) in G_E]
bind = body_me.attributes["bind_bone"].data
groups = {}
for v in body_me.vertices:
    if bind[v.index].value:
        acc = {BIND[bind[v.index].value - 1]: 1.0}
    else:
        c = fsz(v.co)
        dist = []
        for a, b in G_E:
            pa, pb = G_P[a], G_P[b]
            ab = pb - pa
            u = max(0.0, min(1.0, (c - pa).dot(ab) / max(ab.length_squared, 1e-6)))
            dist.append((pa + ab * u - c).length - (G_R[a] + (G_R[b] - G_R[a]) * u))
        near = min(range(len(G_E)), key=dist.__getitem__)
        acc = {}
        for j in adjacent[near]:
            acc[G_BONE[j]] = acc.get(G_BONE[j], 0.0) + math.exp(-(dist[j] - dist[near]) / FALLOFF)
    top = sorted(acc.items(), key=lambda kv: -kv[1])[:3]
    total = sum(w for _, w in top)
    for name, w in top:
        if name not in groups:
            groups[name] = body.vertex_groups.new(name=name)
        groups[name].add([v.index], w / total, 'REPLACE')
body_me.attributes.remove(body_me.attributes["bind_bone"])
body.parent = rig
body.modifiers.new("Armature", 'ARMATURE').object = rig
body_me.color_attributes.active_color = body_me.color_attributes["Col"]


# ---------------- Обрастание: тина и патлы — сменный слой (мох накладывается скриптами, не в модели) ----------------
# Отдельные объекты на том же скелете; каждый строится из своего сида — так получаются варианты для спавна.
# Каждая прядь жёстко сидит на кости, из которой растёт.
ALGAE_COLORS = [(0.03, 0.05, 0.015), (0.05, 0.055, 0.025), (0.04, 0.04, 0.02)]
HAIR_COLORS = [(0.045, 0.05, 0.04), (0.06, 0.065, 0.05), (0.035, 0.045, 0.03)]
DOWN = Vector((0, 0, -1))


def bone_at(c):
    best, bd = "pelvis", 1e9
    for k, (a, b) in enumerate(G_E):
        pa, pb = G_P[a], G_P[b]
        ab = pb - pa
        u = max(0.0, min(1.0, (c - pa).dot(ab) / max(ab.length_squared, 1e-6)))
        d = (pa + ab * u - c).length - (G_R[a] + (G_R[b] - G_R[a]) * u)
        if d < bd:
            bd, best = d, G_BONE[k]
    return best


# Поверхность тела для раскладки: центр грани, нормаль, площадь, кость (всё в f,s,z).
SURFACE = []
for poly in body_me.polygons:
    c = fsz(poly.center)
    n = Vector((-poly.normal.y, poly.normal.x, poly.normal.z))
    SURFACE.append((c, n, poly.area * 1e4, bone_at(c)))


class Part:
    """Отдельный объект обрастания: пряди, у каждой — своя кость."""

    def __init__(self, name, seed):
        self.name, self.rng = name, random.Random(seed)
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.float_color.new("Col")
        self.bind = self.bm.verts.layers.int.new("bind")
        self.bones = []

    def _own(self, verts, color, bone_name):
        if bone_name not in self.bones:
            self.bones.append(bone_name)
        faces = set()
        for v in verts:
            v[self.bind] = self.bones.index(bone_name)
            faces.update(v.link_faces)
        for f in faces:
            k = 1 + self.rng.uniform(-0.12, 0.12)
            for loop in f.loops:
                loop[self.col] = (color[0] * k, color[1] * k, color[2] * k, 0)
            f.smooth = False

    def strand(self, points, radii, color, bone_name, sides=3):
        pts = [bl(p) for p in points]
        rings, u_prev, verts = [], None, []
        for i, p in enumerate(pts):
            if radii[i] <= 0:
                v = self.bm.verts.new(p); rings.append([v]); verts.append(v)
                continue
            axis = (pts[min(i + 1, len(pts) - 1)] - pts[max(i - 1, 0)]).normalized()
            ref = u_prev or (Vector((0, 0, 1)) if abs(axis.z) < 0.9 else Vector((1, 0, 0)))
            u = (ref - axis * ref.dot(axis)).normalized(); w = axis.cross(u); u_prev = u
            ring = [self.bm.verts.new(p + (u * math.cos(a) + w * math.sin(a)) * radii[i] * 0.01)
                    for a in (2 * math.pi * k / sides for k in range(sides))]
            rings.append(ring); verts += ring
        for i, (r0, r1) in enumerate(zip(rings, rings[1:])):
            mid = (pts[i] + pts[i + 1]) / 2
            for k in range(sides):
                k1 = (k + 1) % sides
                f = self.bm.faces.new((r0[k], r0[k1], r1[0]) if len(r1) == 1 else (r0[k], r0[k1], r1[k1], r1[k]))
                f.normal_update()
                if f.normal.dot(f.calc_center_median() - mid) < 0:
                    f.normal_flip()
        self._own(verts, color, bone_name)

    def hang(self, root, length, radius, color, bone_name, steps=4, sway=0.25, push=None):
        pts = [root]
        for i in range(1, steps + 1):
            t = i / steps
            d = DOWN + Vector((self.rng.uniform(-sway, sway), self.rng.uniform(-sway, sway), 0))
            if push is not None:
                d += push * (1 - t)
            pts.append(pts[-1] + d.normalized() * length / steps)
        self.strand(pts, [radius * (1 - i / steps) for i in range(steps + 1)], color, bone_name)

    def finish(self):
        me = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(me)
        self.bm.free()
        ob = bpy.data.objects.new(self.name, me)
        col.objects.link(ob)
        bind_data = me.attributes["bind"].data
        for idx, bone_name in enumerate(self.bones):
            ob.vertex_groups.new(name=bone_name).add([v.index for v in me.vertices if bind_data[v.index].value == idx], 1.0, 'REPLACE')
        me.attributes.remove(me.attributes["bind"])
        me.color_attributes.active_color = me.color_attributes["Col"]
        ob.parent = rig
        ob.modifiers.new("Armature", 'ARMATURE').object = rig
        return ob


def build_algae(seed, count=26):
    part = Part(f"Antsybal_Algae_{seed}", seed)
    rng = part.rng
    belt = [(c, n, a, b) for c, n, a, b in SURFACE if b in ("pelvis", "spine_01", "thigh_l", "thigh_r") and 78 < c.z < 100 and n.z > -0.3]
    for c, n, a, b in rng.choices(belt, weights=[a for _, _, a, _ in belt], k=count):  # тина свисает с пояса
        out = Vector((n.x, n.y, 0)).normalized() * 0.3 if n.xy.length > 0.1 else None
        part.hang(c, rng.uniform(15, 35), rng.uniform(0.8, 1.3), rng.choice(ALGAE_COLORS), b, steps=4, sway=0.15, push=out)
    return part.finish()


def build_hair(seed, count=40):
    part = Part(f"Antsybal_Hair_{seed}", seed)
    rng = part.rng
    skull = hp(-1, 0, 3)
    made = 0
    while made < count:
        d = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-0.2, 1))).normalized()
        if d.x > 0.25:  # лицо открыто: патлы растут с макушки, висков и затылка
            continue
        out = Vector((d.x, d.y, 0))
        out = out.normalized() if out.length > 0.05 else Vector((-1, 0, 0))
        root = on_face(skull, d) - d * 0.6
        part.hang(root, rng.uniform(22, 45), rng.uniform(1.0, 1.6), rng.choice(HAIR_COLORS), "head",
                  steps=5, sway=0.1, push=out * 0.9)
        made += 1
    return part.finish()


build_algae(1)
build_hair(1)

unweighted = sum(1 for v in body_me.vertices if not any(g.weight > 0 for g in v.groups))
tris = {o.name: sum(len(p.vertices) - 2 for p in o.data.polygons) for o in col.objects if o.type == 'MESH' and not o.hide_render}
print(f"Antsybal: {len(arm_data.bones)} bones, unweighted body verts: {unweighted}, tris: {tris}, total {sum(tris.values())}")
