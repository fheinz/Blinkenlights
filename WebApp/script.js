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
import { Dropzone } from "dropzone";

const css = require("./style.css");

let gammaTable;
let currentImage;
let blinkenlights;

const COLS = 16;
const ROWS = COLS;

const MIN_GAMMA = 0.5;
const MAX_GAMMA = 3.0;
const DEFAULT_GAMMA = 2.0;

const protocolTimeout = 300;
const log = document.getElementById('log');
const butConnect = document.getElementById('butConnect');
const butSend = document.getElementById('butSend');
const commandTextInput = document.getElementById('commandTxt');
const pixelArtContainer = document.getElementById('pixelArtContainer');
const gammaSlider = document.getElementById('gammaSlider');
const gammaDisplay = document.getElementById('gammaDisplay');
const debugButton = document.getElementById('debugButton');
const clockButton = document.getElementById('clockButton');
const gifDropzone = document.getElementById('gif-dropzone');

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
	const lines = this.container.split('\r\n');
	this.container = lines.pop();
	lines.forEach(line => controller.enqueue(line));
    }

    flush(controller) {
	controller.enqueue(this.container)
    }
}

/**
 * @name paddedHex
 * Return the hex string representation of `number`, padded to `width` places.
 */
function paddedHex(number, width) {
    return number.toString(16).padStart(width, '0').toUpperCase();
}

const States = {
    UNKNOWN: "unknown",
    IDLE: "idle",
    RESETTING: "resetting",
    SYNC: "sync",
    GET_VERSION: "get version",
    WF_ACK: {
	RGB: "waiting for RGB line ack"
    },
}

const Events = {
    ANY: "wildcard event",
    TIMEOUT: "timeout",
    ACK: {
	FRM: "frame ack",
	RGB: "rgb ack",
	VER: "version ack",
	SYN: "synchronization ack",
	RST:  "reset ack",
    },
    NAK: {
	SYN: "synchronization nack",
	CMD: "command nack",
    }

}

const Transitions = {};
Transitions[States.UNKNOWN] = {}
Transitions[States.IDLE] = {}

Transitions[States.SYNC] =  {};
Transitions[States.SYNC][Events.ACK.SYN] = function (bl, ev) {
    if (ev.line == bl.expected_line) {
	bl.clearTimeout();
	return States.IDLE;
    }
    return bl.state;
}
Transitions[States.SYNC][Events.TIMEOUT] = function (bl, ev) { bl.synchronize(); return bl.state; }
Transitions[States.SYNC][Events.NAK.SYN] = function (bl, ev) { bl.synchronize(); return bl.state; }
Transitions[States.SYNC][Events.NAK.CMD] = function (bl, ev) { bl.synchronize(); return bl.state; }

Transitions[States.GET_VERSION] = {};
Transitions[States.GET_VERSION][Events.ACK.VER] =  function (bl, ev) {
    bl.clearTimeout();
    bl.protocol_version = ev.line.substring(8);
    return States.IDLE;
};

Transitions[States.RESETTING] = {}
Transitions[States.RESETTING][Events.ACK.RST] = function (bl, ev) { return States.IDLE; }
Transitions[States.RESETTING][Events.TIMEOUT] = function (bl, ev) { bl.reset(); ; return bl.state; }

Transitions[States.WF_ACK.RGB] = {};
Transitions[States.WF_ACK.RGB][Events.ACK.RGB] = function (bl, ev) {
    if (++bl.acked_lines < COLS) { return bl.state; }
    return States.IDLE;
}

class Blinkenlights {

    constructor() {
	this.port = null;
	this.state = States.IDLE;
	this.timeout = null;
	this.job_queue = [];
    }

    /**
     * @name isConnected
     * Returns true of connection to the board is actuve.
     */
    isConnected() {
	return this.port !== null;
    }

    /**
     * @name connect
     * Opens a Web Serial connection to the board and sets up the input and
     * output stream.
     */
    async connect() {
	if (this.isConnected()) return;
	const ports = await navigator.serial.getPorts();
	var port = null;
	if (ports.length == 1) {
	    port = ports[0];
	} else {
	    port = await navigator.serial.requestPort();
	}
	await port.open({ baudRate: 115200 });

	this.port = port;
	const encoder = new TextEncoderStream();
	this.outputDone = encoder.readable.pipeTo(port.writable);
	this.outputStream = encoder.writable;

	let decoder = new TextDecoderStream();
	this.inputDone = port.readable.pipeTo(decoder.writable);
	let inputStream = decoder.readable.pipeThrough(new TransformStream(new LineBreakTransformer()));
	this.reader = inputStream.getReader();
    }

    /**
     * @name sendCommands
     * Gets a writer from the output stream and send the lines to the micro:bit.
     * @param  {...string} lines: lines to send to the board
     */
    sendCommands(...lines) {
	if (!this.isConnected()) return;
	const writer = this.outputStream.getWriter();
	lines.forEach((line) => {
	    console.log('[SEND]', line);
	    writer.write(line + '\n');
	});
	writer.releaseLock();
    }

    /**
     * @name getTransition
     * Gets the transition function for the current State, Event combination.
     * @param {string} first, second: primary and secondary event Protocol Unit ids
     */
    getTransition(first, second) {
	let valid_transitions = Transitions[this.state];
	if (first in Events) {
	    if (Events[first].constructor === Object &&
		second in Events[first] && Events[first][second] in valid_transitions) {
		return valid_transitions[Events[first][second]];
	    }
	    if (Events[first] in valid_transitions) {
		return valid_transitions[Events[first]];
	    }
	}
	if (Events.ANY in valid_transitions) {
	    return valid_transitions[Events.ANY];
	}
	return null;
    }

    /**
     * @name run
     * Run the protocol state machine.
     */
    async run() {
	let _this = this;
	if (!this.isConnected()) return;
	this.addToQueue(function () { _this.getProtocolVersion(); });
	this.addToQueue(function () { _this.reset(); });
	while (true) {
	    this.processQueue();
	    let { value, done } = await this.reader.read();
	    if (value) {
		console.log('[RECV] ' + value + '\n');
		var transition = this.getTransition(value.substring(0, 3), value.substring(4, 7));
		if (transition) { this.state = transition(this, { line: value }); }
	    }
	    if (done) {
		console.log('[readLoop] DONE', done);
		break;
	    }
	}
    }

    /**
     * @name clearTimeout
     * Stop the timeout counter
     */
    clearTimeout() {
	if (this.timeout) {
	    window.clearTimeout(this.timeout);
	    this.timeout = null;
	}
    }

    /**
     * @name setTimeout
     * Set a timeout for a response to a command to arrive
     */
    setTimeout(ms) {
	let _this = this;
	this.timeout = window.setTimeout(function () { _this.triggerTimeout(); }, ms);
    }

    /**
     * @name triggerTimeout
     * Trigger a timeout event in the state machine.
     */
    triggerTimeout() {
	this.clearTimeout();
	const transition = this.getTransition(Events.TIMEOUT, null);
	if (transition) { this.state = transition(this, {}) };
    }

    /**
     * @name clearQueue
     * Get rid of any remaining jobs in the queue
     */
    clearQueue() {
	this.job_queue = [];
    }

    /**
     * @name addToQueue
     * Add a job to the queue.
     */
    addToQueue(job) {
	this.job_queue.push(job);
	this.processQueue();
    }


    /**
     * @name processQueue
     * Process items from the command queue
     */
    processQueue() {
	while (this.state == States.IDLE && this.job_queue.length > 0) {
	    this.job_queue.shift()();
	}
    }

    /**
     * @name disconnect
     * Closes the Web Serial connection.
     */
    async disconnect() {
	if (this.reader) {
	    this.reader.releaseLock();
	    await this.reader.cancel();
	    await this.inputDone.catch(() => {});
	    this.reader = null;
	    this.inputDone = null;
	}
	if (this.outputStream) {
	    await this.outputStream.getWriter().close();
	    await this.outputDone;
	    this.outputStream = null;
	    this.outputDone = null;
	}
	await this.port.close();
	this.port = null;
    }

    /**
     * @name reset
     * Reset the job queue and get the board to the startup state
     */
    reset() {
	this.clearQueue();
	this.sendCommands('RST');
	this.state = States.RESETTING;
    }

    /**
     * @name getProtocolVersion
     * Query the protocol version from the board
     */
    getProtocolVersion() {
	this.sendCommands('VER');
	this.setTimeout(protocolTimeout);
	this.state = States.GET_VERSION;
    }

    /**
     * @name sendAnimation
     * Send an animation to the board
     */
    sendAnimation(anim) {
	let _this = this;
	this.addToQueue(function() { _this.sendCommands('ANM ' + anim.duration); });
	anim.frames.forEach(function(frame) {
	    _this.addToQueue(function() {
		_this.sendCommands('FRM ' + frame.duration);
		frame.rows.forEach(row => _this.sendCommands('RGB ' + row));
		_this.state = States.WF_ACK.RGB
	    });
	});
	this.addToQueue(function () { _this.sendCommands('DON', 'NXT'); });
    }

}


/**
 * @name clickConnect
 * Click handler for the connect/disconnect button.
 */
async function clickConnect() {
    if (blinkenlights.isConnected()) {
	blinkenlights.disconnect();
	toggleUIConnected(false);
	return;
    }
    await blinkenlights.connect();
    blinkenlights.run();
    toggleUIConnected(true);
    if (currentImage)
	sendGifAnimation(currentImage);
}


/**
 * @name clickSend
 * Opens Send a command to the board
 */
async function clickSend() {
    if (!port) return;
    blinkenlights.sendCommands(commandTextInput.value);
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
		blinkenlights.sendAnimation(animation);
	    }
	}
    }
    oReq.send(null)
}


function initPixelArtImage(img) {
    img.crossOrigin = "Anonymous";
    img.onclick = function() {
	sendGifAnimation(img);
	currentImage = img;
    }
}

function gammaValueFromSlider(v) {
    return MIN_GAMMA + v*(MAX_GAMMA-MIN_GAMMA)/100.0;
}

function sliderValueFromGamma(g) {
    return (g-MIN_GAMMA)*100.0/(MAX_GAMMA-MIN_GAMMA);
}

function updateSliderDisplay(slider, display) {
    gammaDisplay.innerHTML = gammaValueFromSlider(gammaSlider.value).toFixed(2);
}

function updateGamma() {
    var gamma = gammaValueFromSlider(gammaSlider.value);
    let i = 0;
    gammaTable = Array.from(Array(256), () => Math.round(255*((i++/255.0)**gamma)));
    if (currentImage) {
	sendGifAnimation(currentImage);
    }
}

function initGamma(g) {
    gammaSlider.value = sliderValueFromGamma(g);
    gammaSlider.oninput = function () {
	updateSliderDisplay(gammaSlider, gammaDisplay);
    };
    gammaSlider.oninput();
    gammaSlider.onchange = updateGamma;
    updateGamma();
}

function toggleUIConnected(connected) {
    let lbl = 'Connect';
    if (connected) {
	lbl = 'Disconnect';
    }
    butConnect.textContent = lbl;
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

function* fillRowIterator(color) {
    for (var c = 0; c < COLS; c++) yield color;
}

function* bitmapRowIterator(row, fg, bg) {
    for (var i = 0; i < row.length; i++) yield row[i] ? fg : bg;
}

function* digitRowIterator(row, h10, h1, m10, m1, fg, bg, colon) {
    yield* bitmapRowIterator(Digits[h10][row], fg, bg);
    yield bg;
    yield* bitmapRowIterator(Digits[h1][row], fg, bg);
    var center = row % 2 ? colon : bg;
    yield center;
    yield center;
    yield* bitmapRowIterator(Digits[m10][row], fg, bg);
    yield bg;
    yield* bitmapRowIterator(Digits[m1][row], fg, bg);
}

function* digitalClockIterator(h10, h1, m10, m1, fg, bg, colon) {
    for (var r = 0; r < CLOCK_VERTICAL_OFFSET; r++) {
	yield fillRowIterator(bg);
    }
    for (var r = 0; r < DIGIT_ROWS; r++) {
	yield digitRowIterator(r, h10, h1, m10, m1, fg, bg, colon);
    }
    for (var r = 0; r < ROWS-(CLOCK_VERTICAL_OFFSET+DIGIT_ROWS); r++) {
	yield fillRowIterator(bg);
    }
}


function drawClockMinute(fg, bg, colon) {
    var t = new Date();
    var hours = t.getHours();
    var minutes = t.getMinutes();
    var animation = new Animation(60000);
    animation.addFrame(new Frame(500, digitalClockIterator(
	  Math.floor(hours/10), hours%10,Math.floor(minutes/10),minutes%10, fg, bg, colon)));
    animation.addFrame(new Frame(500, digitalClockIterator(
	  Math.floor(hours/10), hours%10,Math.floor(minutes/10),minutes%10, fg, bg, bg)));
    blinkenlights.sendAnimation(animation);
}

Dropzone.options.gifDropzone = {
    autoProcessQueue: false,
    autoQueue: false,
    createImageThumbnails: false,
    accept: function (file, done) {
	var fr = new FileReader();
        fr.onload = function () {
	    var img = document.createElement('img');
            img.src = fr.result;

	    var oReq = new XMLHttpRequest();
	    oReq.open('GET', img.src, true);
	    oReq.responseType = 'arraybuffer';
	    oReq.onload = function(oEvent) {
		var arrayBuffer = oReq.response;
		if (arrayBuffer) {
		    var gif = parseGIF(arrayBuffer);
		    if (gif.lsd.width != ROWS || gif.lsd.height != COLS) {
			console.log(file.name + ": only " + ROWS + "x" + COLS + " GIF supported");
			return;
		    }
		    img.className = "pixelArt";
		    initPixelArtImage(img);
		    pixelArtContainer.appendChild(img);
		}
	    }
	    oReq.send(null)
        }
        fr.readAsDataURL(file);
	Dropzone.forElement('#gif-dropzone').removeFile(file);
	done();
    },
    init: function () {
	console.log("Dropzone init!")
    }
}

document.addEventListener('DOMContentLoaded', () => {
    blinkenlights = new Blinkenlights();
    butConnect.addEventListener('click', clickConnect);
    butSend.addEventListener('click', clickSend);

    const notSupported = document.getElementById('notSupported');
    notSupported.classList.toggle('hidden', 'serial' in navigator);
    document.querySelectorAll('img.pixelArt').forEach(initPixelArtImage);
    initGamma(DEFAULT_GAMMA);
    debugButton.onclick = function() { blinkenlights.sendCommands('DBG'); };
    clockButton.onclick = function() { drawClockMinute([0xff, 0xff, 0xff], [0x00, 0x00, 0x00], [0x00, 0x00, 0xff]); };
});
