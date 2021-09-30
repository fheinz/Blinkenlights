#!/usr/bin/python3

from random import choice
from time import sleep
import  argparse
import sys

from blinken import Blinken

FIGURES = [
        [[1], [1], [1], [1]],
        [[2, 2], [2, 2]],
        [[0, 3, 0], [3, 3, 3]],
        [[4, 0, 0], [4, 4, 4]],
        [[0, 0, 5], [5, 5, 5]],
        [[6, 0], [6, 6], [0, 6]],
        [[0, 7], [7, 7], [7, 0]]
        ]

#BOARD = '007700'
#FIGURE = '770000'
#EMPTY = '000000'

OCCUPIED_CACHE = {}

COLORS = {
        0: '000000',
        1: '007070',
        2: '707000',
        3: '600070',
        4: '000070',
        5: '706000',
        6: '007000',
        7: '700000'
        }


def init_board():
    res = []
    for i in range(16):
        res.append([])
        for j in range(16):
            res[-1].append(0)
    return res


def rotations(figure):
    # Returns all rotations of a tetris figure
    yield figure # the figure itself
    yield list(map(lambda x: x[::-1], figure))[::-1] # Rotate upside down
    x = len(figure)
    y = len(figure[0])
    left = []
    right = []
    for i in range(y):
        curr_l = []
        curr_r = []
        for j in range(x):
            curr_l.append(figure[j][y-i-1])
            curr_r.append(figure[x-j-1][i])
        left.append(curr_l)
        right.append(curr_r)
    yield left
    yield right


def touches_stuff(board, figure, x, y):
    for i in range(0, len(figure)):
        for j in range(0, len(figure[0])):
            if i+x > 15 or j+y > 15:
                continue
                # Should not normally get here; it means that
                # the figure has reached the bottom
            if i+x == 15:
                return True # We've hit the bottom!
            if not figure[i][j]:
                continue
            if board[i+x+1][j+y]:
                #print('HERE!', i, j, i+x+1, j+y)
                return True # The figure has touched the remaining blocks
    return False # The figure has not touched anything yet


def get_lowest_x(board, figure, x, y):
    low_x = x
    while not touches_stuff(board, figure, low_x, y):
        low_x += 1
    return low_x


def occupied(board, figure, x, y, pos_x, pos_y):
    if board[pos_x][pos_y]:
        return True
    if figure and (pos_x >= x and pos_x < len(figure) + x
            and pos_y >= y and pos_y < len(figure[0]) + y
            and figure[pos_x - x][pos_y - y]):
        return True
    return False


def rows(board, figure, x, y):
    low_x = x
    rows = 0
    for i in range(16):
        curr = 0
        for j in range(16):
            if occupied(board, figure, low_x, y, i, j):
                curr += 1
        if curr == 16:
            rows += 1
    return rows


def holes(board, figure, x, y):
    low_x = x
    holes = 0.0
    for i in range(1, 16):
        for j in range(16):
            if not occupied(board, figure, low_x, y, i, j) and occupied(board, figure, low_x, y, i-1, j):
                holes += 1.0
                for k in range(i-1):
                    if occupied(board, figure, low_x, y, i-1-k, j):
                        holes += 0.5
                    else:
                        break
    return holes


def height(board, figure, x, y):
    low_x = x
    max_h = 0
    for i in range(16):
        if max_h:
            continue
        for j in range(16):
            if occupied(board, figure, low_x, y, i, j):
                max_h = 16 - i
                break
    res = max_h * 5
    for j in range(16 - max_h, 16):
        for i in range(16):
            if occupied(board, figure, x, y, i, j):
                res += 16 - i
    return res


def get_longest_row(board, figure, x, y):
    low_x = x
    max_row = 0
    for i in range(16):
        curr = 0
        for j in range(16):
            if occupied(board, figure, low_x, y, i, j):
                curr += 1
        if curr > max_row:
            max_row = curr
    return max_row


def eval_position(board, figure, x, y):
    res = 0.0
    low_x = get_lowest_x(board, figure, x, y)
    res += rows(board, figure, low_x, y) * 1000
    res -= holes(board, figure, low_x, y) * 900
    res -= height(board, figure, low_x, y) * 100
    res += get_longest_row(board, figure, low_x, y) * 10
    return res


def spawn_new_figure(board, figure, curr_depth = 0):
    # In order to figure out the optimal position and
    # rotation of the figure, the following logic is applied:
    # all lateral positions of every rotations are checked;
    # 1. If any of them lead to row completion, the move that
    # completes the most rows is selected.
    # 2. If neither move completes any rows, the moves that create
    # the least amount of holes are selected
    # 3. Out of those moves, the one that leads to the least
    # height is preferred.
    max_val = -999999999999
    all_are_touching = True
    preferred_position = (None, -1) # (figure, y)
    for rot in rotations(figure):
        for pos in range(16 - len(rot[0]) + 1):
            if touches_stuff(board, rot, 0, pos):
                continue
            all_are_touching = False
            curr_val = eval_position(board, rot, 0, pos)
            if curr_val > max_val:
                max_val = curr_val
                preferred_position = (rot, pos)
    if all_are_touching:
        return (None, -1) # We've lost: there is no way to spawn this figure
                    # without touching existing blocks
    #print(f'INFO: rows: {max_rows}, holes: {min_holes}, height: {min_height}, row: {longest_row}')
    return preferred_position


def print_board(board, figure, x, y, bl):
    bl.command('NXT')
    bl.command('ANM 1000')
    bl.command('FRM 1000')
    cmds = []
    for i in range(16):
        curr = 'RGB '
        for j in range(16):
            if board[i][j]:
                curr += COLORS[board[i][j]]
            elif figure and (i >= x and i < x + len(figure)) and (j >= y and j < y + len(figure[0])) and figure[i - x][j - y]:
                curr += COLORS[figure[i-x][j-y]]
            else:
                curr += COLORS[0]
        cmds.append(curr)
        #print(curr)
    bl.commands(cmds)
    bl.command('DON')
    #bl.command('NXT')

def fix_figure(board, figure, x, y):
    new_board = []
    for i in range(16):
        curr = []
        zeros = False
        for j in range(16):
            if occupied(board, figure, x, y, i, j):
                if i >= x and i < x + len(figure) and j >= y and j < y + len(figure[0]) and figure[i - x][j - y]:
                    curr.append(figure[i - x][j - y])
                else:
                    curr.append(board[i][j])
            else:
                curr.append(0)
                zeros = True
        if zeros:
            new_board.append(curr)
    for i in range(16 - len(new_board)):
        new_board.insert(0, [0 for j in range(16)])
    return new_board


def init_blink(device):
    bl = Blinken(dev=device)
    bl.command('VER')
    bl.command('RST')
    return bl


def parse_args(args):
    parser = argparse.ArgumentParser(
        description="Watch a tetris game")
    parser.add_argument('-d', '--device', type=str, default="/dev/ttyUSB0",
            help="Use DEVICE  to talk to board. (default: %(default)s)")
    return parser.parse_args(args)


def tetris():
    args = parse_args(sys.argv[1:])
    bl = init_blink(args.device)
    board = init_board()
    figure = None
    x = 0
    y = 0
    while True:
        if not figure:
            x = 0
            (figure, y) = spawn_new_figure(board, choice(FIGURES))
            if not figure:
                break
        else:
            x += 1
            if touches_stuff(board, figure, x, y):
                board = fix_figure(board, figure, x, y)
                figure = None
        print_board(board, figure, x, y, bl)
        print('='*20)


if __name__ == '__main__':
    tetris()
