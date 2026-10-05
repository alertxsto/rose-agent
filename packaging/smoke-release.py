#!/usr/bin/env python3
"""Exercise real staged approval, native persistence and optional packaged UI/TLS."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--probe')
    parser.add_argument('--tls-url', default='https://example.com/')
    args = parser.parse_args()
    cli = str(Path(args.cli).resolve())
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    workspace = Path(tempfile.mkdtemp(prefix='native-smoke-', dir=output))
    native = workspace / 'reviewed.mdl'
    env = os.environ.copy()
    # This is an isolated probe, never the user's provider/settings namespace.
    env['XDG_CONFIG_HOME'] = str(workspace / 'config')
    if os.name != 'nt':
        env['QT_QPA_PLATFORM'] = 'offscreen'
    process = subprocess.Popen([cli, 'session', '--allow-root', str(workspace)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, env=env)
    counter = 0

    def request(method, params):
        nonlocal counter
        counter += 1
        identifier = f'smoke-{counter}'
        process.stdin.write(json.dumps({'id': identifier, 'method': method, 'params': params}) + '\n')
        process.stdin.flush()
        while True:
            line = process.stdout.readline()
            if not line:
                raise RuntimeError('Actual CLI exited before replying: ' + process.stderr.read())
            response = json.loads(line)
            if response.get('id') == identifier:
                return response

    def success(method, params):
        response = request(method, params)
        if not response.get('ok'):
            raise RuntimeError(f'{method}: {response}')
        return response['result']

    try:
        baseline = success('create', {'file': str(native), 'petalVersion': 44})
        owner = next(e['id'] for e in baseline['elements']
                     if e['kind'] == 'Class_Category' and e['name'] == 'Logical View')
        success('save', {})
        initial = hashlib.sha256(native.read_bytes()).digest()
        commands = [
            {'type': 'createElement', 'clientId': 'order', 'kind': 'Class', 'name': 'Order', 'owner': owner, 'unit': ''},
            {'type': 'createElement', 'clientId': 'entity', 'kind': 'Class', 'name': 'Entity', 'owner': owner, 'unit': ''},
            {'type': 'createElement', 'clientId': 'total', 'kind': 'ClassAttribute', 'name': 'total', 'owner': 'order', 'unit': ''},
            {'type': 'setProperty', 'id': {'kind': 'element', 'value': 'total'}, 'key': 'type', 'value': 'double'},
            {'type': 'createRelation', 'clientId': 'owns', 'kind': 'Association', 'name': 'owns', 'owner': owner, 'endpoints': ['order', 'entity'], 'properties': {}},
            {'type': 'createDiagram', 'clientId': 'domain', 'kind': 'ClassDiagram', 'name': 'ReleaseSmoke', 'owner': owner, 'unit': ''},
            {'type': 'addPresentation', 'clientId': 'order-view', 'diagram': 'domain', 'subject': {'kind': 'element', 'value': 'order'}, 'geometry': {'x': 800, 'y': 800, 'width': 600, 'height': 400}},
            {'type': 'addPresentation', 'clientId': 'entity-view', 'diagram': 'domain', 'subject': {'kind': 'element', 'value': 'entity'}, 'geometry': {'x': 2000, 'y': 800, 'width': 600, 'height': 400}},
            {'type': 'addPresentation', 'clientId': 'owns-view', 'diagram': 'domain', 'subject': {'kind': 'relation', 'value': 'owns'}, 'geometry': {'x': 1400, 'y': 800, 'width': 160, 'height': 100}},
        ]
        proposal = success('propose', {'baseRevision': '0', 'commands': commands})
        wrong = request('apply', {'id': proposal['id'], 'baseRevision': proposal['baseRevision'], 'digest': '0' * 64})
        if wrong.get('ok'):
            raise RuntimeError('Wrong digest approved')
        if hashlib.sha256(native.read_bytes()).digest() != initial:
            raise RuntimeError('Unapproved proposal changed native disk bytes')
        success('reject', {'id': proposal['id']})
        rejected = success('inspect', {'kind': 'modelTree'})
        if any(e['kind'] == 'Class' for e in rejected['elements']):
            raise RuntimeError('Rejected proposal changed live classes')
        proposal = success('propose', {'baseRevision': '0', 'commands': commands})
        success('apply', {key: proposal[key] for key in ('id', 'baseRevision', 'digest')})
        if hashlib.sha256(native.read_bytes()).digest() != initial:
            raise RuntimeError('Apply implicitly saved')
        success('save', {})
        process.stdin.close()
        process.wait(timeout=20)
        if process.returncode:
            raise RuntimeError(process.stderr.read())
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    projection = json.loads(subprocess.check_output([cli, 'inspect', '--file', str(native),
                             '--allow-root', str(workspace)], text=True, env=env, timeout=30))
    classes = {e['name']: e['id'] for e in projection['elements'] if e['kind'] == 'Class'}
    if set(classes) != {'Order', 'Entity'}:
        raise RuntimeError('Native reopen lost approved classes')
    member = next(e for e in projection['elements'] if e['name'] == 'total')
    if member['owner'] != classes['Order'] or member['properties']['type'] != 'double':
        raise RuntimeError('Native reopen lost member ownership/type')
    relationship = next(r for r in projection['relations'] if r['name'] == 'owns')
    if set(relationship['endpoints']) != set(classes.values()):
        raise RuntimeError('Native reopen lost association endpoints')
    if 'version 44 ' not in native.read_text(encoding='ascii').splitlines()[0]:
        raise RuntimeError('Native save lost Petal44 profile')
    if args.probe:
        subprocess.run([str(Path(args.probe).resolve()), str(native),
                        str(output / 'packaged-ui.png'), args.tls_url],
                       check=True, env=env, timeout=90)
    print('RELEASE_NATIVE_PASS real reject/wrong-digest/exact apply/separate save/independent native reopen')


if __name__ == '__main__':
    main()
