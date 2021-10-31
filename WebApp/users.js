const users = [];

const addUser = (id, name, room) => {
  if (!name) {
    return { error: "You must specify username" };
  }
  if (!room) {
    return { error: "You must specify room name" };
  }

  const user = { id, name, room };
  users.push(user);

  return { user };
};

const findUser = (id) => {
  return users.find((user) => user.id === id);
};

const deleteUser = (id) => {
  let idx = users.findIndex((user) => user.id === id);
  if (idx == -1) {
    return;
  }
  users.splice(idx, 1);
};

const getUsers = (room) => {
  return users.filter((user) => user.room === room);
};

module.exports = { addUser, findUser, deleteUser, getUsers };
