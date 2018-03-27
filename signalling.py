import os
import sys
import logging
import websockets
import asyncio
import argparse

parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument('--addr', default='0.0.0.0', help='Address to listen on')
parser.add_argument('--port', default=8443, type=int, help='Port to listen on')
parser.add_argument('--keepalive-timeout', dest='keepalive_timeout', default=30, type=int,
                    help='Timeout for keepalive ping (in seconds)')

options = parser.parse_args(sys.argv[1:])

ADDR_PORT = (options.addr, options.port)
KEEPALIVE_TIMEOUT = options.keepalive_timeout

peers    = dict()
sessions = dict()

async def recv_msg_ping(ws, raddr):
    '''
    Wait for a message forever, and send a regular ping to prevent bad routers
    from closing the connection.
    '''
    msg = None
    while msg is None:
        try:
            msg = await asyncio.wait_for(ws.recv(), KEEPALIVE_TIMEOUT)
        except:
            print('Sending keepalive ping to {!r} in recv'.format(raddr))
            await ws.ping()
    return msg

async def cleanup_session(uid):
    if uid in sessions:
        other_id = sessions[uid]
        del sessions[uid]
        print("Cleaned up {} session".format(uid))
        if other_id in sessions:
            del sessions[other_id]
            print("Also cleaned up {} session".format(other_id))
            # If there was a session with this peer, also close the connection to reset its state
            if other_id in peers:
                print("Closing connection to {}".format(other_id))
                wso, oaddr, _ = peers[other_id]
                del peers[other_id]
                await wso.close()

async def remove_peer(uid):
    await cleanup_session(uid)
    if uid in peers:
        ws, raddr, status = peers[uid]
        del peers[uid]
        await ws.close()
        print("Disconnected from peer {!r} at {!r}".format(uid, raddr))

async def connection_handler(ws, uid):
    global peers, sessions
    raddr = ws.remote_address
    peer_status = None
    peers[uid] = [ws, raddr, peer_status]
    print("Registered peer {!r} at {!r}".format(uid, raddr))
    while True:
        # Receive command, wait forever if necessary
        msg = await recv_msg_ping(ws, raddr)
        # Update current status
        peer_status = peers[uid][2]
        # We are in a session, messages must be relayed
        if peer_status is not None:
            # We're in a session, route message to connected peer
            if peer_status == 'session':
                other_id = sessions[uid]
                wso, oaddr, status = peers[other_id]
                assert(status == 'session')
                print("uid {} -> uid {}: {}".format(uid, other_id, msg))
                await wso.send(msg)
            else:
                raise AssertionError('Unknown peer status {!r}'.format(peer_status))
        elif msg.startswith('SESSION'):
            print("{!r} command {!r}".format(uid, msg))
            _, callee_id = msg.split(maxsplit=1)
            if callee_id not in peers:
                await ws.send('ERROR peer {!r} not found'.format(callee_id))
                continue
            if peer_status is not None:
                await ws.send('ERROR peer {!r} busy'.format(callee_id))
                continue
            await ws.send('SESSION_OK')
            wsc = peers[callee_id][0]
            print('Session from {!r} ({!r}) to {!r} ({!r})'.format(uid, raddr, callee_id, wsc.remote_address))
            # Register session
            peers[uid][2] = peer_status = 'session'
            sessions[uid] = callee_id
            peers[callee_id][2] = 'session'
            sessions[callee_id] = uid
        else:
            print('Ignoring unknown message {!r} from {!r}'.format(msg,uid))

async def hello_peer(ws):
    '''
    Exchange hello, register peer
    '''
    print("hello peer called!")
    raddr = ws.remote_address
    hello = await ws.recv()
    # uid is a random integer
    hello,uid = hello.split(maxsplit=1)
    if hello != 'HELLO':
        await ws.close(code=1002, reason='invalid protocol')
        raise Exception("Invalid hello from {!r}".format(raddr))

    if not uid or uid in peers or uid.split() != [uid]:
        await ws.close(code=1002, reason='invalid peer uid')
        raise Exception("Invalid uid {!r} from {!r}".format(uid, raddr))

    # send hello back
    await ws.send('HELLO')
    print("hello msg: {}, uid: {}".format(hello, uid))
    return uid

async def handler(ws, path):
    raddr = ws.remote_address
    print("Connected to {!r}".format(raddr))
    peer_id = await hello_peer(ws)
    try:
        await connection_handler(ws, peer_id)
    except websockets.ConnectionClosed:
        print("Connection to peer {!r} closed, exiting handler".format(raddr))
    finally:
        await remove_peer(peer_id)

print("Listening on http://{}:{}".format(*ADDR_PORT))
wsd = websockets.serve(handler, *ADDR_PORT, max_queue=16)

logger = logging.getLogger('websockets.server')

logger.setLevel(logging.ERROR)

asyncio.get_event_loop().run_until_complete(wsd)
asyncio.get_event_loop().run_forever()
