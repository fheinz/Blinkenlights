#!/usr/bin/python3

from copy import deepcopy
from random import randint, seed
import argparse
import sys

from blinken import Blinken
from time import sleep

LIVE = '00cc00'
DEAD = '000000'


def empty_board():
    res = []
    for i in range(16):
        res.append([])
        for j in range(16):
            res[-1].append(0)
    return res


def wrap(n):
    if n < 0:
        return 15
    if n == 16:
        return 0
    return n


def get_neighbours(board, x, y):
    res = 0
    for i in range(x-1, x+2):
        pos_x = wrap(i)
        for j in range(y-1, y+2):
            pos_y = wrap(j)
            if pos_x == x and pos_y == y:
                continue
            res += board[pos_x][pos_y]
    return res


def next_turn(board):
    new_board = empty_board()
    for i in range(16):
        for j in range(16):
            neigh = get_neighbours(board, i, j)
            if neigh < 2 or neigh > 3:
                new_board[i][j] = 0
            elif neigh == 2:
                new_board[i][j] = board[i][j]
            elif neigh == 3:
                new_board[i][j] = 1
    return new_board


def generate_board():
    seed()
    board = []
    for i in range(16):
        board.append([])
        for j in range(16):
            if randint(0, 10) < 7:
                board[-1].append(0)
            else:
                board[-1].append(1)
    return board


def board_to_commands(board):
    res = []
    for i in range(16):
        curr = 'RGB '
        for j in range(16):
            if board[i][j]:
                curr += LIVE
            else:
                curr += DEAD
        res.append(curr)
    return res


def parse_args(args):
    parser = argparse.ArgumentParser(
        description="Watch a tetris game")
    parser.add_argument('-d', '--device', type=str, default="/dev/ttyUSB0",
            help="Use DEVICE  to talk to board. (default: %(default)s)")
    return parser.parse_args(args)


def main():
    args = parse_args(sys.argv[1:])
    bl = Blinken(dev=args.device)
    board = generate_board()
    bl.command('VER')
    bl.command('RST')
    for turns in range(1000):
        bl.command('ANM 600')
        bl.command('FRM 600')
        bl.commands(board_to_commands(board))
        board = next_turn(board)
        sleep(0.45)
        bl.command('DON')


if __name__ == "__main__":
    main()

