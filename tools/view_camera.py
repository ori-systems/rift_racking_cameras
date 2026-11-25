#!/usr/bin/env python3
import argparse
from http.client import HTTPException, HTTPResponse
from urllib.request import urlopen

import cv2
import numpy as np


def open_stream(host: str, port: int) -> HTTPResponse:
    """Open the live JPEG stream at the given board address."""
    # Use the Ethernet address and port supplied on the command line.
    url = f"http://{host}:{port}/stream"
    try:
        # Ask for the stream and bound the wait if the camera stops responding.
        return urlopen(url, timeout=5)
    except (OSError, HTTPException) as error:
        # Show the address that the viewer was trying to reach.
        raise ConnectionError(f"Could not open {url}. Check Ethernet and the UART log.") from error


def read_frame(stream: HTTPResponse) -> np.ndarray:
    """Read and decode one JPEG at the size stored in that picture."""
    # Skip the blank line between pictures and check for the next picture marker.
    marker = stream.readline(1024)
    if marker == b"\r\n":
        marker = stream.readline(1024)
    if marker != b"--frame\r\n":
        # An empty or unexpected marker means there is no next picture to read.
        raise ConnectionError("Camera stream ended before the next frame.")

    # Read each header up to the blank line that marks the start of the picture.
    headers = {}
    for header in iter(lambda: stream.readline(1024), b"\r\n"):
        # Detect a broken connection or an incomplete header before reading more.
        if not header.endswith(b"\r\n"):
            raise ConnectionError("Camera stream sent an incomplete frame header.")
        # Save the details needed to read the JPEG.
        name, _, value = header.partition(b":")
        headers[name.lower()] = value.strip()

    # Check that this picture is a JPEG before reading its bytes.
    if headers.get(b"content-type") != b"image/jpeg":
        raise ConnectionError("Camera stream sent an unknown frame type.")
    # Find the number of bytes the board sent for this picture.
    size_text = headers.get(b"content-length", b"")
    # Keep the read within the largest size expected from the camera.
    size = int(size_text) if size_text.isdigit() else 0
    if not 0 < size <= 8 * 1024 * 1024:
        raise ConnectionError("Camera stream sent an invalid frame size.")
    # Read the complete frame so the next read starts at a new frame marker.
    data = stream.read(size)
    if len(data) != size:
        raise ConnectionError("Camera stream ended partway through a frame.")

    # Point OpenCV at the received bytes without making another copy.
    pixels = np.frombuffer(data, dtype=np.uint8)
    # Read the dimensions from this JPEG so a UART change can resize the live image.
    frame = cv2.imdecode(pixels, cv2.IMREAD_COLOR)
    if frame is None:
        raise ConnectionError("Camera stream sent an unreadable JPEG.")
    return frame


def main() -> int:
    """Show frames until the window is closed or the stream ends."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host", help="IPv4 address printed by the board on UART")
    parser.add_argument("--port", type=int, default=8080, help="stream port (default: 8080)")
    args = parser.parse_args()

    try:
        # Open the board's live JPEG stream.
        stream = open_stream(args.host, args.port)
    except ConnectionError as error:
        parser.exit(1, f"{error}\n")

    window = "RIFT racking camera"
    try:
        cv2.namedWindow(window, cv2.WINDOW_NORMAL)
        while True:
            # Decode and display the next JPEG.
            cv2.imshow(window, read_frame(stream))
            key = cv2.waitKey(1) & 0xFF
            if key in (27, ord("q")) or cv2.getWindowProperty(window, cv2.WND_PROP_VISIBLE) < 1:
                return 0
    except KeyboardInterrupt:
        return 0
    except (OSError, HTTPException) as error:
        parser.exit(1, f"{error} Check Ethernet and the UART log.\n")
    finally:
        stream.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    raise SystemExit(main())
