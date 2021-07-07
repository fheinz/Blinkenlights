/*
 * @license
 * Getting Started with Web Serial Codelab (https://todo)
 * Copyright 2019 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */
'use strict';

import "regenerator-runtime/runtime";
import { parseGIF, decompressFrames } from "gifuct-js";

let port;
let reader;
let inputDone;
let outputDone;
let inputStream;
let outputStream;
let gammaTable;
let currentImage;

const COLS = 16;
const ROWS = COLS;

const log = document.getElementById('log');
const ledCBs = document.querySelectorAll('input.led');
const divLeftBut = document.getElementById('leftBut');
const divRightBut = document.getElementById('rightBut');
const butConnect = document.getElementById('butConnect');
const canvas = new OffscreenCanvas(16,16); // document.getElementById('myCanvas');
const pixelArt = document.querySelectorAll('img.pixelArt');
const gammaSlider = document.getElementById('gammaSlider');
const gammaDisplay = document.getElementById('gammaDisplay');
const redCCSlider = document.getElementById('redCCSlider');
const redCCDisplay = document.getElementById('redCCDisplay');
const blueCCSlider = document.getElementById('blueCCSlider');
const blueCCDisplay = document.getElementById('blueCCDisplay');
const greenCCSlider = document.getElementById('greenCCSlider');
const greenCCDisplay = document.getElementById('greenCCDisplay');
const debugButton = document.getElementById('debugButton');
const clockButton = document.getElementById('clockButton');

const usbFilter = [
    {usbVendorId: 0x1a86, usbProductId: 0x7523}
];

document.addEventListener('DOMContentLoaded', () => {
    butConnect.addEventListener('click', clickConnect);

    const notSupported = document.getElementById('notSupported');
    notSupported.classList.toggle('hidden', 'serial' in navigator);
    initCheckboxes();
    initPixelArt();
    initGamma();
    debugButton.onclick = function() { if (port) writeToStream('DBG'); };
    clockButton.onclick = function() { drawClockMinute('FFFFFF', '000000', '0000FF'); };
});


/**
 * @name paddedHex
 * Return the hex string representation of `number`, padded to `width` places.
 */
function paddedHex(number, width) {
    return number.toString(16).padStart(width, '0').toUpperCase();
}


/**
 * @name connect
 * Opens a Web Serial connection to the board and sets up the input and
 * output stream.
 */
async function connect() {
    const ports = await navigator.serial.getPorts();
    try {
	if (ports.length == 1) {
	    port = ports[0];
	} else {
	    port = await navigator.serial.requestPort({ filters: usbFilter });
	}
	await port.open({ baudRate: 115200 });
    } catch (e) {
	return;
    }

    const encoder = new TextEncoderStream();
    outputDone = encoder.readable.pipeTo(port.writable);
    outputStream = encoder.writable;
    writeToStream('', 'RST', 'VER', 'PWR');
    updateColorCorrection();

    let decoder = new TextDecoderStream();
    inputDone = port.readable.pipeTo(decoder.writable);
    inputStream = decoder.readable.pipeThrough(new TransformStream(new LineBreakTransformer()));
    reader = inputStream.getReader();
    readLoop();
}


/**
 * @name disconnect
 * Closes the Web Serial connection.
 */
async function disconnect() {
    writeToStream('RST');
    if (reader) {
	await reader.cancel();
	await inputDone.catch(() => {});
	reader = null;
	inputDone = null;
    }
    if (outputStream) {
	await outputStream.getWriter().close();
	await outputDone;
	outputStream = null;
	outputDone = null;
    }
    await port.close();
    port = null;
}


/**
 * @name clickConnect
 * Click handler for the connect/disconnect button.
 */
async function clickConnect() {
    if (port) {
	await disconnect();
	toggleUIConnected(false);
	return;
    }
    await connect();
    toggleUIConnected(true);
    if (currentImage)
	sendGifAnimation(currentImage);
    else
	sendGrid();
}


/**
 * @name readLoop
 * Reads data from the input stream and displays it on screen.
 */
async function readLoop() {
    while (true) {
	const { value, done } = await reader.read();
	if (value) {
	    console.log('[RECV]' + value + '\n');
	}
	if (done) {
	    console.log('[readLoop] DONE', done);
	    reader.releaseLock();
	    break;
	}
    }
}


class Frame {
    constructor(ms, pixels) {
	this.duration = ms;
	this.rows = Array.from(pixels, row => Array.from(
	      row, ([r, g, b]) => paddedHex(gammaTable[r]<<16 | gammaTable[g] << 8 | gammaTable[b], 6)).join(''));
    }
}


class Animation{
    constructor(ms) {
	this.duration = ms;
	this.frames = [];
    }

    addFrame(f) {
	this.frames.push(f);
    }
}


function sendAnimation(anim) {
    writeToStream('ANM ' + anim.duration);
    anim.frames.forEach(function(frame) {
	writeToStream('FRM ' + frame.duration);
	frame.rows.forEach(row => writeToStream('RGB ' + row));
    });
    writeToStream('DON', 'NXT');
}


/**
 * @name sendGrid
 * Iterates over the checkboxes and generates the command to set the LEDs.
 */
function sendGrid() {
    writeToStream('ANM 600000', 'FRM 1000');
    var i = 0;
    var px = [];
    ledCBs.forEach((cb) => {
	px.push(cb.checked ? 'FFFFFF' : '000000');
	if (++i % COLS == 0 ) {
	    writeToStream('RGB ' + px.join(''));
	    px = [];
	}
    });
    writeToStream('DON', 'NXT');
}


/**
 * @name sendGifAnimation
 * Sends a GIF animation to the display
 */
function sendGifAnimation(img) {
    var oReq = new XMLHttpRequest();
    oReq.open('GET', img.src, true);
    oReq.responseType = 'arraybuffer';

    oReq.onload = function(oEvent) {
	var arrayBuffer = oReq.response;
	if (arrayBuffer) {
	    var gif = parseGIF(arrayBuffer);
	    if (gif.lsd.width != ROWS || gif.lsd.height != COLS)
		return;
	    var frames = decompressFrames(gif, true);
	    if (frames) {
		var animation = new Animation(600000);
		var pixels = Array(ROWS).fill().map(() => Array(COLS));
		frames.forEach(function(gifFrame) {
		    pixels.forEach(row => row.fill(0));
		    var bitmap = gifFrame.patch;
		    var row_offset = gifFrame.dims.top;
		    var col_offset = gifFrame.dims.left;
		    for (var r = 0; r < gifFrame.dims.height; r++) {
			for (var c = 0; c < gifFrame.dims.width; c++) {
			    var offset = (r * gifFrame.dims.width + c) * 4;
			    pixels[r+row_offset][c+col_offset] = [bitmap[offset], bitmap[offset+1], bitmap[offset+2]];
			}
		    }
		    animation.addFrame(new Frame('delay' in gifFrame ? gifFrame.delay : 1000, pixels));
		});
		sendAnimation(animation);
	    }
	}
    }
    oReq.send(null)
}


/**
 * @name writeToStream
 * Gets a writer from the output stream and send the lines to the micro:bit.
 * @param  {...string} lines lines to send to the micro:bit
 */
function writeToStream(...lines) {
    const writer = outputStream.getWriter();
    lines.forEach((line) => {
	console.log('[SEND]', line);
	writer.write(line + '\n');
    });
    writer.releaseLock();
}

/**
 * @name LineBreakTransformer
 * TransformStream to parse the stream into lines.
 */
class LineBreakTransformer {
    constructor() {
	// A container for holding stream data until a new line.
	this.container = '';
    }

    transform(chunk, controller) {
	this.container += chunk;
	const lines = this.container.split('\n');
	this.container = lines.pop();
	lines.forEach(line => controller.enqueue(line));
    }

    flush(controller) {
	controller.enqueue(this.container)
    }
}

function initCheckboxes() {
    ledCBs.forEach((cb) => {
	cb.addEventListener('change', () => {
	    if (port) sendGrid();
	    currentImage = null;
	});
    });
}

function initPixelArt() {
    pixelArt.forEach((img) => {
	var ctx = canvas.getContext('2d');
	img.crossOrigin = "Anonymous";
	ctx.drawImage(img, 0, 0);
	img.onclick = function() {
	    if (port) sendGifAnimation(img);
	    currentImage = img;
	};
    });
}

function updateGammaDisplay() {
    gammaDisplay.innerHTML = gammaSlider.value/100.0;
}

function updateSliderDisplay(slider, display) {
    display.innerHTML = slider.value/100.0;
}

function updateGamma() {
    var invGamma = 100.0/gammaSlider.value;
    let i = 0;
    gammaTable = Array.from(Array(256), () => Math.round(255*((i++/255.0)**invGamma)));
    if (currentImage) {
	sendGifAnimation(currentImage);
    }
}

function updateColorCorrection() {
    writeToStream(
	'CLC ' + paddedHex(Math.round(2.55*redCCSlider.value), 2) +
	    paddedHex(Math.round(2.55*greenCCSlider.value), 2) +
	    paddedHex(Math.round(2.55*blueCCSlider.value), 2)
    );
}

function initGamma() {
    gammaSlider.oninput = function () {
	updateSliderDisplay(gammaSlider, gammaDisplay);
    };
    gammaSlider.oninput();
    redCCSlider.oninput = function () {
	updateSliderDisplay(redCCSlider, redCCDisplay);
    };
    redCCSlider.oninput();
    greenCCSlider.oninput = function () {
	updateSliderDisplay(greenCCSlider, greenCCDisplay);
    };
    greenCCSlider.oninput();
    blueCCSlider.oninput = function () {
	updateSliderDisplay(blueCCSlider, blueCCDisplay);
    };
    blueCCSlider.oninput();
    gammaSlider.onchange = updateGamma;
    redCCSlider.onchange = updateColorCorrection;
    greenCCSlider.onchange = updateColorCorrection;
    blueCCSlider.onchange = updateColorCorrection;
    updateGamma();
}

function drawGrid(grid) {
    if (grid) {
	grid.forEach((v, i) => {
	    ledCBs[i].checked = !!v;
	});
    }
}

function toggleUIConnected(connected) {
    let lbl = 'Connect';
    if (connected) {
	lbl = 'Disconnect';
    }
    butConnect.textContent = lbl;
    ledCBs.forEach((cb) => {
	if (connected) {
	    cb.removeAttribute('disabled');
	    return;
	}
	cb.setAttribute('disabled', true);
    });
}

const DIGIT_ROWS = 5;
const CLOCK_VERTICAL_OFFSET = 5;
const Digits = [
  [[1,1,1],
   [1,0,1],
   [1,0,1],
   [1,0,1],
   [1,1,1]],
  [[0,1,0],
   [1,1,0],
   [0,1,0],
   [0,1,0],
   [1,1,1]],
  [[1,1,0],
   [0,0,1],
   [0,1,0],
   [1,0,0],
   [1,1,1]],
  [[1,1,1],
   [0,0,1],
   [0,1,1],
   [0,0,1],
   [1,1,1]],
  [[1,0,1],
   [1,0,1],
   [1,1,1],
   [0,0,1],
   [0,0,1]],
  [[1,1,1],
   [1,0,0],
   [1,1,0],
   [0,0,1],
   [1,1,0]],
  [[1,1,1],
   [1,0,0],
   [1,1,1],
   [1,0,1],
   [1,1,1]],
  [[1,1,1],
   [0,0,1],
   [0,1,0],
   [0,1,0],
   [0,1,0]],
  [[1,1,1],
   [1,0,1],
   [1,1,1],
   [1,0,1],
   [1,1,1]],
  [[1,1,1],
   [1,0,1],
   [1,1,1],
   [0,0,1],
   [1,1,1]]
];


function digitRow(d, row, fg, bg) {
    return Digits[d][row].map(p => p ? fg : bg).join('');
}

function drawClockMinute(fg, bg, colon) {
    var t = new Date();
    var hours = t.getHours();
    var hours10 = Math.floor(hours/10);
    var hours1 = hours%10;
    var minutes = t.getMinutes();
    var minutes10 = Math.floor(minutes/10);
    var minutes1 = minutes%10;
    var allBg = bg.repeat(COLS);
    writeToStream('ANM 60000');
    for (var c = 1; c >= 0; c--) {
    writeToStream('FRM 500');
	for (var i = 0; i < CLOCK_VERTICAL_OFFSET; i++) {
            writeToStream('RGB ' + allBg)
	}
	for (var i = 0; i < DIGIT_ROWS; i++) {
	    writeToStream('RGB ' + digitRow(hours10, i, fg, bg)+bg+digitRow(hours1, i, fg, bg)+
			  (c && i%2 ? colon : bg).repeat(2)+
			  digitRow(minutes10, i, fg, bg)+bg+digitRow(minutes1, i, fg, bg));
	}
	for (var i = 0; i < ROWS-(CLOCK_VERTICAL_OFFSET+DIGIT_ROWS); i++) {
            writeToStream('RGB ' + allBg)
	}
    }
    writeToStream('DON', 'NXT');
}
