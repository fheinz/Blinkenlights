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
# An exceedingly crude script to upload a bunch of png files to the matrix.

import png
import serial
import sys

if len(sys.argv) < 3 :
    print("Usage: %s <gamma> <16x16 png file>..."%sys.argv[0], file=sys.stderr, );
    exit(1)

_InvGamma = 1/float(sys.argv[1])
GammaLUT = [int(round(255*((v/255.0)**_InvGamma))) for v in range(256)]

Serial = serial.Serial('/dev/ttyACM0', 9600)

while True:
    Serial.write("VER\n".encode())
    l = Serial.readline()
    print(l);
    if l == b"ACK VER 1.0\r\n":
        break
Serial.write("ANM 600000\n".encode())
print(Serial.readline())

for png_file in sys.argv[2:]:
    (width, height, rows, meta) = png.Reader(file=open(png_file, "rb")).asRGB8()
    Serial.write("FRM 1000\n".encode())
    print(Serial.readline())
    for r in rows:
        Serial.write(("RGB %s\n"%"".join("%02X"%GammaLUT[v] for v in r)).encode())
        print(Serial.readline())
Serial.write("DON\n".encode())
print(Serial.readline())
Serial.write("QUE\n".encode())
print(Serial.readline())
