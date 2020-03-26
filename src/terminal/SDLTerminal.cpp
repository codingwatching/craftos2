/*
 * SDLTerminal.cpp
 * CraftOS-PC 2
 * 
 * This file implements the SDLTerminal class.
 * 
 * This code is licensed under the MIT license.
 * Copyright (c) 2019-2020 JackMacWindows.
 */

#define CRAFTOSPC_INTERNAL
#include "SDLTerminal.hpp"
#ifndef NO_PNG
#include <png++/png.hpp>
#endif
#include <sstream>
#include <assert.h>
#include "../favicon.h"
#include "../config.hpp"
#include "../gif.hpp"
#include "../os.hpp"
#include "../peripheral/monitor.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#define EMSCRIPTEN_ENABLED 1
#else
#define EMSCRIPTEN_ENABLED 0
#endif
#define rgb(color) ((color.r << 16) | (color.g << 8) | color.b)

extern "C" {
    struct font_image {
        unsigned int 	 width;
        unsigned int 	 height;
        unsigned int 	 bytes_per_pixel; /* 2:RGB16, 3:RGB, 4:RGBA */
        unsigned char	 pixel_data[128 * 175 * 2 + 1];
    };
    extern struct font_image font_image;
}

// from Terminal.hpp
Color defaultPalette[16] = {
    {0xf0, 0xf0, 0xf0},
    {0xf2, 0xb2, 0x33},
    {0xe5, 0x7f, 0xd8},
    {0x99, 0xb2, 0xf2},
    {0xde, 0xde, 0x6c},
    {0x7f, 0xcc, 0x19},
    {0xf2, 0xb2, 0xcc},
    {0x4c, 0x4c, 0x4c},
    {0x99, 0x99, 0x99},
    {0x4c, 0x99, 0xb2},
    {0xb2, 0x66, 0xe5},
    {0x33, 0x66, 0xcc},
    {0x7f, 0x66, 0x4c},
    {0x57, 0xa6, 0x4e},
    {0xcc, 0x4c, 0x4c},
    {0x11, 0x11, 0x11}
};

void MySDL_GetDisplayDPI(int displayIndex, float* dpi, float* defaultDpi)
{
    const float kSysDefaultDpi =
#ifdef __APPLE__
        72.0f;
#elif defined(_WIN32)
        96.0f;
#else
        96.0f;
#endif
 
    if (SDL_GetDisplayDPI(displayIndex, NULL, dpi, NULL) != 0)
    {
        // Failed to get DPI, so just return the default value.
        if (dpi) *dpi = kSysDefaultDpi;
    }
 
    if (defaultDpi) *defaultDpi = kSysDefaultDpi;
}

int SDLTerminal::fontScale = 2;
std::list<Terminal*> Terminal::renderTargets;
std::mutex Terminal::renderTargetsLock;
#ifdef __EMSCRIPTEN__
std::list<Terminal*>::iterator Terminal::renderTarget = Terminal::renderTargets.end();
SDL_Window *SDLTerminal::win = NULL;
int nextWindowID = 1;

extern "C" {
    void EMSCRIPTEN_KEEPALIVE nextRenderTarget() {
        if (++Terminal::renderTarget == Terminal::renderTargets.end()) Terminal::renderTarget = Terminal::renderTargets.begin();
        (*Terminal::renderTarget)->changed = true;
    }

    void EMSCRIPTEN_KEEPALIVE previousRenderTarget() {
        if (Terminal::renderTarget == Terminal::renderTargets.begin()) Terminal::renderTarget = Terminal::renderTargets.end();
        Terminal::renderTarget--;
        (*Terminal::renderTarget)->changed = true;
    }

    bool EMSCRIPTEN_KEEPALIVE selectRenderTarget(int id) {
        for (Terminal::renderTarget = Terminal::renderTargets.begin(); Terminal::renderTarget != Terminal::renderTargets.end(); Terminal::renderTarget++) if ((*Terminal::renderTarget)->id == id) break;
        (*Terminal::renderTarget)->changed = true;
        return Terminal::renderTarget != Terminal::renderTargets.end();
    }

    const char * EMSCRIPTEN_KEEPALIVE getRenderTargetName() {
        return (*Terminal::renderTarget)->title.c_str();
    }

    extern void syncfs();
}

void onWindowCreate(int id, const char * title) {EM_ASM({if (Module.windowEventListener !== undefined) Module.windowEventListener.onWindowCreate($0, $1);}, id, title);}
void onWindowDestroy(int id) {EM_ASM({if (Module.windowEventListener !== undefined) Module.windowEventListener.onWindowDestroy($0);}, id);}
#endif

SDLTerminal::SDLTerminal(std::string title): Terminal(51, 19) {
    this->title = title;
#ifdef __EMSCRIPTEN__
    dpiScale = emscripten_get_device_pixel_ratio();
#else
    float dpi, defaultDpi;
    MySDL_GetDisplayDPI(0, &dpi, &defaultDpi);
    dpiScale = (dpi / defaultDpi) - floor(dpi / defaultDpi) > 0.5 ? ceil(dpi / defaultDpi) : floor(dpi / defaultDpi);
#endif
    if (config.customFontPath == "hdfont") {
        fontScale = 1;
        charScale = 1;
        charWidth = fontWidth * 2/fontScale * charScale;
        charHeight = fontHeight * 2/fontScale * charScale;
    } else if (!config.customFontPath.empty()) {
        fontScale = config.customFontScale;
        charScale = fontScale;
        charWidth = fontWidth * 2/fontScale * charScale;
        charHeight = fontHeight * 2/fontScale * charScale;
    }
    if (config.customCharScale > 0) {
        charScale = config.customCharScale;
        charWidth = fontWidth * 2/fontScale * charScale;
        charHeight = fontHeight * 2/fontScale * charScale;
    }
#if defined(__EMSCRIPTEN__) && !defined(NO_EMSCRIPTEN_HIDPI)
    if (win == NULL)
#else
    dpiScale = 1;
#endif
    win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width*charWidth*dpiScale+(4 * charScale * (2 / fontScale)*dpiScale), height*charHeight*dpiScale+(4 * charScale * (2 / fontScale)*dpiScale), SDL_WINDOW_SHOWN | 
#if !(defined(__EMSCRIPTEN__) && defined(NO_EMSCRIPTEN_HIDPI))
    SDL_WINDOW_ALLOW_HIGHDPI |
#endif
    SDL_WINDOW_RESIZABLE | SDL_WINDOW_INPUT_FOCUS);
    if (win == nullptr || win == NULL || win == (SDL_Window*)0) {
        overridden = true;
        throw window_exception("Failed to create window");
    }
#ifndef __EMSCRIPTEN__
    id = SDL_GetWindowID(win);
#else
    id = nextWindowID++;
    onWindowCreate(id, title.c_str());
#endif
#if !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    char * icon_pixels = new char[favicon_width * favicon_height * 4];
    memset(icon_pixels, 0xFF, favicon_width * favicon_height * 4);
    const char * icon_data = header_data;
    for (unsigned i = 0; i < favicon_width * favicon_height; i++) HEADER_PIXEL(icon_data, (&icon_pixels[i*4]));
    SDL_Surface* icon = SDL_CreateRGBSurfaceFrom(icon_pixels, favicon_width, favicon_height, 32, favicon_width * 4, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    SDL_SetWindowIcon(win, icon);
    SDL_FreeSurface(icon);
    delete[] icon_pixels;
#endif
    SDL_Surface* old_bmp;
    if (config.customFontPath.empty()) 
        old_bmp = SDL_CreateRGBSurfaceWithFormatFrom((void*)font_image.pixel_data, font_image.width, font_image.height, font_image.bytes_per_pixel * 8, font_image.bytes_per_pixel * font_image.width, SDL_PIXELFORMAT_RGB565);
#ifndef STANDALONE_ROM
    else if (config.customFontPath == "hdfont") old_bmp = SDL_LoadBMP((getROMPath() + "/hdfont.bmp").c_str());
#endif
    else old_bmp = SDL_LoadBMP(config.customFontPath.c_str());
    if (old_bmp == nullptr || old_bmp == NULL || old_bmp == (SDL_Surface*)0) {
#ifndef __EMSCRIPTEN__
        SDL_DestroyWindow(win);
#endif
        overridden = true;
        throw window_exception("Failed to load font");
    }
    bmp = SDL_ConvertSurfaceFormat(old_bmp, SDL_PIXELFORMAT_RGBA32, 0);
    if (bmp == nullptr || bmp == NULL || bmp == (SDL_Surface*)0) {
#ifndef __EMSCRIPTEN__
        SDL_DestroyWindow(win);
#endif
        overridden = true;
        throw window_exception("Failed to convert font");
    }
    SDL_FreeSurface(old_bmp);
    SDL_SetColorKey(bmp, SDL_TRUE, SDL_MapRGB(bmp->format, 0, 0, 0));
    renderTargets.push_back(this);
#ifdef __EMSCRIPTEN__
    if (renderTargets.size() == 1) renderTarget = renderTargets.begin();
#endif
}

SDLTerminal::~SDLTerminal() {
#ifdef __EMSCRIPTEN__
    onWindowDestroy(id);
#endif
    Terminal::renderTargetsLock.lock();
    std::lock_guard<std::mutex> locked_g(locked);
    for (auto it = renderTargets.begin(); it != renderTargets.end(); it++) {
        if (*it == this)
            it = renderTargets.erase(it);
        if (it == renderTargets.end()) break;
    }
    Terminal::renderTargetsLock.unlock();
    if (!overridden) {
        if (surf != NULL) SDL_FreeSurface(surf);
        SDL_FreeSurface(bmp);
#ifndef __EMSCRIPTEN__
        SDL_DestroyWindow(win);
#endif
    }
}

void SDLTerminal::setPalette(Color * p) {
    for (int i = 0; i < 16; i++) palette[i] = p[i];
}

void SDLTerminal::setCharScale(int scale) {
    if (scale < 1) scale = 1;
    charScale = scale;
    charWidth = fontWidth * (2/fontScale) * charScale;
    charHeight = fontHeight * (2/fontScale) * charScale;
    SDL_SetWindowSize(win, width*charWidth+(4 * charScale), height*charHeight+(4 * charScale));
}

bool operator!=(Color lhs, Color rhs) {
    return lhs.r != rhs.r || lhs.g != rhs.g || lhs.b != rhs.b;
}

bool SDLTerminal::drawChar(unsigned char c, int x, int y, Color fg, Color bg, bool transparent) {
    SDL_Rect srcrect = getCharacterRect(c);
    SDL_Rect destrect = {
        x * charWidth * dpiScale + 2 * charScale * 2/fontScale * dpiScale, 
        y * charHeight * dpiScale + 2 * charScale * 2/fontScale * dpiScale, 
        fontWidth * 2/fontScale * charScale * dpiScale, 
        fontHeight * 2/fontScale * charScale * dpiScale
    };
    if (!transparent && bg != palette[15]) {
        if (gotResizeEvent) return false;
        if (SDL_FillRect(surf, &destrect, rgb(bg)) != 0) return false;
    }
    if (c != ' ' && c != '\0') {
        if (gotResizeEvent) return false;
        if (SDL_SetSurfaceColorMod(bmp, fg.r, fg.g, fg.b) != 0) return false;
        if (gotResizeEvent) return false;
        if (SDL_BlitScaled(bmp, &srcrect, surf, &destrect) != 0) return false;
    }
    return true;
}

SDL_Rect * setRect(SDL_Rect * rect, int x, int y, int w, int h) {
    rect->x = x;
    rect->y = y;
    rect->w = w;
    rect->h = h;
    return rect;
}

static unsigned char circlePix[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

void SDLTerminal::render() {
    std::lock_guard<std::mutex> locked_g(locked);
    if (gotResizeEvent) {
        gotResizeEvent = false;
        this->screen.resize(newWidth, newHeight, ' ');
        this->colors.resize(newWidth, newHeight, 0xF0);
        this->pixels.resize(newWidth * fontWidth, newHeight * fontHeight, 0x0F);
        this->width = newWidth;
        this->height = newHeight;
        changed = true;
    }
    if (!changed && !shouldScreenshot && !shouldRecord) return;
    changed = false;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    if (surf != NULL) SDL_FreeSurface(surf);
    surf = SDL_CreateRGBSurfaceWithFormat(0, ww, wh, 24, SDL_PIXELFORMAT_RGB888);
    if (surf == NULL) {
        printf("Could not allocate renderer: %s\n", SDL_GetError());
        return;
    }
    SDL_Rect rect;
    if (gotResizeEvent || SDL_FillRect(surf, NULL, mode == 0 ? rgb(palette[15]) : rgb(defaultPalette[15])) != 0) return;
    if (mode != 0) {
        for (int y = 0; y < height * charHeight; y+=(2/fontScale)*charScale) {
            for (int x = 0; x < width * charWidth; x+=(2/fontScale)*charScale) {
                unsigned char c = pixels[y / (2/fontScale) / charScale][x / (2/fontScale) / charScale];
                if (gotResizeEvent) return;
                if (SDL_FillRect(surf, setRect(&rect, x + (2 * (2/fontScale) * charScale), y + (2 * (2/fontScale) * charScale), (2/fontScale) * charScale, (2/fontScale) * charScale), rgb(palette[(int)c])) != 0) return;
            }
        }
    } else {
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) 
            if (gotResizeEvent || !drawChar(screen[y][x], x, y, palette[colors[y][x] & 0x0F], palette[colors[y][x] >> 4])) return;
        if (gotResizeEvent) return;
        if (blink && blinkX >= 0 && blinkY >= 0 && blinkX < width && blinkY < height) if (!drawChar('_', blinkX, blinkY, palette[0], palette[colors[blinkY][blinkX] >> 4], true)) return;
    }
    currentFPS++;
    if (lastSecond != time(0)) {
        lastSecond = time(0);
        lastFPS = currentFPS;
        currentFPS = 0;
    }
    if (/*showFPS*/ false) {
        // later?
    }
    if (shouldScreenshot) {
        shouldScreenshot = false;
        if (gotResizeEvent) return;
#ifdef PNGPP_PNG_HPP_INCLUDED
        SDL_Surface * temp = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGB24, 0);
        if (screenshotPath == "clipboard") {
            copyImage(temp);
        } else {
            png::solid_pixel_buffer<png::rgb_pixel> pixbuf(temp->w, temp->h);
            memcpy((void*)&pixbuf.get_bytes()[0], temp->pixels, temp->h * temp->pitch);
            png::image<png::rgb_pixel, png::solid_pixel_buffer<png::rgb_pixel> > img(temp->w, temp->h);
            img.set_pixbuf(pixbuf);
            img.write(screenshotPath);
        }
        SDL_FreeSurface(temp);
#else
        SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGB888, 0);
        SDL_SaveBMP(conv, screenshotPath.c_str());
        SDL_FreeSurface(conv);
#endif
#ifdef __EMSCRIPTEN__
        queueTask([](void*)->void*{syncfs(); return NULL;}, NULL, true);
#endif
    }
    if (shouldRecord) {
        if (recordedFrames >= config.maxRecordingTime * config.recordingFPS) stopRecording();
        else if (--frameWait < 1) {
            recorderMutex.lock();
            uint32_t uw = static_cast<uint32_t>(surf->w), uh = static_cast<uint32_t>(surf->h);
            std::string rle = std::string((char*)&uw, 4) + std::string((char*)&uh, 4);
            SDL_Surface * temp = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
            uint32_t * px = ((uint32_t*)temp->pixels);
            uint32_t data = px[0] & 0xFFFFFF;
            for (int y = 0; y < surf->h; y++) {
                for (int x = 0; x < surf->w; x++) {
                    uint32_t p = px[y*surf->w+x];
                    if ((p & 0xFFFFFF) != (data & 0xFFFFFF) || (data & 0xFF000000) == 0xFF000000) {
                        rle += std::string((char*)&data, 4);
                        data = p & 0xFFFFFF;
                    } else data += 0x1000000;
                }
            }
            rle += std::string((char*)&data, 4);
            SDL_FreeSurface(temp);
            recording.push_back(rle);
            recordedFrames++;
            frameWait = config.clockSpeed / config.recordingFPS;
            recorderMutex.unlock();
            if (gotResizeEvent) return;
        }
        SDL_Surface* circle = SDL_CreateRGBSurfaceWithFormatFrom(circlePix, 10, 10, 32, 40, SDL_PIXELFORMAT_BGRA32);
        if (circle == NULL) { printf("Error: %s\n", SDL_GetError()); assert(false); }
        if (gotResizeEvent) return;
        if (SDL_BlitSurface(circle, NULL, surf, setRect(&rect, (width * charWidth * dpiScale + 2 * charScale * (2/fontScale) * dpiScale) - 10, 2 * charScale * (2/fontScale) * dpiScale, 10, 10)) != 0) return;
        SDL_FreeSurface(circle);
    }
    /*if (gotResizeEvent) return;
#ifdef __linux__
    queueTask([ ](void* arg)->void*{SDL_UpdateWindowSurface((SDL_Window*)arg); return NULL;}, win);
#else
    if (SDL_UpdateWindowSurface(win) != 0) {
        printf("Error rendering: %s\n", SDL_GetError());
        surf = SDL_GetWindowSurface(win);
    }
#endif*/
}

void convert_to_renderer_coordinates(SDL_Renderer *renderer, int *x, int *y) {
    SDL_Rect viewport;
    float scale_x, scale_y;
    SDL_RenderGetViewport(renderer, &viewport);
    SDL_RenderGetScale(renderer, &scale_x, &scale_y);
    *x = (int) (*x / scale_x) - viewport.x;
    *y = (int) (*y / scale_y) - viewport.y;
}

void SDLTerminal::getMouse(int *x, int *y) {
    SDL_GetMouseState(x, y);
    //convert_to_renderer_coordinates(ren, x, y);
}

SDL_Rect SDLTerminal::getCharacterRect(unsigned char c) {
    SDL_Rect retval;
    retval.w = fontWidth * 2/fontScale;
    retval.h = fontHeight * 2/fontScale;
    retval.x = ((fontWidth + 2) * 2/fontScale)*(c & 0x0F)+2/fontScale;
    retval.y = ((fontHeight + 2) * 2/fontScale)*(c >> 4)+2/fontScale;
    return retval;
}

bool SDLTerminal::resize(int w, int h) {
    newWidth = w;
    newHeight = h;
    gotResizeEvent = (newWidth != width || newHeight != height);
    if (!gotResizeEvent) return false;
    while (gotResizeEvent) std::this_thread::yield();
    return true;
}

void SDLTerminal::screenshot(std::string path) {
    shouldScreenshot = true;
    if (path != "") screenshotPath = path;
    else {
        time_t now = time(0);
        struct tm * nowt = localtime(&now);
        screenshotPath = getBasePath();
#ifdef WIN32
        screenshotPath += "\\screenshots\\";
#else
        screenshotPath += "/screenshots/";
#endif
        createDirectory(screenshotPath.c_str());
        char * tstr = new char[24];
        strftime(tstr, 24, "%F_%H.%M.%S", nowt);
#ifdef NO_PNG
        screenshotPath += std::string(tstr) + ".bmp";
#else
        screenshotPath += std::string(tstr) + ".png";
#endif
        delete[] tstr;
    }
}

void SDLTerminal::record(std::string path) {
    shouldRecord = true;
    recordedFrames = 0;
    frameWait = 0;
    if (path != "") recordingPath = path;
    else {
        time_t now = time(0);
        struct tm * nowt = localtime(&now);
        recordingPath = getBasePath();
#ifdef WIN32
        recordingPath += "\\screenshots\\";
#else
        recordingPath += "/screenshots/";
#endif
        createDirectory(recordingPath.c_str());
        char * tstr = new char[20];
        strftime(tstr, 24, "%F_%H.%M.%S", nowt);
        recordingPath += std::string(tstr) + ".gif";
        delete[] tstr;
    }
}

uint32_t *memset_int(uint32_t *ptr, uint32_t value, size_t num) {
    for (size_t i = 0; i < num; i++) memcpy(&ptr[i], &value, 4);
    return &ptr[num];
}

void SDLTerminal::stopRecording() {
    shouldRecord = false;
    recorderMutex.lock();
    if (recording.size() < 1) { recorderMutex.unlock(); return; }
    GifWriter g;
    GifBegin(&g, recordingPath.c_str(), ((uint32_t*)(&recording[0][0]))[0], ((uint32_t*)(&recording[0][0]))[1], 100 / config.recordingFPS);
    for (std::string s : recording) {
        uint32_t w = ((uint32_t*)&s[0])[0], h = ((uint32_t*)&s[0])[1];
        uint32_t* ipixels = new uint32_t[w * h];
        uint32_t* lp = ipixels;
        for (unsigned i = 2; i*4 < s.size(); i++) {
            uint32_t c = ((uint32_t*)&s[0])[i];
            lp = memset_int(lp, c & 0xFFFFFF, ((c & 0xFF000000) >> 24) + 1);
        }
        GifWriteFrame(&g, (uint8_t*)ipixels, w, h, 100 / config.recordingFPS);
        delete[] ipixels;
    }
    GifEnd(&g);
    recording.clear();
    recorderMutex.unlock();
#ifdef __EMSCRIPTEN__
    queueTask([](void*)->void*{syncfs(); return NULL;}, NULL, true);
#endif
}

void SDLTerminal::showMessage(Uint32 flags, const char * title, const char * message) {SDL_ShowSimpleMessageBox(flags, title, message, win);}

void SDLTerminal::toggleFullscreen() {
    fullscreen = !fullscreen;
    if (fullscreen) queueTask([ ](void* param)->void*{SDL_SetWindowFullscreen((SDL_Window*)param, SDL_WINDOW_FULLSCREEN_DESKTOP); return NULL;}, win);
    else queueTask([ ](void* param)->void*{SDL_SetWindowFullscreen((SDL_Window*)param, 0); return NULL;}, win);
}

void SDLTerminal::setLabel(std::string label) {
    title = label;
    queueTask([label](void*win)->void*{SDL_SetWindowTitle((SDL_Window*)win, label.c_str()); return NULL;}, win, true);
}

extern Uint32 task_event_type, render_event_type;
extern std::thread * renderThread;
extern void termRenderLoop();

void SDLTerminal::init() {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_SetHint(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
#if SDL_VERSION_ATLEAST(2, 0, 8)
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    task_event_type = SDL_RegisterEvents(2);
    render_event_type = task_event_type + 1;
    renderThread = new std::thread(termRenderLoop);
}

void SDLTerminal::quit() {
    renderThread->join();
    delete renderThread;
    SDL_Quit();
}

extern std::queue< std::tuple<int, std::function<void*(void*)>, void*, bool> > taskQueue;
extern std::unordered_map<int, void*> taskQueueReturns;
extern std::mutex taskQueueReturnsMutex;
extern monitor * findMonitorFromWindowID(Computer *comp, unsigned id, std::string& sideReturn);

extern bool rawClient;
extern void sendRawEvent(SDL_Event e);

#ifdef __EMSCRIPTEN__
#define checkWindowID(c, wid) (c->term == *SDLTerminal::renderTarget || findMonitorFromWindowID(c, (*SDLTerminal::renderTarget)->id, tmps) != NULL)
#else
#define checkWindowID(c, wid) (wid == c->term->id || findMonitorFromWindowID(c, wid, tmps) != NULL)
#endif

bool SDLTerminal::pollEvents() {
	SDL_Event e;
	std::string tmps;
#ifdef __EMSCRIPTEN__
	if (SDL_PollEvent(&e)) {
#else
	if (SDL_WaitEvent(&e)) {
#endif
		if (e.type == task_event_type) {
			while (taskQueue.size() > 0) {
				auto v = taskQueue.front();
				void* retval = std::get<1>(v)(std::get<2>(v));
				if (!std::get<3>(v)) {
                    std::lock_guard<std::mutex> lock2(taskQueueReturnsMutex);
                    taskQueueReturns[std::get<0>(v)] = retval;
                }
				taskQueue.pop();
			}
		} else if (e.type == render_event_type) {
#ifdef __EMSCRIPTEN__
			SDLTerminal* term = dynamic_cast<SDLTerminal*>(*SDLTerminal::renderTarget);
			std::lock_guard<std::mutex> lock(term->locked);
			if (term->surf != NULL) {
				SDL_BlitSurface(term->surf, NULL, SDL_GetWindowSurface(SDLTerminal::win), NULL);
				SDL_UpdateWindowSurface(SDLTerminal::win);
				SDL_FreeSurface(term->surf);
				term->surf = NULL;
			}
#else
			for (Terminal* term : Terminal::renderTargets) {
				SDLTerminal * sdlterm = dynamic_cast<SDLTerminal*>(term);
				if (sdlterm != NULL) {
					std::lock_guard<std::mutex> lock(sdlterm->locked);
					if (sdlterm->surf != NULL) {
						SDL_BlitSurface(sdlterm->surf, NULL, SDL_GetWindowSurface(sdlterm->win), NULL);
						SDL_UpdateWindowSurface(sdlterm->win);
						SDL_FreeSurface(sdlterm->surf);
						sdlterm->surf = NULL;
					}
				}
			}
#endif
		} else {
            if (rawClient) {
                sendRawEvent(e);
            } else {
                for (Computer * c : computers) {
                    if (((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && checkWindowID(c, e.key.windowID)) ||
                        ((e.type == SDL_DROPFILE || e.type == SDL_DROPTEXT || e.type == SDL_DROPBEGIN || e.type == SDL_DROPCOMPLETE) && checkWindowID(c, e.drop.windowID)) ||
                        ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && checkWindowID(c, e.button.windowID)) ||
                        (e.type == SDL_MOUSEMOTION && checkWindowID(c, e.motion.windowID)) ||
                        (e.type == SDL_MOUSEWHEEL && checkWindowID(c, e.wheel.windowID)) ||
                        (e.type == SDL_TEXTINPUT && checkWindowID(c, e.text.windowID)) ||
                        (e.type == SDL_WINDOWEVENT && checkWindowID(c, e.window.windowID)) ||
                        e.type == SDL_QUIT) {
                        c->termEventQueue.push(e);
                        c->event_lock.notify_all();
                    }
                }
            }
			if (e.type == SDL_QUIT) return true;
		}
	}
	return false;
}