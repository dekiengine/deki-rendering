// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

#include "GLApi.h"

#include <deki/LogSystem.h>

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace DekiGL
{

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Windows: WGL on a hidden window, desktop OpenGL 3.3 core.
//
// WGL cannot make a context without a window, and only a context can hand
// out wglCreateContextAttribsARB, so a legacy context is made first, used to
// fetch it, and replaced by a 3.3 core one. opengl32.dll and gdi32.dll are
// loaded by name: the package then links nothing beyond what every Windows
// program does.
// ---------------------------------------------------------------------------

namespace
{
constexpr int kWglContextMajorVersion = 0x2091;
constexpr int kWglContextMinorVersion = 0x2092;
constexpr int kWglContextProfileMask = 0x9126;
constexpr int kWglContextCoreProfileBit = 0x0001;
constexpr const char* kWindowClass = "DekiGLOffscreen";

using CreateContextFn = HGLRC(WINAPI*)(HDC);
using DeleteContextFn = BOOL(WINAPI*)(HGLRC);
using MakeCurrentFn = BOOL(WINAPI*)(HDC, HGLRC);
using GetCurrentContextFn = HGLRC(WINAPI*)();
using GetProcAddressFn = PROC(WINAPI*)(LPCSTR);
using CreateContextAttribsFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using ChoosePixelFormatFn = int(WINAPI*)(HDC, const PIXELFORMATDESCRIPTOR*);
using SetPixelFormatFn = BOOL(WINAPI*)(HDC, int, const PIXELFORMATDESCRIPTOR*);
}  // namespace

struct GLContext::Native
{
    HMODULE opengl32 = nullptr;
    HMODULE gdi32 = nullptr;
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC context = nullptr;

    CreateContextFn createContext = nullptr;
    DeleteContextFn deleteContext = nullptr;
    MakeCurrentFn makeCurrent = nullptr;
    GetCurrentContextFn getCurrentContext = nullptr;
    GetProcAddressFn getProcAddress = nullptr;

    // wglGetProcAddress answers for extensions and everything past GL 1.1;
    // the 1.1 functions come from opengl32.dll itself. Some drivers return
    // small integers instead of null for "no".
    void* Load(const char* name) const
    {
        const PROC p = getProcAddress(name);
        const intptr_t v = reinterpret_cast<intptr_t>(p);
        if (p && v != 1 && v != 2 && v != 3 && v != -1)
        {
            return reinterpret_cast<void*>(p);
        }
        return reinterpret_cast<void*>(GetProcAddress(opengl32, name));
    }

    void Release()
    {
        if (context)
        {
            if (getCurrentContext() == context)
            {
                makeCurrent(nullptr, nullptr);
            }
            deleteContext(context);
            context = nullptr;
        }
        if (dc)
        {
            ReleaseDC(window, dc);
            dc = nullptr;
        }
        if (window)
        {
            DestroyWindow(window);
            window = nullptr;
        }
        if (opengl32)
        {
            FreeLibrary(opengl32);
            opengl32 = nullptr;
        }
        if (gdi32)
        {
            FreeLibrary(gdi32);
            gdi32 = nullptr;
        }
    }
};

namespace
{

bool Fail(GLContext::Native* native, const char* why)
{
    DEKI_LOG_WARNING("OpenGL: no OpenGL context (%s); drawing stays on the CPU", why);
    native->Release();
    return false;
}

bool Open(GLContext::Native* n)
{
    n->opengl32 = LoadLibraryA("opengl32.dll");
    n->gdi32 = LoadLibraryA("gdi32.dll");
    if (!n->opengl32 || !n->gdi32)
    {
        return Fail(n, "opengl32.dll or gdi32.dll did not load");
    }
    n->createContext = reinterpret_cast<CreateContextFn>(GetProcAddress(n->opengl32, "wglCreateContext"));
    n->deleteContext = reinterpret_cast<DeleteContextFn>(GetProcAddress(n->opengl32, "wglDeleteContext"));
    n->makeCurrent = reinterpret_cast<MakeCurrentFn>(GetProcAddress(n->opengl32, "wglMakeCurrent"));
    n->getCurrentContext = reinterpret_cast<GetCurrentContextFn>(GetProcAddress(n->opengl32, "wglGetCurrentContext"));
    n->getProcAddress = reinterpret_cast<GetProcAddressFn>(GetProcAddress(n->opengl32, "wglGetProcAddress"));
    const auto choosePixelFormat = reinterpret_cast<ChoosePixelFormatFn>(GetProcAddress(n->gdi32, "ChoosePixelFormat"));
    const auto setPixelFormat = reinterpret_cast<SetPixelFormatFn>(GetProcAddress(n->gdi32, "SetPixelFormat"));
    if (!n->createContext || !n->deleteContext || !n->makeCurrent || !n->getCurrentContext || !n->getProcAddress ||
        !choosePixelFormat || !setPixelFormat)
    {
        return Fail(n, "WGL entry points missing");
    }

    // The class names DefWindowProc, which lives in user32, so a class left
    // registered after this package unloads points at nothing of ours.
    const HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return Fail(n, "RegisterClass failed");
    }
    n->window = CreateWindowExA(0, kWindowClass, "", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
    n->dc = n->window ? GetDC(n->window) : nullptr;
    if (!n->dc)
    {
        return Fail(n, "no hidden window");
    }

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int format = choosePixelFormat(n->dc, &pfd);
    if (format == 0 || !setPixelFormat(n->dc, format, &pfd))
    {
        return Fail(n, "no pixel format");
    }

    const HGLRC legacy = n->createContext(n->dc);
    if (!legacy || !n->makeCurrent(n->dc, legacy))
    {
        if (legacy)
        {
            n->deleteContext(legacy);
        }
        return Fail(n, "wglCreateContext failed");
    }
    const auto createAttribs =
        reinterpret_cast<CreateContextAttribsFn>(n->getProcAddress("wglCreateContextAttribsARB"));
    const int attribs[] = { kWglContextMajorVersion,
                            3,
                            kWglContextMinorVersion,
                            3,
                            kWglContextProfileMask,
                            kWglContextCoreProfileBit,
                            0 };
    n->context = createAttribs ? createAttribs(n->dc, nullptr, attribs) : nullptr;
    n->makeCurrent(nullptr, nullptr);
    n->deleteContext(legacy);
    if (!n->context || !n->makeCurrent(n->dc, n->context))
    {
        return Fail(n, "the driver has no OpenGL 3.3 core");
    }
    return true;
}

}  // namespace

std::unique_ptr<GLContext> GLContext::Create()
{
    std::unique_ptr<GLContext> ctx(new GLContext());
    ctx->m_Native = new Native();
    if (!Open(ctx->m_Native))
    {
        return nullptr;
    }
    ctx->m_IsES = false;
    auto load = [](const char* name, void* user) { return static_cast<Native*>(user)->Load(name); };
    if (!ctx->LoadFunctions(load, ctx->m_Native))
    {
        return nullptr;
    }
    return ctx;
}

namespace
{
bool MakeNativeCurrent(GLContext::Native* n)
{
    if (!n->context)
    {
        return false;
    }
    if (n->getCurrentContext() == n->context)
    {
        return true;
    }
    return n->makeCurrent(n->dc, n->context) != FALSE;
}
}  // namespace

#elif defined(__linux__)

// ---------------------------------------------------------------------------
// Linux: EGL, OpenGL ES 3.0. The same code on a desktop (Mesa) and on a
// handheld's Mali, which is why the backend speaks ES there too.
//
// A pbuffer when the display has one, otherwise no surface at all
// (EGL_KHR_surfaceless_context), and Mesa's surfaceless platform when there
// is no default display, as on a machine with no X or Wayland session. The
// backend draws into its own framebuffer object either way.
// ---------------------------------------------------------------------------

namespace
{
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLint = int32_t;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;

constexpr EGLint kEglNone = 0x3038;
constexpr EGLint kEglAlphaSize = 0x3021;
constexpr EGLint kEglBlueSize = 0x3022;
constexpr EGLint kEglGreenSize = 0x3023;
constexpr EGLint kEglRedSize = 0x3024;
constexpr EGLint kEglSurfaceType = 0x3033;
constexpr EGLint kEglRenderableType = 0x3040;
constexpr EGLint kEglHeight = 0x3056;
constexpr EGLint kEglWidth = 0x3057;
constexpr EGLint kEglContextClientVersion = 0x3098;
constexpr EGLint kEglPbufferBit = 0x0001;
constexpr EGLint kEglOpenGLES3Bit = 0x0040;
constexpr EGLenum kEglOpenGLESApi = 0x30A0;
constexpr EGLenum kEglPlatformSurfacelessMesa = 0x31DD;

using GetProcAddressFn = void* (*)(const char*);
using GetDisplayFn = EGLDisplay (*)(void*);
using GetPlatformDisplayFn = EGLDisplay (*)(EGLenum, void*, const EGLint*);
using InitializeFn = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using BindApiFn = EGLBoolean (*)(EGLenum);
using ChooseConfigFn = EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
using CreateContextFn = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
using CreatePbufferSurfaceFn = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*);
using MakeCurrentFn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using GetCurrentContextFn = EGLContext (*)();
using DestroyContextFn = EGLBoolean (*)(EGLDisplay, EGLContext);
using DestroySurfaceFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using TerminateFn = EGLBoolean (*)(EGLDisplay);
}  // namespace

struct GLContext::Native
{
    void* egl = nullptr;
    void* gles = nullptr;
    EGLDisplay display = nullptr;
    EGLContext context = nullptr;
    EGLSurface surface = nullptr;

    GetProcAddressFn getProcAddress = nullptr;
    MakeCurrentFn makeCurrent = nullptr;
    GetCurrentContextFn getCurrentContext = nullptr;
    DestroyContextFn destroyContext = nullptr;
    DestroySurfaceFn destroySurface = nullptr;
    TerminateFn terminate = nullptr;

    // eglGetProcAddress need not answer for core functions before EGL 1.5;
    // libGLESv2 exports them.
    void* Load(const char* name) const
    {
        if (void* p = getProcAddress(name))
        {
            return p;
        }
        return gles ? dlsym(gles, name) : nullptr;
    }

    void Release()
    {
        if (display)
        {
            if (context && getCurrentContext() == context)
            {
                makeCurrent(display, nullptr, nullptr, nullptr);
            }
            if (context)
            {
                destroyContext(display, context);
            }
            if (surface)
            {
                destroySurface(display, surface);
            }
            terminate(display);
        }
        display = context = surface = nullptr;
        if (gles)
        {
            dlclose(gles);
            gles = nullptr;
        }
        if (egl)
        {
            dlclose(egl);
            egl = nullptr;
        }
    }
};

namespace
{

bool Fail(GLContext::Native* native, const char* why)
{
    DEKI_LOG_WARNING("OpenGL: no OpenGL ES context (%s); drawing stays on the CPU", why);
    native->Release();
    return false;
}

void* OpenFirst(const char* a, const char* b)
{
    void* lib = dlopen(a, RTLD_NOW | RTLD_LOCAL);
    return lib ? lib : dlopen(b, RTLD_NOW | RTLD_LOCAL);
}

bool Open(GLContext::Native* n)
{
    n->egl = OpenFirst("libEGL.so.1", "libEGL.so");
    n->gles = OpenFirst("libGLESv2.so.2", "libGLESv2.so");
    if (!n->egl)
    {
        return Fail(n, "libEGL did not load");
    }
    auto sym = [n](const char* name) { return dlsym(n->egl, name); };
    n->getProcAddress = reinterpret_cast<GetProcAddressFn>(sym("eglGetProcAddress"));
    const auto getDisplay = reinterpret_cast<GetDisplayFn>(sym("eglGetDisplay"));
    const auto initialize = reinterpret_cast<InitializeFn>(sym("eglInitialize"));
    const auto bindApi = reinterpret_cast<BindApiFn>(sym("eglBindAPI"));
    const auto chooseConfig = reinterpret_cast<ChooseConfigFn>(sym("eglChooseConfig"));
    const auto createContext = reinterpret_cast<CreateContextFn>(sym("eglCreateContext"));
    const auto createPbuffer = reinterpret_cast<CreatePbufferSurfaceFn>(sym("eglCreatePbufferSurface"));
    n->makeCurrent = reinterpret_cast<MakeCurrentFn>(sym("eglMakeCurrent"));
    n->getCurrentContext = reinterpret_cast<GetCurrentContextFn>(sym("eglGetCurrentContext"));
    n->destroyContext = reinterpret_cast<DestroyContextFn>(sym("eglDestroyContext"));
    n->destroySurface = reinterpret_cast<DestroySurfaceFn>(sym("eglDestroySurface"));
    n->terminate = reinterpret_cast<TerminateFn>(sym("eglTerminate"));
    if (!n->getProcAddress || !getDisplay || !initialize || !bindApi || !chooseConfig || !createContext ||
        !createPbuffer || !n->makeCurrent || !n->getCurrentContext || !n->destroyContext || !n->destroySurface ||
        !n->terminate)
    {
        return Fail(n, "EGL entry points missing");
    }

    EGLint major = 0, minor = 0;
    n->display = getDisplay(nullptr);
    if (!n->display || !initialize(n->display, &major, &minor))
    {
        const auto platformDisplay =
            reinterpret_cast<GetPlatformDisplayFn>(n->getProcAddress("eglGetPlatformDisplayEXT"));
        n->display = platformDisplay ? platformDisplay(kEglPlatformSurfacelessMesa, nullptr, nullptr) : nullptr;
        if (!n->display || !initialize(n->display, &major, &minor))
        {
            n->display = nullptr;
            return Fail(n, "no EGL display");
        }
    }
    if (!bindApi(kEglOpenGLESApi))
    {
        return Fail(n, "EGL has no OpenGL ES");
    }

    const EGLint withPbuffer[] = { kEglSurfaceType, kEglPbufferBit, kEglRenderableType, kEglOpenGLES3Bit,
                                   kEglRedSize,     8,              kEglGreenSize,      8,
                                   kEglBlueSize,    8,              kEglAlphaSize,      8,
                                   kEglNone };
    const EGLint anySurface[] = { kEglRenderableType, kEglOpenGLES3Bit, kEglRedSize,   8, kEglGreenSize, 8,
                                  kEglBlueSize,       8,                kEglAlphaSize, 8, kEglNone };
    EGLConfig config = nullptr;
    EGLint count = 0;
    bool pbuffer = chooseConfig(n->display, withPbuffer, &config, 1, &count) && count > 0;
    if (!pbuffer && !(chooseConfig(n->display, anySurface, &config, 1, &count) && count > 0))
    {
        return Fail(n, "no OpenGL ES 3 configuration");
    }

    const EGLint contextAttribs[] = { kEglContextClientVersion, 3, kEglNone };
    n->context = createContext(n->display, config, nullptr, contextAttribs);
    if (!n->context)
    {
        return Fail(n, "the driver has no OpenGL ES 3.0");
    }
    if (pbuffer)
    {
        const EGLint size[] = { kEglWidth, 1, kEglHeight, 1, kEglNone };
        n->surface = createPbuffer(n->display, config, size);
    }
    if (!n->makeCurrent(n->display, n->surface, n->surface, n->context))
    {
        return Fail(n, "eglMakeCurrent failed");
    }
    return true;
}

}  // namespace

std::unique_ptr<GLContext> GLContext::Create()
{
    std::unique_ptr<GLContext> ctx(new GLContext());
    ctx->m_Native = new Native();
    if (!Open(ctx->m_Native))
    {
        return nullptr;
    }
    ctx->m_IsES = true;
    auto load = [](const char* name, void* user) { return static_cast<Native*>(user)->Load(name); };
    if (!ctx->LoadFunctions(load, ctx->m_Native))
    {
        return nullptr;
    }
    return ctx;
}

namespace
{
bool MakeNativeCurrent(GLContext::Native* n)
{
    if (!n->context)
    {
        return false;
    }
    if (n->getCurrentContext() == n->context)
    {
        return true;
    }
    return n->makeCurrent(n->display, n->surface, n->surface, n->context) != 0;
}
}  // namespace

#else

// ---------------------------------------------------------------------------
// Anywhere else (macOS): no off-screen context yet. A display may still
// adopt its own window's context.
// ---------------------------------------------------------------------------

struct GLContext::Native
{
    void Release() {}
};

std::unique_ptr<GLContext> GLContext::Create()
{
    DEKI_LOG_INFO("OpenGL: no off-screen OpenGL context on this OS yet; drawing stays on the CPU");
    return nullptr;
}

namespace
{
bool MakeNativeCurrent(GLContext::Native*)
{
    return false;
}
}  // namespace

#endif

// ---------------------------------------------------------------------------
// Every platform
// ---------------------------------------------------------------------------

bool GLContext::LoadFunctions(void* (*load)(const char* name, void* user), void* user)
{
    bool complete = true;
#define DEKI_GL_LOAD(ret, name, args)                                              \
    m_GL.name = reinterpret_cast<decltype(m_GL.name)>(load("gl" #name, user)); \
    complete = complete && m_GL.name != nullptr;
    DEKI_GL_FUNCTIONS(DEKI_GL_LOAD)
#undef DEKI_GL_LOAD
    m_GL.ClearDepthf = reinterpret_cast<decltype(m_GL.ClearDepthf)>(load("glClearDepthf", user));
    m_GL.ClearDepth = reinterpret_cast<decltype(m_GL.ClearDepth)>(load("glClearDepth", user));
    m_GL.InvalidateFramebuffer =
        reinterpret_cast<decltype(m_GL.InvalidateFramebuffer)>(load("glInvalidateFramebuffer", user));
    if (!complete || (!m_GL.ClearDepthf && !m_GL.ClearDepth))
    {
        DEKI_LOG_WARNING("OpenGL: the OpenGL driver lacks functions Deki needs; drawing stays on the CPU");
        return false;
    }
    return true;
}

std::unique_ptr<GLContext> GLContext::Adopt(const Host& host)
{
    std::unique_ptr<GLContext> ctx(new GLContext());
    ctx->m_Host = host;
    ctx->m_IsES = host.isES;
    auto load = [](const char* name, void* user) { return static_cast<const Host*>(user)->load(name); };
    if (!ctx->LoadFunctions(load, &ctx->m_Host))
    {
        return nullptr;
    }
    return ctx;
}

GLContext::~GLContext()
{
    if (m_Native)
    {
        m_Native->Release();
        delete m_Native;
    }
}

bool GLContext::MakeCurrent()
{
    if (m_Lost)
    {
        return false;
    }
    if (m_Host.makeCurrent)
    {
        return m_Host.makeCurrent(m_Host.user);
    }
    return m_Native && MakeNativeCurrent(m_Native);
}

namespace
{
std::shared_ptr<GLContext> s_WindowContext;
std::shared_ptr<GLContext> s_Offscreen;
bool s_OffscreenFailed = false;
}  // namespace

std::shared_ptr<GLContext> SharedContext()
{
    if (s_WindowContext)
    {
        return s_WindowContext;
    }
    if (!s_Offscreen && !s_OffscreenFailed)
    {
        s_Offscreen = GLContext::Create();
        s_OffscreenFailed = !s_Offscreen;
    }
    return s_Offscreen;
}

void SetWindowContext(std::shared_ptr<GLContext> context)
{
    s_WindowContext = std::move(context);
}

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
