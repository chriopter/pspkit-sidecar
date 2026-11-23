#!/usr/bin/env python3
"""Stream the PSP workspace, including the pointer, to the gud framebuffer."""
import os
from pathlib import Path
import signal
import subprocess
import sys


def stream(device, output):
    info = Path("/sys/class/graphics") / Path(device).name
    if ((info / "name").read_text().strip() != "guddrmfb"
            or (info / "virtual_size").read_text().strip() != "480,272"
            or int((info / "bits_per_pixel").read_text()) != 16
            or int((info / "stride").read_text()) != 960):
        raise ValueError(f"Unexpected PSP framebuffer layout on {device}")

    stride = 480 * 2
    frame_size = 272 * stride
    # Capture at output refresh even when only the hardware cursor has moved.
    command = ["wf-recorder", "-D", "-o", output, "-c", "rawvideo", "-x", "rgb565le",
               "-p", "threads=1", "-m", "rawvideo", "-f", "pipe:1"]
    with open(device, "r+b", buffering=0) as fb, subprocess.Popen(command, stdout=subprocess.PIPE) as capture:
        previous = b""
        try:
            while info.exists():
                frame = capture.stdout.read(frame_size)
                if len(frame) != frame_size:
                    raise RuntimeError("PSP screen capture ended")
                if frame == previous:
                    continue
                first, end = 0, frame_size
                if previous:
                    # Send only the affected scanlines when the pointer moves.
                    while frame[first:first + stride] == previous[first:first + stride]:
                        first += stride
                    while frame[end - stride:end] == previous[end - stride:end]:
                        end -= stride
                if os.pwrite(fb.fileno(), frame[first:end], first) != end - first:
                    raise OSError("Incomplete PSP framebuffer write")
                previous = frame
        finally:
            capture.terminate()
            capture.stdout.close()


def stop(signum, stack):
    raise SystemExit(0)


if __name__ == "__main__":
    device, output = sys.argv[1:]
    signal.signal(signal.SIGTERM, stop)
    try:
        stream(device, output)
    finally:
        subprocess.run(["hyprctl", "output", "remove", output], check=False)
