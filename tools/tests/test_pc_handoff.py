import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch
SCRIPT=Path(__file__).resolve().parents[1]/'pc-handoff.py'
spec=importlib.util.spec_from_file_location('pc_handoff',SCRIPT)
handoff=importlib.util.module_from_spec(spec);spec.loader.exec_module(handoff)
URL='https://portal.example/pair#code=12345678'
class HandoffTests(unittest.TestCase):
    def test_https_only_without_credentials_or_controls(self):
        self.assertTrue(handoff.valid_login_url(URL))
        for value in ['http://portal.example','https://user:pass@portal.example','file:///tmp/x','javascript:alert(1)',URL+'\n','https://example%0a.com',None]:
            self.assertFalse(handoff.valid_login_url(value))
    @patch.object(handoff.subprocess,'check_output',return_value='C:\\repo\\tools\\pc-handoff.ps1\n')
    @patch.object(handoff.subprocess,'run')
    def test_url_is_stdin_only(self,run,_path):
        run.return_value=subprocess.CompletedProcess([],0,b'{"ok":true}',b'')
        handoff.windows_request('open',{'url':URL})
        args,kwargs=run.call_args
        self.assertNotIn(URL,args[0]);self.assertIn(URL.encode(),kwargs['input']);self.assertNotIn('shell',kwargs)
    def test_no_callback_registration_or_clipboard_action(self):
        for action in ['register','auto-login','clipboard']:
            with self.assertRaises(ValueError):handoff.windows_request(action,{'url':URL})
if __name__=='__main__':unittest.main()
