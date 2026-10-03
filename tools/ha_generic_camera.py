#!/usr/bin/env python3
"""Create (or list) a Home Assistant "generic" camera entry for the doorbell over the REST config-flow
API. Runs on a host that can reach HA (du7); the long-lived token is read from ~/.ha_token.

  ha_generic_camera.py list
  ha_generic_camera.py add --name "Doorbell BK7258" \
      --still http://192.168.2.89:8080/snapshot.jpg --stream http://192.168.2.89:8080/stream
  ha_generic_camera.py delete ENTRY_ID

The generic flow fetches the still image to validate it, so the doorbell must be reachable.
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
    a.add_argument("--still", required=True)
    a.add_argument("--stream", default="")
    a.add_argument("--framerate", type=int, default=5)
    d = sub.add_parser("delete")
    d.add_argument("entry_id")
    args = ap.parse_args()

    if args.cmd == "list":
        st, entries = api("GET", "/api/config/config_entries/entry?domain=generic")
        for e in entries or []:
            print(e["entry_id"], e["title"], e.get("state"))
        return
    if args.cmd == "delete":
        print(api("DELETE", f"/api/config/config_entries/entry/{args.entry_id}"))
        return

    st, flow = api("POST", "/api/config/config_entries/flow", {"handler": "generic"})
    print("init:", st, flow.get("step_id"), flow.get("errors"))
    fid = flow["flow_id"]
    # HA 2026.x schema: top-level URLs/credentials, advanced options in a nested "advanced" section
    data = {"still_image_url": args.still, "advanced": {"framerate": args.framerate, "verify_ssl": False}}
    if args.stream:
        data["stream_source"] = args.stream
    for _ in range(6):
        if flow.get("type") == "create_entry":
            print("created:", flow["result"]["entry_id"], flow["title"])
            break
        if flow.get("type") == "abort":
            print("aborted:", flow.get("reason"))
            break
        step = flow.get("step_id")
        if step == "user":
            st, flow = api("POST", f"/api/config/config_entries/flow/{fid}", data)
        elif step in ("user_confirm_still", "user_confirm", "confirm_still"):
            st, flow = api("POST", f"/api/config/config_entries/flow/{fid}", {"confirmed_ok": True})
        else:
            # unknown step: send an empty form and show what HA asks for
            print("step", step, "schema:", json.dumps(flow.get("data_schema"))[:400])
            st, flow = api("POST", f"/api/config/config_entries/flow/{fid}", {})
        print("->", st, flow.get("type"), flow.get("step_id"), flow.get("errors"))
    else:
        print("gave up:", json.dumps(flow)[:600])


if __name__ == "__main__":
    main()
