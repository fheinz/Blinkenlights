const express = require("express");
const { Server } = require("socket.io");
const { exec } = require("child_process");
const { addUser, findUser, deleteUser, getUsers } = require("./users");

const app = express();
const imageBackground = "black";
const gravity = "center";
const pointsize = "10";
const extent = "16x16";
const gifName = "msg.gif";

app.enable("strict routing");

app.use(express.static("public"));
app.use(express.static("pixelart"));
app.get("/", (_, res) => {
  res.sendFile(__dirname + "/public/index.html");
});
app.get("/blinkmoji", (_, res) => {
  res.sendFile(__dirname + "/public/blinkmoji.html");
});
app.get("/blinkmoji/room", (_, res) => {
  res.sendFile(__dirname + "/public/blinkmoji-room.html");
});

const server = app.listen(3000, () => {
  console.log("Server is up");
});

const io = new Server(server);

io.on("connection", (socket) => {
  socket.on("join room", (username, room) => {
    const { user, error } = addUser(socket.id, username, room);

    if (error) {
      console.log("Error while joining the room: " + error);
      socket.emit("error", error);
      return;
    }

    console.log(
      "User: " +
        user.name +
        " joined room: " +
        user.room +
        ". Users in room: " +
        getUsers(user.room).map((user) => user.name)
    );
    socket.join(user.room);
    // console.log("Users list to room: " + user.roo)
    io.to(user.room).emit("users list", getUsers(user.room));
  });

  socket.on("chat message", (msg) => {
    console.log("Received message: " + msg);
    const user = findUser(socket.id);
    printEmoji(String.fromCodePoint(msg.codePointAt(0)), user.room);
  });

  socket.on("disconnect", () => {
    const user = findUser(socket.id);
    deleteUser(socket.id);
    if (user) {
      console.log("user: " + user.name + " disconnected");
      io.to(user.room).emit("users list", getUsers(user.room));
    }
  });
});

function printEmoji(emojiChar, room) {
  exec(
    `convert -extent ${extent} -pointsize ${pointsize} -gravity ${gravity} -background ${imageBackground} -fill "#fff"  pango:'<span font="Noto Color Emoji">${emojiChar}</span>' public/${gifName}`,
    async (err, _, __) => {
      if (err) {
        console.log(err);
        return;
      }
      io.to(room).emit("show gif", `/${gifName}`);
    }
  );
}

// Prints a message character by character in short time intervals.
// It should work, so simply uncomment and use.
// function printChar(msg, currIdx) {
//   if (currIdx >= msg.length) {
//     return;
//   }
//   const currentChar = String.fromCodePoint(msg.codePointAt(currIdx));
//   const regexEmoji = /\p{Emoji_Presentation}/u;
//   console.log(
//     "Is " +
//       currentChar +
//       " an emoji: " +
//       regexEmoji.test(currentChar) +
//       " length: " +
//       currentChar.length
//   );
//   if (currentChar != " ") {
//     const cat = exec(
//       `convert -extent ${extent} -pointsize ${pointsize} -gravity ${gravity} -background ${imageBackground} -fill "#fff"  pango:"${currentChar}" public/${gifName}`,
//       async (err, stdout, stderr) => {
//         if (err) {
//           console.log(err);
//           return;
//         }
//         io.emit("show gif", `${gifName}`);
//         shortSleep();
//         printChar(msg, currIdx + currentChar.length);
//       }
//     );
//   } else {
//     printChar(msg, currIdx + 1);
//   }
// }

// async function shortSleep() {
//   await sleep(200);
// }

// function sleep(ms) {
//   return new Promise((resolve) => {
//     setTimeout(resolve, ms);
//   });
// }
