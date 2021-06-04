# Binkenlights

A LED display for pixel art animations.

This project builds on [Matt Lai](https://github.com/matthewlai)'s [LED matrix
control board](https://github.com/matthewlai/ESP32LEDControl) (see also his
[blog post on the development
process](https://dubiouscreations.com/2021/05/04/designing-an-esp32-based-rgb-matrix-driver-and-making-500-of-them/)!)
to create a display for pixel art animations.

Coupled with a companion webapp and corresponding back-end (TBD), it can be used
by groups of people to share animations amongst themselves, creating a shared
experience despite being physically distant.

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


## How to use the WebApp

The WebApp allows you to interact with the display from your browser. Two
caveats:

* it requires Chrome, because other browsers don't support WebSerial (yet?)
* it won't work if you just load Webapp/index.html from the disk: WebSerial will
  not get access to the port unless the page has been loaded over HTTP.

If you just want to take the WebApp for a spin, the easiest way to satisfy the
second requirement is to [click
here](https://fheinz.github.io/Blinkenlights/WebApp/).

That won't work if you want to test your own local changes to the WebApp. In
that case, running Python's default HTTP server in the WebApp directory will do:

```sh
cd Blinkenlights/WebApp
npm install
npm run-script build
python -m http.server
```

You can now load the app at http://localhost:8000/ and get it to talk to the
board by clicking on the `Connect` button and selecting the approriate serial
port in the pop-up that appears.

Once connected, you use the various UI elements to display stuff in the matrix.


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
