"""Stands in for Half-Life 2 VR's built-in bHaptics client.

Speaks the same protocol the bHaptics SDKs speak - a WebSocket client to
127.0.0.1:15881/v2/feedbacks that sends a Register array on connect and Submit
arrays as events happen. Used to prove the listener, the JSON scan, the key
mapper and the adapter all work end to end, with no game installed.

The key names are INVENTED for this test and are not claimed to be HL2VR's.
They are written in the style the bHaptics Alyx integration uses for its .tact
files (ChamberedRound_1, ClipInserted_1, DamageExplosion_1), which is the only
naming evidence available without the game.
"""
import base64
import json
import os
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", 15881


def mask_frame(payload: bytes) -> bytes:
    """A client->server text frame. RFC 6455 requires client frames be masked."""
    header = bytearray([0x81])  # FIN + text
    n = len(payload)
    if n < 126:
        header.append(0x80 | n)
    elif n < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", n)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", n)
    mask = os.urandom(4)
    header += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return bytes(header) + masked


def main() -> int:
    keys = [
        "PistolFire_1", "SmgFire_1", "ShotgunFire_1", "Ar2Fire_1",
        "MagnumFire_1", "CrossbowFire_1", "RpgFire_1",
        "ShotgunReload_1", "ClipInserted_1", "ChamberedRound_1",
        "PhysCannonGrab_1", "PhysCannonLaunch_1", "PhysCannonDrop_1",
        "CrowbarHit_1", "DamageExplosion_1", "DamageBullet_1",
        "DamageFire_1", "DamageSpark_1", "DamageLaser_1",
        "EnvironmentFire_1", "EnvironmentPoison_1",
        "ShockOnHandLeft_1", "ShockOnHandRight_1",
        "KickbackShotgun_1", "PlayerShootDefault_1",
        "HealthCharger_1", "AmmoPickup_1",
        # Things a vest can represent and a controller cannot - these must be
        # dropped, not mapped.
        "Heartbeat_1", "Footstep_1", "AirboatRide_1",
        # Something no rule should claim, to prove unmapped keys are reported.
        "XenPortalAmbience_1",
    ]

    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
    except OSError as e:
        print("could not connect:", e)
        return 1

    nonce = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET /v2/feedbacks?app_id=hl2vr-test&app_name=HalfLife2VR HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {nonce}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = s.recv(4096).decode(errors="replace")
    if "101" not in resp:
        print("handshake rejected:\n", resp)
        return 1
    print("handshake accepted")

    # Register everything, the way an SDK does on connect.
    s.sendall(mask_frame(json.dumps({
        "Register": [{"Key": k, "Project": {"tracks": []}} for k in keys],
        "Submit": [],
    }).encode()))
    time.sleep(0.3)

    # Then fire a plausible sequence.
    for k in ["PistolFire_1", "PistolFire_1", "ClipInserted_1",
              "ChamberedRound_1", "PhysCannonGrab_1", "PhysCannonLaunch_1",
              "CrowbarHit_1", "DamageExplosion_1", "DamageFire_1",
          "DamageSpark_1", "EnvironmentPoison_1", "ShockOnHandRight_1",
          "Heartbeat_1",
              "XenPortalAmbience_1", "ShotgunFire_1"]:
        s.sendall(mask_frame(json.dumps({
            "Register": [],
            "Submit": [{"type": "key", "key": k, "Parameters": {},
                        "Frame": {"intensity": 1.0}}],
        }).encode()))
        time.sleep(0.12)

    time.sleep(0.4)
    s.close()
    print("sent", len(keys), "registrations and 11 submits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
