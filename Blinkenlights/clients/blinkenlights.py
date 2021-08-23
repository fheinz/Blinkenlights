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


def parse_args(args):
    parser = argparse.ArgumentParser(
        description='Upload 16x16 PNGs to blinkenlights board.')
    parser.add_argument('png_file',
        type=str, nargs='+',
        help='Path to a 16x16 PNG.')
    parser.add_argument('-v', '--verbose',
        type=bool, default=False, action=argparse.BooleanOptionalAction,
        help='Print data sent to / received from the board.')
    parser.add_argument('-r', '--reset',
        type=bool, default=True, action=argparse.BooleanOptionalAction,
        help='Reset data stored on board before sending new data.')
    parser.add_argument('-l', '--loop',
        type=int, default=1, metavar='COUNT',
        help='Loop provided PNGs COUNT times. (default: %(default)s)')
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


def main():
    args = parse_args(sys.argv[1:])
    bl = blinken.Blinken(
        gamma=args.gamma,
        debug=args.verbose,
        dev=args.device,
        speed=args.speed)

    if args.reset:
        bl.command("RST")

    frames = len(args.png_file)
    with bl.animation(args.time * frames * args.loop):
        for frame, png_file in enumerate(args.png_file):
            bl.frame_from_png(png_file, args.time)
            print(f"Wrote frame {frame+1} of {frames}")


if __name__ == '__main__':
    main()
