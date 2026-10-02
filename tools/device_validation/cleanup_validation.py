"""Conservative, user-run cleanup of one manifest-owned validation run.

Preview is read-only. Removal archives bounded generated directories, then asks
Git to remove clean detached worktrees without force. Never purges archives.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import uuid


class Refused(RuntimeError):
    pass


def full_path(value):
    if not isinstance(value, str) or not value or any(c in value for c in '*?[]\x00\r\n'):
        raise Refused('Use one explicit absolute path without wildcards or control characters.')
    path = Path(value)
    if not path.is_absolute() or '..' in path.parts:
        raise Refused('Absolute paths without parent traversal are required: ' + value)
    if os.name == 'nt':
        if value.startswith(('\\\\', '//')) or not re.match(r'^[A-Za-z]:[\\/]', value):
            raise Refused('Only plain local drive paths are supported: ' + value)
        if any(part.endswith((' ', '.')) or ':' in part for part in path.parts[1:]):
            raise Refused('Windows path aliases and alternate data streams are refused: ' + value)
    return Path(os.path.abspath(value))


def same(left, right):
    return os.path.normcase(str(left)) == os.path.normcase(str(right))


def within(child, parent):
    try:
        return same(Path(os.path.commonpath([str(child), str(parent)])), parent)
    except ValueError:
        return False


def plain(path):
    """lstat every existing ancestor: includes Windows junction/reparse points."""
    for item in [path, *path.parents]:
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise Refused('Symlink/junction/reparse point refused: ' + str(item))
        if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
            raise Refused('Special filesystem object refused: ' + str(item))


def scan(path):
    """Do not follow links, including reparse points below an approved directory."""
    plain(path)
    if not path.exists():
        return []
    result = []
    pending = [path]
    while pending:
        current = pending.pop()
        plain(current)
        if current.is_dir():
            pending.extend(current.iterdir())
        else:
            result.append(current)
    return result


class Git:
    def __call__(self, repo, *args):
        env = dict(os.environ)
        # Never let inherited repository selectors redirect a -C invocation.
        for key in list(env):
            if key.startswith('GIT_'):
                del env[key]
        env['GIT_OPTIONAL_LOCKS'] = '0'
        result = subprocess.run(['git', '-C', str(repo), *args], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise Refused('git ' + ' '.join(args) + ': ' + result.stderr.decode('utf-8', 'replace').strip())
        return result.stdout.decode('utf-8', 'surrogateescape')


def registry(repo, git):
    records = []
    for record in git(repo, 'worktree', 'list', '--porcelain', '-z').split('\0\0'):
        if not record:
            continue
        fields = dict(field.partition(' ')[::2] for field in record.split('\0') if field)
        if 'worktree' not in fields:
            raise Refused('Unrecognized Git worktree registry.')
        fields['path'] = full_path(fields['worktree'])
        records.append(fields)
    if not records:
        raise Refused('No Git worktree registry available.')
    return records


def owned_dirs(entry):
    wt = Path(entry['worktree'])
    return [Path(entry['build']), wt / 'managed_components', wt / '.cache' / 'bmi270',
            wt / '.cache' / 'codecs' / 'minimp3', wt / '.cache' / 'codecs' / 'opus-1.6.1']


def inspect_worktree(repo, entry, records, git):
    wt = Path(entry['worktree'])
    registered = [r for r in records if same(r['path'], wt)]
    if not wt.exists():
        if registered:
            raise Refused('Missing worktree is still registered; inspect manually: ' + str(wt))
        return dict(entry, action='already absent', archive=[])
    if len(registered) != 1 or same(records[0]['path'], wt):
        raise Refused('Target is not a registered secondary worktree: ' + str(wt))
    item = registered[0]
    if 'detached' not in item or 'locked' in item or 'prunable' in item or item.get('HEAD') != entry['sha']:
        raise Refused('Worktree must be unlocked, detached and at the manifest SHA: ' + str(wt))
    if not (wt / '.git').is_file():
        raise Refused('Expected a linked-worktree .git file: ' + str(wt))
    common = full_path(git(repo, 'rev-parse', '--path-format=absolute', '--git-common-dir').strip())
    other = full_path(git(wt, 'rev-parse', '--path-format=absolute', '--git-common-dir').strip())
    plain(common)
    plain(full_path(git(wt, 'rev-parse', '--absolute-git-dir').strip()))
    if not same(common, other) or git(wt, 'rev-parse', 'HEAD').strip() != entry['sha']:
        raise Refused('Repository ownership or HEAD mismatch: ' + str(wt))
    if git(wt, 'status', '--porcelain', '--untracked-files=no').strip():
        raise Refused('Tracked changes (including dependencies.lock) must be reviewed and preserved manually: ' + str(wt))
    for record in git(wt, 'ls-files', '-v', '-z').split('\0'):
        if record and (record[0].islower() or record[0] == 'S'):
            raise Refused('assume-unchanged/skip-worktree entries are refused: ' + str(wt))
    tracked = set()
    for record in git(wt, 'ls-files', '--stage', '-z').split('\0'):
        if not record:
            continue
        meta, name = record.split('\t', 1)
        if meta.split()[0] == '160000' or meta.split()[2] != '0':
            raise Refused('Submodules or unmerged index entries are refused: ' + str(wt))
        tracked.add(name)
    generated = owned_dirs(entry)
    for name in tracked:
        if any(within(wt / name, directory) for directory in generated):
            raise Refused('A generated-directory path contains tracked source: ' + name)
    files = scan(wt)
    for file in files:
        relative = file.relative_to(wt).as_posix()
        if file.name == '.git' and relative != '.git':
            raise Refused('Nested linked-worktree/submodule metadata is refused: ' + str(file))
        if relative not in tracked and relative != '.git' and not any(within(file, d) for d in generated):
            raise Refused('Unknown untracked/ignored file; preserve it manually: ' + str(file))
    archive = []
    for directory in generated:
        if directory.exists():
            if not directory.is_dir():
                raise Refused('Expected generated directory: ' + str(directory))
            contents = [file for file in files if within(file, directory)]
            archive.append({'path': str(directory), 'relative': directory.relative_to(wt).as_posix(),
                            'files': len(contents), 'bytes': sum(file.stat().st_size for file in contents)})
    return dict(entry, action='archive generated directories, then git worktree remove', archive=archive)


def inspect_run(repo, root, git):
    repo, root = full_path(str(repo)), full_path(str(root))
    plain(repo)
    plain(root)
    if within(root, repo) or within(repo, root):
        raise Refused('RunRoot and Repo must be separate, non-overlapping directories.')
    if not same(full_path(git(repo, 'rev-parse', '--show-toplevel').strip()), repo):
        raise Refused('Repo must name the exact repository root.')
    if os.path.lexists(root / '.integrated-device.lock'):
        raise Refused('Integrated device lock exists; do not clean or remove the lock to bypass this refusal.')
    manifest_path = root / 'manifest.json'
    plain(manifest_path)
    raw = manifest_path.read_bytes()
    manifest = json.loads(raw.decode('utf-8-sig'))
    if not isinstance(manifest, dict):
        raise Refused('Expected a manifest object.')
    if manifest.get('status') not in ('built', 'failed'):
        raise Refused('Only terminal built/failed runs are eligible; never infer idle from age.')
    if not same(full_path(manifest.get('sourceRepository')), repo):
        raise Refused('Manifest sourceRepository does not match Repo.')
    schema = manifest.get('schema')
    if type(schema) is int and schema == 1:
        roles, build_name = ['baseline', 'candidate'], 'build_device_validation'
    elif schema == 'pocketjs-integrated-build-v1' and manifest.get('manifestType') == 'integrated-build-only':
        roles, build_name = ['integrated'], 'build_integrated'
    else:
        raise Refused('Unsupported validation manifest schema.')
    builds = manifest.get('builds')
    if not isinstance(builds, list) or len(builds) != len(roles):
        raise Refused('Unexpected manifest build entries.')
    entries = []
    for role in roles:
        matches = [b for b in builds if isinstance(b, dict) and b.get('role') == role]
        if len(matches) != 1:
            raise Refused('Missing or duplicated build role: ' + role)
        entry = matches[0]
        wt = root / ('wt-' + role)
        build = wt / build_name
        if not same(full_path(entry.get('worktree')), wt) or not same(full_path(entry.get('build')), build):
            raise Refused('Manifest paths must match the exact harness layout: ' + role)
        allowed_statuses = ('built',) if manifest['status'] == 'built' else ('built', 'failed', 'planned')
        if entry.get('status') not in allowed_statuses:
            raise Refused('Active or unknown build-entry state: ' + role)
        if not isinstance(entry.get('sha'), str) or not re.fullmatch('[0-9a-f]{40}', entry['sha']):
            raise Refused('Expected an exact full lowercase SHA: ' + role)
        entries.append({'role': role, 'sha': entry['sha'], 'worktree': str(wt), 'build': str(build)})
    records = registry(repo, git)
    for record in records:
        if within(root, record['path']):
            raise Refused('RunRoot must be outside every registered worktree.')
        if within(record['path'], root) and not any(same(record['path'], Path(e['worktree'])) for e in entries):
            raise Refused('Unexpected worktree under RunRoot: ' + str(record['path']))
    # Inspect every existing run descendant before mutation, including retained
    # logs and archives. A junction anywhere is a hard refusal, not followed.
    scan(root)
    for entry in entries:
        if within(Path.cwd(), Path(entry['worktree'])):
            raise Refused('Change to a directory outside the target worktree first.')
    # Newer device stages are not all described by the build manifest. Explicit
    # user attestation remains necessary even when these records look terminal.
    for name in ('last-flash.json', 'last-integrated-flash.json'):
        last_flash = root / name
        if last_flash.is_file():
            record = json.loads(last_flash.read_text(encoding='utf-8-sig'))
            if not isinstance(record, dict) or record.get('status') not in ('failed', 'flash-verified-awaiting-manual-reboot'):
                raise Refused('Last-flash record is active or unrecognized: ' + name)
    for child in root.iterdir():
        if child.name.startswith(('flash-', 'device-')) and child.is_dir():
            result = child / 'result.json'
            if not result.is_file():
                raise Refused('Device stage has no terminal result: ' + str(child))
            record = json.loads(result.read_text(encoding='utf-8-sig'))
            if not isinstance(record, dict) or record.get('status') not in ('failed', 'flash-verified-awaiting-manual-reboot',
                                          'hello-markers-passed', 'hello-and-grid-markers-passed'):
                raise Refused('Device stage is active or unrecognized: ' + str(child))
    worktrees = [inspect_worktree(repo, entry, records, git) for entry in entries]
    return {'mode': 'preview', 'repo': str(repo), 'runRoot': str(root),
            'manifestSha256': hashlib.sha256(raw).hexdigest(),
            'retained': 'Run root, manifest, logs and every archived generated file; no archive purge.',
            'archiveBytes': sum(d['bytes'] for e in worktrees for d in e['archive']),
            'worktrees': worktrees}


def remove_run(plan, git):
    repo, root = Path(plan['repo']), Path(plan['runRoot'])
    fresh = inspect_run(repo, root, git)
    if fresh != plan:
        raise Refused('Run changed since preflight; preview again.')
    targets = [e for e in plan['worktrees'] if e['action'] != 'already absent']
    if not targets:
        return dict(plan, mode='no worktrees to remove')
    archive = root / ('cleanup-archive-' + uuid.uuid4().hex)
    archive.mkdir()  # New, exact path. Never overwrite an earlier archive.
    receipt = dict(plan, mode='removal started', archiveRoot=str(archive), completed=[])
    def save():
        (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    save()
    try:
        for target in targets:
            current = inspect_run(repo, root, git)
            if current['manifestSha256'] != plan['manifestSha256']:
                raise Refused('Manifest changed; stopping without further removal.')
            wt = Path(target['worktree'])
            for generated in target['archive']:
                source = Path(generated['path'])
                destination = archive / target['role'] / generated['relative']
                destination.parent.mkdir(parents=True, exist_ok=True)
                plain(source)
                plain(destination)
                if destination.exists():
                    raise Refused('Archive destination already exists: ' + str(destination))
                source.rename(destination)  # Same run/volume; no recursive deletion.
                receipt['completed'].append({'archived': str(source), 'to': str(destination)})
                save()
            # Recheck ownership, active manifest and every source after moves.
            inspect_run(repo, root, git)
            if hashlib.sha256((root / 'manifest.json').read_bytes()).hexdigest() != plan['manifestSha256']:
                raise Refused('Manifest changed; stopping without further removal.')
            git(repo, 'worktree', 'remove', str(wt))  # Deliberately no --force.
            receipt['completed'].append({'removedWorktree': str(wt)})
            save()
        remaining = registry(repo, git)
        if any(any(same(r['path'], Path(t['worktree'])) for r in remaining) or Path(t['worktree']).exists() for t in targets):
            raise Refused('Post-removal worktree verification failed.')
        receipt['mode'] = 'worktrees removed; generated files and logs retained'
        save()
        return receipt
    except Exception as exc:
        receipt['mode'] = 'incomplete; inspect retained archive before retrying'
        receipt['failure'] = str(exc)
        save()
        raise Refused(str(exc) + '\nRetained cleanup receipt: ' + str(archive / 'receipt.json')) from exc


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--run-root', required=True)
    parser.add_argument('--remove', action='store_true')
    parser.add_argument('--confirm-no-active-processes', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.remove and not args.confirm_no_active_processes:
            raise Refused('--remove also requires --confirm-no-active-processes; do not stop or guess about running jobs.')
        git = Git()
        plan = inspect_run(args.repo, args.run_root, git)
        result = remove_run(plan, git) if args.remove else plan
        print(json.dumps(result, indent=2))
        return 0
    except (Refused, OSError, ValueError, KeyError, TypeError) as exc:
        print('REFUSED / INCOMPLETE: ' + str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
