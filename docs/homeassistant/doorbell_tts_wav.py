#!/usr/bin/env python3
"""doorbell_tts_wav.py MESSAGE [LANG] [NAME]: HA TTS -> /config/www/doorbell/NAME.wav (16 kHz mono s16le).
Runs inside the Home Assistant core container (shell_command), which ships ffmpeg."""
import json, os, re, subprocess, sys, urllib.request
TTS_ENTITY = os.environ.get("DOORBELL_TTS_ENTITY", "tts.google_translate_en_com")
msg = sys.argv[1]
lang = sys.argv[2] if len(sys.argv) > 2 else "en"
name = re.sub(r"[^A-Za-z0-9_-]", "_", sys.argv[3]) if len(sys.argv) > 3 else "say"
token = open("/config/.doorbell_ha_token").read().strip()
req = urllib.request.Request("http://localhost:8123/api/tts_get_url",
    data=json.dumps({"engine_id": TTS_ENTITY, "message": msg, "language": lang}).encode(),
    headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
path = json.load(urllib.request.urlopen(req, timeout=30))["path"]   # "/api/tts_proxy/<hash>.mp3"
mp3 = urllib.request.urlopen("http://localhost:8123" + path, timeout=30).read()
os.makedirs("/config/www/doorbell", exist_ok=True)
out = f"/config/www/doorbell/{name}.wav"
subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", "pipe:0", "-ac", "1", "-ar", "16000", "-f", "wav", out],
               input=mp3, check=True)
print(out, os.path.getsize(out))
