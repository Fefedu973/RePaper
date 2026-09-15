#!/usr/bin/env python3
"""Open an HTTPS page in Windows from the PC emulator. No callback/companion.

The URL travels over stdin to a fixed PowerShell helper. This utility never
registers a protocol, reads a Moodle token or accesses the clipboard.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import unquote, urlsplit

def valid_login_url(value):
    if not isinstance(value,str) or len(value)>4096 or not value.isascii():return False
    if any(ord(c)<=32 or c in '\\"\'<>`' for c in value):return False
    try:
        url=urlsplit(value)
        return (url.scheme=='https' and bool(url.hostname) and url.username is None and url.password is None
                and (url.port is None or 1<=url.port<=65535)
                and not any(ord(c)<32 or c=='\\' for c in unquote(value)))
    except (ValueError,UnicodeError):return False

def windows_request(action,payload,validate=False):
    if action!='open' or not valid_login_url(payload.get('url')):raise ValueError('Invalid request')
    script=Path(__file__).with_suffix('.ps1')
    windows_script=subprocess.check_output(['wslpath','-w',str(script)],text=True,timeout=5).strip()
    command=['powershell.exe','-NoLogo','-NoProfile','-NonInteractive','-WindowStyle','Hidden',
             '-ExecutionPolicy','Bypass','-File',windows_script,'-Action','open']
    if validate:command.append('-Validate')
    result=subprocess.run(command,input=json.dumps(payload).encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15,check=False)
    if result.returncode or len(result.stdout)>2048:raise RuntimeError('Windows browser unavailable')
    response=json.loads(result.stdout.decode('utf-8-sig'))
    if not isinstance(response,dict) or not response.get('ok'):raise RuntimeError('Windows browser unavailable')
    return {'ok':True}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=('open',));parser.add_argument('--validate',action='store_true');args=parser.parse_args()
    try:
        if not args.validate and os.environ.get('REPAPER_PC_EMULATOR')!='1':raise ValueError('PC integration disabled')
        data=sys.stdin.buffer.read(8193)
        if len(data)>8192:raise ValueError('Oversized request')
        payload=json.loads(data)
        if not isinstance(payload,dict) or not valid_login_url(payload.get('url')):raise ValueError('Invalid request')
        if not args.validate:windows_request('open',{'url':payload['url']})
        print('{"ok":true}');return 0
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):
        print('{"ok":false,"error":"browser_unavailable"}');return 1

if __name__=='__main__':raise SystemExit(main())
