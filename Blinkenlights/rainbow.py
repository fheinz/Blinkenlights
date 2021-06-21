#!/bin/env python3
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
        description='Draw an animated square rainbow.')
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


colours = [
    ImageColor.getrgb('#000000'),
    ImageColor.getrgb('#3f00ff'),
    ImageColor.getrgb('#00007f'),
    ImageColor.getrgb('#007fff'),
    ImageColor.getrgb('#00ff00'),
    ImageColor.getrgb('#ffff00'),
    ImageColor.getrgb('#ff3f00'),
    ImageColor.getrgb('#ff0000'),
]


def main():
    args = parse_args(sys.argv[1:])
    bl = blinken.Blinken(
        gamma=args.gamma,
        debug=args.verbose,
        dev=args.device,
        speed=args.speed)

    if args.reset:
        bl.command("RST")

    blank = Image.new(mode='RGB', size=(16, 16))
    with bl.animation(1000) as anim:
        anim.frame_from_image(blank, 1000)
    frame = 0
    start = time.clock_gettime(time.CLOCK_MONOTONIC)
    while True:
        buf = blank.copy()
        draw = ImageDraw.Draw(buf)
        for i, colour in enumerate(colours):
            size = (frame+i) % 8
            draw.rectangle([7-size,7-size,8+size,8+size], outline=colour)
        with bl.animation(1000, start_next=True) as anim:
            anim.frame_from_image(buf, 1000)
        frame += 1
        # It takes around 180ms to create an animation, upload a frame, and
        # switch out from the last uploaded animation.
        frametime = (start + ((args.time/1000)*frame)) - time.clock_gettime(time.CLOCK_MONOTONIC)
        if frametime < 0:
            continue
        time.sleep(frametime)


if __name__ == '__main__':
    main()
