#!/usr/bin/env python3

import argparse
import binascii
import struct
import hashlib
from math import floor

def get_chunk_hash(file_handler):
    res = file_handler.read()
    res = hashlib.sha256(res).digest()
    res = hashlib.sha256(res).digest()
    res = binascii.hexlify(res).decode('utf-8')
    return res

def get_chunk_height(file_handler):
    res = file_handler.read(4)
    res = struct.unpack('<I', res)[0]
    return res

def get_chunk_offset(file_handler):
    res = file_handler.read(4)
    res = struct.unpack('<I', res)[0]
    return res

def get_num_utxos(file_handler):
    res = file_handler.read(1)
    res = struct.unpack('<B', res)[0]

    if res < 253:
        pass
    elif res == 253:
        res = file_handler.read(2)
        res = struct.unpack('<H', res)[0]
    elif res == 254:
        res = file_handler.read(4)
        res = struct.unpack('<I', res)[0]
    else:
        res = file_handler.read(8)
        res = struct.unpack('<Q', res)[0]
    return res

def parse_outpoint(file_handler):
    res_hash = file_handler.read(32)[::-1]
    res_hash = binascii.hexlify(res_hash)
    res_n = file_handler.read(4)
    res_n = struct.unpack('<I', res_n)[0]
    return res_hash, res_n

def parse_varint(file_handler):
    n = 0
    while True:
        b = file_handler.read(1)
        b = struct.unpack('B', b)[0]
        n = (n << 7) | (b & 0x7F)
        if (b & 0x80):
            n += 1
        else:
            return n

def parse_compressed_txout(file_handler):
    def decompress_value(x):  # See bitcoin core compressor.cpp:169
        if x == 0:
            return 0
        x -= 1
        e = x % 10
        x = int(floor(x / 10))
        n = 0
        if (e < 9):
            d = (x % 9) + 1
            x = int(floor(x / 9))
            n = x * 10 + d
        else:
            n = x + 1
        while e > 0:
            n *= 10
            e -= 1
        return n

    def decompress_script(file_handler):  # see bitcoin core compressor.h:63
        special_scripts = 6
        size = parse_varint(file_handler)
        if size < special_scripts:
            case = size
            size = 20 if size in [0, 1] else 32  # see bitcoin core compressor.cpp:87
            script_payload = file_handler.read(size)
            # see bitcoin core compressor.cpp:96
            if case == 0:  # P2PKH
                script = b'\x76\xa9' + bytes([20]) + script_payload + b'\x9d\xac'
            elif case == 1:  # P2SH
                script = b'\xa9' + bytes([20]) + script_payload + b'\x87'
            elif case == 2 or case == 3:  # P2PK compressed pubkey
                script = bytes([33]) + bytes([case]) + script_payload + b'\xac'
            elif case == 4 or case == 5:  # P2PK uncompressed pubkey
                script_payload = '\x00' * 65  # Now this is really too irrelevant for our application
                script = bytes([65]) + script_payload + b'\xac'

            return script
                
        size -= special_scripts
        script = file_handler.read(size)

        return script

    value = decompress_value(parse_varint(file_handler))
    script = binascii.hexlify(decompress_script(file_handler))
    return (script, value)

def parse_coin(file_handler):
    code = parse_varint(file_handler)
    block_height = code >> 1
    is_coinbase = code & 1
    txout = parse_compressed_txout(file_handler)

    return (block_height, txout, is_coinbase)
    

if __name__ == '__main__':

    argparser = argparse.ArgumentParser()
    argparser.add_argument('filename', type=str, help='Name of the state chunk file to load')
    args = argparser.parse_args()

    with open(args.filename, 'rb') as f:
        chunk_hash = get_chunk_hash(f)

    utxos = list()
    with open(args.filename, 'rb') as f:
        chunk_height = get_chunk_height(f)
        chunk_offset = get_chunk_offset(f)
        chunk_num_utxos = get_num_utxos(f)
        for i in range(chunk_num_utxos)[:10]:
            outpoint = parse_outpoint(f)
            coin = parse_coin(f)
            utxos.append((outpoint, coin))

    print('Chunk file name: {}'.format(args.filename))
    print('')
    print('State block height: {}'.format(chunk_height))
    print('Chunk offset in state: {}'.format(chunk_offset))
    print('Number UTXos in chunk: {}'.format(chunk_num_utxos))
    print('Chunk hash: {}'.format(chunk_hash))

    print('\n\nUTXOs in chunk:\n')
    ctr = 0
    for utxo in utxos:
        ctr += 1
        outpoint, coin = utxo[0], utxo[1]
        print('{:6d}: {}, {} ({}, {}, {})'.format(
            ctr,
            str(outpoint[0]),
            str(outpoint[1]),
            str(coin[0]),
            str(coin[1]),
            str(coin[2]),
        ))
