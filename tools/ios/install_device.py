#!/usr/bin/env python3
"""Signs the iOS build with your own Apple ID and installs it on your iPad.

    python3 -I tools/ios/install_device.py [--device "<name>"] [--team ID] [--game]

After macnativegames' windows_games/ra2 scripts/ios/install_device.py.
A free (Personal Team) Apple ID signs for 7 days, a paid membership for a
year: run this again to renew; the app's Documents (saves, settings, the
game) are kept. Xcode needs your Apple ID with an Apple Development
certificate (Xcode > Settings > Apple Accounts > Manage Certificates), the
iPad must be paired with this Mac with Developer Mode on, and the first
time, the developer trusted on the iPad (Settings > General > VPN & Device
Management).

The provisioning profile comes from a throwaway Xcode project with the same
bundle identifier (xcodebuild -allowProvisioningUpdates registers the device
and the App ID); the app is then re-signed with it.

--game copies the game (game/, 1.2 GB) into the app's Documents/Game, once:
the app itself does not carry it (tools/ios/build_app.sh device), so a new
build installs in seconds.
"""
import argparse
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = Path.home() / 'Library/Caches/h4port/ios-signing'


def run(cmd, **kw):
    return subprocess.run([str(x) for x in cmd], check=True, **kw)


def out(cmd):
    return subprocess.run([str(x) for x in cmd], check=True, capture_output=True, text=True).stdout


def teams():
    pems = out(['security', 'find-certificate', '-a', '-c', 'Apple Development', '-p'])
    found = set()
    for pem in re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', pems, re.S):
        subject = subprocess.run(['openssl', 'x509', '-noout', '-subject'], input=pem,
                                 capture_output=True, text=True).stdout
        if m := re.search(r'OU\s*=\s*([A-Z0-9]{10})', subject):
            found.add(m.group(1))
    return sorted(found)


def devicectl_json(args):
    with tempfile.NamedTemporaryFile(suffix='.json') as f:
        run(['xcrun', 'devicectl', *args, '--json-output', f.name], capture_output=True)
        return json.load(open(f.name))


def pick_device(name):
    devs = [d for d in devicectl_json(['list', 'devices'])['result']['devices']
            if d.get('hardwareProperties', {}).get('platform') == 'iOS'
            and d.get('hardwareProperties', {}).get('reality') == 'physical']
    match = [d for d in devs if name in (d['deviceProperties'].get('name'), d['identifier'],
                                         d['hardwareProperties'].get('udid'))] if name else devs
    if len(match) != 1:
        listed = '\n'.join(f"  {d['deviceProperties'].get('name')} "
                           f"({d['hardwareProperties'].get('marketingName')})" for d in devs)
        sys.exit(f'choose the device with --device <name>:\n{listed}' if devs else
                 'no paired iPad found: connect it, trust this Mac, and turn on Developer Mode')
    return match[0]


def make_profile(team, bundle_id, udid):
    """An empty app with the same bundle identifier; Xcode makes its profile."""
    proj = CACHE / bundle_id
    proj.mkdir(parents=True, exist_ok=True)
    (proj / 'main.m').write_text('int main(void) { return 0; }\n')
    (proj / 'project.yml').write_text(f'''name: Signing
options: {{deploymentTarget: {{iOS: "17.0"}}}}
targets:
  App:
    type: application
    platform: iOS
    sources: [main.m]
    settings:
      GENERATE_INFOPLIST_FILE: YES
      PRODUCT_BUNDLE_IDENTIFIER: {bundle_id}
      DEVELOPMENT_TEAM: {team}
      CODE_SIGN_STYLE: Automatic
''')
    run(['xcodegen', 'generate', '--quiet'], cwd=proj)
    log = proj / 'xcodebuild.log'
    with log.open('w') as f:
        r = subprocess.run(['xcodebuild', '-project', 'Signing.xcodeproj', '-scheme', 'App',
                            '-configuration', 'Debug', '-destination', f'id={udid}',
                            '-derivedDataPath', str(proj / 'derived'), '-allowProvisioningUpdates',
                            '-allowProvisioningDeviceRegistration', 'build'],
                           cwd=proj, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        errors = [l for l in log.read_text(errors='replace').splitlines() if 'error:' in l]
        sys.exit('Xcode could not make a provisioning profile:\n' + '\n'.join(errors[-8:]) +
                 f'\nfull log: {log}')
    app = next((proj / 'derived/Build/Products/Debug-iphoneos').glob('*.app'))
    return app / 'embedded.mobileprovision', app


def signing_identity(signed_app):
    """The SHA-1 of the certificate Xcode signed the throwaway app with."""
    info = subprocess.run(['codesign', '-dvvv', str(signed_app)], capture_output=True, text=True).stderr
    authority = re.search(r'Authority=(Apple Development: [^\n]+)', info).group(1)
    for line in out(['security', 'find-identity', '-v', '-p', 'codesigning']).splitlines():
        if authority in line:
            return line.split()[1]
    sys.exit(f'no signing identity for {authority}')


def profile_plist(path):
    return plistlib.loads(out(['security', 'cms', '-D', '-i', path]).encode())


def sign(app_src, bundle_id, profile_path, identity):
    staging = app_src.parent / 'signed'
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    app = staging / app_src.name
    shutil.copytree(app_src, app, symlinks=True)
    info_path = app / 'Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    info['CFBundleIdentifier'] = bundle_id
    info_path.write_bytes(plistlib.dumps(info))
    shutil.copy(profile_path, app / 'embedded.mobileprovision')
    prof = profile_plist(profile_path)
    ents = {k: v for k, v in prof['Entitlements'].items()
            if k in ('application-identifier', 'com.apple.developer.team-identifier',
                     'get-task-allow')}
    ent_path = staging / 'app.entitlements'
    ent_path.write_bytes(plistlib.dumps(ents))
    run(['codesign', '--force', '--sign', identity, '--entitlements', ent_path,
         '--generate-entitlement-der', app])
    return app, prof['ExpirationDate']


def copy_game(dev_id, bundle_id):
    """game/ into the app's Documents/Game (devicectl replaces what is there)."""
    print('copying the game into Documents/Game (1.2 GB, a few minutes)...')
    run(['xcrun', 'devicectl', 'device', 'copy', 'to', '--device', dev_id,
         '--domain-type', 'appDataContainer', '--domain-identifier', bundle_id,
         '--source', ROOT / 'game', '--destination', 'Documents/Game'])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('app', nargs='?', type=Path, default=ROOT / 'build/ios-device/h4.app')
    ap.add_argument('--device', help='device name, identifier or UDID (as Finder shows it)')
    ap.add_argument('--team', help='Apple team ID, when the keychain has several')
    ap.add_argument('--bundle-id', default='local.h4port.heroes4')
    ap.add_argument('--game', action='store_true', help='also copy game/ into Documents/Game')
    ap.add_argument('--launch', action='store_true', help='start the app when installed')
    a = ap.parse_args()
    if not a.app.is_dir():
        sys.exit(f'{a.app} not found: build it with tools/ios/build_app.sh device')
    ts = [a.team] if a.team else teams()
    if len(ts) != 1:
        sys.exit('choose the team with --team: ' + (', '.join(ts) if ts else
                 'none found; make an Apple Development certificate in Xcode > Settings > '
                 'Apple Accounts > Manage Certificates'))
    team = ts[0]
    dev = pick_device(a.device)
    udid, dev_id = dev['hardwareProperties']['udid'], dev['identifier']
    # Free to register on your team; an identifier given with the suffix
    # already is used as it is.
    suffix = '.' + team.lower()
    bundle_id = a.bundle_id if a.bundle_id.endswith(suffix) else a.bundle_id + suffix
    print(f'team {team}, device {dev["deviceProperties"].get("name")}, bundle {bundle_id}')
    profile_path, throwaway = make_profile(team, bundle_id, udid)
    app, expires = sign(a.app, bundle_id, profile_path, signing_identity(throwaway))
    print(f'signed; valid until {expires:%Y-%m-%d %H:%M} (run again to renew)')
    run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', dev_id, app])
    print('installed. First time: Settings > General > VPN & Device Management > trust the developer.')
    if a.game:
        copy_game(dev_id, bundle_id)
    if a.launch:
        run(['xcrun', 'devicectl', 'device', 'process', 'launch', '--device', dev_id, bundle_id])


if __name__ == '__main__':
    os.environ.setdefault('DEVELOPER_DIR', out(['xcode-select', '-p']).strip())
    main()
