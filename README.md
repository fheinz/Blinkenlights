# Binkenlights

A LED display for pixel art animations.

This project builds on [Matt Lai](https://github.com/matthewlai)'s LED matrix
board](http://todo] to create a display for pixel art animations.

Coupled with a companion webapp and corresponding back-end (which are in the
works), it can be used by groups of people to share animations and create a
sense of shared space despite being physically distant.

## Contents:

* Blinkenlights/Blinkenlights.ino: Arduino code for the firmware
* Blinkenlights/blinkenlights.py: Crude utility to upload 16x16 PNGs to the
  board
* WebApp/*: proof-of-concept single-page web application that can talk to the
  board using the [WebSerial API](https://wicg.github.io/serial/).
  
The web app is very bare-bones, still needs dealing with coordinating the input
and output streams, so it lacks any ability to recover from any errors that the
board may report. It also doesn't have any ability to talk to a server (which
isn't in place yet anyway).

The web app code is heavily based on [the webserial
codelab](https://goo.gle/web-serial-codelab), and the sample pixel art is based
on [henrysoftware](https://henrysoftware.itch.io/)'s [free pixel
food](https://henrysoftware.itch.io/pixel-food) collection.

## License

Copyright 2019 Google, Inc.

Licensed to the Apache Software Foundation (ASF) under one or more contributor
license agreements. See the NOTICE file distributed with this work for
additional information regarding copyright ownership. The ASF licenses this
file to you under the Apache License, Version 2.0 (the “License”); you may not
use this file except in compliance with the License. You may obtain a copy of
the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an “AS IS” BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
