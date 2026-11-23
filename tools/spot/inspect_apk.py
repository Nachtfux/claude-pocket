#!/usr/bin/env python3
"""inspect_apk.py - look for an official way into the Claude app's voice mode.

Usage: inspect_apk.py base.apk [split1.apk ...]

Prints package/version/minSdk, every activity (and activity-alias) with its
intent filters, app shortcuts (android.app.shortcuts meta-data), everything
that mentions VOICE_COMMAND / ASSIST / voice, a few deep-link-looking strings
from the dex files, and a one-line CONCLUSION.

Runs from tools/spot/inspect-claude-apk.sh inside a venv with androguard.
The APK is treated as untrusted data: it is only parsed, never executed.
"""
import re
import sys
import zipfile

try:  # androguard prints a lot via loguru; silence it
    from loguru import logger as _lg

    _lg.remove()
except Exception:  # pragma: no cover
    pass

try:  # androguard >= 4
    from androguard.core.apk import APK
    from androguard.core.axml import AXMLPrinter
except ImportError:  # androguard 3.x
    from androguard.core.bytecodes.apk import APK  # type: ignore
    from androguard.core.bytecodes.axml import AXMLPrinter  # type: ignore

NS = "{http://schemas.android.com/apk/res/android}"
VOICE_RE = re.compile(r"voice|VOICE_COMMAND|ASSIST|speech|dictat|sprach|audio_mode|live_mode", re.I)
VOICE_ACTIONS = {
    "android.intent.action.VOICE_COMMAND",
    "android.intent.action.ASSIST",
    "android.intent.action.VOICE_ASSIST",
    "android.speech.action.RECOGNIZE_SPEECH",
    "android.speech.action.WEB_SEARCH",
}

findings = []  # (strength, text): strength "direct" or "hint"


def a(el, name):
    v = el.get(NS + name)
    if v is None:
        v = el.get(name)
    return v


def hdr(t):
    print("\n== " + t + " " + "=" * max(0, 60 - len(t)))


def describe_filter(f):
    acts = [a(x, "name") for x in f.findall("action")]
    cats = [a(x, "name") for x in f.findall("category")]
    datas = []
    for d in f.findall("data"):
        parts = []
        for k in ("scheme", "host", "port", "path", "pathPrefix", "pathPattern", "mimeType"):
            v = a(d, k)
            if v:
                parts.append("%s=%s" % (k, v))
        datas.append(" ".join(parts))
    return acts, cats, [d for d in datas if d]


def resolve_xml_resource(apk, value):
    """meta-data android:resource -> list of file paths inside the APK."""
    if value is None:
        return []
    paths = []
    try:
        s = str(value).lstrip("@")
        rid = int(s, 16) if not s.isdigit() else int(s)
        res = apk.get_android_resources()
        for _cfg, val in res.get_resolved_res_configs(rid):
            if isinstance(val, str) and val.endswith(".xml"):
                paths.append(val)
    except Exception:
        pass
    return sorted(set(paths))


def parse_axml(apk, path):
    try:
        return AXMLPrinter(apk.get_file(path)).get_xml_obj()
    except Exception:
        return None


def main(paths):
    apks = []
    for p in paths:
        try:
            apks.append((p, APK(p)))
        except Exception as e:  # not an APK / broken
            print("!! cannot parse %s: %s" % (p, e))
    if not apks:
        sys.exit(2)
    # base = the one with activities
    base_path, apk = max(apks, key=lambda t: len(t[1].get_activities() or []))

    hdr("Package")
    print("file       :", base_path, "(+%d split(s))" % (len(apks) - 1))
    print("package    :", apk.get_package())
    print("version    :", apk.get_androidversion_name(), "(code %s)" % apk.get_androidversion_code())
    print("minSdk     :", apk.get_min_sdk_version(), " targetSdk:", apk.get_target_sdk_version())
    try:
        msdk = int(apk.get_min_sdk_version() or 0)
        print("Android 11 :", "OK (minSdk <= 30)" if msdk <= 30 else "NOT SUPPORTED (minSdk %d > 30)" % msdk)
    except ValueError:
        pass
    print("main act.  :", apk.get_main_activity())

    man = apk.get_android_manifest_xml()
    app = man.find("application")
    shortcut_res = []

    hdr("Activities + intent filters")
    comps = []
    if app is not None:
        comps = [e for e in app if e.tag in ("activity", "activity-alias")]
    for e in comps:
        name = a(e, "name")
        exported = a(e, "exported")
        filters = e.findall("intent-filter")
        target = a(e, "targetActivity")
        line = "- %s%s%s" % (
            name,
            " [alias->%s]" % target if target else "",
            " exported=%s" % exported if exported is not None else "",
        )
        if not filters and not VOICE_RE.search(name or ""):
            continue  # keep the list readable: only components with filters (or voice-ish names)
        print(line)
        if VOICE_RE.search(name or ""):
            findings.append(("hint", "component name mentions voice: %s" % name))
        for f in filters:
            acts, cats, datas = describe_filter(f)
            print("    actions   :", ", ".join(acts) or "-")
            if cats:
                print("    categories:", ", ".join(cats))
            for d in datas:
                print("    data      :", d)
            for act in acts:
                if act in VOICE_ACTIONS:
                    findings.append(("direct", "%s handles %s" % (name, act)))
            for d in datas:
                if VOICE_RE.search(d):
                    findings.append(("direct", "deep link %s -> %s" % (d, name)))
        for md in e.findall("meta-data"):
            if a(md, "name") == "android.app.shortcuts":
                shortcut_res.append(a(md, "resource"))
    print("(%d activities/aliases in total; those without intent filters are hidden)" % len(comps))

    hdr("App shortcuts")
    sc_files = []
    for r in shortcut_res:
        sc_files += resolve_xml_resource(apk, r)
    if not sc_files:  # fallback by file name
        sc_files = [f for f in apk.get_files() if f.startswith("res/") and "shortcut" in f.lower() and f.endswith(".xml")]
    if not sc_files:
        print("none declared")
    for path in sorted(set(sc_files)):
        x = parse_axml(apk, path)
        if x is None:
            continue
        print("file:", path)
        for sc in x.iter("shortcut"):
            sid = a(sc, "shortcutId")
            label = a(sc, "shortcutShortLabel")
            print("  - id=%s label=%s enabled=%s" % (sid, label, a(sc, "enabled")))
            for it in sc.iter("intent"):
                desc = " ".join("%s=%s" % (k, a(it, k)) for k in ("action", "targetPackage", "targetClass", "data") if a(it, k))
                print("      intent:", desc)
                for ex in it.iter("extra"):
                    print("      extra : %s=%s" % (a(ex, "name"), a(ex, "value")))
                if VOICE_RE.search(desc) or VOICE_RE.search(sid or ""):
                    findings.append(("direct", "shortcut %s: %s" % (sid, desc)))
            if VOICE_RE.search(sid or "") and not any("shortcut %s" % sid in t for _, t in findings):
                findings.append(("direct", "shortcut %s" % sid))

    hdr("Manifest strings mentioning voice / assist")
    hits = set()
    for el in man.iter():
        for k, v in el.attrib.items():
            if isinstance(v, str) and VOICE_RE.search(v):
                hits.add("%s %s=%s" % (el.tag, k.replace(NS, "android:"), v))
    for h in sorted(hits):
        print(" ", h)
    if not hits:
        print("  none")

    hdr("Deep-link / voice strings in dex (hints only)")
    link_re = re.compile(rb"(?:claude|anthropic)://[\x21-\x7e]{0,80}")
    voice_re = re.compile(rb"[A-Za-z0-9_./:-]{0,40}(?:voice_mode|voiceMode|VoiceMode|start_voice|open_voice|/voice)[A-Za-z0-9_./:-]{0,40}")
    dex_hits = {}
    for p, _ in apks:
        try:
            z = zipfile.ZipFile(p)
        except zipfile.BadZipFile:
            continue
        for n in z.namelist():
            if re.match(r"classes\d*\.dex$", n):
                data = z.read(n)
                for m in link_re.finditer(data):
                    dex_hits.setdefault(m.group(0).decode("ascii", "replace"), "link")
                for m in voice_re.finditer(data):
                    dex_hits.setdefault(m.group(0).decode("ascii", "replace"), "voice")
    shown = 0
    for s, kind in sorted(dex_hits.items(), key=lambda t: (t[1] != "link", t[0])):
        if shown >= 60:
            print("  ... (%d more)" % (len(dex_hits) - shown))
            break
        print("  [%s] %s" % (kind, s))
        shown += 1
        if kind == "link" and VOICE_RE.search(s):
            findings.append(("hint", "dex string %s (try: adb shell am start -a android.intent.action.VIEW -d '%s')" % (s, s)))
    if not dex_hits:
        print("  none")

    hdr("CONCLUSION")
    direct = [t for k, t in findings if k == "direct"]
    hints = [t for k, t in findings if k == "hint"]
    for t in direct:
        print("  direct:", t)
    for t in hints:
        print("  hint  :", t)
    if direct:
        print("CONCLUSION: a direct voice-mode entry EXISTS - test it with adb before relying on it.")
    elif hints:
        print("CONCLUSION: no declared voice-mode entry; only unconfirmed hints - keep the AccessibilityService auto-tap.")
    else:
        print("CONCLUSION: no direct voice-mode entry found - Hey Claude must use the AccessibilityService auto-tap.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
