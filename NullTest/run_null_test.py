import ctypes, math, random, struct, hashlib, os

dll_path = r"C:\Program Files\Avid\Pro Tools\System Plug-Ins\HEAT.aaxplugin\Contents\x64\HEAT.aaxplugin"
out_dir  = r"C:\Users\anton\Documents\Codex\2026-11-16\pu\outputs\FL-Heat-PAPIELGUAPO\NullTest"

with open(dll_path, "rb") as f:
    aax_bytes = f.read()
aax_sha256 = hashlib.sha256(aax_bytes).hexdigest()

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
kernel32.LoadLibraryExW.restype = ctypes.c_void_p

base = kernel32.LoadLibraryExW(dll_path, None, 1)
alg_entry_addr = base + 0x1B70
ALG_ENTRY = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)(alg_entry_addr)
lut = ctypes.cast(base + 0x194B0, ctypes.POINTER(ctypes.c_float * 32769)).contents

lut_bytes = ctypes.string_at(base + 0x194B0, 32768 * 4)
lut_sha256 = hashlib.sha256(lut_bytes).hexdigest()

class OcelotCoefPacket(ctypes.Structure):
    _fields_ = [
        ("tableOffset", ctypes.c_int32),
        ("preCoef1", ctypes.c_float),
        ("preDiffCoef", ctypes.c_float),
        ("preScaleCoef", ctypes.c_float),
        ("normScale", ctypes.c_float),
        ("postMix1", ctypes.c_float),
        ("driveDepth", ctypes.c_float),
        ("dryMakeUp", ctypes.c_float),
        ("postB0", ctypes.c_float),
        ("postA1", ctypes.c_float),
        ("postG", ctypes.c_float),
        ("bypassFlag", ctypes.c_int32),
    ]

class AlgContext(ctypes.Structure):
    _fields_ = [
        ("ppIn", ctypes.c_void_p),
        ("ppOut", ctypes.c_void_p),
        ("pNumSamples", ctypes.c_void_p),
        ("pCoefs", ctypes.c_void_p),
        ("pState", ctypes.c_void_p),
        ("ppMeter", ctypes.c_void_p),
    ]

def f32(v):
    return ctypes.c_float(v).value

def make_coefs(drive_steps, tone_steps, sr=48000.0):
    c = OcelotCoefPacket()
    xmm9 = float(drive_steps) * 0.2
    xmm8 = float(tone_steps)
    xmm10 = 44100.0 / float(sr)
    if int(xmm8) in (2, 4):
        xmm8 *= 1.06
    elif int(xmm8) == 3:
        xmm8 *= 1.12
    if xmm9 > 0.0:
        c.tableOffset = 0x4000
        sp128, sp20, xmm11, xmm13, xmm14, sp130, sp24, sp138 = (
            0.099609375, 0.359375, 0.625, 0.35058594, 0.453125, 0.734375, 0.9375, 0.9921875
        )
    elif xmm9 < 0.0:
        c.tableOffset = 0
        xmm9 = -xmm9
        sp128, xmm11, xmm13, xmm14, sp130, sp24, sp138, sp20 = (
            0.10546875, 1.0, 0.31152344, 0.171875, 0.9921875, -0.25, 0.9375, 0.2421875
        )
    else:
        c.tableOffset = 0x4000
        c.preCoef1, c.preDiffCoef, c.preScaleCoef = 0.625, 0.7011719, 1.453125
        c.bypassFlag = 1
        return c

    c.bypassFlag = 0
    xmm8 = xmm8 * 0.125 + 0.25
    xmm6 = (xmm9 * xmm9 + xmm9) * 0.5
    c.normScale  = f32(1.0 / float(2 << 13))
    c.driveDepth = f32((math.sqrt(xmm9) + (xmm9 - xmm6) * 2.0) * sp128)
    c.postG      = f32(sp130 * xmm6)
    c.postB0     = f32(sp138 * xmm6)
    c.postA1     = f32(1.0 - (((xmm6 + xmm9) * 0.6 + xmm9) * 0.5 * sp20))
    c.postMix1   = f32(sp24)
    c.dryMakeUp  = f32((xmm9 * 0.5 + 0.5) * 0.9375 + 1.0)
    c.preCoef1   = f32(xmm11 * xmm8)
    scaled_sr    = xmm10 * 2.0 * xmm8
    c.preDiffCoef  = f32(xmm13 * scaled_sr * 2.0)
    c.preScaleCoef = f32(xmm14 * scaled_sr + 1.0)
    return c

def run_fl_heat_1x(in_samples, c):
    z0 = 0.0
    z1 = 0.0
    out = []
    base_idx = c.tableOffset
    in_gain = f32(1.0 - c.driveDepth)
    fb_coeff = f32(1.0 - c.preDiffCoef)
    post_sub = f32(c.postG * c.postB0)

    for raw_in in in_samples:
        x3 = f32(raw_in * in_gain)
        u1 = f32(f32(f32(f32(x3 * c.preScaleCoef) - z0) * c.dryMakeUp) - (-1.0))
        idx1 = f32(u1 * 8191.5)
        if idx1 > 16383.0: idx1 = 16383.0
        elif idx1 < 0.0:   idx1 = 0.0
        i1 = int(idx1)
        frac1 = f32(idx1 - float(i1))
        lut1 = f32(f32((1.0 - frac1) * lut[base_idx + i1]) + f32(frac1 * lut[base_idx + i1 + 1]))

        w1 = f32(lut1 * c.driveDepth)
        buf1F30 = f32(w1 * c.preCoef1)

        u2 = f32(f32(x3 + w1) - (-1.0))
        idx2 = f32(u2 * 8191.5)
        if idx2 > 16383.0: idx2 = 16383.0
        elif idx2 < 0.0:   idx2 = 0.0
        i2 = int(idx2)
        frac2 = f32(idx2 - float(i2))
        buf2F2C = f32(f32((1.0 - frac2) * lut[base_idx + i2]) + f32(frac2 * lut[base_idx + i2 + 1]))

        z0 = x3
        z1 = f32(f32(z1 * fb_coeff) + f32(f32(buf1F30 + buf2F2C) * c.preDiffCoef))
        buf0F2C = f32(f32(buf1F30 * c.postMix1) + f32(z1 * c.postG))
        wet_harm = f32(buf0F2C - f32(x3 * post_sub))
        y_out = f32(f32(x3 + wet_harm) * c.postA1)
        out.append(y_out)
    return out

def write_wav_f32(filename, samples, sr=48000):
    n = len(samples)
    data_bytes = struct.pack(f"<{n}f", *samples)
    with open(filename, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data_bytes)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 3, 1, sr, sr * 4, 4, 32))
        f.write(b"data")
        f.write(struct.pack("<I", len(data_bytes)))
        f.write(data_bytes)

print(f"HEAT.aaxplugin SHA256: {aax_sha256}")
print(f"Ocelot 32768-float LUT SHA256: {lut_sha256}")

# Generate 1 second (48,000 samples in 1024-sample blocks) of test audio & null WAVs for Drive=+3, Tone=+2
sr = 48000
total_samples = 48000
block_size = 1024
random.seed(2026)
full_in = [f32(math.sin(2.0 * math.pi * 110.0 * i / sr) * 0.45 +
               math.sin(2.0 * math.pi * 880.0 * i / sr) * 0.25 +
               (random.random() * 2.0 - 1.0) * 0.08) for i in range(total_samples)]

coefs = make_coefs(3, 2, sr)
state_aax = (ctypes.c_float * 2)(0.0, 0.0)
meter_val = ctypes.c_float(0.0)
p_meter = ctypes.cast(ctypes.pointer(meter_val), ctypes.c_void_p)
full_aax = []

for offset in range(0, total_samples, block_size):
    chunk = full_in[offset:offset+block_size]
    n = len(chunk)
    in_b = (ctypes.c_float * n)(*chunk)
    out_b = (ctypes.c_float * n)()
    p_in = ctypes.cast(in_b, ctypes.c_void_p)
    p_out = ctypes.cast(out_b, ctypes.c_void_p)
    num_s = ctypes.c_int32(n)
    ctx = AlgContext(ctypes.addressof(p_in), ctypes.addressof(p_out), ctypes.addressof(num_s),
                     ctypes.addressof(coefs), ctypes.addressof(state_aax), ctypes.addressof(p_meter))
    ctx_ptr = ctypes.c_void_p(ctypes.addressof(ctx))
    ALG_ENTRY(ctypes.byref(ctx_ptr), ctypes.byref(ctx_ptr, 8))
    full_aax.extend(out_b[i] for i in range(n))

full_fl = run_fl_heat_1x(full_in, coefs)
full_null = [f32(full_aax[i] - full_fl[i]) for i in range(total_samples)]

write_wav_f32(os.path.join(out_dir, "01_ProTools_HEAT_aaxplugin_Raw.wav"), full_aax, sr)
write_wav_f32(os.path.join(out_dir, "02_FL_HEAT_1x_NativePT.wav"), full_fl, sr)
write_wav_f32(os.path.join(out_dir, "03_NULL_DIFFERENCE_PhaseInverted.wav"), full_null, sr)

max_err = max(abs(x) for x in full_null)
print(f"48,000-sample WAV Null Test Max Error: {max_err:.8e} (0.0 = -inf dBFS)")
