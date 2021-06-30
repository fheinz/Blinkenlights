#!/usr/bin/python3
# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# A script to upload a bunch of png files to the matrix.

import argparse
import blinken
import sys
import time

from PIL import Image, ImageColor, ImageDraw

def parse_args(args):
    parser = argparse.ArgumentParser(
        description='Draw a clock showing the current time.')
    parser.add_argument('-v', '--verbose',
        type=bool, default=False, action=argparse.BooleanOptionalAction,
        help='Print data sent to / received from the board.')
    parser.add_argument('-r', '--reset',
        type=bool, default=True, action=argparse.BooleanOptionalAction,
        help='Reset data stored on board before sending new data.')
    parser.add_argument('-t', '--time',
        type=int, default=1000, metavar='TIME',
        help='Display each frame for TIME ms. (default: %(default)s)')
    parser.add_argument('-g', '--gamma',
        type=float, default=0.7, metavar='GAMMA',
        help='Apply GAMMA correction to PNG. (default: %(default)s)')
    parser.add_argument('-d', '--device',
        type=str, default='/dev/ttyUSB0', metavar='DEVICE',
        help='Use DEVICE to talk to board. (default: %(default)s)')
    parser.add_argument('-s', '--speed',
        type=int, default=115200, metavar='SPEED',
        help='Connect serial device at SPEED bps. (default: %(default)s)')

    return parser.parse_args(args)


class Clock(object):
    """Draws a full-width clock to a PIL image at a specified Y offset."""
    # HHMM character x offset in px.
    # Static because we need the entire width to fit 4 5x3 chars.
    x_offsets = [0,4,9,13]

    numbers = [
        blinken.Bitmap(
            ".w.",
            "w w",
            "w w",
            "w w",
            ".w."),
        blinken.Bitmap(
            " w ",
            "ww ",
            " w ",
            " w ",
            "www"),
        blinken.Bitmap(
            "ww ",
            "  w",
            " w ",
            "w  ",
            "www"),
        blinken.Bitmap(
            "www",
            "  w",
            " ww",
            "  w",
            "www"),
        blinken.Bitmap(
            "w w",
            "w w",
            "www",
            "  w",
            "  w"),
        blinken.Bitmap(
            "wwW",
            "w  ",
            "ww ",
            "  w",
            "ww "),
        blinken.Bitmap(
            "www",
            "w  ",
            "www",
            "w w",
            "www"),
        blinken.Bitmap(
            "www",
            "  w",
            " w ",
            " w ",
            " w "),
        blinken.Bitmap(
            "www",
            "w w",
            "www",
            "w w",
            "www"),
        blinken.Bitmap(
            "www",
            "w w",
            "www",
            "  w",
            "www"),
    ]
    mid_dot = blinken.Bitmap("pp", "  ", "pp")
    mid_dot_x = 7

    def __init__(self, y_offset=6):
        self.y_offset =  y_offset

    def Draw(self, draw):
        _, _, _, h, m, s, _, _, _ = time.localtime()

        for i, n in enumerate(divmod(h, 10) + divmod(m, 10)):
            num = self.numbers[n]
            num.Blit(draw, (self.x_offsets[i], self.y_offset))

        if s % 2:
            self.mid_dot.Blit(draw, (self.mid_dot_x, self.y_offset+1))


def main():
    args = parse_args(sys.argv[1:])
    if args.time < 200:
        print("error: --time values <200ms are not supported.\n"
              "Uploading a frame takes ~180ms...")
        sys.exit(1)

    bl = blinken.Blinken(
        gamma=args.gamma,
        debug=args.verbose,
        dev=args.device,
        speed=args.speed)

    if args.reset:
        bl.command("RST")

    frame = 0
    start = time.clock_gettime(time.CLOCK_MONOTONIC)
    clock = Clock()
    while True:
        buf = Image.new(mode='RGB', size=(16, 16))
        draw = ImageDraw.Draw(buf)
        clock.Draw(draw)
        with bl.animation(1000, start_next=True) as anim:
            anim.frame_from_image(buf, 1000)
        frame += 1
        frametime = (start + ((args.time/1000)*frame)) - time.clock_gettime(time.CLOCK_MONOTONIC)
        if frametime < 0:
            continue
        time.sleep(frametime)


if __name__ == '__main__':
    main()
