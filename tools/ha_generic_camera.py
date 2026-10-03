#!/usr/bin/env python3
"""Create (or list) a Home Assistant "generic" camera entry for the doorbell over the REST config-flow
API. Runs on a host that can reach HA (du7); the long-lived token is read from ~/.ha_token.

  ha_generic_camera.py list
  ha_generic_camera.py add --name "Doorbell BK7258" \
      --still http://192.168.2.89:8080/snapshot.jpg --stream http://192.168.2.89:8080/stream
  ha_generic_camera.py add --stream rtsp://192.168.2.89:554/live            # stream-only (stills from the stream)
  ha_generic_camera.py options ENTRY_ID --stream rtsp://192.168.2.89:554/live --still ""   # update an existing entry
  ha_generic_camera.py delete ENTRY_ID

The generic flow fetches the still image to validate it, so the doorbell must be reachable. With an
RTSP stream_source HA probes the stream too (first DESCRIBE opens the camera: allow ~3 s).
"""
import argparse, json, os, sys, urllib.request

HA = os.environ.get("HA_URL", "http://192.168.2.80:8123")


def api(method, path, body=None):
    tok = open(os.path.expanduser("~/.ha_token")).read().strip()
    req = urllib.request.Request(HA + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": "Bearer " + tok, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def main():
    ap = argparse.ArgumentParser()
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
        data["rtsp_transport"] = args.rtsp_transport
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
