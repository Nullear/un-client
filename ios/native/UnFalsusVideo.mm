#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/object/object.h"

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
    id end_observer = nil;
    std::atomic_bool finished = false;
    UIView *render_view = nil;
    BOOL render_was_opaque = YES;
    BOOL layer_was_opaque = YES;
    UIColor *render_background = nil;

    static UIView *find_render_view(UIView *root) {
        NSString *layer_name = NSStringFromClass(root.layer.class);
        if ([layer_name containsString:@"Metal"] || [layer_name containsString:@"EAGL"]) return root;
        for (UIView *child in root.subviews) {
            UIView *found = find_render_view(child);
            if (found) return found;
        }
        return nil;
    }

    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("play", "absolute_path", "loop"), &UnFalsusVideo::play);
        ClassDB::bind_method(D_METHOD("stop"), &UnFalsusVideo::stop);
        ClassDB::bind_method(D_METHOD("is_finished"), &UnFalsusVideo::is_finished);
        ClassDB::bind_method(D_METHOD("set_title", "absolute_path"), &UnFalsusVideo::set_title);
        ClassDB::bind_method(D_METHOD("set_white", "opacity"), &UnFalsusVideo::set_white);
        ClassDB::bind_method(D_METHOD("is_cellular"), &UnFalsusVideo::is_cellular);
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
        [title_view removeFromSuperview];
        title_view = nil;
        [view removeFromSuperview];
        view = nil;
        if (render_view) {
            render_view.opaque = render_was_opaque;
            render_view.layer.opaque = layer_was_opaque;
            render_view.backgroundColor = render_background;
            render_view = nil;
            render_background = nil;
        }
    }

public:
    bool play(const String &absolute_path, bool loop) {
        NSString *path = [NSString stringWithUTF8String:absolute_path.utf8().get_data()];
        if (!path || ![[NSFileManager defaultManager] fileExistsAtPath:path]) return false;
        finished.store(false);
        dispatch_async(dispatch_get_main_queue(), ^{
            stop_on_main();
            UIWindow *window = UIApplication.sharedApplication.keyWindow;
            if (!window) window = UIApplication.sharedApplication.windows.firstObject;
            if (!window) { finished.store(true); return; }

            render_view = find_render_view(window.rootViewController.view);
            if (!render_view || !render_view.superview) {
                NSLog(@"[UnFalsusVideo] Godot render view is unavailable");
                finished.store(true);
                return;
            }
            render_was_opaque = render_view.opaque;
            layer_was_opaque = render_view.layer.opaque;
            render_background = render_view.backgroundColor;
            render_view.opaque = NO;
            render_view.layer.opaque = NO;
            render_view.backgroundColor = UIColor.clearColor;
            view = [[UFVideoView alloc] initWithFrame:render_view.frame];
            view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            view.userInteractionEnabled = NO;
            view.backgroundColor = UIColor.blackColor;
            AVPlayerItem *item = [AVPlayerItem playerItemWithURL:[NSURL fileURLWithPath:path]];
            player = [AVPlayer playerWithPlayerItem:item];
            AVPlayerLayer *layer = (AVPlayerLayer *)view.layer;
            layer.player = player;
            layer.videoGravity = AVLayerVideoGravityResizeAspectFill;
            [render_view.superview insertSubview:view belowSubview:render_view];
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
