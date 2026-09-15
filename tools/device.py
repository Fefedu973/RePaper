"""Development SSH transport. Password is read from env/getpass, never persisted.

First contact is pinned using the fingerprint observed during the authorized probe.
This is a workstation deployment tool, never a Paper Bridge API.
"""
import argparse
import getpass
import os
from pathlib import Path
import sys
import paramiko

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--fingerprint', required=True)
    sub = parser.add_subparsers(dest='action', required=True)
    ex = sub.add_parser('exec')
    ex.add_argument('command')
    put = sub.add_parser('put')
    put.add_argument('local'); put.add_argument('remote')
    get = sub.add_parser('get')
    get.add_argument('remote'); get.add_argument('local')
    args = parser.parse_args()
    class PinnedKey(paramiko.MissingHostKeyPolicy):
        def missing_host_key(self, client, hostname, key):
            if key.fingerprint != args.fingerprint:
                raise paramiko.SSHException('Host key differs from pinned fingerprint')
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(PinnedKey())
    password = os.environ.pop('PAPER_SSH_PASSWORD', None) or getpass.getpass('SSH password: ')
    client.connect(args.host, username='root', password=password, look_for_keys=False, allow_agent=False, timeout=10)
    del password
    try:
        if args.action == 'exec':
            _, out, err = client.exec_command(args.command, timeout=120)
            sys.stdout.buffer.write(out.read()); sys.stdout.buffer.flush()
            sys.stderr.buffer.write(err.read()); sys.stderr.buffer.flush()
            return out.channel.recv_exit_status()
        with client.open_sftp() as sftp:
            if args.action == 'put': sftp.put(args.local, args.remote)
            else:
                Path(args.local).parent.mkdir(parents=True, exist_ok=True)
                sftp.get(args.remote, args.local)
    finally:
        client.close()
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
