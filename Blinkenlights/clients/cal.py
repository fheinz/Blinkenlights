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
import arrow
import blinken
import clock
import ics
import os
import requests
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
    parser.add_argument('-c', '--calendar',
        type=str, metavar='CAL',
        default=('https://calendar.google.com/calendar/ical/' +
            os.environ.get('USER') + '%40google.com/public/basic.ics'),
        help='File or URL to fetch an ICS-format calendar from.')

    return parser.parse_args(args)


colours = {
    'white':  ImageColor.getrgb('#ffffff'),
    'grey':   ImageColor.getrgb('#3f3f3f'),
    'red':    ImageColor.getrgb('#ff0000'),
    'orange': ImageColor.getrgb('#ff3f00'),
    'yellow': ImageColor.getrgb('#ffff00'),
    'green':  ImageColor.getrgb('#00ff00'),
    'cyan':   ImageColor.getrgb('#007fff'),
    'blue':   ImageColor.getrgb('#0000ff'),
    'purple': ImageColor.getrgb('#3f00ff'),
    'pink':   ImageColor.getrgb('#ff3f3f'),
}

rainbow = ['red', 'orange', 'yellow', 'green', 'cyan', 'blue', 'purple']

def splash(bl):
    buf = Image.new(mode='RGB', size=(16, 16))
    draw = ImageDraw.Draw(buf)
    # Splash animation is 210ms long.
    # Hopefully 21s is enough time to load everything from calendar.
    with bl.animation(210*100) as anim:
        for shuf in reversed(range(7)):
            r = rainbow.copy()
            r = r[shuf:len(r)] + r[0:shuf]
            for i, colour in enumerate(r):
                draw.rectangle([(2*i)+1, 0, (2*i)+2, 15], colours[colour])
            anim.frame_from_image(buf, 30)


class Box(object):
    def __init__(self, n, begin, end):
        self._n = n
        self._begin = begin
        self._end = end
        self._events = []

    def __bool__(self):
        return bool(self._events)

    def __len__(self):
        return len(self._events)

    def __iter__(self):
        return self._events.__iter__()

    def BeginsIn(self, event):
        return event.begin >= self._begin and event.begin < self._end

    def EndsIn(self, event):
        return event.end > self._begin and event.end <= self._end

    def AddEvent(self, event):
        self._events.append(event)

    def Fill(self, draw, colour):
        x, y = divmod(self._n, 2)
        draw.rectangle([(3*x)+1,(4*y)+1,(3*x)+2,(4*y)+3], colour)


class Cal(object):

    def __init__(self, file_or_url, now=arrow.now()):
        self._url = None
        self._current_hour = None
        if file_or_url.startswith('https://'):
            self._last_fetch = None
            self._url = file_or_url
            self.RefreshCal()
        else:
            self._cal = ics.Calendar(open(file_or_url).read())
        self.UpdateBoxes(now)

    def RefreshCal(self):
        if not self._url:
            return
        if self._last_fetch and self._last_fetch > arrow.now().shift(days=-1):
            return
        print(f'Fetching calendar data from {self._url} ... ',
                end='', flush=True)
        self._cal = ics.Calendar(requests.get(self._url).text)
        self._last_fetch = arrow.now()
        print('done.')

    def Upcoming(self, start, end):
        events = list(self._cal.timeline.overlapping(start, end))
        sf, ef = start.format('HH:mm'), end.format('HH:mm')
        print(f'Found {len(events)} upcoming events for period {sf} to {ef}.')
        return events

    def Boxes(self, start):
        end = start.shift(hours=5, seconds=-1)

        # Calendar events are quantized to 30 minute boxes.
        # Grid shows current hour + next 4, 1h per column.
        # List index == box position in grid, 10 boxes total.
        boxes = []
        for hour in range(5):
            boxes.append(Box(2*hour,
                         begin=start.shift(hours=hour, minutes=0),
                         end=start.shift(hours=hour, minutes=30)))
            boxes.append(Box(2*hour+1,
                         begin=start.shift(hours=hour, minutes=30),
                         end=start.shift(hours=hour+1, minutes=0)))

        # Map upcoming events to boxes.
        for event in self.Upcoming(start, end):
            if event.all_day or event.end == start:
                continue
            in_event = False
            for box in boxes:
                if box.BeginsIn(event):
                    in_event = True
                if in_event:
                    box.AddEvent(event)
                if box.EndsIn(event):
                    in_event = False
        return boxes

    def UpdateBoxes(self, now):
        self.RefreshCal()
        if self._current_hour and self._current_hour == now.hour:
            return
        self._current_hour = now.hour
        start = now.floor('hour')
        self._boxes = self.Boxes(start)

    def Draw(self, draw, now):
        boxes = self._boxes

        # Rules:
        #   - If in meeting now or <5 mins, grid should be red, otherwise blue
        meeting_tests = [0]
        if now.minute >= 25:
            # In the 5 mins between 25 past and half past, test both box 0 and
            # box 1 for the presence of meetings.
            meeting_tests.append(1)
        if now.minute >= 30:
            meeting_tests.pop(0)
        if now.minute >= 55:
            meeting_tests.append(2)

        grid_colour = colours['blue']
        for boxno in meeting_tests:
            if boxes[boxno]:
                grid_colour = colours['red']
        self._Grid(draw, grid_colour)

        #   - Upcoming events colour their related box cyan.
        for box in boxes[1:]:
            if box:
                box.Fill(draw, colours['cyan'])

        #   - For first column, if currently in a meeting, box is orange.
        #   - If in second 30m of hour, box 0 is grey.
        if now.minute >= 30:
            boxes[0].Fill(draw, colours['grey'])
            if boxes[1]:
                boxes[1].Fill(draw, colours['orange'])
        else:
            if boxes[0]:
                boxes[0].Fill(draw, colours['orange'])

        #   - TODO: If no meeting now and meeting in >1m, flash box for next meeting?
        #   - TODO: Different colours / patterns for >1 meeting in a box?
        #   - TODO: Different colour if event=yes? Not available with free/busy.

    def _Grid(self, draw, colour):
        # Lay out grid.
        draw.rectangle([0, 0, 15, 8], outline=colour)
        # This one is always white because it's the 30m separator.
        draw.line([1,4,14,4], colours['white'])
        for x in range(3, 15, 3):
            draw.line([x,1,x,7], colour)


def main():
    args = parse_args(sys.argv[1:])
    if args.time < 200:
        print('error: --time values <200ms are not supported.\n'
              'Uploading a frame takes ~180ms...')
        sys.exit(1)

    bl = blinken.Blinken(
        gamma=args.gamma,
        debug=args.verbose,
        dev=args.device,
        speed=args.speed)

    if args.reset:
        bl.command('RST')

    # Render a splash screen to ensure there's always
    # one animation in the queue as our "framebuffer"
    # and cause people seizures as we fetch calendar data ;-)
    splash(bl)
    clk = clock.Clock(10)
    cal = Cal(args.calendar)

    frame = 0
    start = time.clock_gettime(time.CLOCK_MONOTONIC)
    while True:
        now = arrow.now()
        buf = Image.new(mode='RGB', size=(16, 16))
        draw = ImageDraw.Draw(buf)
        cal.Draw(draw, now)
        clk.Draw(draw)
        with bl.animation(1100, start_next=True) as anim:
            anim.frame_from_image(buf, 1100)
        # Keep calendar updates in the dead time between frame draws.
        # Update for *next* second because that's when it'll be rendered.
        # TODO: put it on a separate thread, fetching and parsing 500kb
        #   of ICS data with raw python is multiple-seconds slow and will
        #   cause a noticeable period of blank screen.
        cal.UpdateBoxes(now.shift(seconds=1))
        frame += 1
        frametime = (start + ((args.time/1000)*frame)) - time.clock_gettime(time.CLOCK_MONOTONIC)
        if frametime < 0:
            print('Negative frame time, things are taking too long :-O')
            continue
        time.sleep(frametime)


if __name__ == '__main__':
    main()
