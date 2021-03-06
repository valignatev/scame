#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#define ARRAY_COUNT(static_array) ( sizeof(static_array) / sizeof(*(static_array)) )

#if defined DEBUG
#define assert(expression, ...)                                         \
    if(!(expression)) {                                                 \
        if(ARRAY_COUNT(#__VA_ARGS__) - 1) {                             \
            printf("\n" __VA_ARGS__);                                   \
        }                                                               \
        printf("\nin: %s:%i(%s):\n    " #expression "\n\n",             \
               __FILE__, __LINE__, __FUNCTION__);                       \
        /* POSIX-specific. Will be __debugbreak(); on Windows */        \
        raise(SIGTRAP);                                                 \
        exit(1);                                                        \
    }
#else
#define assert(expression, ...)                 \
    if(!(expression)) {                         \
        if(ARRAY_COUNT(#__VA_ARGS__) - 1) {     \
            printf("\n" __VA_ARGS__);           \
            exit(1);                            \
        }                                       \
    }
#endif

void stbtt_assert_wrapper(int expression) {
    assert(expression);
}

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(expression) stbtt_assert_wrapper(expression)
#include "stb_truetype.h"

typedef char* cstring;
typedef uint8_t u8;
typedef int32_t s32;
typedef float f32;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t usize;

// Probably doesn't make sense to have all of this as global;
Display* display;
int root_window;
int default_screen;
XVisualInfo visinfo;
Window window;

struct u8_array {
    u8* base;
    usize count;
};

typedef u8_array String;

// Maybe it's better to use strlen instead, won't have to subtract 1 then
#define S(static_string) String { (u8*)static_string, ARRAY_COUNT(static_string) - 1 }

// Apparently, using anonymous strutcs inside unions like in C is considered UB
// in C++, but it's very convenient and all compilers do the right thing,
// so I don't give a fuck about this particular part of the C++ spec
#pragma GCC diagnostic ignored "-Wpedantic"
// NOTE: red and blue are inverted it seems, so an actual layout is bgra
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
#pragma GCC diagnostic pop

struct SR_Frame_Buffer {
    s32 width, height;
    rgba8 *base;
};

u8* platform_allocate_bytes(usize byte_count) {
    u8* base = (u8*)malloc(byte_count);
    assert(base, "ERROR: out of memory");
    return base;
}

SR_Frame_Buffer make_frame_buffer(s32 width, s32 height) {
    SR_Frame_Buffer frame_buffer = {};
    frame_buffer.width = width;
    frame_buffer.height = height;
    frame_buffer.base = (rgba8*)platform_allocate_bytes(width * height * sizeof(rgba8));

    return frame_buffer;
}

void fill_box(SR_Frame_Buffer* frame_buffer,
              s32 x, s32 y, s32 width, s32 height, rgba8 color) {
    // If x or y are negative, we decrease the size of the box we draw,
    // as if would be partially off-screen
    if(x < 0) {
        width += x;
        x = 0;
    }

    if(y < 0) {
        height += y;
        y = 0;
    }

    if(x >= frame_buffer->width)
        return;

    if(y >= frame_buffer->height)
        return;
    s32 end_x = std::min(x + width, frame_buffer->width);
    s32 end_y = std::min(y + height, frame_buffer->height);

    // Iterating row by row
    // I want (0,0) to be in the bottom-left corner, but XImage has (0,0) in the
    // top left. So we're iterating from bottom to top.
    s32 y_inverted_start = frame_buffer->height - 1 - y;
    s32 y_inverted_end = frame_buffer->height - 1 - end_y;
    for(s32 y_it = y_inverted_start; y_it > y_inverted_end; y_it--) {
        for(s32 x_it = x; x_it < end_x; x_it++) {
            frame_buffer->base[y_it * frame_buffer->width + x_it] = color;
        }
    }
}

void blit(SR_Frame_Buffer* dest, s32 dest_x, s32 dest_y,
          SR_Frame_Buffer* src, s32 src_x, s32 src_y, s32 src_width, s32 src_height) {
    assert(dest->base != src->base);
    assert((0 <= src_x) && (src_x < src->width));
    assert((0 <= src_y) && (src_y < src->height));
    assert(src_width >= 0);
    assert(src_x + src_width <= src->width);
    assert(src_height >=0);
    assert(src_y + src_height <= src->height);

    if(dest_x < 0) {
        src_width += dest_x;
        src_x -= dest_x;
        dest_x = 0;
    }

    if(dest_y < 0) {
        src_height += dest_y;
        src_y -= dest_y;
        dest_y = 0;
    }

    if(dest_x >= dest->width) {
        return;
    }
    if(dest_y >= dest->height) {
        return;
    }

    s32 width = std::min(src_width, dest->width - dest_x);
    s32 height = std::min(src_height, dest->height - dest_y);

    for(s32 y = 0; y < height; y++) {
        for(s32 x = 0; x < width; x++) {
            // Should we also invert Y of the source buffer?
            auto color = src->base[(y + src_y) * src->width + x + src_x];
            dest->base[(dest->height - 1 - dest_y - y) * dest->width + x + dest_x] = color;
        }
    }
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
    assert(XInitImage(&image), "Fucked up XImage initializationi, dawg");
    XPutImage(display, window, default_gc, &image,
              0, 0, 0, 0, image.width, image.height);
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

u8_array platform_read_entire_file(cstring file_path) {

    u8_array result = {};
    int fd = open(file_path, O_RDONLY);
    assert(fd > -1, "Couldn't open the file %s", file_path);

    struct stat statbuf;
    int err = fstat(fd, &statbuf);
    assert(err > -1, "Couldn't open the file %s", file_path);

    auto memory = (u8*)mmap(NULL, statbuf.st_size, PROT_READ, MAP_SHARED,
                            fd, 0);
    assert(memory != MAP_FAILED, "Couldn't read the file's contents");
    close(fd);

    result.base = memory;
    result.count = statbuf.st_size;
    return result;
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
    assert(XMatchVisualInfo(display, default_screen, screen_bit_depth, TrueColor, &visinfo),
           "No matching visual info");

    XSetWindowAttributes window_attr;
    // This helps with flickering. Default value is ForgetGravity, which
    // makes X server to clear the window to background_pixel upon resize before
    // we can draw anything to it. StaticGravity prevents it.
    window_attr.bit_gravity = StaticGravity;
    window_attr.background_pixel = 0; // Black
    window_attr.colormap = XCreateColormap(display, root_window, visinfo.visual, AllocNone);
    window_attr.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask;
    u64 attribute_mask = CWBitGravity | CWBackPixel | CWColormap | CWEventMask;

    // Windowing
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

    // Input setup
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
    auto test_buffer = make_frame_buffer(200, 200);
    for(s32 x = 0; x < test_buffer.width; x++) {
        test_buffer.base[0 * test_buffer.width + x] = {0, 0, 255, 0};
        test_buffer.base[(test_buffer.height - 1) * test_buffer.width + x] = {0, 0, 255, 0};
    }
    for(s32 y = 0; y < test_buffer.height; y++) {
        test_buffer.base[y * test_buffer.width + 0] = {0, 0, 255, 0};
        test_buffer.base[y* test_buffer.width + test_buffer.width - 1] = {0, 0, 255, 0};
    }

    Atom WM_DELETE_WINDOW = XInternAtom(display, "WM_DELETE_WINDOW", False);
    if(!XSetWMProtocols(display, window, &WM_DELETE_WINDOW, 1))
        printf("Couldn't register WM_DELETE_WINDOW property\n");

    int size_change = 0;
    int window_open = 1;
    rgba8 clear_color = {0, 128, 128, 0};

    // Font stuff
    auto font_atlas = make_frame_buffer(256, 256);
    {
        stbtt_fontinfo font;
        auto ttf_data = platform_read_entire_file("/usr/share/fonts/TTF/Hack-Regular.ttf");

        auto ok = stbtt_InitFont(&font, ttf_data.base, 0);
        assert(ok, "stb_truetype couldn't initialize a font");

        f32 pixel_height = 17 * 2.18; // 2.18 is my laptop's hidpi scale factor
        f32 scale = stbtt_ScaleForPixelHeight(&font, pixel_height);
        s32 ascent;
        s32 descent;
        s32 line_gap;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);

        s32 line_spacing = scale * (ascent - descent + line_gap);

        u8_array glyph_buffer;
        glyph_buffer.count = 64 * 64;
        glyph_buffer.base = platform_allocate_bytes(glyph_buffer.count);

        s32 x_offset = 0;
        s32 y_offset = 0;
        s32 y_max_height = 0;
        for(u32 code_point = ' '; code_point < 128; code_point++) {
            s32 x0, x1, y0, y1;
            stbtt_GetCodepointBitmapBoxSubpixel(&font, code_point, scale, scale, 0, 0,
                                                &x0, &y0, &x1, &y1);
            s32 width = x1 - x0;
            s32 height = y1 - y0;
            stbtt_MakeCodepointBitmapSubpixel(&font, glyph_buffer.base, width, height,
                                              width, scale, scale, 0, 0, code_point);


            if(x_offset + width + 1 > font_atlas.width) {
                x_offset = 0;
                y_offset += y_max_height;
                assert(y_offset + height <= font_atlas.height);
            }
            y_max_height = std::max(y_max_height, height);

            for(s32 y = 0; y < height; y++) {
                for(s32 x = 0; x < width; x++) {
                    rgba8 color;
                    color.r = glyph_buffer.base[y * width + x];
                    color.g = color.r;
                    color.b = color.r;
                    color.a = color.r;
                    // stb_truetype has top to bottom Y coordinate, flip it
                    font_atlas.base[(height - 1 - y + y_offset) * font_atlas.width + x + x_offset] = color;
                }
            }

            x_offset += width + 1;
            assert(x_offset <= font_atlas.width);
        }
    }

    // Event loop
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
                if(size_change) {
                    size_change = 0;
                    // Maybe realloc is better;
                    free(frame_buffer.base);
                    frame_buffer = make_frame_buffer(width, height);
                }
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

        fill_box(&frame_buffer, 0, 0, frame_buffer.width, frame_buffer.height, clear_color);

        blit(&frame_buffer, frame_buffer.width - 100, frame_buffer.height - 100, &test_buffer, 0, 0, test_buffer.width, test_buffer.height);
        blit(&frame_buffer, 10, 10, &font_atlas, 0, 0, font_atlas.width, font_atlas.height);
        present(frame_buffer);
    }

    return 0;
}
