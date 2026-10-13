#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/variant/variant.h"

#import <AVFoundation/AVFoundation.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <UIKit/UIKit.h>
#include <cstring>
#include <netinet/in.h>

#include <algorithm>
#include <atomic>

extern "C" void register_unfalsus_video_types_c();
extern "C" void unregister_unfalsus_video_types_c();

@interface UFVideoView : UIView
@end

@implementation UFVideoView
+ (Class)layerClass { return [AVPlayerLayer class]; }
@end

class UnFalsusVideo : public Object {
    GDCLASS(UnFalsusVideo, Object);

    UFVideoView *view = nil;
    UIImageView *title_view = nil;
    UIView *white_view = nil;
    AVPlayer *player = nil;
    AVPlayerItemVideoOutput *output = nil;
    int frame_width = 0;
    int frame_height = 0;
    id end_observer = nil;
    std::atomic_bool finished = false;
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("play", "absolute_path", "loop"), &UnFalsusVideo::play);
        ClassDB::bind_method(D_METHOD("stop"), &UnFalsusVideo::stop);
        ClassDB::bind_method(D_METHOD("is_finished"), &UnFalsusVideo::is_finished);
        ClassDB::bind_method(D_METHOD("set_title", "absolute_path"), &UnFalsusVideo::set_title);
        ClassDB::bind_method(D_METHOD("set_white", "opacity"), &UnFalsusVideo::set_white);
        ClassDB::bind_method(D_METHOD("is_cellular"), &UnFalsusVideo::is_cellular);
        ClassDB::bind_method(D_METHOD("get_frame"), &UnFalsusVideo::get_frame);
        ClassDB::bind_method(D_METHOD("get_frame_width"), &UnFalsusVideo::get_frame_width);
        ClassDB::bind_method(D_METHOD("get_frame_height"), &UnFalsusVideo::get_frame_height);
    }

    void stop_on_main() {
        [view setHidden:YES];
        [white_view setHidden:YES];
        if (end_observer) {
            [[NSNotificationCenter defaultCenter] removeObserver:end_observer];
            end_observer = nil;
        }
        [player pause];
        player = nil;
        output = nil;
        frame_width = 0;
        frame_height = 0;
        [title_view removeFromSuperview];
        title_view = nil;
        [view removeFromSuperview];
        view = nil;
    }

public:
    PackedByteArray get_frame() {
        PackedByteArray bytes;
        if (!output || !player) return bytes;
        __block CVPixelBufferRef buffer = nil;
        void (^read_frame)(void) = ^{
            CMTime item_time = [output itemTimeForHostTime:CACurrentMediaTime()];
            if ([output hasNewPixelBufferForItemTime:item_time])
                buffer = [output copyPixelBufferForItemTime:item_time itemTimeForDisplay:nil];
        };
        if ([NSThread isMainThread]) read_frame();
        else dispatch_sync(dispatch_get_main_queue(), read_frame);
        if (!buffer) return bytes;
        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        const size_t width = CVPixelBufferGetWidth(buffer);
        const size_t height = CVPixelBufferGetHeight(buffer);
        frame_width = int(width);
        frame_height = int(height);
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        const uint8_t *source = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
        if (source && CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA) {
            bytes.resize(int(width * height * 4));
            uint8_t *destination = bytes.ptrw();
            for (size_t y = 0; y < height; ++y) {
                const uint8_t *row = source + y * stride;
                for (size_t x = 0; x < width; ++x) {
                    const uint8_t *pixel = row + x * 4;
                    uint8_t *out_pixel = destination + (y * width + x) * 4;
                    out_pixel[0] = pixel[2];
                    out_pixel[1] = pixel[1];
                    out_pixel[2] = pixel[0];
                    out_pixel[3] = pixel[3];
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(buffer);
        return bytes;
    }

    int get_frame_width() const { return frame_width; }
    int get_frame_height() const { return frame_height; }

    bool play(const String &absolute_path, bool loop) {
        NSString *path = [NSString stringWithUTF8String:absolute_path.utf8().get_data()];
        if (!path || ![[NSFileManager defaultManager] fileExistsAtPath:path]) return false;
        finished.store(false);
        dispatch_async(dispatch_get_main_queue(), ^{
            stop_on_main();
            UIWindow *window = UIApplication.sharedApplication.keyWindow;
            if (!window) window = UIApplication.sharedApplication.windows.firstObject;
            if (!window) { finished.store(true); return; }

            AVPlayerItem *item = [AVPlayerItem playerItemWithURL:[NSURL fileURLWithPath:path]];
            NSDictionary *settings = @{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) };
            output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:settings];
            [item addOutput:output];
            player = [AVPlayer playerWithPlayerItem:item];
            if (white_view) [window bringSubviewToFront:white_view];

            end_observer = [[NSNotificationCenter defaultCenter]
                addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:item queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *notification) {
                    if (loop) {
                        [player seekToTime:kCMTimeZero completionHandler:^(BOOL done) {
                            if (done) [player play];
                        }];
                    } else {
                        finished.store(true);
                    }
                }];
            [player play];
        });
        return true;
    }

    void set_title(const String &absolute_path) {
        NSString *path = [NSString stringWithUTF8String:absolute_path.utf8().get_data()];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!view) return;
            [title_view removeFromSuperview];
            UIImage *image = [UIImage imageWithContentsOfFile:path];
            if (!image) return;
            UIView *host = view;
            CGFloat scale = MIN(host.bounds.size.width / 1920.0, host.bounds.size.height / 1080.0);
            CGFloat width = 1415.0 * scale;
            CGFloat height = 395.0 * scale;
            CGFloat center_y = host.bounds.size.height / 2.0 + 354.0 * scale;
            title_view = [[UIImageView alloc] initWithFrame:CGRectMake(
                (host.bounds.size.width - width) / 2.0, center_y - height / 2.0, width, height)];
            title_view.image = image;
            title_view.userInteractionEnabled = NO;
            [host addSubview:title_view];
        });
    }

    void set_white(float opacity) {
        CGFloat alpha = std::clamp(opacity, 0.0f, 1.0f);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!white_view && alpha <= 0) return;
            UIWindow *window = UIApplication.sharedApplication.keyWindow;
            if (!window) window = UIApplication.sharedApplication.windows.firstObject;
            if (!window) return;
            if (!white_view) {
                white_view = [[UIView alloc] initWithFrame:window.bounds];
                white_view.backgroundColor = UIColor.whiteColor;
                white_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
                white_view.userInteractionEnabled = NO;
                [window addSubview:white_view];
            }
            white_view.alpha = alpha;
            [window bringSubviewToFront:white_view];
            if (alpha <= 0) { [white_view removeFromSuperview]; white_view = nil; }
        });
    }

    void stop() {
        finished.store(false);
        if ([NSThread isMainThread]) stop_on_main();
        else dispatch_sync(dispatch_get_main_queue(), ^{ stop_on_main(); });
    }

    bool is_finished() const { return finished.load(); }

    bool is_cellular() const {
        struct sockaddr_in zero;
        memset(&zero, 0, sizeof(zero));
        zero.sin_len = sizeof(zero);
        zero.sin_family = AF_INET;
        SCNetworkReachabilityRef ref = SCNetworkReachabilityCreateWithAddress(
            kCFAllocatorDefault, reinterpret_cast<const struct sockaddr *>(&zero));
        if (!ref) return false;
        SCNetworkReachabilityFlags flags = 0;
        const Boolean ok = SCNetworkReachabilityGetFlags(ref, &flags);
        CFRelease(ref);
        return ok && (flags & kSCNetworkReachabilityFlagsIsWWAN) != 0;
    }

    ~UnFalsusVideo() { stop(); }
};

static UnFalsusVideo *singleton = nullptr;

extern "C" void register_unfalsus_video_types_c() {
    singleton = memnew(UnFalsusVideo);
    Engine::get_singleton()->add_singleton(Engine::Singleton("UnFalsusVideo", singleton));
}

extern "C" void unregister_unfalsus_video_types_c() {
    if (singleton) { memdelete(singleton); singleton = nullptr; }
}

void register_unfalsus_video_types() { register_unfalsus_video_types_c(); }
void unregister_unfalsus_video_types() { unregister_unfalsus_video_types_c(); }
