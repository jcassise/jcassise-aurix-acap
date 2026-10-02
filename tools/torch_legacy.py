"""Part of AURIX tools. Minimal reader for PyTorch's legacy (non-zip) .pt format without torch installed.
Returns the unpickled object with tensors as numpy arrays and modules as plain stubs."""
import pickle, struct, collections, numpy as np

DTYPES = {"FloatStorage": np.float32, "LongStorage": np.int64, "IntStorage": np.int32,
          "HalfStorage": np.float16, "DoubleStorage": np.float64, "ByteStorage": np.uint8,
          "BoolStorage": np.bool_}

class Stub:
    def __init__(self, *a, **k): pass
    def __setstate__(self, st):
        if isinstance(st, dict): self.__dict__.update(st)
        else: self.__dict__["_state"] = st

class StorageRef:
    def __init__(self, key, dtype, size): self.key, self.dtype, self.size, self.data = key, dtype, size, None

def _rebuild_tensor(storage, offset, size, stride, *rest):
    return ("TENSOR", storage, offset, tuple(size), tuple(stride))

def _rebuild_parameter(t, *rest): return t

class Unpickler(pickle.Unpickler):
    def __init__(self, f, storages):
        super().__init__(f, encoding="latin1"); self.storages = storages
    def find_class(self, mod, name):
        if name in ("_rebuild_tensor_v2", "_rebuild_tensor"): return _rebuild_tensor
        if name == "_rebuild_parameter": return _rebuild_parameter
        if name in DTYPES: return name
        if mod == "collections" and name == "OrderedDict": return collections.OrderedDict
        if mod.startswith("torch") or mod not in ("builtins", "__builtin__"):
            return type(name, (Stub,), {"_module": mod})
        return super().find_class(mod, name)
    def persistent_load(self, pid):
        # ('storage', storage_type, key, location, numel, view_metadata)
        _, stype, key, _, numel, _ = pid
        stype = stype if isinstance(stype, str) else stype.__name__
        if key not in self.storages: self.storages[key] = StorageRef(key, DTYPES[stype], numel)
        return self.storages[key]

def load(path):
    storages = {}
    with open(path, "rb") as f:
        magic = pickle.load(f); assert magic == 0x1950a86a20f9469cfc6c, "not legacy torch"
        pickle.load(f); pickle.load(f)                      # protocol, sys_info
        obj = Unpickler(f, storages).load()
        keys = pickle.load(f)
        for k in keys:
            s = storages[k]; n, = struct.unpack("<q", f.read(8))
            s.data = np.frombuffer(f.read(n * np.dtype(s.dtype).itemsize), dtype=s.dtype)
    def fix(x):
        if isinstance(x, tuple) and x and x[0] == "TENSOR":
            _, st, off, size, stride = x
            if not size: return st.data[off:off+1].reshape(())
            return np.lib.stride_tricks.as_strided(st.data[off:], size,
                       [s * st.data.itemsize for s in stride]).copy()
        if isinstance(x, dict): return type(x)((k, fix(v)) for k, v in x.items())
        if isinstance(x, list): return [fix(v) for v in x]
        if isinstance(x, Stub): x.__dict__.update({k: fix(v) for k, v in x.__dict__.items()}); return x
        return x
    return fix(obj)

def state_dict(obj, prefix=""):
    """Flatten an nn.Module stub (or dict) into name -> array, like module.state_dict()."""
    out = collections.OrderedDict()
    if isinstance(obj, dict) and all(isinstance(v, np.ndarray) for v in obj.values()):
        return collections.OrderedDict(obj)
    d = obj.__dict__ if isinstance(obj, Stub) else obj
    for group in ("_parameters", "_buffers"):
        for k, v in (d.get(group) or {}).items():
            if isinstance(v, np.ndarray): out[prefix + k] = v
    for k, v in (d.get("_modules") or {}).items():
        out.update(state_dict(v, prefix + k + "."))
    return out
