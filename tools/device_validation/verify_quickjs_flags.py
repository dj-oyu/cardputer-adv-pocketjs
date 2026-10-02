"""Verify the explicit H setting in a Windows/GCC compilation database.

ESP-IDF 6 moves CMAKE_C_FLAGS into @toolchain/cflags. Read only response
files actually referenced by quickjs.c; never execute the recorded command.
"""
import argparse
import hashlib
import json
import ntpath
import os
import re
from pathlib import Path
import sys

MACRO = 'POCKET_VM_TYPED_PUT_INT_FAST'
MAX_DEPTH = 16
MAX_FILES = 128
MAX_BYTES = 4 * 1024 * 1024
MAX_TOKENS = 100000


class VerificationError(ValueError):
    pass


def split_windows_command(text):
    """Decode CRT-style argv quoting, preserving ordinary Windows backslashes."""
    if '\0' in text:
        raise VerificationError('NUL in compile command')
    result, token = [], []
    quoted = started = False
    i = 0
    while i < len(text):
        char = text[i]
        if char in ' \t\r\n' and not quoted:
            if started:
                result.append(''.join(token))
                token, started = [], False
            i += 1
            continue
        started = True
        if char == '\\':
            end = i
            while end < len(text) and text[end] == '\\':
                end += 1
            count = end - i
            if end < len(text) and text[end] == '"':
                token.extend('\\' * (count // 2))
                if count % 2:
                    token.append('"')
                    i = end + 1
                    continue
                i = end
            else:
                token.extend('\\' * count)
                i = end
                continue
        if text[i] == '"':
            if quoted and i + 1 < len(text) and text[i + 1] == '"':
                token.append('"')
                i += 2
                continue
            quoted = not quoted
        else:
            token.append(text[i])
        i += 1
    if quoted:
        raise VerificationError('Unclosed quote in compile command')
    if started:
        result.append(''.join(token))
    return result


def split_response(text):
    """GCC @files use both quote types and backslash escaping, not CRT rules."""
    if '\0' in text:
        raise VerificationError('NUL in response file')
    result, token = [], []
    quote = None
    started = False
    i = 0
    while i < len(text):
        char = text[i]
        if char.isspace() and quote is None:
            if started:
                result.append(''.join(token))
                token, started = [], False
        elif char == '\\':
            i += 1
            if i == len(text):
                raise VerificationError('Trailing escape in response file')
            token.append(text[i])
            started = True
        elif char == quote:
            quote = None
        elif char in '\'"' and quote is None:
            quote, started = char, True
        else:
            token.append(char)
            started = True
        i += 1
    if quote is not None:
        raise VerificationError('Unclosed quote in response file')
    if started:
        result.append(''.join(token))
    return result


def expand_responses(arguments, directory):
    """Resolve nested @files against the compiler cwd, as GCC does."""
    expanded, evidence = [], []
    byte_count = file_count = 0

    def visit(tokens, stack):
        nonlocal byte_count, file_count
        for token in tokens:
            if not token.startswith('@'):
                expanded.append(token)
                if len(expanded) > MAX_TOKENS:
                    raise VerificationError('Response expansion token limit exceeded')
                continue
            if len(stack) >= MAX_DEPTH:
                raise VerificationError('Response expansion depth limit exceeded')
            if len(token) == 1:
                raise VerificationError('Empty response-file path')
            path = Path(token[1:])
            if not path.is_absolute():
                path = directory / path
            path = path.resolve()
            key = os.path.normcase(str(path))
            if key in stack:
                raise VerificationError(f'Cyclic response file: {path}')
            file_count += 1
            if file_count > MAX_FILES:
                raise VerificationError('Response expansion file limit exceeded')
            try:
                with path.open('rb') as stream:
                    data = stream.read(MAX_BYTES - byte_count + 1)
            except OSError as error:
                raise VerificationError(f'Cannot read response file {path}: {error}') from error
            byte_count += len(data)
            if byte_count > MAX_BYTES:
                raise VerificationError('Response expansion byte limit exceeded')
            try:
                text = data.decode('utf-8-sig')
            except UnicodeError as error:
                raise VerificationError(f'Response file is not UTF-8: {path}') from error
            evidence.append({'path': str(path), 'bytes': len(data),
                             'sha256': hashlib.sha256(data).hexdigest()})
            visit(split_response(text), stack + (key,))

    visit(arguments, ())
    return expanded, evidence


def verify(database, expected):
    entries = [entry for entry in database
               if isinstance(entry, dict)
               and ntpath.basename(entry.get('file', '')) == 'quickjs.c']
    if len(entries) != 1:
        raise VerificationError('Expected exactly one QuickJS compile command')
    entry = entries[0]
    directory = entry.get('directory')
    if not isinstance(directory, str) or not Path(directory).is_absolute():
        raise VerificationError('Compile directory must be an absolute path')
    if 'arguments' in entry:
        arguments = entry['arguments']
        if not isinstance(arguments, list) or not all(isinstance(v, str) for v in arguments):
            raise VerificationError('Invalid compile arguments array')
    elif isinstance(entry.get('command'), str):
        arguments = split_windows_command(entry['command'])
    else:
        raise VerificationError('Missing compile command/arguments')
    if not arguments or not arguments[0]:
        raise VerificationError('Empty compile command')
    flags, evidence = expand_responses(arguments[1:], Path(directory))
    definitions, undefines = [], []
    i = 0
    while i < len(flags):
        flag = flags[i]
        if flag == '-Xpreprocessor' or (flag.startswith('-Wp,') and MACRO in flag):
            raise VerificationError('Cannot verify H through preprocessor forwarding options')
        if flag.startswith(('-D', '-U')):
            operation, body = flag[:2], flag[2:]
            if not body:
                i += 1
                if i == len(flags):
                    raise VerificationError(f'Missing operand for {operation}')
                body = flags[i]
            name, separator, value = body.partition('=')
            # GCC accepts whitespace after a macro name. Treat every spelling
            # of the target identifier as relevant, but accept only canonical =0/1.
            identifier = re.match(r'[ \t\r\n\v\f]*([A-Za-z_][A-Za-z_0-9]*)', body)
            if identifier and identifier.group(1) == MACRO:
                if operation == '-U':
                    undefines.append(body)
                else:
                    definitions.append(value if separator and name == MACRO else None)
        i += 1
    if undefines or definitions != [str(expected)]:
        raise VerificationError(
            f'H={expected} was not verified: expected exactly one {MACRO}={expected} '
            f'and no undefines; definitions={definitions!r}, undefines={undefines!r}')
    return {'macro': MACRO, 'value': expected, 'definitionCount': 1,
            'responseFiles': evidence}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-commands', required=True, type=Path)
    parser.add_argument('--expected', required=True, choices=('0', '1'))
    args = parser.parse_args()
    try:
        database = json.loads(args.compile_commands.read_text(encoding='utf-8-sig'))
        if not isinstance(database, list):
            raise VerificationError('Compilation database must be an array')
        result = verify(database, int(args.expected))
    except (OSError, UnicodeError, ValueError) as error:
        print(f'QuickJS flag verification failed: {error}', file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
