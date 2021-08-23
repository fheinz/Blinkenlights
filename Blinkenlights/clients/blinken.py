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

import contextlib
import serial
import sys

from PIL import Image, ImageColor

class Blinken(object):
    def __init__(self, gamma=0.7, debug=False, dev='/dev/ttyUSB0', speed=115200):
        self._serial = serial.Serial(dev, speed)
        self._debug = debug

        self._gamma_table = [
            int(round(255*((v/255.0)**(1/gamma)))) for v in range(256)]
        while True:
            if self.command('VER') == 'VER 1.0':
                break

    def write(self, line):
        if self._debug:
            print(f'W: {line}')
        self._serial.write(f'{line}\n'.encode())

    def read(self):
        line = self._serial.readline().decode().rstrip()
        if self._debug:
            print(f'R: {line:s}')
        return line

    def command(self, cmd):
        """Sends a single command to the board and waits for a response."""
        self.write(cmd)
        return self.check()

    def check(self):
        """Checks and returns the board response for a single command."""
        rsp = self.read().split(maxsplit=1)
        if rsp[0] == 'NAK':
            self.write('RST')
            self.read()
            print(f'ERR: {rsp[1]}')
            sys.exit(1)
        return rsp[1]

    def commands(self, cmds):
        """Send all commands before checking any results."""
        for cmd in cmds:
            self.write(cmd)
        for cmd in cmds:
            self.check()

    @contextlib.contextmanager
    def animation(self, ms, start_next=False):
        anim = Animation(ms, start_next, self._gamma_table)
        yield anim
        anim.render(self)


class Animation(object):
    """Container for a complete animation to be sent to the board.

    You probably want to use Blinken.animation to create these.
    """
    def __init__(self, ms, start_next, gamma_table):

        self._cmdlist = [f'ANM {ms}']
        self._next = start_next
        self._gamma_table = gamma_table

    def frame_from_png(self, png_file, ms):
        """Creates a frame for the animation from a PNG file.

        Args:
            png_file: The path to a 16x16 PNG file.
            ms: Frame display time in (int) milliseconds.
        """
        image = Image.open(png_file).convert(mode='RGB')
        self.frame_from_image(image, ms)

    def frame_from_image(self, image, ms):
        """Creates a frame for the animation from a PIL RGB image.

        Args:
            image: A 1x616 RGB PIL Image.
            ms: Frame display time in (int) milliseconds.
        """
        if image.width != 16 or image.height != 16:
            raise ValueError(f'image is {image.width}x{image.height} not 16x16')
        if image.mode != 'RGB':
            raise ValueError(f'image is {image.mode} not RGB')

        self._cmdlist.append(f"FRM {ms}")

        data = image.tobytes()
        for line in range(16):
            rgb = ""
            for byte in data[3*16*line:3*16*(line+1)]:
                corrected = self._gamma_table[byte]
                rgb += f'{corrected:02X}'
            self._cmdlist.append(f'RGB {rgb}')

    def render(self, blinken):
        self._cmdlist.append("DON")
        if self._next:
            self._cmdlist.append("NXT")
        blinken.commands(self._cmdlist)


class Bitmap(object):
    """Describe a simple image using chars for colours. Example:

    Bitmap(
        "  yyy  ",
        " yyyyy ",
        "yy_y_yy",
        "yyyyyyy",
        "y_yyy_y",
        " y___y ",
        "  yyy  ")

    This would draw a 7x7 smiley face.
    """

    colours = {
        'w': ImageColor.getrgb('#ffffff'),
        'r': ImageColor.getrgb('#ff0000'),
        'o': ImageColor.getrgb('#ff3f00'),
        'y': ImageColor.getrgb('#ffff00'),
        'g': ImageColor.getrgb('#00ff00'),
        'c': ImageColor.getrgb('#007fff'),
        'b': ImageColor.getrgb('#0000ff'),
        'p': ImageColor.getrgb('#3f00ff'),
        'k': ImageColor.getrgb('#ff3f3f'),
        '.': ImageColor.getrgb('#3f3f3f'),
        '_': ImageColor.getrgb('#000000'),
    }

    def __init__(self, *lines):
        self._pixels = []
        self._width = 0
        self._height = len(lines)

        if self._height > 16:
            raise ValueError("Can't draw anything bigger than 16x16")

        for y, line in enumerate(lines):
            width = len(line)
            if width > self._width:
                self._width = width
            if width > 16:
                raise ValueError("Can't draw anything bigger than 16x16")

            for x, char in enumerate(line):
                if char != ' ':
                    if char not in self.colours:
                        char = 'w'
                    self._pixels.append((x, y, self.colours[char]))

    def Blit(self, draw, pos):
        """Writes the pixel data to an ImageDraw draw context.

        Args:
            draw: An ImageDraw.Draw created with Image.draw().
            pos: An (x,y) tuple for the top left corner of the image.
        """
        if pos[0] + self._width > 16:
            raise ValueError("Bitmap is too wide to draw at this location.")
        if pos[1] + self._height > 16:
            raise ValueError("Bitmap is too tall to draw at this location.")

        for p in self._pixels:
            draw.point((p[0]+pos[0], p[1]+pos[1]), p[2])
