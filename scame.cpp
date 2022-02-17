#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmath>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#define ARRAY_COUNT(static_array) ( sizeof(static_array) / sizeof(*(static_array)) )

typedef uint8_t u8;
typedef int32_t s32;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t usize;

// Probably doesn't make sense to have all of this as global;
Display* display;
int root_window;
int default_screen;
XVisualInfo visinfo;
Window window;

struct String {
    u8 *base;
    usize count;
};

// Maybe it's better to use strlen instead, won't have to subtract 1 then
#define S(static_string) String { (u8*)static_string, ARRAY_COUNT(static_string) - 1 }

union rgba8 {
    struct {
        u8 red, green, blue, alpha;
    };

    struct {
        u8 r, g, b, a;
    };

    u32 value32;
    u8 values8[4];
};

struct SR_Frame_Buffer {
    s32 width, height;
    rgba8 *base;
};

u8* platform_allocate_bytes(usize byte_count) {
    u8* base = (u8*)malloc(byte_count);
    if (!base) {
        printf("ERROR: out of memory\n");
        exit(1);
    }
    return base;
}

SR_Frame_Buffer make_frame_buffer(s32 width, s32 height) {
    SR_Frame_Buffer frame_buffer = {};
    frame_buffer.width = width;
    frame_buffer.height = height;
    frame_buffer.base = (rgba8*)platform_allocate_bytes(width * height * sizeof(rgba8));

    return frame_buffer;
}

void present(SR_Frame_Buffer frame_buffer) {
    if((frame_buffer.width <= 0) || (frame_buffer.height <= 0)) {
        return;
    }

    // We're manually creating XImage here instead of calling
    // XCreateImage so we manually manage its memory instead of letting
    // xlib to allocate it on the heap with calloc. 
    XImage image = {};
    image.width = frame_buffer.width;
    image.height = frame_buffer.height;
    image.format = ZPixmap;
    image.byte_order = ImageByteOrder(display);
    image.bitmap_unit = BitmapUnit(display);
    image.bitmap_bit_order = BitmapBitOrder(display);
    image.red_mask = visinfo.visual->red_mask;
    image.green_mask = visinfo.visual->green_mask;
    image.blue_mask = visinfo.visual->blue_mask;
    image.xoffset = 0;
    image.bitmap_pad = 32;
    image.depth = visinfo.depth;
    image.data = (char*)frame_buffer.base;
    image.bits_per_pixel = 32;
    // XInitImage will initialize it instead;
    image.bytes_per_line = 0;

    GC default_gc = DefaultGC(display, default_screen);
    if(!XInitImage(&image)) {
        printf("Fucked up XImage initializationi, dawg\n");
    }
    XPutImage(display, window, default_gc, &image,
              0, 0, 0, 0, frame_buffer.width, frame_buffer.height);
}

void set_size_hint(Display* display, Window window,
                   u32 min_width, u32 min_height,
                   u32 max_width, u32 max_height) {
    XSizeHints hints = {};
    if(min_width > 0 && min_height > 0) hints.flags |= PMinSize;
    if(max_width > 0 && max_height > 0) hints.flags |= PMaxSize;

    hints.min_width = min_width;
    hints.min_height = min_height;
    hints.max_width = max_width;
    hints.max_height = max_height;

    XSetWMNormalHints(display, window, &hints);
}

Status toggle_maximize(Display* display, Window window) {
    XClientMessageEvent ev = {};
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom max_h = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom max_v = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);

    if(wm_state == None) return 0;

    ev.type = ClientMessage;
    ev.format = 32;
    ev.window = window;
    ev.message_type = wm_state;
    ev.data.l[0] = 2; // _NET_WM_STATE_TOGGLE
    ev.data.l[1] = max_h;
    ev.data.l[2] = max_v;
    ev.data.l[3] = 1;

    return XSendEvent(display, DefaultRootWindow(display), False,
                      SubstructureNotifyMask,
                      (XEvent*)&ev);
}

int main() {
    int width = 800;
    int height = 600;

    display = XOpenDisplay(NULL);

    if(!display) {
        printf("No display available\n");
    }

    root_window = DefaultRootWindow(display);
    default_screen = DefaultScreen(display);

    int screen_bit_depth = 24;
    visinfo = {};
    if(!XMatchVisualInfo(display, default_screen, screen_bit_depth, TrueColor, &visinfo)) {
        printf("No matching visual info\n");
        exit(1);
    }

    XSetWindowAttributes window_attr;
    // This helps with flickering. Default value is ForgetGravity, which
    // makes X server to clear the window to background_pixel upon resize before
    // we can draw anything to it. StaticGravity prevents it.
    window_attr.bit_gravity = StaticGravity;
    window_attr.background_pixel = 0; // Black
    window_attr.colormap = XCreateColormap(display, root_window, visinfo.visual, AllocNone);
    window_attr.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask;
    u64 attribute_mask = CWBitGravity | CWBackPixel | CWColormap | CWEventMask;

    window = XCreateWindow(display, root_window,
                                  0, 0,
                                  width, height, 0,
                                  visinfo.depth, InputOutput,
                                  visinfo.visual, attribute_mask, &window_attr);

    if(!window) {
        printf("Window wasn't created properly\n");
    }

    XStoreName(display, window, "Scame");
    set_size_hint(display, window, 400, 300, 0, 0);
    
    XIM x_input_method = XOpenIM(display, 0, 0, 0);
    if(!x_input_method)
        printf("Input Method could not be opened\n");

    XIMStyles* styles = 0;
    if(XGetIMValues(x_input_method, XNQueryInputStyle, &styles, NULL) || !styles)
        printf("Input Styles could not be retrieved\n");

    XIMStyle best_match_style = 0;
    for(int i = 0; i < styles->count_styles; i++) {
        XIMStyle this_style = styles->supported_styles[i];
        if(this_style == (XIMPreeditNothing | XIMStatusNothing)) {
            best_match_style = this_style;
            break;
        }
    }
    XFree(styles);

    if(!best_match_style)
        printf("No matching input style could be determined\n");

    XIC x_input_context = XCreateIC(x_input_method, XNInputStyle, best_match_style,
                                    XNClientWindow, window,
                                    XNFocusWindow, window,
                                    NULL);
    if(!x_input_context)
        printf("Input Context could not be created\n");

    XMapWindow(display, window);
    XFlush(display);

    // The Buffer
    auto frame_buffer = make_frame_buffer(width, height);

    Atom WM_DELETE_WINDOW = XInternAtom(display, "WM_DELETE_WINDOW", False);
    if(!XSetWMProtocols(display, window, &WM_DELETE_WINDOW, 1))
        printf("Couldn't register WM_DELETE_WINDOW property\n");

    int size_change = 0;
    int window_open = 1;
    while(window_open) {
        XEvent ev = {};
        while(XPending(display) > 0) {
            XNextEvent(display, &ev);
            switch(ev.type) {
            case DestroyNotify: {
                auto e = (XDestroyWindowEvent*) &ev;
                if(e->window == window) {
                    window_open = 0;
                }
            } break;
            case ClientMessage: {
                auto e = (XClientMessageEvent*) &ev;
                if((Atom)e->data.l[0] == WM_DELETE_WINDOW) {
                    XDestroyWindow(display, window);
                    window_open = 0;
                }
            } break;
            case ConfigureNotify: {
                auto e = (XConfigureEvent*) &ev;
                width = e->width;
                height = e->height;
                size_change = 1;
            } break;
            case KeyPress: {
                auto e = (XKeyPressedEvent*)&ev;
                int symbol = 0;
                Status status = 0;
                Xutf8LookupString(x_input_context, e, (char*)&symbol,
                                  4, 0, &status);
                if(status == XBufferOverflow) {
                    // Should not happen since there are no utf-8 characters larger
                    // than 24bits, but something to be aware of when used to directly
                    // write to a string buffer
                    printf("Buffer overflow when trying to create keyboard symbol map\n");
                } else if(status == XLookupChars)
                    printf("%s\n", (char*)&symbol);
            } break;
            }
        }

        if(size_change) {
            size_change = 0;
            // Maybe realloc is better;
            free(frame_buffer.base);
            frame_buffer = make_frame_buffer(width, height);
        }

        int pitch = width * sizeof(rgba8);
        for(int x = 0; x < width; x++) {
            for(int y = 0; y < height; y++) {
                u8* row = (u8*)frame_buffer.base + (y * pitch);
                u32* p = (u32*) (row + (x * sizeof(rgba8)));
                if(x % 16 && y % 16) {
                    //     AARRGGBB
                    *p = 0x00ffffff;
                } else {
                    *p = 0;
                }
            }
        }
        present(frame_buffer);
    }

    return 0;
}
