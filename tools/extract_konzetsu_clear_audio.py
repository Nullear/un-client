"""Copy the original clear audio from an explicitly supplied APK, preserving existing files."""
import argparse
import hashlib
from pathlib import Path
import struct
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("apk", type=Path)
    args = parser.parse_args()
    destination = Path(__file__).resolve().parents[1] / "assets/resources/audio/konzetsu"
    if not destination.is_dir():
        raise SystemExit(f"Missing audio directory: {destination}")
    with zipfile.ZipFile(args.apk) as archive:
        for name in ("challenge_chain_break.wav", "node_chain_break.wav"):
            data = archive.read(f"assets/audio/konzetsu/{name}")
            if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
                raise SystemExit(f"Invalid WAV: {name}")
            offset = 12
            byte_rate = data_size = 0
            while offset + 8 <= len(data):
                chunk, size = struct.unpack_from("<4sI", data, offset)
                if chunk == b"fmt ":
                    byte_rate = struct.unpack_from("<I", data, offset + 16)[0]
                elif chunk == b"data":
                    data_size = size
                offset += 8 + size + (size & 1)
            seconds = data_size / byte_rate
            target = destination / name
            if target.exists() and target.read_bytes() != data:
                raise SystemExit(f"Refusing to overwrite different audio: {target}")
            if not target.exists():
                target.write_bytes(data)
            print(f"{name}: {seconds:.3f}s, SHA-256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
