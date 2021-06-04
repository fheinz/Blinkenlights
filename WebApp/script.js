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
	sendAnimation(currentImage);
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
 * @name sendAnimation
 * Sends a GIF animation to the display
 */
function sendAnimation(img) {
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
		writeToStream('ANM 600000');
		var pixels = Array(ROWS).fill().map(() => Array(COLS));
		frames.forEach(function(frame) {
		    writeToStream('FRM ' +
				  ('delay' in frame ? frame.delay : 1000));
		    pixels.forEach(row => row.fill(0));
		    var bitmap = frame.patch;
		    var row_offset = frame.dims.top;
		    var col_offset = frame.dims.left;
		    for (var r = 0; r < frame.dims.height; r++) {
			for (var c = 0; c < frame.dims.width; c++) {
			    var offset = (r * frame.dims.width + c) * 4;
			    pixels[r+row_offset][c+col_offset] = gammaTable[bitmap[offset]]<<16 |
				gammaTable[bitmap[offset+1]] << 8 |
				gammaTable[bitmap[offset+2]];
			}
		    }
		    for (var r = 0; r < ROWS; r++) {
			writeToStream('RGB ' + (pixels[r].map(p => paddedHex(p, 6)).join('')));
		    }
		});
		writeToStream('DON', 'NXT');
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
	    if (port) sendAnimation(img);
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
	sendAnimation(currentImage);
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
