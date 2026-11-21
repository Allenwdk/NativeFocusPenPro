// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <CoreBluetooth/CoreBluetooth.h>
#import <AccessorySetupKit/AccessorySetupKit.h>
#import <UIKit/UIKit.h>
#import <PencilKit/PencilKit.h>
#import <os/log.h>

static NSString * const PenName = @"Xiaomi Focus Pen Pro";

@interface UkePenPairingViewController : UIViewController <PKCanvasViewDelegate, UIPencilInteractionDelegate>
@property(nonatomic, strong) ASAccessorySession *session;
@property(nonatomic, strong) UIButton *pairButton;
@property(nonatomic, strong) UIButton *removeButton;
@property(nonatomic, strong) UILabel *status;
@property(nonatomic, strong) UIView *hoverMarker;
@property(nonatomic, strong) PKCanvasView *canvas;
@property(nonatomic) NSUInteger lastStrokeCount;
@property(nonatomic) NSUInteger pencilGestureLogs;
@end

@implementation UkePenPairingViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel *title = [UILabel new];
    title.text = @"配对焦点触控笔 Pro";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    title.numberOfLines = 0;
    title.textAlignment = NSTextAlignmentCenter;

    self.status = [UILabel new];
    self.status.text = @"正在启动系统配件选择器…";
    self.status.textColor = UIColor.secondaryLabelColor;
    self.status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    self.status.numberOfLines = 0;
    self.status.textAlignment = NSTextAlignmentCenter;
    self.pairButton = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration *button = [UIButtonConfiguration filledButtonConfiguration];
    button.title = @"查找触控笔";
    self.pairButton.configuration = button;
    self.pairButton.enabled = NO;
    [self.pairButton addTarget:self action:@selector(showPicker) forControlEvents:UIControlEventTouchUpInside];

    self.removeButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.removeButton setTitle:@"取消配对" forState:UIControlStateNormal];
    self.removeButton.hidden = YES;
    [self.removeButton addTarget:self action:@selector(removePen) forControlEvents:UIControlEventTouchUpInside];

    UIButton *clear = [UIButton buttonWithType:UIButtonTypeSystem];
    [clear setTitle:@"清空画布" forState:UIControlStateNormal];
    [clear addTarget:self action:@selector(clearCanvas) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *actions = [[UIStackView alloc] initWithArrangedSubviews:@[self.pairButton, self.removeButton, clear]];
    actions.alignment = UIStackViewAlignmentCenter;
    actions.spacing = 12;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, self.status, actions]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 8;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    self.canvas = [[PKCanvasView alloc] initWithFrame:CGRectMake(0, 0, 0, 0)];
    self.canvas.translatesAutoresizingMaskIntoConstraints = NO;
    self.canvas.backgroundColor = UIColor.secondarySystemBackgroundColor;
    self.canvas.drawingPolicy = PKCanvasViewDrawingPolicyPencilOnly;
    self.canvas.tool = [[PKInkingTool alloc] initWithInkType:PKInkTypePen color:UIColor.labelColor width:4];
    self.canvas.scrollEnabled = NO;
    self.canvas.delegate = self;
    self.canvas.accessibilityLabel = @"触控笔测试画布";
    [self.view addSubview:self.canvas];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-24],
        [self.canvas.topAnchor constraintEqualToAnchor:stack.bottomAnchor constant:12],
        [self.canvas.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12],
        [self.canvas.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12],
        [self.canvas.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
    ]];

    self.hoverMarker = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 16, 16)];
    self.hoverMarker.layer.cornerRadius = 8;
    self.hoverMarker.layer.borderWidth = 2;
    self.hoverMarker.layer.borderColor = UIColor.systemBlueColor.CGColor;
    self.hoverMarker.userInteractionEnabled = NO;
    self.hoverMarker.hidden = YES;
    [self.view addSubview:self.hoverMarker];
    UIHoverGestureRecognizer *hover = [[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(pencilHover:)];
    hover.allowedTouchTypes = @[@(UITouchTypePencil)];
    [self.view addGestureRecognizer:hover];
    UIPencilInteraction *pencilInteraction = [UIPencilInteraction new];
    pencilInteraction.delegate = self;
    [self.view addInteraction:pencilInteraction];

    self.session = [ASAccessorySession new];
    if (!self.session) {
        self.status.text = @"系统配件选择器不可用。";
        return;
    }
    __weak UkePenPairingViewController *owner = self;
    [self.session activateWithQueue:dispatch_get_main_queue() eventHandler:^(ASAccessoryEvent *event) {
        [owner handleEvent:event];
    }];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(enteredBackground:)
                                               name:UIApplicationDidEnterBackgroundNotification object:nil];
}

- (void)clearCanvas {
    self.canvas.drawing = [PKDrawing new];
    self.lastStrokeCount = 0;
}

- (void)canvasViewDrawingDidChange:(PKCanvasView *)canvasView {
    NSArray<PKStroke *> *strokes = canvasView.drawing.strokes;
    const NSUInteger count = strokes.count;
    if (count != self.lastStrokeCount && count <= 24) {
        os_log(OS_LOG_DEFAULT, "UKE P81c PencilKit stroke count=%lu", (unsigned long)count);
        PKStrokePath *path = strokes.lastObject.path;
        if (path.count) {
            CGFloat minimum = path[0].force;
            CGFloat maximum = minimum;
            const NSUInteger stride = MAX(1, path.count / 32);
            for (NSUInteger i = stride; i < path.count; i += stride) {
                minimum = MIN(minimum, path[i].force);
                maximum = MAX(maximum, path[i].force);
            }
            os_log(OS_LOG_DEFAULT, "UKE P81c PencilKit force min=%ld max=%ld points=%lu",
                   (long)(minimum * 1000), (long)(maximum * 1000), (unsigned long)path.count);
        }
    }
    self.lastStrokeCount = count;
}

- (void)pencilHover:(UIHoverGestureRecognizer *)hover {
    const UIGestureRecognizerState state = hover.state;
    const CGPoint point = [hover locationInView:self.view];
    self.hoverMarker.hidden = state != UIGestureRecognizerStateBegan && state != UIGestureRecognizerStateChanged;
    if (!self.hoverMarker.hidden) self.hoverMarker.center = point;
    if (state == UIGestureRecognizerStateBegan || state == UIGestureRecognizerStateEnded ||
        state == UIGestureRecognizerStateCancelled) {
        os_log(OS_LOG_DEFAULT, "UKE P81c UIKit pencil hover state=%ld x=%ld y=%ld z=%ld altitude=%ld",
               (long)state, (long)point.x, (long)point.y, (long)(hover.zOffset * 1000),
               (long)(hover.altitudeAngle * 1000));
    }
}

- (void)pencilInteraction:(UIPencilInteraction *)interaction didReceiveTap:(UIPencilInteractionTap *)tap {
    (void)interaction;
    if (self.pencilGestureLogs++ < 24)
        os_log(OS_LOG_DEFAULT, "UKE P81c UIKit Pencil double tap hover=%d", tap.hoverPose != nil);
}

- (void)pencilInteraction:(UIPencilInteraction *)interaction didReceiveSqueeze:(UIPencilInteractionSqueeze *)squeeze {
    (void)interaction;
    if (self.pencilGestureLogs++ < 24)
        os_log(OS_LOG_DEFAULT, "UKE P81c UIKit Pencil squeeze phase=%ld hover=%d",
               (long)squeeze.phase, squeeze.hoverPose != nil);
}

- (ASAccessory *)pairedPen {
    ASAccessory *selected = nil;
    for (ASAccessory *accessory in self.session.accessories) {
        if (accessory.state != ASAccessoryStateAuthorized || !accessory.bluetoothIdentifier ||
            ![accessory.displayName isEqualToString:PenName]) continue;
        if (selected && ![selected.bluetoothIdentifier isEqual:accessory.bluetoothIdentifier]) return nil;
        selected = accessory;
    }
    return selected;
}

- (void)updatePairingStatus {
    const BOOL paired = [self pairedPen] != nil;
    self.pairButton.hidden = paired;
    self.pairButton.enabled = YES;
    self.removeButton.hidden = !paired;
    self.removeButton.enabled = YES;
    self.status.text = paired ? @"系统已配对。取下笔后会自动连接。"
                              : @"打开蓝牙，取下触控笔，再把它吸回充电位。";
}

- (void)removePen {
    ASAccessory *accessory = [self pairedPen];
    if (!accessory) return;
    self.removeButton.enabled = NO;
    __weak UkePenPairingViewController *owner = self;
    [self.session removeAccessory:accessory completionHandler:^(NSError *error) {
        [owner updatePairingStatus];
        if (error) owner.status.text = error.localizedDescription;
    }];
}

- (void)enteredBackground:(NSNotification *)notification {
    (void)notification;
    self.hoverMarker.hidden = YES;
}

- (void)handleEvent:(ASAccessoryEvent *)event {
    os_log(OS_LOG_DEFAULT, "UKE P81c AccessorySetupKit event=%ld error=%{public}@/%ld",
           (long)event.eventType, event.error.domain ?: @"", (long)event.error.code);
    switch (event.eventType) {
        case ASAccessoryEventTypeActivated:
            [self updatePairingStatus];
            break;
        case ASAccessoryEventTypeInvalidated:
            self.pairButton.enabled = NO;
            self.removeButton.hidden = YES;
            self.status.text = event.error.localizedDescription ?: @"系统配件会话已结束。";
            break;
        case ASAccessoryEventTypePickerSetupPairing:
            self.status.text = @"系统正在配对触控笔…";
            break;
        case ASAccessoryEventTypeAccessoryAdded:
        case ASAccessoryEventTypeAccessoryChanged:
        case ASAccessoryEventTypeAccessoryRemoved:
            [self updatePairingStatus];
            break;
        default:
            break;
    }
}

- (void)showPicker {
    ASDiscoveryDescriptor *descriptor = [ASDiscoveryDescriptor new];
    descriptor.bluetoothServiceUUID = [CBUUID UUIDWithString:@"FEF5"];
    descriptor.bluetoothNameSubstring = PenName;
    descriptor.supportedOptions = ASAccessorySupportBluetoothPairingLE | ASAccessorySupportBluetoothHID;
    ASPickerDisplayItem *item = [[ASPickerDisplayItem alloc] initWithName:PenName
        productImage:[UIImage systemImageNamed:@"pencil.tip"] descriptor:descriptor];
    if (!item) {
        self.status.text = @"无法创建触控笔选择项。";
        return;
    }
    self.pairButton.enabled = NO;
    __weak UkePenPairingViewController *owner = self;
    [self.session showPickerForDisplayItems:@[item] completionHandler:^(NSError *error) {
        os_log(OS_LOG_DEFAULT, "UKE P81c AccessorySetupKit picker error=%{public}@/%ld",
               error.domain ?: @"", (long)error.code);
        owner.pairButton.enabled = YES;
        if (error) owner.status.text = error.localizedDescription;
    }];
}
@end

@interface UkePenPairingAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation UkePenPairingAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)app;
    (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [UkePenPairingViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

@interface UkePenPairingApplication : UIApplication
@property(nonatomic) unsigned loggedPencilEdges;
@property(nonatomic) unsigned pencilMoves;
@end

@implementation UkePenPairingApplication
- (void)sendEvent:(UIEvent *)event {
    if (event.type == UIEventTypeHover) {
        static unsigned remainingHoverLogs = 16;
        if (remainingHoverLogs) {
            UIWindow *window = nil;
            NSUInteger direct = 0, pencil = 0, pointer = 0;
            for (UITouch *touch in event.allTouches) {
                window = touch.window ?: window;
                if (touch.type == UITouchTypePencil) ++pencil;
                else if (touch.type == UITouchTypeIndirectPointer) ++pointer;
                else if (touch.type == UITouchTypeDirect) ++direct;
            }
            --remainingHoverLogs;
            os_log(OS_LOG_DEFAULT, "UKE P81c UIKit hover event direct=%lu pencil=%lu pointer=%lu window=%d",
                   (unsigned long)direct, (unsigned long)pencil, (unsigned long)pointer, window != nil);
        }
    }
    if (event.type == UIEventTypeTouches) {
        for (UITouch *touch in event.allTouches) {
            UIWindow *window = touch.window;
            if (![window.rootViewController isKindOfClass:UkePenPairingViewController.class]) continue;
            if (touch.type == UITouchTypePencil && touch.phase != UITouchPhaseMoved &&
                touch.phase != UITouchPhaseStationary && self.loggedPencilEdges < 48) {
                ++self.loggedPencilEdges;
                os_log(OS_LOG_DEFAULT, "UKE P81c Pencil touch phase=%ld force=%ld max=%ld",
                       (long)touch.phase, (long)(touch.force * 1000), (long)(touch.maximumPossibleForce * 1000));
            }
            if (touch.type == UITouchTypePencil && touch.phase == UITouchPhaseMoved && self.pencilMoves < 256) {
                if (++self.pencilMoves % 8 == 1)
                    os_log(OS_LOG_DEFAULT, "UKE P81c Pencil move force=%ld max=%ld estimated=%d",
                           (long)(touch.force * 1000), (long)(touch.maximumPossibleForce * 1000),
                           (touch.estimatedProperties & UITouchPropertyForce) != 0);
            }
            if (touch.phase != UITouchPhaseBegan) continue;
            const CGPoint point = [touch locationInView:window];
            const CGSize size = window.bounds.size;
            os_log(OS_LOG_DEFAULT, "UKE P81c input begin type=%ld x=%ld y=%ld w=%ld h=%ld",
                   (long)touch.type, (long)point.x, (long)point.y, (long)size.width, (long)size.height);
        }
    }
    [super sendEvent:event];
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, NSStringFromClass(UkePenPairingApplication.class),
                                 NSStringFromClass(UkePenPairingAppDelegate.class));
    }
}
