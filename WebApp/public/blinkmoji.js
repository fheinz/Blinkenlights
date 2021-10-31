"use strict";

const { showGif, writeToStream, connect, disconnect } = require("./blinken");
import { EmojiButton } from "@joeattardi/emoji-button";

const DEFAULT_GAMMA = 2.0;
let i = 0;
let gammaTable = Array.from(Array(256), () =>
    Math.round(255 * (i++ / 255.0) ** DEFAULT_GAMMA)
);
let socket = io();
let port;
let room;
let currentImage;

socket.on("error", (error) => {
    console.log("error :" + error);
    window.location = "../blinkmoji.html";
});

socket.on("show gif", (gifSrc) => {
    console.log("Showing gif: " + gifSrc);
    showGif(gifSrc, gammaTable);
});

socket.on("users list", (users) => {
    document.getElementById("roomTitle").innerHTML =
        "Users in room <i>" + room + "</i>";
    let usersListElement = document.getElementById("usersList");
    while (usersListElement.firstChild) {
        usersListElement.removeChild(usersListElement.firstChild);
    }
    for (const user of users) {
        const liElement = document.createElement("li");
        liElement.classList.add("textSmall");
        liElement.innerHTML = user.name;
        usersListElement.appendChild(liElement);
    }
});

const picker = new EmojiButton({
    theme: "dark",
});
const trigger = document.getElementById("emoji-trigger");
picker.on("emoji", (selection) => {
    trigger.innerHTML = selection.emoji;
    socket.emit("chat message", selection.emoji);
});
trigger.addEventListener("click", () => picker.togglePicker(trigger));

/**
 * @name clickConnect
 * Click handler for the connect/disconnect button.
 */
async function clickConnect() {
    if (port) {
        await disconnect();
        toggleUIConnected(false);
        port = null;
        return;
    }
    port = await connect();
    toggleUIConnected(true);
    if (currentImage) sendGifAnimation(currentImage, gammaTable);
}

function toggleUIConnected(connected) {
    let lbl = "Connect";
    if (connected) {
        lbl = "Disconnect";
    }
    buttonConnect.textContent = lbl;
}

function clear() {
    writeToStream("RST");
}

document.addEventListener("DOMContentLoaded", () => {
    buttonConnect.addEventListener("click", clickConnect);
    buttonClear.addEventListener("click", clear);
    console.log("dom loaded");
    let params = new URLSearchParams(location.search.substring(1));
    room = params.get("room");
    socket.emit("join room", params.get("username"), room);
});
