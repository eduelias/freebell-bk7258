#!/usr/bin/env python3
"""Create (or list) a Home Assistant "generic" camera entry for the doorbell over the REST config-flow
API. Run it on any host that can reach HA. Settings (flag > environment > ~/.config/doorbell-bk7258/env):
HA_URL (default http://homeassistant.local:8123) and HA_TOKEN_FILE (default ~/.ha_token, a file with a
long-lived access token), or --ha-url / --token-file.

  ha_generic_camera.py list
  ha_generic_camera.py add --name "Doorbell BK7258" \
      --still http://<device-ip>:8080/snapshot.jpg --stream http://<device-ip>:8080/stream
  ha_generic_camera.py add --stream rtsp://<device-ip>:554/live            # stream-only (stills from the stream)
  ha_generic_camera.py options ENTRY_ID --stream rtsp://<device-ip>:554/live --still ""   # update an existing entry
  ha_generic_camera.py delete ENTRY_ID

The generic flow fetches the still image to validate it, so the doorbell must be reachable. With an
RTSP stream_source HA probes the stream too (first DESCRIBE opens the camera: allow ~3 s).
"""
import argparse, json, os, sys, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbenv  # noqa: E402

HA = TOKEN_FILE = None   # resolved in main()


def api(method, path, body=None):
    try:
        tok = open(os.path.expanduser(TOKEN_FILE)).read().strip()
    except OSError as e:
        sys.exit(f"cannot read the HA token file {TOKEN_FILE} ({e.strerror}); set HA_TOKEN_FILE or --token-file")
    req = urllib.request.Request(HA.rstrip("/") + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": "Bearer " + tok, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def main():
    global HA, TOKEN_FILE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ha-url", default=None, help="Home Assistant base URL (default: HA_URL)")
    ap.add_argument("--token-file", default=None, help="file with a long-lived token (default: HA_TOKEN_FILE)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    a = sub.add_parser("add")
    a.add_argument("--name", default="Doorbell BK7258")
    a.add_argument("--still", default="")
    a.add_argument("--stream", default="")
    a.add_argument("--framerate", type=int, default=5)
    a.add_argument("--rtsp-transport", default="tcp", choices=["tcp", "udp", "udp_multicast", "http"])
    o = sub.add_parser("options", help="options flow of an existing entry (same fields as add)")
    o.add_argument("entry_id")
    o.add_argument("--still", default=None)
    o.add_argument("--stream", default=None)
    o.add_argument("--framerate", type=int, default=5)
    o.add_argument("--rtsp-transport", default="tcp", choices=["tcp", "udp", "udp_multicast", "http"])
    d = sub.add_parser("delete")
    d.add_argument("entry_id")
    args = ap.parse_args()
    HA = args.ha_url or dbenv.get("HA_URL")
    TOKEN_FILE = args.token_file or dbenv.get("HA_TOKEN_FILE")
    if args.cmd == "add" and not args.still and not args.stream:
        sys.exit("add: give --still and/or --stream")

    if args.cmd == "list":
        st, entries = api("GET", "/api/config/config_entries/entry?domain=generic")
        for e in entries or []:
            print(e["entry_id"], e["title"], e.get("state"))
        return
    if args.cmd == "delete":
        print(api("DELETE", f"/api/config/config_entries/entry/{args.entry_id}"))
        return

    if args.cmd == "options":
        base, handler = "/api/config/config_entries/options/flow", args.entry_id
    else:
        base, handler = "/api/config/config_entries/flow", "generic"
    st, flow = api("POST", base, {"handler": handler})
    print("init:", st, flow.get("step_id"), flow.get("errors"))
    fid = flow["flow_id"]
    # HA 2026.x schema: top-level URLs/credentials, advanced options in a nested "advanced" section
    data = {"advanced": {"framerate": args.framerate, "verify_ssl": False}}
    if args.still:
        data["still_image_url"] = args.still
    if args.stream:
        data["stream_source"] = args.stream
        # HA 2026.9 keeps rtsp_transport inside the "advanced" section (verified on the live entries)
        data["advanced"]["rtsp_transport"] = args.rtsp_transport
    for _ in range(6):
        if flow.get("type") == "create_entry":
            res = flow.get("result") or {}
            print("done:", res.get("entry_id", res) if isinstance(res, dict) else res, flow.get("title"))
            break
        if flow.get("type") == "abort":
            print("aborted:", flow.get("reason"))
            break
        step = flow.get("step_id")
        if step in ("user", "init"):
            st, flow = api("POST", f"{base}/{fid}", data)
        elif step in ("user_confirm_still", "user_confirm", "confirm_still"):
            st, flow = api("POST", f"{base}/{fid}", {"confirmed_ok": True})
        else:
            # unknown step: send an empty form and show what HA asks for
            print("step", step, "schema:", json.dumps(flow.get("data_schema"))[:400])
            st, flow = api("POST", f"{base}/{fid}", {})
        print("->", st, flow.get("type"), flow.get("step_id"), flow.get("errors"))
    else:
        print("gave up:", json.dumps(flow)[:600])


if __name__ == "__main__":
    main()
