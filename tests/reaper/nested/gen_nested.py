#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates setup.json and nested.json next to this file. Run prettier on them afterwards (see tests/reaper/README.md)."""
import json, math, sys, os, copy

OUT = os.path.dirname(os.path.abspath(__file__))
EXPLORE = len(sys.argv) > 1 and sys.argv[1] == "explore"


def pan(p):
    return (p + 100.0) / 200.0


def vol(db):
    return (db + 24.0) / 36.0


def width(w):
    return w / 100.0


# ---------------------------------------------------------------- plugins and states
TEMPLATE = "states/template.rpp"


def st(track, fx):
    return {"template": TEMPLATE, "track": track, "fx": fx}


PLUGINS = {
    "input": {
        "bundle": "SCVB Input.vst3",
        "states": {
            "a1": st("VA1", 1),
            "a2": st("VA2", 1),
            "b1": st("VB1", 1),
            "h1": st("GA", 2),
            "h2": st("GB", 2),
            "h3": st("SPARE", 1),
            "a3": st("SPARE", 2),
        },
    },
    "output": {
        "bundle": "SCVB Output.vst3",
        "states": {"h": st("TOP", 1), "a": st("GA", 1), "b": st("GB", 1)},
    },
    "monitor": {"bundle": "SCVB Monitor.vst3", "states": {"h": st("SPARE", 3)}},
    "delay": {
        "jsfx": "harness-pdc-delay",
        "expectPdc": 64,
        "params": {"Delay (samples)": {"normalized": 0.0078125, "display": "64.0"}},
    },
}


# The projects the analysis session saved, as states of their own: <name>-<suffix>.
for suffix, rpp in (("on", "analyzed-on"), ("off-first", "analyzed-off-first"), ("off-after", "analyzed-off-after"), ("auto", "automated-on")):
    path = f"states/{rpp}.rpp"
    for name, track, slot in (("a1", "VA1", 1), ("a2", "VA2", 1), ("b1", "VB1", 1), ("h1", "GA", 2), ("h2", "GB", 2)):
        PLUGINS["input"]["states"][f"{name}-{suffix}"] = {"template": path, "track": track, "fx": slot}
    for name, track in (("h", "TOP"), ("a", "GA"), ("b", "GB")):
        PLUGINS["output"]["states"][f"{name}-{suffix}"] = {"template": path, "track": track, "fx": 1}


def fx(plugin, state=None, params=None, bypass=None):
    d = {"plugin": plugin}
    if state:
        d["state"] = state
    if params:
        d["params"] = params
    if bypass is not None:
        d["bypass"] = bypass
    return d


VA1 = fx("input", "a1")
VA2 = fx("input", "a2")
VB1 = fx("input", "b1")
HA = fx("input", "h1")
HB = fx("input", "h2")
OA = fx("output", "a")
OB = fx("output", "b")
OH = fx("output", "h")


def src(name, parent, seed, chain):
    return {"name": name, "source": True, "parent": parent, "signal": {"seed": seed}, "fx": chain}


def silent(name, parent, chain):
    return {"name": name, "source": False, "parent": parent, "fx": chain}


def nest(top=None, ga=None, gb=None, va1=None, va2=None, vb1=None, sig=(True, True, True), gbParent="TOP", gaExtra=None, gbExtra=None, extra=None):
    """L-nest: TOP > GA[Output A, Input H1] > VA1, VA2; GB[Output B, Input H2] > VB1."""
    t = [{"name": "TOP", "source": False, "folder": True, "fx": top if top is not None else [OH]}]
    ga_t = {"name": "GA", "source": False, "folder": True, "parent": "TOP", "fx": ga if ga is not None else [OA, HA]}
    if gaExtra:
        ga_t.update(gaExtra)
    t.append(ga_t)
    for name, seed, chain, on in (("VA1", 11, va1 if va1 else [VA1], sig[0]), ("VA2", 12, va2 if va2 else [VA2], sig[1])):
        t.append(src(name, "GA", seed, chain) if on else silent(name, "GA", chain))
    gb_t = {"name": "GB", "source": False, "folder": True, "fx": gb if gb is not None else [OB, HB]}
    if gbParent:
        gb_t["parent"] = gbParent
    if gbExtra:
        gb_t.update(gbExtra)
    t.append(gb_t)
    t.append(src("VB1", "GB", 13, vb1 if vb1 else [VB1]) if sig[2] else silent("VB1", "GB", vb1 if vb1 else [VB1]))
    if extra:
        t.extend(extra)
    return {"tracks": t}


def ref(ga=None, gb=None, va1=None, va2=None, vb1=None, sig=(True, True, True)):
    """L-ref: the same two groups without the upper level."""
    t = [{"name": "GA", "source": False, "folder": True, "fx": ga if ga is not None else [OA]}]
    for name, seed, chain, on in (("VA1", 11, va1 if va1 else [VA1], sig[0]), ("VA2", 12, va2 if va2 else [VA2], sig[1])):
        t.append(src(name, "GA", seed, chain) if on else silent(name, "GA", chain))
    t.append({"name": "GB", "source": False, "folder": True, "fx": gb if gb is not None else [OB]})
    t.append(src("VB1", "GB", 13, vb1 if vb1 else [VB1]) if sig[2] else silent("VB1", "GB", vb1 if vb1 else [VB1]))
    return {"tracks": t}


AUX = {"name": "AUX", "source": False}


def oa_pos():
    return fx("output", "a", {"V1 T01 Pan": pan(-100), "V1 T02 Pan": pan(-100)})


def ob_pos(db=None):
    p = {"V1 T01 Pan": pan(100)}
    if db is not None:
        p["V1 T01 Vol"] = vol(db)
    return fx("output", "b", p)


LAYOUTS = {
    "base": {
        "tracks": [
            {"name": "GA", "source": False, "folder": True, "fx": [OA]},
            src("VA1", "GA", 11, [VA1]),
            src("VA2", "GA", 12, [VA2]),
        ]
    },
    "ref": ref(),
    "nest": nest(),
    # N3: every group's own positions
    "ref-pos": ref(ga=[oa_pos()], gb=[ob_pos()]),
    "nest-pos": nest(ga=[oa_pos(), HA], gb=[ob_pos(), HB]),
    "ref-pos-b24": ref(ga=[oa_pos()], gb=[ob_pos(-24)]),
    "nest-pos-b24": nest(ga=[oa_pos(), HA], gb=[ob_pos(-24), HB]),
    # N4: the upper level places a whole group (one source sounding, so L and R follow from the two-level formula)
    "dry-va1": {
        "tracks": [
            {"name": "VA1", "source": True, "signal": {"seed": 11}},
        ]
    },
    "nest-h-va1": nest(
        top=[fx("output", "h", {"V1 T01 Pan": pan(-50), "V1 T01 Width": width(30)})], sig=(True, False, False)
    ),
    "nest-a-h-va1": nest(
        top=[fx("output", "h", {"V1 T01 Pan": pan(-50), "V1 T01 Width": width(30)})],
        ga=[fx("output", "a", {"V1 T01 Pan": pan(40), "V1 T01 Width": width(50), "V1 T01 Vol": vol(-6)}), HA],
        sig=(True, False, False),
    ),
    "nest-h-vol": nest(top=[fx("output", "h", {"V1 T01 Vol": vol(-12), "V1 T02 Pan": pan(100), "V1 T02 Width": width(0)})]),
    "ref-h-vb-only": ref(sig=(False, False, True)),
    # N6: what the group bus fader, pan, mute and sends do once the group goes up through Input H
    "nest-ga-vol0": nest(gaExtra={"volume": 0.0}),
    "nest-ga-pan": nest(gaExtra={"pan": -1.0}),
    "nest-ga-mute": nest(gaExtra={"mute": True}),
    "nest-ga-sends": nest(gaExtra={"sends": [{"to": "AUX", "mode": "post"}, {"to": "AUX", "mode": "pre"}, {"to": "AUX", "mode": "prefx"}]}, extra=[AUX]),
    # N7: Input H bypassed on GA
    "nest-hbyp": nest(ga=[OA, fx("input", "h1", bypass=True)]),
    "ref-a-only": ref(sig=(True, True, False)),
    "ref-b-only": ref(sig=(False, False, True)),
    # N8: latency
    "ref-pdc-ga": ref(ga=[OA, fx("delay")]),
    "nest-pdc-ga": nest(ga=[OA, fx("delay"), HA]),
    "ref-pdc-v": ref(va1=[fx("delay"), VA1]),
    "nest-pdc-v": nest(va1=[fx("delay"), VA1]),
    "nest-pdc-top": nest(top=[OH, fx("delay")]),
    # N9: wrong wiring, throwaway projects
    "bad-top-h3": nest(top=[OH, fx("input", "h3")]),
    "bad-ga-a3": nest(ga=[OA, fx("input", "a3"), HA]),
    "bad-ga-reversed": nest(ga=[HA, OA]),
    "bad-gb-master": nest(gbParent=None),
    # N10: conflicts in one group
    "conf-h-ch1": nest(gb=[OB, HA]),
    "conf-two-oh": nest(gb=[OH, HB]),
    "conf-top-a": nest(top=[OA]),
    # N13: Monitor
    "nest-monitor": nest(top=[OH, fx("monitor", "h")], ga=[OA, fx("monitor"), HA]),
}



def analysed(suffix, sig=(True, True, True)):
    return nest(
        top=[fx("output", f"h-{suffix}")],
        ga=[fx("output", f"a-{suffix}"), fx("input", f"h1-{suffix}")],
        gb=[fx("output", f"b-{suffix}"), fx("input", f"h2-{suffix}")],
        va1=[fx("input", f"a1-{suffix}")],
        va2=[fx("input", f"a2-{suffix}")],
        vb1=[fx("input", f"b1-{suffix}")],
        sig=sig,
    )


LAYOUTS.update(
    {
        # N8: more places for a plugin with latency
        "nest-pdc-v-post": nest(va1=[VA1, fx("delay")]),
        "ref-pdc-v-post": ref(va1=[VA1, fx("delay")]),
        "nest-pdc-ga-pre": nest(ga=[fx("delay"), OA, HA]),
        "nest-pdc-gb": nest(gb=[OB, fx("delay"), HB]),
        "nest-pdc-both": nest(ga=[OA, fx("delay"), HA], gb=[OB, fx("delay"), HB]),
        "nest-pdc-ga-post": nest(ga=[OA, HA, fx("delay")]),
        "nest-pdc-top-pre": nest(top=[fx("delay"), OH]),
        # Follow: projects after the two-level analysis
        "dry-va2": {"tracks": [{"name": "VA2", "source": True, "signal": {"seed": 12}}]},
        "dry-vb1": {"tracks": [{"name": "VB1", "source": True, "signal": {"seed": 13}}]},
        "an-on": analysed("on"),
        "an-on-va1": analysed("on", (True, False, False)),
        "an-on-va2": analysed("on", (False, True, False)),
        "an-on-vb1": analysed("on", (False, False, True)),
        "an-off-first": analysed("off-first"),
        "an-off-first-va1": analysed("off-first", (True, False, False)),
        "an-off-after": analysed("off-after"),
        "an-off-after-va1": analysed("off-after", (True, False, False)),
        "an-auto": analysed("auto"),
    }
)

nest_top_vol = copy.deepcopy(LAYOUTS["nest"])
nest_top_vol["tracks"][0]["volume"] = 0.5
LAYOUTS["nest-top-vol"] = nest_top_vol
# No SCVB at all: shows what a render setting does by itself.
LAYOUTS["plain-3"] = {"tracks": [{"name": n, "source": True, "signal": {"seed": sd}} for n, sd in (("VA1", 11), ("VA2", 12), ("VB1", 13))]}

LOOSE = 24  # records the residual without judging it


def r(id_, layout, **kw):
    d = {"id": id_, "kind": "render", "layout": layout}
    d.update(kw)
    return d


def cmp_(layout, **kw):
    c = {"layout": layout}
    c.update(kw)
    return c


# Two-level expected levels (N4), from PanMath: a stereo source through dual-pan matrices.
def dual(p, w):
    def g(x):
        t = (max(-100.0, min(100.0, x)) + 100.0) * math.pi / 400.0
        return math.cos(t), math.sin(t)

    lL, rL = g(p - w)
    lR, rR = g(p + w)
    return [[lL, lR], [rL, rR]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(2)) for j in range(2)] for i in range(2)]


def change_db(m):
    # L and R of the source are uncorrelated noise of the same level, so the power of each output adds up.
    return [10 * math.log10(m[i][0] ** 2 + m[i][1] ** 2) for i in range(2)]


def rng(db, tol=0.1):
    return [round(db - tol, 3), round(db + tol, 3)]


h_only = change_db(dual(-50, 30))
a_h = dual(-50, 30)
a_l = dual(40, 50)
a_gain = 10 ** (-6 / 20)
two = change_db(mul(a_h, [[x * a_gain for x in row] for row in a_l]))
print("N4 expected dB: h-only L,R", h_only, " a+h L,R", two)

# Follow: the pans and levels states/analysis.json recorded (GA -60/+60, GB 0, TOP -60 at -1.065 dB and +60 at +1.564 dB).
def scaled(m, db):
    g = 10 ** (db / 20)
    return [[x * g for x in row] for row in m]


TOP1 = scaled(dual(-60, 100), -1.065)
TOP2 = scaled(dual(60, 100), 1.564)
f_va1 = change_db(mul(TOP1, dual(-60, 100)))
f_va2 = change_db(mul(TOP1, dual(60, 100)))
f_vb1 = change_db(mul(TOP2, dual(0, 100)))
f_first = change_db(dual(-60, 100))
print("follow expected dB: va1", f_va1, "va2", f_va2, "vb1", f_vb1, "off-first va1", f_first)

STRICT = -90 if not EXPLORE else LOOSE


def w(id_, layout, mode="offline", settle=None, play=None, ops=None, compare=None, maxdiff=None, maxtail=None, mindiff=None, minpeak=None, maxpeak=None, regions=None, rms=None):
    a = []
    for key, value in (
        ("settle", settle),
        ("play", play),
        ("ops", ops),
        ("compare", compare),
        ("max-diff-db", maxdiff),
        ("max-tail-db", maxtail),
        ("min-diff-db", mindiff),
        ("min-peak-db", minpeak),
        ("max-peak-db", maxpeak),
        ("regions", regions),
        ("rms-change", None if EXPLORE or rms is None else f"{rms[0]},{rms[1]}"),
    ):
        if value is not None:
            a += [f"--{key}", str(value)]
    return {"id": id_, "kind": "scvb-session", "layout": layout, "script": "render-from-window.mjs", "mode": mode, "timeoutSec": 240, "args": a}


ALL = lambda x: x  # noqa: E731
SCENARIOS = [
    # N0: single-level baseline in REAPER
    r("n0-base", "base", matrix={"mode": ["offline", "1x"]}, compare="dry", maxDiffDb=-90 if not EXPLORE else LOOSE, notSilent=True),
    r("n0-base-online", "base", mode="online", compare="dry", maxDiffDb=LOOSE, notSilent=True),
    r("n0-ref", "ref", matrix={"mode": ["offline", "1x"]}, compare="dry", maxDiffDb=-90 if not EXPLORE else LOOSE, notSilent=True),
    r("n0-ref-online", "ref", mode="online", compare="dry", maxDiffDb=LOOSE, notSilent=True),
    # N1, N2: sound and no doubling in real time and offline: nested against the same groups without the upper level
    r("n1-nest", "nest", matrix={"mode": ["offline", "1x"]}, compare=cmp_("ref", mode="offline"), maxDiffDb=-90 if not EXPLORE else LOOSE, notSilent=True),
    r("n1-nest-online", "nest", mode="online", compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE, notSilent=True),
    # N3: every group's own positions survive the upper level
    r("n3-pos", "nest-pos", compare=cmp_("ref-pos"), maxDiffDb=-90 if not EXPLORE else LOOSE, notSilent=True),
    r("n3-pos-b24", "nest-pos-b24", compare=cmp_("ref-pos-b24"), maxDiffDb=-90 if not EXPLORE else LOOSE, notSilent=True),
    r("n3-pos-online", "nest-pos", mode="online", compare=cmp_("ref-pos", mode="offline"), maxDiffDb=LOOSE, notSilent=True),
    # N4: the upper level moves whole groups by the two-level formula
    r("n4-h-va1", "nest-h-va1", compare=cmp_("dry-va1"), rmsChangeDb={"L": rng(h_only[0]), "R": rng(h_only[1])}, maxDiffDb=LOOSE),
    r("n4-a-h-va1", "nest-a-h-va1", compare=cmp_("dry-va1"), rmsChangeDb={"L": rng(two[0]), "R": rng(two[1])}, maxDiffDb=LOOSE),
    r("n4-h-vol", "nest-h-vol", compare=cmp_("ref"), maxDiffDb=LOOSE),
    # N5: export
    r("n5-online-vs-offline", "nest", mode="online", compare=cmp_("nest", mode="offline"), maxDiffDb=LOOSE),
    r("n5-top-stem", "nest", stems=[{"track": "TOP", "compare": "none", "notSilent": True}, {"track": "GA", "compare": "none"}, {"track": "GB", "compare": "none"}, {"track": "VA1", "compare": "none"}], compare="none"),
    # N6: the group bus controls while the group goes up through Input H
    r("n6-ga-vol0", "nest-ga-vol0", compare=cmp_("nest"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    r("n6-ga-pan", "nest-ga-pan", compare=cmp_("nest"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    r("n6-ga-mute", "nest-ga-mute", compare=cmp_("ref-b-only"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    r("n6-ga-sends", "nest-ga-sends", compare=cmp_("nest"), maxDiffDb=LOOSE, stems=[{"track": "AUX", "compare": "none"}]),
    r("n6-top-vol", "nest-top-vol", compare=cmp_("nest"), rmsChangeDb={"L": rng(-6.02), "R": rng(-6.02)}, maxDiffDb=LOOSE),
    # N7: a group bus exported on its own; Input H bypassed on it
    r("n7-hbyp-master", "nest-hbyp", compare=cmp_("ref-b-only"), maxDiffDb=-90 if not EXPLORE else LOOSE, stems=[{"track": "GA", "compare": "none", "notSilent": True}]),
    # N8: latency
    r("n8-ref-ga", "ref-pdc-ga", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-ga", "nest-pdc-ga", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-ga-online", "nest-pdc-ga", mode="online", compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE),
    r("n8-ref-v", "ref-pdc-v", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-v", "nest-pdc-v", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-v-online", "nest-pdc-v", mode="online", compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE),
    r("n8-nest-top", "nest-pdc-top", compare=cmp_("ref"), maxDiffDb=LOOSE),
    # N9: wrong wiring must not blow up
    r("n9-top-h3", "bad-top-h3", matrix={"mode": ["offline", "online"]}, compare="none", peakMaxDb=12),
    r("n9-ga-a3", "bad-ga-a3", matrix={"mode": ["offline", "online"]}, compare="none", peakMaxDb=12),
    r("n9-ga-reversed", "bad-ga-reversed", matrix={"mode": ["offline", "online"]}, compare=cmp_("ref-b-only", mode="offline"), maxDiffDb=LOOSE),
    r("n9-gb-master", "bad-gb-master", matrix={"mode": ["offline", "online"]}, compare="none", peakMaxDb=12),
    # N10: conflicts
    r("n10-h-ch1", "conf-h-ch1", matrix={"mode": ["offline", "online"]}, compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE),
    r("n10-two-oh-offline", "conf-two-oh", compare=cmp_("ref", mode="offline"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    r("n10-two-oh-online", "conf-two-oh", mode="online", compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE),
    r("n10-top-a", "conf-top-a", matrix={"mode": ["offline", "online"]}, compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE),
    # N11: save and reopen
    {"id": "n11-device", "kind": "device", "layout": "nest"},
    {"id": "n11-savereopen", "kind": "savereopen", "layout": "nest", "cycles": 3},
    # N12: host scheduling
    r("n12-block", "nest", mode="online", matrix={"block": [32, 128, 2048]}, compare=cmp_("ref", mode="offline", block=512), maxDiffDb=LOOSE, notSilent=True),
    r("n12-block-offline", "nest", matrix={"block": [32, 2048]}, compare=cmp_("ref", block=512), maxDiffDb=-90 if not EXPLORE else LOOSE, notSilent=True),
    r("n12-af-off", "nest", mode="online", host={"anticipativeFx": False}, compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE, notSilent=True),
    r("n12-sr44-offline", "nest", sr=44100, compare=cmp_("ref", mode="offline"), maxDiffDb=-90 if not EXPLORE else LOOSE, notSilent=True),
    r("n12-sr44-online", "nest", sr=44100, mode="online", compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE, notSilent=True),
    # N13: Monitor changes nothing
    r("n13-monitor", "nest-monitor", compare=cmp_("nest"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    # N8: more places for a plugin with latency
    r("n8-nest-v-post", "nest-pdc-v-post", compare=cmp_("ref"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    r("n8-ref-v-post", "ref-pdc-v-post", compare=cmp_("ref"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    r("n8-nest-ga-pre", "nest-pdc-ga-pre", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-gb", "nest-pdc-gb", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-both", "nest-pdc-both", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-ga-post", "nest-pdc-ga-post", compare=cmp_("ref"), maxDiffDb=-90 if not EXPLORE else LOOSE),
    r("n8-nest-top-pre", "nest-pdc-top-pre", compare=cmp_("ref"), maxDiffDb=LOOSE),
    r("n8-nest-both-online", "nest-pdc-both", mode="online", compare=cmp_("ref", mode="offline"), maxDiffDb=LOOSE),
    # Follow: what the two-level analysis left in the project, one source at a time against the two-level formula
    r("f-on-va1", "an-on-va1", compare=cmp_("dry-va1"), rmsChangeDb={"L": rng(f_va1[0], 0.3), "R": rng(f_va1[1], 0.3)}, maxDiffDb=LOOSE),
    r("f-on-va2", "an-on-va2", compare=cmp_("dry-va2"), rmsChangeDb={"L": rng(f_va2[0], 0.3), "R": rng(f_va2[1], 0.3)}, maxDiffDb=LOOSE),
    r("f-on-vb1", "an-on-vb1", compare=cmp_("dry-vb1"), rmsChangeDb={"L": rng(f_vb1[0], 0.3), "R": rng(f_vb1[1], 0.3)}, maxDiffDb=LOOSE),
    r("f-off-first-va1", "an-off-first-va1", compare=cmp_("dry-va1"), rmsChangeDb={"L": rng(f_first[0], 0.3), "R": rng(f_first[1], 0.3)}, maxDiffDb=LOOSE),
    r("f-off-after-va1", "an-off-after-va1", compare=cmp_("dry-va1"), rmsChangeDb={"L": rng(f_va1[0], 0.3), "R": rng(f_va1[1], 0.3)}, maxDiffDb=LOOSE),
    # What a user gets: a render started from the open project. References first: rendered by the command line.
    {"id": "r-ref", "kind": "render", "layout": "ref", "compare": "none"},
    {"id": "r-ref-pos", "kind": "render", "layout": "ref-pos", "compare": "none"},
    {"id": "r-ref-pos-b24", "kind": "render", "layout": "ref-pos-b24", "compare": "none"},
    {"id": "r-an-on", "kind": "render", "layout": "an-on", "compare": "none"},
    w("w1-ref-pos", "ref-pos", compare="r-ref-pos", maxdiff=STRICT),
    w("w1-nest-pos", "nest-pos", compare="r-ref-pos", maxdiff=STRICT, minpeak=-30),
    w("w1-nest-pos-short", "nest-pos", settle=300, compare="r-ref-pos", maxdiff=STRICT),
    w("w1-nest-pos-long", "nest-pos", settle=12000, compare="r-ref-pos", maxdiff=STRICT),
    w("w1-nest-pos-played", "nest-pos", play=3000, compare="r-ref-pos", maxdiff=STRICT),
    w("w1-nest", "nest", compare="r-ref", maxdiff=STRICT, minpeak=-30),
    w("w1-nest-pos-b24", "nest-pos-b24", compare="r-ref-pos-b24", maxdiff=STRICT),
    w("w1-neg", "nest-pos", compare="r-ref", mindiff=-20),
    w("w1-on", "an-on", compare="r-an-on", maxdiff=STRICT),
    w("w1-auto", "an-auto", compare="r-an-on", maxdiff=STRICT),
    w("w1-off-first", "an-off-first", maxpeak=30),
    w("w1-off-after", "an-off-after", compare="r-an-on", maxdiff=LOOSE),
    # The DAW's own buttons on the tracks of a nested project
    # Rendered: solo takes effect. Through REAPER's real-time engine it does not (KI-8), mute does.
    w("w2-solo-ga", "nest-pos", ops="setsolo:GA:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-0.67, -3.99)),
    w("w2-solo-gb", "nest-pos", ops="setsolo:GB:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-8.45, -2.22)),
    w("w2-solo-va1", "nest-pos", ops="setsolo:VA1:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-3.68, -7.00)),
    w("w2-solo-top", "nest-pos", ops="setsolo:TOP:1", compare="r-ref-pos", maxdiff=STRICT),
    w("w2-mute-va1", "nest-pos", ops="setmute:VA1:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-2.42, -0.97)),
    w("w2-mute-ga", "nest-pos", ops="setmute:GA:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-8.45, -2.22)),
    w("w2-mute-top", "nest-pos", ops="setmute:TOP:1", compare="r-ref-pos", maxdiff=LOOSE, maxpeak=-120),
    w("w2-solo-ga-online", "nest-pos", mode="online", ops="setsolo:GA:1", compare="r-ref-pos", maxdiff=LOOSE, maxtail=STRICT),
    w("w2-solo-va1-online", "nest-pos", mode="online", ops="setsolo:VA1:1", compare="r-ref-pos", maxdiff=LOOSE, maxtail=STRICT),
    w("w2-mute-ga-online", "nest-pos", mode="online", ops="setmute:GA:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-8.45, -2.22)),
    w("w2-mute-va1-online", "nest-pos", mode="online", ops="setmute:VA1:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-2.42, -0.97)),
    # Bypass
    w("w3-byp-oh", "nest-pos", ops="setbypass:TOP:0:1", compare="r-ref-pos", maxdiff=STRICT),
    w("w3-byp-oa", "nest-pos", ops="setbypass:GA:0:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-1.46, 1.46)),
    w("w3-byp-ha", "nest-pos", ops="setbypass:GA:1:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-8.45, -2.22)),
    w("w3-byp-va1", "nest-pos", ops="setbypass:VA1:0:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-2.42, -0.97)),
    w("w3-byp-oh-online", "nest-pos", mode="online", ops="setbypass:TOP:0:1", compare="r-ref-pos", maxdiff=LOOSE, maxtail=STRICT),
    w("w3-byp-oa-online", "nest-pos", mode="online", ops="setbypass:GA:0:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-1.46, 1.46)),
    w("w3-byp-ha-online", "nest-pos", mode="online", ops="setbypass:GA:1:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-8.45, -2.22)),
    w("w3-byp-va1-online", "nest-pos", mode="online", ops="setbypass:VA1:0:1", compare="r-ref-pos", maxdiff=LOOSE, rms=(-2.42, -0.97)),
    # An upper Output that has positions: bypassed, it stops applying them, as if it had never placed the groups.
    w("w3-byp-top-an", "an-on", ops="setbypass:TOP:0:1", compare="w1-off-first", maxdiff=STRICT),
    # The upper Output on the master track
    w("w4-master", "nest-pos", ops="tomaster:TOP:0", compare="r-ref-pos", maxdiff=STRICT),
    w("w4-master-online", "nest-pos", mode="online", ops="tomaster:TOP:0", compare="r-ref-pos", maxdiff=LOOSE, maxtail=STRICT),
    # In real time every render starts with a handover; what follows has to be the same as the export, to the bit.
    # One level, for comparison with the same start in the two-level project.
    {"id": "p1-plain", "kind": "scvb-session", "layout": "plain-3", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": []},
    {"id": "p1-base", "kind": "scvb-session", "layout": "base", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": []},
    {"id": "p1-ref-pos", "kind": "scvb-session", "layout": "ref-pos", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": []},
    {"id": "p1-nest-pos", "kind": "scvb-session", "layout": "nest-pos", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": []},
    {"id": "p1-master", "kind": "scvb-session", "layout": "nest-pos", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": ["--tomaster", "TOP:0"]},
    # A click on the timeline just before pressing play.
    {"id": "p1l-plain", "kind": "scvb-session", "layout": "plain-3", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": ["--late-seek", "1"]},
    {"id": "p1l-base", "kind": "scvb-session", "layout": "base", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": ["--late-seek", "1"]},
    {"id": "p1l-ref-pos", "kind": "scvb-session", "layout": "ref-pos", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": ["--late-seek", "1"]},
    {"id": "p1l-nest-pos", "kind": "scvb-session", "layout": "nest-pos", "script": "play-head.mjs", "mode": "offline", "timeoutSec": 240, "args": ["--late-seek", "1"]},
    w("w6-base-online", "base", mode="online", maxpeak=30),
    w("w6-ref-online", "ref", mode="online", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT, minpeak=-30),
    w("w6-ref-pos-online", "ref-pos", mode="online", compare="r-ref-pos", maxdiff=LOOSE, maxtail=STRICT, minpeak=-30),
    w("w6-nest-online", "nest", mode="online", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT, minpeak=-30),
    w("w6-nest-pos-online", "nest-pos", mode="online", compare="r-ref-pos", maxdiff=LOOSE, maxtail=STRICT, minpeak=-30),
    # A plugin with latency: only the start of the file may differ.
    w("w6-pdc-ga", "nest-pdc-ga", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    w("w6-pdc-both", "nest-pdc-both", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    w("w6-pdc-top-pre", "nest-pdc-top-pre", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    w("w6-pdc-v", "nest-pdc-v", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    w("w6-pdc-both-online", "nest-pdc-both", mode="online", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    w("w6-pdc-v-online", "nest-pdc-v", mode="online", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    # Wrong wiring: an Input after the Output of its own group. The export is right; in real time the bus goes quiet.
    w("w7-top-h3", "bad-top-h3", compare="r-ref", maxdiff=STRICT),
    w("w7-top-h3-online", "bad-top-h3", mode="online", compare="r-ref", mindiff=-20),
    # What the render dialog offers beyond the project's plain settings. The reference goes the same way, without the upper level.
    w("w5-ref-sr", "ref-pos", ops="renderset:RENDER_SRATE:44100", minpeak=-30),
    w("w5-nest-sr", "nest-pos", ops="renderset:RENDER_SRATE:44100", compare="w5-ref-sr", maxdiff=STRICT, minpeak=-30),
    w("w5-ref-timesel", "ref-pos", ops="timesel:1.5:5.5,renderset:RENDER_BOUNDSFLAG:2", minpeak=-30),
    w("w5-nest-timesel", "nest-pos", ops="timesel:1.5:5.5,renderset:RENDER_BOUNDSFLAG:2", compare="w5-ref-timesel", maxdiff=STRICT, minpeak=-30),
    w("w5-ref-regions", "ref-pos", regions="a:0.5:2.5,b:4:6.5", minpeak=-30),
    w("w5-nest-regions", "nest-pos", regions="a:0.5:2.5,b:4:6.5", compare="w5-ref-regions", maxdiff=STRICT, minpeak=-30),
    w("w5-ref-regions4", "ref-pos", regions="a:0.5:1.7,b:2.1:3.33,c:4.1:5.07,d:5.5:6.9", minpeak=-30),
    w("w5-nest-regions4", "nest-pos", regions="a:0.5:1.7,b:2.1:3.33,c:4.1:5.07,d:5.5:6.9", compare="w5-ref-regions4", maxdiff=STRICT, minpeak=-30),
    w("w5-byp-regions4", "nest-pos", ops="setbypass:TOP:0:1", regions="a:0.5:1.7,b:2.1:3.33,c:4.1:5.07,d:5.5:6.9", compare="w5-ref-regions4", maxdiff=STRICT, minpeak=-30),
    w("w5-ref-regions-rev", "ref-pos", regions="a:5.5:6.9,b:0.5:1.7,c:3:4.2", minpeak=-30),
    w("w5-nest-regions-rev", "nest-pos", regions="a:5.5:6.9,b:0.5:1.7,c:3:4.2", compare="w5-ref-regions-rev", maxdiff=STRICT, minpeak=-30),
    # Selected tracks via master: REAPER leaves the other tracks running, so their Inputs still hand over. plain-3 is the control.
    w("w5-stems-va1", "nest-pos", ops="seltrack:VA1,renderset:RENDER_SETTINGS:128", compare="r-ref-pos", maxdiff=STRICT),
    w("w5-stems-plain-va1", "plain-3", ops="seltrack:VA1,renderset:RENDER_SETTINGS:128", minpeak=-30, maxpeak=1),
    # The same buttons in real time.
    w("w2-ga-vol0-online", "nest", mode="online", ops="setvol:GA:0", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    w("w2-ga-pan-online", "nest", mode="online", ops="setpan:GA:-1", compare="r-ref", maxdiff=LOOSE, maxtail=STRICT),
    w("w2-top-vol-online", "nest", mode="online", ops="setvol:TOP:0.5", compare="r-ref", maxdiff=LOOSE, rms=(-6.02, -6.02)),
    w("w2-top-solo-online", "nest-pos", mode="online", ops="setsolo:TOP:1", compare="r-ref-pos", maxdiff=LOOSE, maxtail=STRICT),
    w("w2-top-mute-online", "nest-pos", mode="online", ops="setmute:TOP:1", compare="r-ref-pos", maxdiff=LOOSE, maxpeak=-120),
    r("n6-ga-sends-online", "nest-ga-sends", mode="online", compare=cmp_("nest", mode="offline"), maxDiffDb=LOOSE, stems=[{"track": "AUX", "compare": "none"}]),
]

SETUP_EXT = {"modules": ["ext/scvb-nested.psm1"], "lua": ["ext/scvb-nested.lua"]}

raw_input = {"plugin": "input"}
raw_output = {"plugin": "output"}
setup = {
    "schema": 1,
    "name": "scvb-nested-setup",
    "notes": "Sets group and channel on every instance through the real bridge and saves the template nested.json reads.",
    "plugins": {
        "input": {"bundle": "SCVB Input.vst3"},
        "output": {"bundle": "SCVB Output.vst3"},
        "monitor": {"bundle": "SCVB Monitor.vst3"},
    },
    "signal": {"seconds": 8},
    "limits": {"learnTimeoutSec": 240},
    "layouts": {
        "nest": {
            "tracks": [
                {"name": "TOP", "source": False, "folder": True, "fx": [raw_output]},
                {"name": "GA", "source": False, "folder": True, "parent": "TOP", "fx": [raw_output, raw_input]},
                src("VA1", "GA", 11, [raw_input]),
                src("VA2", "GA", 12, [raw_input]),
                {"name": "GB", "source": False, "folder": True, "parent": "TOP", "fx": [raw_output, raw_input]},
                src("VB1", "GB", 13, [raw_input]),
                {"name": "SPARE", "source": False, "fx": [raw_input, raw_input, {"plugin": "monitor"}]},
            ]
        }
    },
    "scenarios": [
        {"id": "make-states", "kind": "scvb-session", "layout": "nest", "script": "make-states.mjs", "timeoutSec": 240},
        {"id": "analyze-two-level", "kind": "scvb-session", "layout": "nest", "script": "analyze-two-level.mjs", "timeoutSec": 300},
    ],
    "extensions": SETUP_EXT,
}

cfg = {
    "schema": 1,
    "name": "scvb-nested",
    "notes": "Nested SCVB in REAPER: two groups going up through a third. Run setup.json first; it writes states/template.rpp.",
    "plugins": PLUGINS,
    "signal": {"seconds": 8},
    "limits": {"learnTimeoutSec": 240, "lagSearch": 256},
    "layouts": LAYOUTS,
    "scenarios": SCENARIOS,
    "extensions": {"modules": ["ext/scvb-nested.psm1"], "lua": ["ext/scvb-nested.lua"]},
}

json.dump(setup, open(os.path.join(OUT, "setup.json"), "w", newline="\n"), indent=2)
json.dump(cfg, open(os.path.join(OUT, "nested.json"), "w", newline="\n"), indent=2)
print("layouts", len(LAYOUTS), "scenarios", len(SCENARIOS))
