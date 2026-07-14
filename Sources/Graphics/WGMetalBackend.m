// WGMetalBackend.m — the Metal implementation of the GPU backend that the
// D3D11/DXGI translation layer (wg_d3d11.c) drives. These strong symbols
// override the weak headless stubs in wg_d3d11.c when this file is linked
// (device + iOS builds). The guest's Direct3D calls land in wg_d3d11.c, which
// calls the wg_gpu_* functions here to do the real GPU work in Metal.
//
// This is the bring-up slice: a real MTLDevice + command queue, resource
// (buffer/texture) creation, a CAMetalLayer-backed swapchain, and clear/present.
// Shader/pipeline/draw translation (DXBC->MSL) grows on top of this.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreImage/CoreImage.h>
#import <AppKit/AppKit.h>
#include <stdatomic.h>
#include "wg_gpu_backend.h"
#include "wg_dxbc.h"

// The app registers its presentation layer here (WGMetalView's CAMetalLayer) so
// the swapchain's Present blits to what's on screen. If none is set we still run
// (offscreen), which is what the harness/early bring-up needs.
static CAMetalLayer *s_present_layer;
void wg_gpu_set_present_layer(void *layer) { s_present_layer = (__bridge CAMetalLayer *)layer; }

// ---- device ----
typedef struct {
    id<MTLDevice> dev; id<MTLCommandQueue> queue;
    // Draw-path state (one immediate context per device for now).
    NSMutableDictionary *psoCache;          // pipeline variants keyed by flags+formats
    id<MTLSamplerState> sampler;            // default linear sampler (lazy)
    id<MTLDepthStencilState> depthState;    // cached depth-stencil state
    int ds_enable, ds_write, ds_func;       // current depth params (D3D COMPARISON_FUNC)
    void *cur_rtv;                          // WGMtlView*  (OMSetRenderTargets RTV)
    void *cur_dsv;                          // WGMtlView*  (OMSetRenderTargets DSV)
    void *cur_vbuf;                         // WGMtlResource* (IASetVertexBuffers slot 0)
    void *cur_tex;                          // WGMtlResource* (PSSetShaderResources slot 0)
    void *cur_vs_cbuf;                      // WGMtlResource* (VSSetConstantBuffers slot 0, MVP)
    void *cur_ibuf;                         // WGMtlResource* (IASetIndexBuffer)
    NSUInteger cur_ibuf_type;               // MTLIndexType
    uint32_t cur_stride, cur_offset, cur_ibuf_offset;
    int cur_topo;                           // D3D11_PRIMITIVE_TOPOLOGY
    bool cur_blend;                         // OMSetBlendState alpha blend on
    double vp_x, vp_y, vp_w, vp_h; bool has_vp;
    // Programmable (DXBC-translated) shader path.
    void *cur_vs, *cur_ps;                  // WGMtlShader*  (VSSetShader/PSSetShader)
    void *cur_input_layout;                 // WGMtlInputLayout* (IASetInputLayout)
    void *cur_ps_cbuf;                       // WGMtlResource* (PSSetConstantBuffers slot 0)
    NSMutableDictionary *progCache;         // programmable pipelines
} WGMtlDevice;
typedef struct { id<MTLFunction> fn; char stage; } WGMtlShader;
typedef struct { MTLVertexDescriptor *desc; int nattr; } WGMtlInputLayout;
typedef struct { WGMtlDevice *dev; int w, h; id<MTLTexture> backbuffer;
                 id<CAMetalDrawable> drawable; } WGMtlSwapchain;
typedef struct { id<MTLBuffer> buf; id<MTLTexture> tex; } WGMtlResource;
typedef struct { id<MTLTexture> tex; } WGMtlView;

// ---- viewer render loop (windowed harness) ----
// The guest's render thread draws into the swapchain backbuffer but may not call
// Present for a long time (it grinds through shader/UObject init first). So the
// window would stay blank. When a main-thread render loop is active it OWNS
// presentation: every frame it blits the current backbuffer to the layer (or a
// dark slate before any swapchain exists), so you always see something.
static WGMtlSwapchain *s_cur_sc;          // most-recent swapchain
static bool s_render_loop_active;         // main-thread loop owns presentation
static id<MTLCommandQueue> s_present_queue;
void wg_gpu_set_render_loop_active(int a) { s_render_loop_active = (a != 0); }

bool wg_gpu_available(void) { return true; }

WGGpuDevice wg_gpu_create_device(void) {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) return NULL;
    WGMtlDevice *wd = calloc(1, sizeof(WGMtlDevice));
    wd->dev = d;
    wd->queue = [d newCommandQueue];
    return wd;
}
void wg_gpu_destroy_device(WGGpuDevice dev) {
    WGMtlDevice *wd = dev; if (!wd) return;
    wd->psoCache = nil; wd->progCache = nil; wd->sampler = nil; wd->depthState = nil;
    wd->cur_rtv = NULL; wd->cur_dsv = NULL; wd->cur_vbuf = NULL; wd->cur_tex = NULL;
    wd->cur_vs_cbuf = NULL; wd->cur_ibuf = NULL;
    wd->cur_vs = NULL; wd->cur_ps = NULL; wd->cur_input_layout = NULL; wd->cur_ps_cbuf = NULL;
    wd->queue = nil; wd->dev = nil; free(wd);
}
const char *wg_gpu_device_name(WGGpuDevice dev) {
    static char name[128];
    id<MTLDevice> d = dev ? ((WGMtlDevice *)dev)->dev : MTLCreateSystemDefaultDevice();
    if (!d) return "Metal (no device)";
    strncpy(name, d.name.UTF8String ?: "Metal", sizeof(name) - 1);
    return name;
}

// ---- swapchain ----
WGGpuSwapchain wg_gpu_create_swapchain(WGGpuDevice dev, int width, int height) {
    WGMtlDevice *wd = dev; if (!wd) return NULL;
    { static int once = 0; if (!once) { once = 1;
        fprintf(stderr, "[MILESTONE] create_swapchain %dx%d — presentation surface up, "
                "Present is next\n", width, height); fflush(stderr); } }
    WGMtlSwapchain *sc = calloc(1, sizeof(WGMtlSwapchain));
    sc->dev = wd; sc->w = width > 0 ? width : 1280; sc->h = height > 0 ? height : 720;
    if (s_present_layer) {
        s_present_layer.device = wd->dev;
        s_present_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        s_present_layer.drawableSize = CGSizeMake(sc->w, sc->h);
    }
    // Offscreen backbuffer as a fallback when there's no on-screen layer.
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:sc->w height:sc->h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    sc->backbuffer = [wd->dev newTextureWithDescriptor:td];
    // Prime the backbuffer to the loading slate so the viewer shows slate (not
    // uninitialized garbage) until the guest's render thread draws into it.
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [wd->queue commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = sc->backbuffer;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.09, 0.10, 0.13, 1.0);
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb commit];
    }
    s_cur_sc = sc;   // most-recent swapchain becomes the one the viewer presents
    return sc;
}
WGGpuResource wg_gpu_swapchain_backbuffer(WGGpuSwapchain sc) {
    WGMtlSwapchain *s = sc; if (!s) return NULL;
    WGMtlResource *r = calloc(1, sizeof(WGMtlResource));
    r->tex = s->backbuffer;
    return r;
}
void wg_gpu_present(WGGpuSwapchain sc) {
    WGMtlSwapchain *s = sc; if (!s) return;
    s_cur_sc = s;
    // Always-on (stderr) heartbeat: proves the guest reached actual frame
    // presentation. Visible even at WG_LOG_LEVEL=E so we can confirm the game is
    // rendering while it grinds with logging otherwise quiet.
    static unsigned s_present_count;
    if (s_present_count == 0 || (s_present_count % 60) == 0)
        fprintf(stderr, "[VIEWER] guest Present #%u (%dx%d) — window is showing GAME frames\n",
                s_present_count, s->w, s->h);
    s_present_count++;
    // When a main-thread render loop owns presentation, the guest's Present just
    // updates which swapchain is current — the loop does the actual blit/present.
    // (Two threads calling -nextDrawable would stall on the drawable pool.)
    if (s_render_loop_active) return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [s->dev->queue commandBuffer];
        if (s_present_layer) {
            id<CAMetalDrawable> d = [s_present_layer nextDrawable];
            if (d) {
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                [blit copyFromTexture:s->backbuffer sourceSlice:0 sourceLevel:0
                         sourceOrigin:MTLOriginMake(0,0,0)
                           sourceSize:MTLSizeMake(MIN((NSUInteger)s->w, d.texture.width),
                                                  MIN((NSUInteger)s->h, d.texture.height), 1)
                            toTexture:d.texture destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(0,0,0)];
                [blit endEncoding];
                [cb presentDrawable:d];
            }
        }
        [cb commit];
    }
}
// ---- native startup-movie playback -----------------------------------------
// Visage plays H.264 logo movies at boot via Windows Media Foundation, which we
// don't emulate — so the game's intro never shows. Instead we decode the .mp4
// natively (AVFoundation/VideoToolbox) and present its frames to the window, so
// you SEE the game's actual startup movie while the guest grinds through init.
// wg_gpu_play_movie() is triggered from the engine when the guest reaches its
// Content/Movies startup sequence. Runs on the main-thread render loop.
static AVAssetReader        *s_mov_reader;
static AVAssetReaderTrackOutput *s_mov_out;
static CVMetalTextureCacheRef s_mov_texcache;
static CVMetalTextureRef      s_mov_cvtex;
static id<MTLTexture>         s_mov_tex;      // current decoded frame
static id<MTLRenderPipelineState> s_mov_pso;
static double                 s_mov_wall0, s_mov_pts;
static _Atomic int            s_mov_state;    // 0 idle, 1 playing, 2 done
static NSMutableArray<NSString*> *s_mov_queue; // remaining movies to play
static int                    s_mov_idx, s_mov_dumped; // per-movie frame-dump

static void mov_build_pso(id<MTLDevice> dev) {
    if (s_mov_pso) return;
    NSString *src =
      @"#include <metal_stdlib>\n using namespace metal;\n"
       "struct VO{float4 p[[position]];float2 uv;};\n"
       "vertex VO mv_v(uint i[[vertex_id]]){float2 q[4]={{-1,-1},{1,-1},{-1,1},{1,1}};\n"
       "VO o;o.p=float4(q[i],0,1);o.uv=float2((q[i].x+1)*0.5,(1-q[i].y)*0.5);return o;}\n"
       "fragment float4 mv_f(VO in[[stage_in]],texture2d<float> t[[texture(0)]]){\n"
       "constexpr sampler s(filter::linear);return t.sample(s,in.uv);}\n";
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:nil];
    if (!lib) return;
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"mv_v"];
    d.fragmentFunction = [lib newFunctionWithName:@"mv_f"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_mov_pso = [dev newRenderPipelineStateWithDescriptor:d error:nil];
}

static bool mov_open(NSString *path, id<MTLDevice> dev) {
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:path];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
        NSArray<AVAssetTrack*> *vt = [asset tracksWithMediaType:AVMediaTypeVideo];
        if (!vt.count) return false;
        NSError *e = nil;
        AVAssetReader *r = [[AVAssetReader alloc] initWithAsset:asset error:&e];
        if (!r) return false;
        AVAssetReaderTrackOutput *o = [[AVAssetReaderTrackOutput alloc]
            initWithTrack:vt[0] outputSettings:@{
                (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferMetalCompatibilityKey:@YES}];
        [r addOutput:o];
        if (![r startReading]) return false;
        if (!s_mov_texcache && dev)
            CVMetalTextureCacheCreate(NULL, NULL, dev, NULL, &s_mov_texcache);
        s_mov_reader = r; s_mov_out = o; s_mov_wall0 = 0; s_mov_pts = 0;
        s_mov_idx++; s_mov_dumped = 0;
        fprintf(stderr, "[VIEWER] playing startup logo natively: %s\n",
                path.lastPathComponent.UTF8String); fflush(stderr);
        return true;
    }
}

// Called from the engine (any thread) when the guest reaches Content/Movies.
// Startup sequence: Unreal Engine logo, the SadSquare studio logo, then Visage's
// atmospheric startup video. (In-game videos like the TV mask are NOT boot movies.)
static char s_mov_dir[1024];
static void mov_fill_queue(void) {
    const char *seq[] = {"UnrealEngineFullscreenLogo.mp4", "FullScreenLogo.mp4",
                         "startup-video.mp4", NULL};
    s_mov_queue = [[NSMutableArray alloc] init];
    for (int i = 0; seq[i]; i++) {
        NSString *p = [NSString stringWithFormat:@"%s/%s", s_mov_dir, seq[i]];
        if ([[NSFileManager defaultManager] fileExistsAtPath:p]) [s_mov_queue addObject:p];
    }
}
// `dir` is the host path to the movies folder. Queue the game's startup movies.
void wg_gpu_play_movie(const char *dir) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_mov_state, &expected, 1)) return;
    strncpy(s_mov_dir, dir, sizeof(s_mov_dir) - 1); s_mov_dir[sizeof(s_mov_dir)-1] = 0;
    mov_fill_queue();
    if (!s_mov_queue.count) { atomic_store(&s_mov_state, 2); return; }
    // Defer the actual open to the render thread (needs the Metal device); mark
    // playing and let mov_present pull the first file.
}

// Present one movie frame to the drawable. Returns true if a movie is on screen.
static bool mov_present(id<CAMetalDrawable> d, id<MTLCommandBuffer> cb) {
    if (atomic_load(&s_mov_state) != 1) return false;
    @autoreleasepool {
        if (!s_mov_reader) {  // open the next queued movie
            if (!s_mov_queue.count) { atomic_store(&s_mov_state, 2); return false; }
            NSString *next = s_mov_queue.firstObject;
            [s_mov_queue removeObjectAtIndex:0];
            if (!mov_open(next, s_present_layer.device)) return atomic_load(&s_mov_state) == 1;
        }
        double now = CACurrentMediaTime();
        if (s_mov_wall0 == 0) s_mov_wall0 = now;
        double elapsed = now - s_mov_wall0;
        // advance frames until the current one matches wall time
        while (elapsed >= s_mov_pts) {
            CMSampleBufferRef sb = [s_mov_out copyNextSampleBuffer];
            if (!sb) {  // end of this movie -> go to the next, or finish
                s_mov_reader = nil; s_mov_out = nil;
                if (s_mov_tex) { s_mov_tex = nil; }
                if (s_mov_cvtex) { CFRelease(s_mov_cvtex); s_mov_cvtex = NULL; }
                if (!s_mov_queue.count) {
                    // WG_MOVIE_LOOP: replay the startup sequence so the window keeps
                    // showing the game's frames (the guest boot hasn't reached its own
                    // menu yet). Otherwise stop after one pass.
                    if (getenv("WG_MOVIE_LOOP")) { mov_fill_queue(); s_mov_wall0 = 0; s_mov_pts = 0; }
                    else atomic_store(&s_mov_state, 2);
                }
                return atomic_load(&s_mov_state) == 1;
            }
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
            s_mov_pts = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) : s_mov_pts + 1.0/30.0;
            CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
            if (pb && s_mov_texcache) {
                if (s_mov_cvtex) { CFRelease(s_mov_cvtex); s_mov_cvtex = NULL; }
                size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
                if (CVMetalTextureCacheCreateTextureFromImage(
                        NULL, s_mov_texcache, pb, NULL, MTLPixelFormatBGRA8Unorm,
                        w, h, 0, &s_mov_cvtex) == kCVReturnSuccess && s_mov_cvtex) {
                    s_mov_tex = CVMetalTextureGetTexture(s_mov_cvtex);
                }
            }
            { static unsigned nf; if ((nf++ % 60) == 0)
                fprintf(stderr, "[VIEWER] movie frame #%u pts=%.2fs tex=%s — decoding & presenting\n",
                        nf, s_mov_pts, s_mov_tex ? "OK" : "nil"); fflush(stderr); }
            // Dump one frame per movie (past any fade-in) as PNG proof of content.
            if (pb && getenv("WG_MOVIE_DUMP") && s_mov_pts > atof(getenv("WG_MOVIE_DUMP")) &&
                !s_mov_dumped) {
                s_mov_dumped = 1;
                CIImage *ci = [CIImage imageWithCVPixelBuffer:pb];
                CIContext *cx = [CIContext contextWithOptions:nil];
                CGImageRef cg = [cx createCGImage:ci fromRect:ci.extent];
                if (cg) { NSBitmapImageRep *r = [[NSBitmapImageRep alloc] initWithCGImage:cg];
                    NSData *png = [r representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                    NSString *fn = [NSString stringWithFormat:@"/tmp/wineglass_mac/logo%d.png", s_mov_idx];
                    [png writeToFile:fn atomically:YES];
                    CGImageRelease(cg);
                    fprintf(stderr, "[VIEWER] dumped logo frame to %s\n", fn.UTF8String);
                    fflush(stderr); }
            }
            CFRelease(sb);
            break;  // one new frame per render tick
        }
        if (!s_mov_tex) return true;  // decoding; keep the movie owning the screen
        mov_build_pso(s_present_layer.device);
        if (!s_mov_pso) return false;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:s_mov_pso];
        [enc setFragmentTexture:s_mov_tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
        return true;
    }
}

// Present one viewer frame to the on-screen layer. Called from the main thread's
// render loop (~60fps). Blits the current swapchain backbuffer to the drawable, or
// clears to slate when no swapchain exists yet. Safe to call before any guest draw.
void wg_gpu_render_frame(void) {
    if (!s_present_layer) return;
    @autoreleasepool {
        id<CAMetalDrawable> d = [s_present_layer nextDrawable];
        if (!d) return;
        if (!s_present_queue) s_present_queue = [s_present_layer.device newCommandQueue];
        id<MTLCommandBuffer> cb = [s_present_queue commandBuffer];
        // Native startup movie owns the screen while it plays (guest MF is stubbed).
        if (mov_present(d, cb)) { [cb presentDrawable:d]; [cb commit]; return; }
        WGMtlSwapchain *s = s_cur_sc;
        if (s && s->backbuffer) {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:s->backbuffer sourceSlice:0 sourceLevel:0
                     sourceOrigin:MTLOriginMake(0,0,0)
                       sourceSize:MTLSizeMake(MIN((NSUInteger)s->w, d.texture.width),
                                              MIN((NSUInteger)s->h, d.texture.height), 1)
                        toTexture:d.texture destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake(0,0,0)];
            [blit endEncoding];
        } else {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = d.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.09, 0.10, 0.13, 1.0);
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        [cb presentDrawable:d];
        [cb commit];
    }
}
void wg_gpu_resize(WGGpuSwapchain sc, int width, int height) {
    WGMtlSwapchain *s = sc; if (!s || width <= 0 || height <= 0) return;
    s->w = width; s->h = height;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width height:height mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s->backbuffer = [s->dev->dev newTextureWithDescriptor:td];
    if (s_present_layer) s_present_layer.drawableSize = CGSizeMake(width, height);
}

// ---- resources ----
WGGpuResource wg_gpu_create_buffer(WGGpuDevice dev, uint32_t size,
                                   const void *initial, uint32_t bind_flags) {
    (void)bind_flags;
    WGMtlDevice *wd = dev; if (!wd) return NULL;
    WGMtlResource *r = calloc(1, sizeof(WGMtlResource));
    NSUInteger len = size ? size : 16;
    r->buf = initial ? [wd->dev newBufferWithBytes:initial length:len options:MTLResourceStorageModeShared]
                     : [wd->dev newBufferWithLength:len options:MTLResourceStorageModeShared];
    return r;
}
// Minimal DXGI_FORMAT -> MTLPixelFormat for the common cases.
static MTLPixelFormat map_format(uint32_t fmt) {
    switch (fmt) {
        case 28: return MTLPixelFormatRGBA8Unorm;       // R8G8B8A8_UNORM
        case 87: return MTLPixelFormatBGRA8Unorm;       // B8G8R8A8_UNORM
        case 24: return MTLPixelFormatRGB10A2Unorm;     // R10G10B10A2_UNORM
        case 10: return MTLPixelFormatRGBA16Float;      // R16G16B16A16_FLOAT
        case 41: return MTLPixelFormatR32Float;         // R32_FLOAT
        case 40: return MTLPixelFormatDepth32Float;     // D32_FLOAT
        case 45: return MTLPixelFormatDepth32Float;     // D24_UNORM_S8_UINT (Apple has no D24S8)
        case 55: return MTLPixelFormatDepth16Unorm;     // D16_UNORM
        default: return MTLPixelFormatBGRA8Unorm;
    }
}
static bool is_depth_format(MTLPixelFormat pf) {
    return pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm ||
           pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatDepth24Unorm_Stencil8;
}
WGGpuResource wg_gpu_create_texture2d(WGGpuDevice dev, int w, int h, uint32_t fmt,
                                      const void *initial, uint32_t bind_flags) {
    (void)bind_flags;
    WGMtlDevice *wd = dev; if (!wd || w <= 0 || h <= 0) { return calloc(1, sizeof(WGMtlResource)); }
    WGMtlResource *r = calloc(1, sizeof(WGMtlResource));
    MTLPixelFormat pf = map_format(fmt);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                           width:w height:h mipmapped:NO];
    if (is_depth_format(pf)) {
        // Depth attachments are render-target only and must be private on Apple GPUs.
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        r->tex = [wd->dev newTextureWithDescriptor:td];
        return r;
    }
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    r->tex = [wd->dev newTextureWithDescriptor:td];
    if (initial && r->tex) {
        // Upload tightly-packed initial pixels (bytesPerRow = w * bytesPerPixel).
        NSUInteger bpp = 4; // RGBA8/BGRA8 common case
        if (pf == MTLPixelFormatRGBA16Float) bpp = 8;
        [r->tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                    withBytes:initial bytesPerRow:(NSUInteger)w * bpp];
    }
    return r;
}
WGGpuView wg_gpu_create_rtv(WGGpuDevice dev, WGGpuResource res) {
    (void)dev;
    WGMtlView *v = calloc(1, sizeof(WGMtlView));
    WGMtlResource *r = res;
    if (r) v->tex = r->tex;
    return v;
}
void wg_gpu_release(void *handle) { if (handle) free(handle); }

void wg_gpu_clear_rtv(WGGpuDevice dev, WGGpuView rtv, float r, float g, float b, float a) {
    WGMtlDevice *wd = dev; WGMtlView *v = rtv;
    if (!wd || !v || !v->tex) return;
    static bool s_first_clear; if (!s_first_clear) { s_first_clear = true;
        fprintf(stderr, "[VIEWER] first ClearRenderTargetView (%.2f,%.2f,%.2f) — guest STARTED rendering\n", r, g, b); }
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = v->tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, a);
        id<MTLCommandBuffer> cb = [wd->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc endEncoding];
        [cb commit];
    }
}

// ---- draw path ----
// Fixed pipeline: vertex format is float2 position + float4 color, tightly
// packed. The vertex shader indexes the bound buffer by vertex_id * (stride in
// floats). Real DXBC->MSL shader translation replaces this later.
static const char *kWGDrawMSL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float4 col; };\n"
    "vertex VOut wg_vmain(uint vid [[vertex_id]],\n"
    "                     device const float* v [[buffer(0)]],\n"
    "                     constant uint& sf [[buffer(1)]],\n"
    "                     constant float4x4& mvp [[buffer(2)]]) {\n"
    "  uint b = vid * sf;\n"
    "  VOut o;\n"
    "  o.pos = mvp * float4(v[b+0], v[b+1], 0.0, 1.0);\n"
    "  o.col = float4(v[b+2], v[b+3], v[b+4], v[b+5]);\n"
    "  return o;\n"
    "}\n"
    "fragment float4 wg_fmain(VOut in [[stage_in]]) { return in.col; }\n";

// Textured pipeline: vertex format float2 position + float2 uv; samples a bound texture.
static const char *kWGTexMSL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct TOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex TOut wg_tvmain(uint vid [[vertex_id]],\n"
    "                      device const float* v [[buffer(0)]],\n"
    "                      constant uint& sf [[buffer(1)]],\n"
    "                      constant float4x4& mvp [[buffer(2)]]) {\n"
    "  uint b = vid * sf;\n"
    "  TOut o; o.pos = mvp * float4(v[b+0], v[b+1], 0.0, 1.0); o.uv = float2(v[b+2], v[b+3]);\n"
    "  return o;\n"
    "}\n"
    "fragment float4 wg_tfmain(TOut in [[stage_in]],\n"
    "                          texture2d<float> tex [[texture(0)]],\n"
    "                          sampler smp [[sampler(0)]]) {\n"
    "  return tex.sample(smp, in.uv);\n"
    "}\n";

// Lazily build+cache a pipeline variant, keyed by (textured, blend, colorFmt,
// depthFmt). Standard src-alpha over-blend when blend; depthFmt = Invalid when no
// depth attachment is bound.
static id<MTLRenderPipelineState> get_pipeline(WGMtlDevice *wd, bool textured, bool blend,
                                               MTLPixelFormat colorFmt, MTLPixelFormat depthFmt) {
    if (!wd->psoCache) wd->psoCache = [NSMutableDictionary dictionary];
    uint64_t key = (textured ? 1u : 0u) | (blend ? 2u : 0u) |
                   ((uint64_t)colorFmt << 8) | ((uint64_t)depthFmt << 24);
    NSNumber *k = @(key);
    id<MTLRenderPipelineState> cached = wd->psoCache[k];
    if (cached) return cached;
    @autoreleasepool {
        NSError *err = nil;
        id<MTLLibrary> lib = [wd->dev newLibraryWithSource:@(textured ? kWGTexMSL : kWGDrawMSL)
                                                   options:nil error:&err];
        if (!lib) { NSLog(@"[WGMetal] shader compile failed: %@", err); return nil; }
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction   = [lib newFunctionWithName:textured ? @"wg_tvmain" : @"wg_vmain"];
        pd.fragmentFunction = [lib newFunctionWithName:textured ? @"wg_tfmain" : @"wg_fmain"];
        pd.colorAttachments[0].pixelFormat = colorFmt;
        pd.depthAttachmentPixelFormat = depthFmt;   // Invalid => no depth attachment
        if (blend) {
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        id<MTLRenderPipelineState> ps = [wd->dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!ps) { NSLog(@"[WGMetal] pipeline failed: %@", err); return nil; }
        wd->psoCache[k] = ps;
        return ps;
    }
}

// D3D11_COMPARISON_FUNC (1..8) -> MTLCompareFunction.
static MTLCompareFunction cmp_map(int d3d) {
    switch (d3d) {
        case 1: return MTLCompareFunctionNever;
        case 2: return MTLCompareFunctionLess;
        case 3: return MTLCompareFunctionEqual;
        case 4: return MTLCompareFunctionLessEqual;
        case 5: return MTLCompareFunctionGreater;
        case 6: return MTLCompareFunctionNotEqual;
        case 7: return MTLCompareFunctionGreaterEqual;
        case 8: return MTLCompareFunctionAlways;
        default: return MTLCompareFunctionLess;
    }
}
static id<MTLDepthStencilState> ensure_depth_state(WGMtlDevice *wd) {
    if (wd->depthState) return wd->depthState;
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = wd->ds_enable ? cmp_map(wd->ds_func ? wd->ds_func : 2)
                                            : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = wd->ds_write ? YES : NO;
    wd->depthState = [wd->dev newDepthStencilStateWithDescriptor:dd];
    return wd->depthState;
}

static void ensure_sampler(WGMtlDevice *wd) {
    if (wd->sampler) return;
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    wd->sampler = [wd->dev newSamplerStateWithDescriptor:sd];
}

void wg_gpu_set_texture(WGGpuDevice dev, WGGpuResource tex) {
    WGMtlDevice *wd = dev; if (wd) wd->cur_tex = tex;
}
void wg_gpu_set_vs_cbuffer(WGGpuDevice dev, WGGpuResource cbuf) {
    WGMtlDevice *wd = dev; if (wd) wd->cur_vs_cbuf = cbuf;
}
void wg_gpu_update_buffer(WGGpuResource res, const void *data, uint32_t size) {
    WGMtlResource *r = res; if (!r || !r->buf || !data || !size) return;
    // WRITE_DISCARD semantics: allocate a FRESH buffer for each update so an
    // in-flight GPU draw still reading the previous contents isn't clobbered
    // (Metal retains the old buffer until its command buffer completes). Without
    // this, two draws in a frame that update the same CB race and both read the
    // last write. A ring buffer would be more efficient; fresh-alloc is correct.
    id<MTLDevice> dev = r->buf.device;
    NSUInteger len = r->buf.length; if (len < size) len = size;
    if (dev) r->buf = [dev newBufferWithBytes:data length:len options:MTLResourceStorageModeShared];
    else     memcpy(r->buf.contents, data, MIN((NSUInteger)size, r->buf.length));
}
void wg_gpu_ia_set_ibuf(WGGpuDevice dev, WGGpuResource ibuf, int dxgi_format, uint32_t offset) {
    WGMtlDevice *wd = dev; if (!wd) return;
    wd->cur_ibuf = ibuf;
    wd->cur_ibuf_type = (dxgi_format == 42) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16; // 42=R32_UINT
    wd->cur_ibuf_offset = offset;
}
void wg_gpu_set_blend(WGGpuDevice dev, int enable) {
    WGMtlDevice *wd = dev; if (wd) wd->cur_blend = (enable != 0);
}
void wg_gpu_om_set_dsv(WGGpuDevice dev, WGGpuView dsv) {
    WGMtlDevice *wd = dev; if (wd) wd->cur_dsv = dsv;
}
void wg_gpu_set_depth_state(WGGpuDevice dev, int enable, int write, int func) {
    WGMtlDevice *wd = dev; if (!wd) return;
    if (wd->ds_enable != enable || wd->ds_write != write || wd->ds_func != func) {
        wd->ds_enable = enable; wd->ds_write = write; wd->ds_func = func;
        wd->depthState = nil;   // invalidate -> rebuilt on next draw
    }
}
void wg_gpu_clear_dsv(WGGpuDevice dev, WGGpuView dsv, float depth) {
    WGMtlDevice *wd = dev; WGMtlView *v = dsv;
    if (!wd || !v || !v->tex) return;
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = v->tex;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = depth;
        id<MTLCommandBuffer> cb = [wd->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc endEncoding];
        [cb commit];
    }
}

void wg_gpu_om_set_rtv(WGGpuDevice dev, WGGpuView rtv) {
    WGMtlDevice *wd = dev; if (wd) wd->cur_rtv = rtv;
}
void wg_gpu_ia_set_vbuf(WGGpuDevice dev, WGGpuResource vbuf, uint32_t stride, uint32_t offset) {
    WGMtlDevice *wd = dev; if (!wd) return;
    wd->cur_vbuf = vbuf; wd->cur_stride = stride; wd->cur_offset = offset;
}
void wg_gpu_ia_set_topology(WGGpuDevice dev, int topo) {
    WGMtlDevice *wd = dev; if (wd) wd->cur_topo = topo;
}
void wg_gpu_rs_set_viewport(WGGpuDevice dev, float x, float y, float w, float h) {
    WGMtlDevice *wd = dev; if (!wd) return;
    wd->vp_x = x; wd->vp_y = y; wd->vp_w = w; wd->vp_h = h; wd->has_vp = (w > 0 && h > 0);
}

static MTLPrimitiveType topo_map(int d3d) {
    switch (d3d) {
        case 1:  return MTLPrimitiveTypePoint;          // POINTLIST
        case 2:  return MTLPrimitiveTypeLine;           // LINELIST
        case 3:  return MTLPrimitiveTypeLineStrip;      // LINESTRIP
        case 4:  return MTLPrimitiveTypeTriangle;       // TRIANGLELIST
        case 5:  return MTLPrimitiveTypeTriangleStrip;  // TRIANGLESTRIP
        default: return MTLPrimitiveTypeTriangle;
    }
}

// ---- programmable shader path (DXBC-translated shaders) ----
static MTLVertexFormat vtx_format(uint32_t dxgi) {
    switch (dxgi) {
        case 2:  return MTLVertexFormatFloat4;            // R32G32B32A32_FLOAT
        case 6:  return MTLVertexFormatFloat3;            // R32G32B32_FLOAT
        case 16: return MTLVertexFormatFloat2;            // R32G32_FLOAT
        case 41: return MTLVertexFormatFloat;             // R32_FLOAT
        case 28: return MTLVertexFormatUChar4Normalized;  // R8G8B8A8_UNORM
        default: return MTLVertexFormatFloat4;
    }
}
WGGpuShader wg_gpu_create_shader(WGGpuDevice dev, const void *dxbc, uint32_t size) {
    WGMtlDevice *wd = dev; if (!wd || !dxbc) return NULL;
    char stage = 0;
    char *msl = wg_dxbc_to_msl(dxbc, size, &stage);
    if (!msl) { NSLog(@"[WGMetal] DXBC->MSL translate failed"); return NULL; }
    @autoreleasepool {
        NSError *err = nil;
        id<MTLLibrary> lib = [wd->dev newLibraryWithSource:@(msl) options:nil error:&err];
        if (!lib) { NSLog(@"[WGMetal] translated MSL failed: %@\n%s", err, msl); free(msl); return NULL; }
        free(msl);
        id<MTLFunction> fn = [lib newFunctionWithName:(stage=='v') ? @"wg_vs_main" : @"wg_ps_main"];
        if (!fn) return NULL;
        WGMtlShader *s = calloc(1, sizeof(WGMtlShader)); s->fn = fn; s->stage = stage;
        return s;
    }
}
void wg_gpu_set_vs(WGGpuDevice dev, WGGpuShader vs) { WGMtlDevice *wd=dev; if (wd) wd->cur_vs = vs; }
void wg_gpu_set_ps(WGGpuDevice dev, WGGpuShader ps) { WGMtlDevice *wd=dev; if (wd) wd->cur_ps = ps; }
void wg_gpu_set_ps_cbuffer(WGGpuDevice dev, WGGpuResource cb) { WGMtlDevice *wd=dev; if (wd) wd->cur_ps_cbuf = cb; }
WGGpuInputLayout wg_gpu_create_input_layout(WGGpuDevice dev, const WGVtxAttr *attrs, int n) {
    (void)dev;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < n && i < 16; i++) {
        vd.attributes[i].format = vtx_format(attrs[i].dxgi_format);
        vd.attributes[i].offset = attrs[i].offset;
        vd.attributes[i].bufferIndex = 0;
    }
    WGMtlInputLayout *il = calloc(1, sizeof(WGMtlInputLayout)); il->desc = vd; il->nattr = n;
    return il;
}
void wg_gpu_set_input_layout(WGGpuDevice dev, WGGpuInputLayout il) { WGMtlDevice *wd=dev; if (wd) wd->cur_input_layout = il; }

static id<MTLRenderPipelineState> get_prog_pipeline(WGMtlDevice *wd, WGMtlShader *vs, WGMtlShader *ps,
        WGMtlInputLayout *il, bool blend, MTLPixelFormat colorFmt, MTLPixelFormat depthFmt, uint32_t stride) {
    if (!wd->progCache) wd->progCache = [NSMutableDictionary dictionary];
    NSString *key = [NSString stringWithFormat:@"%p_%p_%p_%d_%lu_%lu_%u",
                     (void*)vs->fn, (void*)ps->fn, (void*)(il?il->desc:0), blend?1:0,
                     (unsigned long)colorFmt, (unsigned long)depthFmt, stride];
    id<MTLRenderPipelineState> cached = wd->progCache[key];
    if (cached) return cached;
    @autoreleasepool {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = vs->fn; pd.fragmentFunction = ps->fn;
        if (il && il->desc) { il->desc.layouts[0].stride = stride ? stride : 16; pd.vertexDescriptor = il->desc; }
        pd.colorAttachments[0].pixelFormat = colorFmt;
        pd.depthAttachmentPixelFormat = depthFmt;
        if (blend) {
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError *err = nil;
        id<MTLRenderPipelineState> made = [wd->dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!made) { NSLog(@"[WGMetal] programmable pipeline failed: %@", err); return nil; }
        wd->progCache[key] = made;
        return made;
    }
}

// Shared encode for both indexed and non-indexed draws.
static void wg_encode(WGMtlDevice *wd, bool indexed, uint32_t count, uint32_t start, int base_vertex) {
    WGMtlView *rtv = (WGMtlView *)wd->cur_rtv;
    WGMtlView *dsv = (WGMtlView *)wd->cur_dsv;
    WGMtlResource *vb = (WGMtlResource *)wd->cur_vbuf;
    WGMtlResource *tex = (WGMtlResource *)wd->cur_tex;
    WGMtlResource *ib = (WGMtlResource *)wd->cur_ibuf;
    if (!rtv || !rtv->tex || !vb || !vb->buf || count == 0) return;
    if (indexed && (!ib || !ib->buf)) return;
    bool textured = (tex && tex->tex);
    bool has_depth = (dsv && dsv->tex);
    MTLPixelFormat colorFmt = rtv->tex.pixelFormat;
    MTLPixelFormat depthFmt = has_depth ? dsv->tex.pixelFormat : MTLPixelFormatInvalid;

    WGMtlShader *vs = (WGMtlShader *)wd->cur_vs;
    WGMtlShader *ps = (WGMtlShader *)wd->cur_ps;
    bool programmable = (vs && vs->fn && ps && ps->fn);  // guest bound its own shaders

    id<MTLRenderPipelineState> pso;
    if (programmable)
        pso = get_prog_pipeline(wd, vs, ps, (WGMtlInputLayout *)wd->cur_input_layout,
                                wd->cur_blend, colorFmt, depthFmt, wd->cur_stride);
    else
        pso = get_pipeline(wd, textured, wd->cur_blend, colorFmt, depthFmt);
    if (!pso) return;
    ensure_sampler(wd);
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rtv->tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;   // preserve prior contents
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (has_depth) {
            rp.depthAttachment.texture = dsv->tex;
            rp.depthAttachment.loadAction = MTLLoadActionLoad;   // preserve cleared depth
            rp.depthAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLCommandBuffer> cb = [wd->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        if (has_depth) [enc setDepthStencilState:ensure_depth_state(wd)];
        if (wd->has_vp) {
            MTLViewport vp = { wd->vp_x, wd->vp_y, wd->vp_w, wd->vp_h, 0.0, 1.0 };
            [enc setViewport:vp];
        }
        [enc setVertexBuffer:vb->buf offset:wd->cur_offset atIndex:0];
        if (programmable) {
            // Attributes read buffer 0 via the vertex descriptor; CBs at 16+, textures/samplers.
            WGMtlResource *vscb = (WGMtlResource *)wd->cur_vs_cbuf;
            if (vscb && vscb->buf) [enc setVertexBuffer:vscb->buf offset:0 atIndex:16];
            WGMtlResource *pscb = (WGMtlResource *)wd->cur_ps_cbuf;
            if (pscb && pscb->buf) [enc setFragmentBuffer:pscb->buf offset:0 atIndex:16];
            if (textured) { [enc setFragmentTexture:tex->tex atIndex:0]; [enc setFragmentSamplerState:wd->sampler atIndex:0]; }
        } else {
            uint32_t sf = wd->cur_stride ? wd->cur_stride / 4 : (textured ? 4 : 6);
            [enc setVertexBytes:&sf length:sizeof(sf) atIndex:1];
            WGMtlResource *cbuf = (WGMtlResource *)wd->cur_vs_cbuf;
            if (cbuf && cbuf->buf) [enc setVertexBuffer:cbuf->buf offset:0 atIndex:2]; // guest MVP
            else { static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                   [enc setVertexBytes:identity length:sizeof(identity) atIndex:2]; }
            if (textured) { [enc setFragmentTexture:tex->tex atIndex:0]; [enc setFragmentSamplerState:wd->sampler atIndex:0]; }
        }
        MTLPrimitiveType pt = topo_map(wd->cur_topo);
        if (indexed) {
            NSUInteger isz = (wd->cur_ibuf_type == MTLIndexTypeUInt32) ? 4 : 2;
            [enc drawIndexedPrimitives:pt indexCount:count indexType:wd->cur_ibuf_type
                           indexBuffer:ib->buf indexBufferOffset:(wd->cur_ibuf_offset + start * isz)
                         instanceCount:1 baseVertex:base_vertex baseInstance:0];
        } else {
            [enc drawPrimitives:pt vertexStart:start vertexCount:count];
        }
        [enc endEncoding];
        [cb commit];
    }
}

static void wg_first_draw_note(uint32_t count) {
    static bool s_first_draw; if (!s_first_draw) { s_first_draw = true;
        fprintf(stderr, "[VIEWER] first Draw (%u verts) — guest is drawing geometry\n", count); }
}
void wg_gpu_draw(WGGpuDevice dev, uint32_t count, uint32_t start) {
    WGMtlDevice *wd = dev; wg_first_draw_note(count); if (wd) wg_encode(wd, false, count, start, 0);
}
void wg_gpu_draw_indexed(WGGpuDevice dev, uint32_t count, uint32_t start, int base_vertex) {
    WGMtlDevice *wd = dev; wg_first_draw_note(count); if (wd) wg_encode(wd, true, count, start, base_vertex);
}
