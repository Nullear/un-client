#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/object/object.h"

#import <AVFoundation/AVFoundation.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAEAGLLayer.h>
#import <QuartzCore/CAMetalLayer.h>
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

@interface UFDebugOverlay : UIView
@property(nonatomic, strong) NSMutableArray<NSString *> *entries;
@property(nonatomic, strong) UIButton *toggle;
@property(nonatomic, strong) UIView *panel;
@property(nonatomic, strong) UITextView *text;
- (void)addEntry:(NSString *)entry;
@end

@implementation UFDebugOverlay
- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.entries = [NSMutableArray array];
    self.userInteractionEnabled = YES;
    self.toggle = [UIButton buttonWithType:UIButtonTypeSystem];
    self.toggle.frame = CGRectMake(12, 42, 132, 38);
    self.toggle.backgroundColor = [UIColor colorWithWhite:0 alpha:.88];
    [self.toggle setTitle:@"iOS Debug" forState:UIControlStateNormal];
    [self.toggle setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.toggle.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    self.toggle.layer.cornerRadius = 7;
    [self.toggle addTarget:self action:@selector(togglePanel) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:self.toggle];
    self.panel = [[UIView alloc] initWithFrame:CGRectMake(12, 88, frame.size.width - 24, frame.size.height - 112)];
    self.panel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.panel.backgroundColor = [UIColor colorWithWhite:0 alpha:.96];
    self.panel.layer.cornerRadius = 8;
    self.panel.hidden = YES;
    [self addSubview:self.panel];
    UIButton *copy = [UIButton buttonWithType:UIButtonTypeSystem];
    copy.frame = CGRectMake(8, 8, 90, 34);
    [copy setTitle:@"复制全部" forState:UIControlStateNormal];
    [copy setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [copy addTarget:self action:@selector(copyLog) forControlEvents:UIControlEventTouchUpInside];
    [self.panel addSubview:copy];
    UIButton *close = [UIButton buttonWithType:UIButtonTypeSystem];
    close.frame = CGRectMake(self.panel.bounds.size.width - 82, 8, 74, 34);
    close.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
    [close setTitle:@"关闭" forState:UIControlStateNormal];
    [close setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [close addTarget:self action:@selector(togglePanel) forControlEvents:UIControlEventTouchUpInside];
    [self.panel addSubview:close];
    self.text = [[UITextView alloc] initWithFrame:CGRectMake(8, 50, self.panel.bounds.size.width - 16, self.panel.bounds.size.height - 58)];
    self.text.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.text.backgroundColor = UIColor.blackColor;
    self.text.textColor = UIColor.whiteColor;
    self.text.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.text.editable = NO;
    self.text.selectable = YES;
    [self.panel addSubview:self.text];
    return self;
}
- (void)addEntry:(NSString *)entry {
    if (!entry) return;
    [self.entries addObject:entry];
    while (self.entries.count > 200) [self.entries removeObjectAtIndex:0];
    self.text.text = [self.entries componentsJoinedByString:@"\n"];
    [self.text scrollRangeToVisible:NSMakeRange(self.text.text.length, 0)];
}
- (void)togglePanel { self.panel.hidden = !self.panel.hidden; }
- (void)copyLog { [UIPasteboard generalPasteboard].string = self.text.text ?: @""; }
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
    UFDebugOverlay *debug_overlay = nil;
    BOOL render_was_opaque = YES;
    BOOL layer_was_opaque = YES;
    UIColor *render_background = nil;

    static UIView *find_render_view(UIView *root) {
        NSString *layer_name = NSStringFromClass(root.layer.class);
        if ([root.layer isKindOfClass:[CAEAGLLayer class]] ||
                [root.layer isKindOfClass:[CAMetalLayer class]] ||
                [layer_name containsString:@"OpenGLLayer"]) return root;
        for (UIView *child in root.subviews) {
            UIView *found = find_render_view(child);
            if (found) return found;
        }
        // Godot 4.4 may expose only its wrapper as GodotView. Keep this as a
        // last resort so a real renderer child always wins when present.
        if ([NSStringFromClass(root.class) isEqualToString:@"GodotView"]) return root;
        return nil;
    }

    static NSString *describe_view_tree(UIView *root, NSUInteger depth) {
        if (!root || depth > 5) return @"";
        NSMutableString *result = [NSMutableString stringWithFormat:@"%@%@ layer=%@ frame=%@\n",
            [@"  " stringByPaddingToLength:depth * 2 withString:@" " startingAtIndex:0],
            NSStringFromClass(root.class), NSStringFromClass(root.layer.class), NSStringFromCGRect(root.frame)];
        for (UIView *child in root.subviews)
            [result appendString:describe_view_tree(child, depth + 1)];
        return result;
    }

    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("play", "absolute_path", "loop"), &UnFalsusVideo::play);
        ClassDB::bind_method(D_METHOD("stop"), &UnFalsusVideo::stop);
        ClassDB::bind_method(D_METHOD("is_finished"), &UnFalsusVideo::is_finished);
        ClassDB::bind_method(D_METHOD("set_title", "absolute_path"), &UnFalsusVideo::set_title);
        ClassDB::bind_method(D_METHOD("set_white", "opacity"), &UnFalsusVideo::set_white);
        ClassDB::bind_method(D_METHOD("is_cellular"), &UnFalsusVideo::is_cellular);
    }

    void debug_log(NSString *message) {
        NSLog(@"[UnFalsusVideo] %@", message);
        if (debug_overlay) [debug_overlay addEntry:message];
    }

    void ensure_debug_overlay(UIWindow *window) {
        if (!window || debug_overlay) return;
        debug_overlay = [[UFDebugOverlay alloc] initWithFrame:window.bounds];
        debug_overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [window addSubview:debug_overlay];
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
            ensure_debug_overlay(window);
            debug_log([NSString stringWithFormat:@"play loop=%@ path=%@", loop ? @"YES" : @"NO", path]);
            if (!window) { debug_log(@"no UIWindow"); finished.store(true); return; }

            render_view = find_render_view(window.rootViewController.view);
            if (!render_view || !render_view.superview) {
                debug_log([NSString stringWithFormat:@"render view unavailable tree=\n%@",
                    describe_view_tree(window.rootViewController.view, 0)]);
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
            debug_log([NSString stringWithFormat:@"playing behind view=%@ layer=%@ frame=%@ opaque=%@",
                NSStringFromClass(render_view.class), NSStringFromClass(render_view.layer.class),
                NSStringFromCGRect(render_view.frame), render_view.opaque ? @"YES" : @"NO"]);
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
                        debug_log(@"player reached end");
                    }
                }];
            [player play];
            debug_log([NSString stringWithFormat:@"AVPlayer started rate=%.2f", player.rate]);
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
        if (debug_overlay) debug_log(@"stop requested");
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
