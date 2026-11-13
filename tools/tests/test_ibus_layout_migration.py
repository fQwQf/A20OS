"""Use real GLib vendor overrides without writing a user preference database."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
SCHEMA='''<schemalist><schema id="org.freedesktop.ibus.general" path="/desktop/ibus/general/">
<key name="use-system-keyboard-layout" type="b"><default>false</default></key>
<key name="preload-engines" type="as"><default>[]</default></key>
</schema></schemalist>'''
class IbusLayoutDefaults(unittest.TestCase):
    def test_compiled_vendor_default_and_existing_preferences_untouched(self):
        source=(ROOT/'packages/overlay/xfce/sbin/init').read_text()
        start=source.index('IBUS_SCHEMA_DIR=')
        script=source[start:source.index('dbus-uuidgen --ensure',start)]
        self.assertNotIn('gsettings set',script)
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp);schema=p/'schemas';schema.mkdir()
            (schema/'org.freedesktop.ibus.gschema.xml').write_text(SCHEMA)
            override=p/'90-a20-ibus.gschema.override'
            shutil.copyfile(ROOT/'packages/overlay/xfce/usr/share/glib-2.0/schemas'/override.name,override)
            config=p/'config';(config/'dconf').mkdir(parents=True)
            user=config/'dconf/user';user.write_bytes(b'existing user preferences must remain unchanged')
            env=dict(os.environ,GSETTINGS_SCHEMA_DIR=str(schema),XDG_CONFIG_HOME=str(config))
            subprocess.run(['glib-compile-schemas',str(schema)],check=True,capture_output=True)
            def get():
                return subprocess.check_output(['gsettings','get','org.freedesktop.ibus.general',
                   'use-system-keyboard-layout'],env=dict(env,GSETTINGS_BACKEND='memory'),text=True).strip()
            self.assertEqual(get(),'false')
            script=script.replace('/usr/share/glib-2.0/schemas',str(schema)).replace('/a20/90-a20-ibus.gschema.override',str(override))
            result=subprocess.run(['/bin/sh','-c',script],env=env,check=True,capture_output=True,text=True)
            self.assertIn('IBus compositor layout default verified',result.stdout)
            self.assertEqual(get(),'true')
            installed=schema/override.name;stamp=installed.stat().st_mtime_ns
            subprocess.run(['/bin/sh','-c',script],env=env,check=True,capture_output=True)
            self.assertEqual(stamp,installed.stat().st_mtime_ns)
            self.assertEqual(user.read_bytes(),b'existing user preferences must remain unchanged')
if __name__=='__main__':unittest.main()
