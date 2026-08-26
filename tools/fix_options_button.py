"""
Bind the PSVR2 right-hand Options button to Half-Life: Alyx's menu.

Why this is needed
------------------
Half-Life: Alyx ships SteamVR bindings for Index, Touch, Vive, Cosmos and WMR,
but none for playstation_vr2_sense. SteamVR therefore auto-remaps Alyx's Oculus
Touch binding onto the Sense controllers and saves the result as a per-app
binding.

In Alyx's Touch binding, the menu action is bound to /user/hand/left/input/y,
which the PSVR2 driver remaps to the left Triangle button. The driver *does*
map Touch's application_menu to the right Options button, but Alyx never binds
application_menu at all, so that mapping leads nowhere and Options does nothing.

This script adds one source to the active binding:

    /user/hand/right/input/options  ->  /actions/dev/in/togglemenu

It is additive: the existing left Triangle binding keeps working.

It is idempotent, backs the file up before the first change, and can be re-run
if SteamVR ever regenerates its autosaved binding.

Usage:
    python fix_options_button.py           apply
    python fix_options_button.py --revert  restore the backup
"""

import json
import os
import shutil
import sys

STEAM_CONFIG = r"C:\Program Files (x86)\Steam\config\steamvr.vrsettings"
WORKSHOP_ROOT = r"C:\Program Files (x86)\Steam\steamapps\workshop\content\250820"
ALYX_APP_KEY = "steam.app.546560"

MENU_ACTION = "/actions/dev/in/togglemenu"
OPTIONS_PATH = "/user/hand/right/input/options"
ACTION_SET = "/actions/dev"


def find_active_binding():
    """Locate the binding SteamVR is actually using for Alyx on PSVR2 Sense."""
    if not os.path.exists(STEAM_CONFIG):
        return None, f"SteamVR settings not found: {STEAM_CONFIG}"

    with open(STEAM_CONFIG, encoding="utf-8") as f:
        settings = json.load(f)

    app = settings.get(ALYX_APP_KEY, {})
    url = None
    for key, value in app.items():
        if "playstation_vr2_sense" in key and key.endswith("AutosaveURL_steamvrinput"):
            url = value
            break
    if not url:
        return None, ("No PSVR2 Sense binding is recorded for Half-Life: Alyx.\n"
                      "  Launch Alyx once with the PSVR2 connected so SteamVR generates one,\n"
                      "  then run this again.")

    # "vr-input-workshop://3790138469"
    if not url.startswith("vr-input-workshop://"):
        return None, f"Unexpected binding URL form: {url!r}"
    item = url.rsplit("/", 1)[-1]

    folder = os.path.join(WORKSHOP_ROOT, item)
    if not os.path.isdir(folder):
        return None, f"Workshop binding folder is missing: {folder}"

    for name in os.listdir(folder):
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            continue
        if data.get("app_key") == ALYX_APP_KEY and \
           data.get("controller_type") == "playstation_vr2_sense":
            return path, None

    return None, f"No Alyx PSVR2 binding file found in {folder}"


def already_bound(data):
    sources = data.get("bindings", {}).get(ACTION_SET, {}).get("sources", [])
    for src in sources:
        if src.get("path") == OPTIONS_PATH and \
           MENU_ACTION in json.dumps(src.get("inputs", {})).lower():
            return True
    return False


def apply(path):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)

    if already_bound(data):
        print("Already bound - nothing to do.")
        return 0

    backup = path + ".before_options_fix"
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
        print(f"Backed up to:\n  {backup}")

    dev = data.setdefault("bindings", {}).setdefault(ACTION_SET, {})
    dev.setdefault("sources", []).append({
        "inputs": {"click": {"output": MENU_ACTION}},
        "mode": "button",
        "path": OPTIONS_PATH,
    })

    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=3, sort_keys=True)

    print(f"Bound {OPTIONS_PATH} -> {MENU_ACTION}")
    print(f"  in {path}")
    return 0


def revert(path):
    backup = path + ".before_options_fix"
    if not os.path.exists(backup):
        print("No backup found; nothing to revert.")
        return 1
    shutil.copy2(backup, path)
    print(f"Restored original binding from:\n  {backup}")
    return 0


def main():
    path, err = find_active_binding()
    if err:
        print(err)
        return 2
    print(f"Active Alyx PSVR2 binding:\n  {path}\n")
    if "--revert" in sys.argv:
        return revert(path)
    rc = apply(path)
    if rc == 0:
        print("\nRestart SteamVR (or just restart Half-Life: Alyx) for this to take effect.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
