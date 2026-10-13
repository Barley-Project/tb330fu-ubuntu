#!/usr/bin/env python3
"""Offscreen GLES2 smoke test through EGL surfaceless (the render node, no display needed): draws a full-screen quad with a loop-heavy fragment shader into
a 1024x1024 FBO, reads one pixel back and prints the renderer and timings.  usage: [MESA_LOADER_DRIVER_OVERRIDE=panfrost] python3 gltest.py [loops] [frames]"""
import ctypes
import sys
import time

loops = int(sys.argv[1]) if len(sys.argv) > 1 else 100
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 20
egl = ctypes.CDLL("libEGL.so.1")
gl = ctypes.CDLL("libGLESv2.so.2")
vp = ctypes.c_void_p
egl.eglGetPlatformDisplay.restype = vp
egl.eglGetPlatformDisplay.argtypes = [ctypes.c_uint, vp, vp]
egl.eglInitialize.argtypes = [vp, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
egl.eglChooseConfig.argtypes = [vp, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(vp), ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
egl.eglCreateContext.restype = vp
egl.eglCreateContext.argtypes = [vp, vp, vp, ctypes.POINTER(ctypes.c_int)]
egl.eglMakeCurrent.argtypes = [vp, vp, vp, vp]
import os
if os.environ.get("GBM_NODE"):
    gbm = ctypes.CDLL("libgbm.so.1")
    gbm.gbm_create_device.restype = vp
    gbm.gbm_create_device.argtypes = [ctypes.c_int]
    fd = os.open(os.environ["GBM_NODE"], os.O_RDWR)
    gdev = gbm.gbm_create_device(fd)
    gbm.gbm_device_get_backend_name.restype = ctypes.c_char_p
    gbm.gbm_device_get_backend_name.argtypes = [vp]
    print("gbm device", gdev, "backend", gbm.gbm_device_get_backend_name(gdev))
    dpy = egl.eglGetPlatformDisplay(0x31D7, gdev, None)  # EGL_PLATFORM_GBM_KHR
else:
    dpy = egl.eglGetPlatformDisplay(0x31DD, None, None)  # EGL_PLATFORM_SURFACELESS_MESA
maj, mnr = ctypes.c_int(), ctypes.c_int()
if not egl.eglInitialize(dpy, ctypes.byref(maj), ctypes.byref(mnr)):
    sys.exit("eglInitialize failed")
egl.eglBindAPI(0x30A0)
cfg = vp()
n = ctypes.c_int()
attr = (ctypes.c_int * 3)(0x3040, 4, 0x3038)  # RENDERABLE_TYPE = ES2_BIT
egl.eglChooseConfig(dpy, attr, ctypes.byref(cfg), 1, ctypes.byref(n))
cattr = (ctypes.c_int * 3)(0x3098, 2, 0x3038)
ctx = egl.eglCreateContext(dpy, cfg, None, cattr)
if not egl.eglMakeCurrent(dpy, None, None, ctx):
    sys.exit("eglMakeCurrent failed")
gl.glGetString.restype = ctypes.c_char_p
print("GL_RENDERER:", gl.glGetString(0x1F01).decode(), "| GL_VERSION:", gl.glGetString(0x1F02).decode())
W = H = 1024
gl.glGetError.restype = ctypes.c_uint


def shader(kind, src):
    s = gl.glCreateShader(kind)
    b = ctypes.c_char_p(src.encode())
    gl.glShaderSource(s, 1, ctypes.byref(b), None)
    gl.glCompileShader(s)
    ok = ctypes.c_int()
    gl.glGetShaderiv(s, 0x8B81, ctypes.byref(ok))
    if not ok.value:
        log = ctypes.create_string_buffer(2048)
        gl.glGetShaderInfoLog(s, 2048, None, log)
        sys.exit("shader error: " + log.value.decode())
    return s


vs = shader(0x8B31, "attribute vec2 p; varying vec2 uv; void main(){ uv = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0);} ")
fs = shader(0x8B30, "precision mediump float; varying vec2 uv; void main(){ float a = uv.x; float b = uv.y; for (int i = 0; i < %d; i++) { a = fract(a*1.7 + b); b = fract(b*1.3 + a); } gl_FragColor = vec4(a, b, 0.25, 1.0);} " % loops)
prog = gl.glCreateProgram()
gl.glAttachShader(prog, vs)
gl.glAttachShader(prog, fs)
gl.glBindAttribLocation(prog, 0, b"p")
gl.glLinkProgram(prog)
ok = ctypes.c_int()
gl.glGetProgramiv(prog, 0x8B82, ctypes.byref(ok))
if not ok.value:
    sys.exit("link failed")
gl.glUseProgram(prog)
tex = ctypes.c_uint()
gl.glGenTextures(1, ctypes.byref(tex))
gl.glBindTexture(0x0DE1, tex)
gl.glTexImage2D(0x0DE1, 0, 0x1908, W, H, 0, 0x1908, 0x1401, None)
gl.glTexParameteri(0x0DE1, 0x2801, 0x2600)
gl.glTexParameteri(0x0DE1, 0x2800, 0x2600)
fbo = ctypes.c_uint()
gl.glGenFramebuffers(1, ctypes.byref(fbo))
gl.glBindFramebuffer(0x8D40, fbo)
gl.glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0)
st = gl.glCheckFramebufferStatus(0x8D40)
print("framebuffer status 0x%x" % st)
quad = (ctypes.c_float * 8)(-1, -1, 1, -1, -1, 1, 1, 1)
gl.glEnableVertexAttribArray(0)
gl.glVertexAttribPointer(0, 2, 0x1406, 0, 0, quad)
gl.glViewport(0, 0, W, H)
t0 = time.time()
for i in range(frames):
    gl.glDrawArrays(0x0005, 0, 4)
gl.glFinish()
dt = time.time() - t0
px = (ctypes.c_ubyte * 4)()
gl.glReadPixels(W // 2, H // 2, 1, 1, 0x1908, 0x1401, px)
print("%d frames of %dx%d with %d shader loops: %.3f s (%.1f Mpix/s), GL error 0x%x, centre pixel %s" % (frames, W, H, loops, dt, frames * W * H / dt / 1e6, gl.glGetError(), list(px)))
