#!/usr/bin/env python3
import json
import shlex
import os

# ➕ Include-Pfade, die ergänzt werden sollen
extra_includes = [
    "/usr/lib/gcc/x86_64-w64-mingw32/12-win32/include",
    "/usr/lib/gcc/x86_64-w64-mingw32/12-win32/include-fixed",
    "/usr/lib/gcc/x86_64-w64-mingw32/12-win32/../../../../x86_64-w64-mingw32/include",
    "/mingw64/include/wx-3.2",
    "/mingw64/lib/wx/include/msw-unicode-static-3.2",
]

# 🚫 nur Headernamen filtern – niemals ganze Einträge löschen
blacklisted_headers = {"go_defs.h"}

filename = "build/win64/compile_commands.json"
backupname = filename + ".bak"

if not os.path.exists(filename):
    print(f"❌ Datei {filename} nicht gefunden.")
    exit(1)

with open(filename, "r", encoding="utf-8") as f:
    data = json.load(f)

# 1) command → arguments
for entry in data:
    if "arguments" not in entry and "command" in entry:
        entry["arguments"] = shlex.split(entry["command"])
        del entry["command"]

# 2) .rsp entpacken
for entry in data:
    args = entry["arguments"]
    new_args, seen = [], set()
    for arg in args:
        if arg.startswith("@") and arg.endswith(".rsp"):
            rsp_path = os.path.join(entry["directory"], arg[1:])
            if os.path.isfile(rsp_path):
                print(f"📎 Entpacke {rsp_path}")
                try:
                    with open(rsp_path, "r", encoding="utf-8") as rsp_file:
                        for line in rsp_file:
                            tokens = shlex.split(line.strip())
                            for token in tokens:
                                if token not in seen:
                                    new_args.append(token)
                                    seen.add(token)
                except Exception as e:
                    print(f"⚠️ Fehler beim Lesen von {rsp_path}: {e}")
            else:
                print(f"⚠️ .rsp-Datei nicht gefunden: {rsp_path}")
        else:
            if arg not in seen:
                new_args.append(arg)
                seen.add(arg)
    entry["arguments"] = new_args

def is_blacklisted_header(path_or_token: str) -> bool:
    t = path_or_token.strip("\"'")
    base = os.path.basename(t)
    return base in blacklisted_headers

# 3) Argumente säubern (keine Einträge löschen!)
total_removed = 0
for entry in data:
    args = entry.get("arguments", [])
    cleaned = []
    i = 0
    while i < len(args):
        a = args[i]

        # -include <file>
        if a == "-include" and i + 1 < len(args):
            target = args[i + 1]
            if is_blacklisted_header(target):
                print(f"🚫 Entferne -include {target}")
                total_removed += 2
                i += 2
                continue
            cleaned += [a, target]
            i += 2
            continue

        # zusammengezogen: -include<file>
        if a.startswith("-include"):
            target = a[len("-include"):]
            if target and is_blacklisted_header(target):
                print(f"🚫 Entferne {a}")
                total_removed += 1
                i += 1
                continue
            cleaned.append(a)
            i += 1
            continue

        # -I <path> / -isystem <path>
        if a in ("-I", "-isystem") and i + 1 < len(args):
            flag, p = a, args[i + 1]
            # nackter Header als "Verzeichnis"? -> weg
            if is_blacklisted_header(p):
                print(f"🚫 Entferne {flag} {p} (zeigt auf Header-Datei)")
                total_removed += 2
                i += 2
                continue
            full = os.path.join(entry["directory"], p) if not os.path.isabs(p) else p
            if not os.path.isdir(full):
                print(f"🚫 Entferne {flag} {p} (kein Verzeichnis)")
                total_removed += 2
                i += 2
                continue
            cleaned += [flag, p]
            i += 2
            continue

        # zusammengezogen: -I<path> / -isystem<path>
        if a.startswith("-I") or a.startswith("-isystem"):
            flag = "-I" if a.startswith("-I") else "-isystem"
            p = a[len(flag):]
            if is_blacklisted_header(p):
                print(f"🚫 Entferne {flag}{p} (zeigt auf Header-Datei)")
                total_removed += 1
                i += 1
                continue
            full = os.path.join(entry["directory"], p) if not os.path.isabs(p) else p
            if not os.path.isdir(full):
                print(f"🚫 Entferne {flag}{p} (kein Verzeichnis)")
                total_removed += 1
                i += 1
                continue
            cleaned.append(a)  # original zusammengezogenes Argument behalten
            i += 1
            continue

        # nackter Token, der direkt ein geblacklisteter Header ist (z.B. aus RSP)
        if is_blacklisted_header(a):
            print(f"🚫 Entferne Verweis auf Header: {a}")
            total_removed += 1
            i += 1
            continue

        cleaned.append(a)
        i += 1

    entry["arguments"] = cleaned

print(f"🧽 {total_removed} Argument(e) entfernt (Header/ungültige -I/-isystem).")

# 4) Includes sortieren/prüfen und neu bauen
for entry in data:
    args = entry["arguments"]
    isystem_args, i_args, other_args = [], [], []
    i = 0
    while i < len(args):
        arg = args[i]

        if arg in ("-I", "-isystem") and i + 1 < len(args):
            flag, path = arg, args[i + 1]
            # hier existiert der Pfad sicher schon (oben geprüft)
            if flag == "-isystem":
                isystem_args.extend([flag, path])
            else:
                i_args.extend([flag, path])
            i += 2
            continue

        if arg.startswith("-I") or arg.startswith("-isystem"):
            flag = "-I" if arg.startswith("-I") else "-isystem"
            path = arg[len(flag):]
            if flag == "-isystem":
                isystem_args.extend([flag, path])
            else:
                i_args.extend([flag, path])
            i += 1
            continue

        other_args.append(arg)
        i += 1

    # 5) Fehlende -isystem ergänzen (nur einmal je Pfad)
    def pair_list_to_set(lst):
        return {(lst[j], lst[j+1]) for j in range(0, len(lst), 2)}

    existing_isystem = {p for f, p in pair_list_to_set(isystem_args)}
    for inc in extra_includes:
        if inc not in existing_isystem:
            isystem_args.insert(0, inc)
            isystem_args.insert(0, "-isystem")

    # 6) final zusammenbauen
    compiler = other_args[0] if other_args else ""
    final_args = [compiler] + isystem_args + i_args + other_args[1:]
    entry["arguments"] = final_args

# 7) Backup
with open(backupname, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
print(f"🔁 Backup gespeichert als {backupname}")

# 8) Schreiben
with open(filename, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
print(f"✅ Datei {filename} erfolgreich gepatcht.")
