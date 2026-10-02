"""Start a temporary validation UI, bound exclusively to 127.0.0.1."""
import argparse
import os
from pathlib import Path
import secrets
import socket
import tempfile
import threading
import webbrowser

import uvicorn

from core import Broker, plain_path
from server import create_app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True, help='Existing project folder (required).')
    parser.add_argument('--idf-path', type=Path, help='ESP-IDF folder; otherwise inherit activated IDF_PATH.')
    parser.add_argument('--no-browser', action='store_true', help='Print URL without opening the default browser.')
    args = parser.parse_args()
    project = plain_path(args.project)
    # OS user temp state, outside Git. No server, credential or port survives exit.
    state = Path(tempfile.mkdtemp(prefix='pocketjs-validation-gui-'))
    os.chmod(state, 0o700)
    broker = Broker(project, state, idf_path=args.idf_path)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        # Reserve a free loopback port before publishing its exact allowed origin.
        if os.name == 'nt':
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        sock.bind(('127.0.0.1', 0))
        sock.listen(128)
        port = sock.getsockname()[1]
        token = secrets.token_urlsafe(32)
        app = create_app(broker, token, port)
        broker.start_scan()
        broker.refresh_ports()
        url = f'http://127.0.0.1:{port}/#token={token}'
        print('Cardputer validation GUI: ' + url, flush=True)
        print('Private bounded session logs: ' + str(state), flush=True)
        print('Keep this terminal open. Closing the browser does not cancel a device job.', flush=True)
        print('Activate ESP-IDF v6.0.1 before launch. No serial port is opened until you approve a plan.', flush=True)
        if not args.no_browser:
            timer = threading.Timer(1.0, lambda: webbrowser.open(url, new=2))
            timer.daemon = True
            timer.start()
        server = uvicorn.Server(uvicorn.Config(app, host='127.0.0.1', port=port,
            access_log=False, proxy_headers=False, log_level='warning', timeout_graceful_shutdown=None))
        server.run(sockets=[sock])
    finally:
        if not broker.closed:
            broker.close()
        sock.close()
        print('Server stopped. Session tokens and plans expired. Retained run evidence is unchanged.', flush=True)


if __name__ == '__main__':
    main()
