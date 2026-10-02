"""Strict loopback HTTP adapter. No credential, device, or generic command API."""
from __future__ import annotations

from contextlib import asynccontextmanager
from pathlib import Path
import secrets

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, ConfigDict, Field, StrictBool, StrictInt, StrictStr

from core import Broker

STATIC = Path(__file__).parent / 'static'


class Input(BaseModel):
    model_config = ConfigDict(extra='forbid')


class Empty(Input):
    pass


class RunInput(Input):
    path: StrictStr = Field(min_length=1, max_length=4096)


class PlanInput(Input):
    run_id: StrictStr = Field(min_length=1, max_length=64)
    recovery_id: StrictStr = Field(default='', max_length=64)
    port: StrictStr = Field(pattern=r'^COM[1-9][0-9]*$', max_length=32)
    confidence: StrictStr = Field(default='unverified-candidate', pattern=r'^(unverified-candidate|user-attested-known-good)$')
    action: StrictStr = Field(pattern=r'^(flash|test)$')
    cycles: StrictInt = Field(default=3, ge=1, le=20)
    grid: StrictBool = False


class ExecuteInput(Input):
    plan_id: StrictStr = Field(pattern=r'^[0-9a-f]{32}$')
    plan_digest: StrictStr = Field(pattern=r'^[0-9a-f]{64}$')
    acknowledgements: list[StrictStr] = Field(max_length=8)


def create_app(broker: Broker, token: str, port: int):
    host = f'127.0.0.1:{port}'
    origin = 'http://' + host

    @asynccontextmanager
    async def lifespan(app):
        yield
        broker.close()

    app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None, lifespan=lifespan)

    @app.middleware('http')
    async def local_only(request: Request, call_next):
        # Host rebinding, cross-site form/fetch, and untrusted proxy headers cannot authorize a job.
        if request.headers.get('host') != host:
            return JSONResponse({'detail': 'Invalid loopback host.'}, status_code=403)
        if request.client and request.client.host not in ('127.0.0.1', 'testclient'):
            return JSONResponse({'detail': 'Loopback clients only.'}, status_code=403)
        if request.headers.get('origin') not in (None, origin):
            return JSONResponse({'detail': 'Cross-origin requests are forbidden.'}, status_code=403)
        if request.headers.get('sec-fetch-site') not in (None, 'same-origin', 'none'):
            return JSONResponse({'detail': 'Cross-site requests are forbidden.'}, status_code=403)
        if request.url.path.startswith('/api/'):
            supplied = request.headers.get('x-session-token', '')
            if not secrets.compare_digest(supplied, token):
                return JSONResponse({'detail': 'Session token is missing or expired; reopen the launcher URL.'}, status_code=401)
            if request.method != 'GET':
                if request.headers.get('origin') != origin or request.headers.get('content-type', '').split(';')[0] != 'application/json':
                    return JSONResponse({'detail': 'Same-origin JSON is required.'}, status_code=403)
                # Reject oversized request bodies before FastAPI buffers/parses them, including chunked bodies.
                body = bytearray()
                async for chunk in request.stream():
                    body.extend(chunk)
                    if len(body) > 16384:
                        return JSONResponse({'detail': 'Request body is too large.'}, status_code=413)
                request._body = bytes(body)
        response = await call_next(request)
        response.headers.update({
            'Cache-Control': 'no-store', 'Pragma': 'no-cache', 'X-Content-Type-Options': 'nosniff',
            'Referrer-Policy': 'no-referrer', 'X-Frame-Options': 'DENY',
            'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
            'Permissions-Policy': 'camera=(), microphone=(), geolocation=(), usb=(), serial=()'})
        return response

    @app.exception_handler(ValueError)
    async def invalid(request, error):
        return JSONResponse({'detail': str(error)}, status_code=409)

    @app.exception_handler(OSError)
    async def inaccessible(request, error):
        return JSONResponse({'detail': str(error)}, status_code=409)

    @app.get('/')
    def index():
        return FileResponse(STATIC / 'index.html')

    @app.get('/static/{name}')
    def asset(name: str):
        if name not in ('app.js', 'styles.css'):
            raise HTTPException(404)
        return FileResponse(STATIC / name)

    @app.get('/api/state')
    def state():
        return broker.state()

    @app.get('/api/jobs/{job_id}')
    def job(job_id: str):
        return broker.get_job(job_id)

    @app.post('/api/scan', status_code=202)
    def scan(data: Empty):
        return broker.start_scan()

    @app.post('/api/cancel-scan')
    def cancel(data: Empty):
        broker.scan_cancel.set()
        return {'status': 'requested'}

    @app.post('/api/ports', status_code=202)
    def ports(data: Empty):
        return broker.refresh_ports()

    @app.post('/api/import-run', status_code=202)
    def import_run(data: RunInput):
        return broker.import_run(data.path)

    @app.post('/api/plan', status_code=202)
    def plan(data: PlanInput):
        return broker.create_plan(data.model_dump())

    @app.post('/api/execute', status_code=202)
    def execute(data: ExecuteInput):
        return broker.execute_plan(data.plan_id, data.plan_digest, data.acknowledgements)

    return app
