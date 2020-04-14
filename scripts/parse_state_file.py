#!/usr/bin/env python3

import argparse
import binascii
import struct

def get_state_height(file_handler):
    res = file_handler.read(4)
    res = struct.unpack('I', res)[0]
    return res

def get_block_hash(file_handler):
    res = file_handler.read(32)[::-1]
    res = binascii.hexlify(res).decode('utf-8')
    return res

def get_num_chunks(file_handler):
    res = file_handler.read(4)
    res = struct.unpack('I', res)[0]
    return res

if __name__ == '__main__':

    argparser = argparse.ArgumentParser()
    argparser.add_argument('filename', type=str, help='Name of the state file to load')
    args = argparser.parse_args()

    with open(args.filename, 'rb') as f:
        state_height = get_state_height(f)
        state_latest_block_hash = get_block_hash(f)
        state_num_chunks = get_num_chunks(f)

    print('State file name: {}'.format(args.filename))
    print('')
    print('State block height: {}'.format(state_height))
    print('Latest block hash: {}'.format(state_latest_block_hash))
    print('Number chunks: {}'.format(state_num_chunks))
