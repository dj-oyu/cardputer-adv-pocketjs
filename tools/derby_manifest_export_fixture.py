"""Synthetic export projection for tests; not the broker producer or an export API."""
import copy
import hashlib

from games.derby_manifest_export import CONTRACT, export_digest


def project(manifest):
    public = copy.deepcopy(manifest)
    envelope = public['diagnostic_modeproof']
    cache = envelope.pop('cmake_cache', None)
    if cache is not None:
        raw = cache.encode('utf-8')
        envelope['cache_evidence'] = dict(retained_private=True, bytes=len(raw),
                                          sha256=hashlib.sha256(raw).hexdigest())
    wrapper = dict(schema_version=1, contract=CONTRACT,
                   original_manifest_sha256=export_digest(manifest), manifest=public)
    return wrapper, export_digest(wrapper)
