"use strict";

import "regenerator-runtime/runtime";
import { decompressFrames, parseGIF } from "gifuct-js";

let port;
let reader;
let inputDone;
let outputDone;
let inputStream;
let outputStream;

const COLS = 16;
const ROWS = COLS;

const MIN_GAMMA = 0.5;
const MAX_GAMMA = 3.0;

/**
 * @name paddedHex
 * Return the hex string representation of `number`, padded to `width` places.
 */
function paddedHex(number, width) {
    return number.toString(16).padStart(width, "0").toUpperCase();
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
            port = await navigator.serial.requestPort();
        }
        await port.open({ baudRate: 115200 });
    } catch (e) {
        return;
    }

    const encoder = new TextEncoderStream();
    outputDone = encoder.readable.pipeTo(port.writable);
    outputStream = encoder.writable;
    writeToStream("", "RST", "VER", "PWR");

    let decoder = new TextDecoderStream();
    inputDone = port.readable.pipeTo(decoder.writable);
    inputStream = decoder.readable.pipeThrough(
        new TransformStream(new LineBreakTransformer())
    );
    reader = inputStream.getReader();
    readLoop();
    return port;
}

/**
 * @name disconnect
 * Closes the Web Serial connection.
 */
async function disconnect() {
    writeToStream("RST");
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
 * @name readLoop
 * Reads data from the input stream and displays it on screen.
 */
async function readLoop() {
    while (true) {
        const { value, done } = await reader.read();
        if (value) {
            console.log("[RECV]" + value + "\n");
        }
        if (done) {
            console.log("[readLoop] DONE", done);
            reader.releaseLock();
            break;
        }
    }
}

class Frame {
    constructor(ms, pixels, gammaTable) {
        this.duration = ms;
        this.rows = Array.from(pixels, (row) =>
            Array.from(row, ([r, g, b]) =>
                paddedHex(
                    (gammaTable[r] << 16) |
                        (gammaTable[g] << 8) |
                        gammaTable[b],
                    6
                )
            ).join("")
        );
    }
}

class Animation {
    constructor(ms) {
        this.duration = ms;
        this.frames = [];
    }

    addFrame(f) {
        this.frames.push(f);
    }
}

function sendAnimation(anim) {
    writeToStream("ANM " + anim.duration);
    anim.frames.forEach(function (frame) {
        writeToStream("FRM " + frame.duration);
        frame.rows.forEach((row) => writeToStream("RGB " + row));
    });
    writeToStream("DON", "NXT");
}

/**
 * @name sendGifAnimation
 * Sends a GIF animation to the display
 */
function sendGifAnimation(img, gammaTable) {
    var oReq = new XMLHttpRequest();
    oReq.open("GET", img.src, true);
    oReq.responseType = "arraybuffer";

    oReq.onload = function (oEvent) {
        var arrayBuffer = oReq.response;
        if (arrayBuffer) {
            var gif = parseGIF(arrayBuffer);
            if (gif.lsd.width != ROWS || gif.lsd.height != COLS) return;
            var frames = decompressFrames(gif, true);
            if (frames) {
                var animation = new Animation(600000);
                var pixels = Array(ROWS)
                    .fill()
                    .map(() => Array(COLS));
                frames.forEach(function (gifFrame) {
                    pixels.forEach((row) => row.fill(0));
                    var bitmap = gifFrame.patch;
                    var row_offset = gifFrame.dims.top;
                    var col_offset = gifFrame.dims.left;
                    for (var r = 0; r < gifFrame.dims.height; r++) {
                        for (var c = 0; c < gifFrame.dims.width; c++) {
                            var offset = (r * gifFrame.dims.width + c) * 4;
                            pixels[r + row_offset][c + col_offset] = [
                                bitmap[offset],
                                bitmap[offset + 1],
                                bitmap[offset + 2],
                            ];
                        }
                    }
                    animation.addFrame(
                        new Frame(
                            "delay" in gifFrame ? gifFrame.delay : 1000,
                            pixels,
                            gammaTable
                        )
                    );
                });
                sendAnimation(animation);
            }
        }
    };
    oReq.send(null);
}

function showGif(fileSrc, gammaTable) {
    var img = document.createElement("img");
    img.src = fileSrc;
    var oReq = new XMLHttpRequest();
    oReq.open("GET", fileSrc, true);
    oReq.responseType = "arraybuffer";
    oReq.onload = function (oEvent) {
        var arrayBuffer = oReq.response;
        if (arrayBuffer) {
            var gif = parseGIF(arrayBuffer);
            console.log(gif);
            if (gif.lsd.width != ROWS || gif.lsd.height != COLS) {
                console.log(
                    fileSrc + ": only " + ROWS + "x" + COLS + " GIF supported"
                );
                return;
            }
            img.className = "pixelArt";
            if (port) sendGifAnimation(img, gammaTable);
        }
    };
    oReq.send(null);
}

/**
 * @name writeToStream
 * Gets a writer from the output stream and send the lines to the micro:bit.
 * @param  {...string} lines lines to send to the micro:bit
 */
function writeToStream(...lines) {
    const writer = outputStream.getWriter();
    lines.forEach((line) => {
        console.log("[SEND]", line);
        writer.write(line + "\n");
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
        this.container = "";
    }

    transform(chunk, controller) {
        this.container += chunk;
        const lines = this.container.split("\n");
        this.container = lines.pop();
        lines.forEach((line) => controller.enqueue(line));
    }

    flush(controller) {
        controller.enqueue(this.container);
    }
}

module.exports = {
    writeToStream,
    sendGifAnimation,
    connect,
    disconnect,
    showGif,
};
