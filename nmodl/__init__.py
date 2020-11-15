try:
# check if nmodl binds are built before importing
    from ._nmodl import NmodlDriver, to_json, to_nmodl  # noqa
    from ._nmodl import __version__
    __all__ = ["NmodlDriver", "to_json", "to_nmodl"]
except ImportError:
    print("[NMODL] [warning] :: Python bindings are not available")

