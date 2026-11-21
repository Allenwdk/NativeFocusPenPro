// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "UkePenRadio.h"
#import "UkePenPower.h"
#import "UkePenRadioProtocol.h"
#if defined(UKE_PEN_STOCK_DOCK_BRIDGE)
#import "UkePenDock.h"
#endif
#import <CoreBluetooth/CoreBluetooth.h>
#import <IOKit/IOKitLib.h>
#import <os/log.h>
#include <sys/sysctl.h>

static NSString * const ServiceUUID = @"0000FE10-AA6C-462A-964A-7F2ED5B3E512";
static NSString * const WriteUUID = @"0000FE11-AA6C-462A-964A-7F2ED5B3E512";
static NSString * const NotifyUUID = @"0000FE12-AA6C-462A-964A-7F2ED5B3E512";
static NSString * const PenName = @"Xiaomi Focus Pen Pro";
static NSString * const PencilProTag = @"A2538";
#if defined(UKE_PEN_ACTIVE_INTERVAL)
static NSString * const PenIntervalTag = @"UkeP81c";
#else
// CBCentralManagerConnectionLatencyToString in 24A446 names -8 Low-Fixed.
static const NSInteger UkePenConnectionLatencyLowFixed = -8;
#endif
extern NSString * const CBManagerIsPrivilegedDaemonKey __attribute__((weak_import));

// IOPowerSources accessory publication. This source is backed by Uke's PMIC,
// and never participates in the tablet's internal battery selection.
typedef struct OpaqueIOPSPowerSourceID *IOPSPowerSourceID;
extern IOReturn IOPSCreatePowerSource(IOPSPowerSourceID *source);
extern IOReturn IOPSSetPowerSourceDetails(IOPSPowerSourceID source, CFDictionaryRef details);
extern IOReturn IOPSReleasePowerSource(IOPSPowerSourceID source);

// Read-only system bond access, verified against the 24A446 CoreBluetooth image.
@interface CBPairingAgent : NSObject
@property(nonatomic, weak) id delegate;
- (NSArray<CBPeer *> *)retrievePairedPeers;
- (BOOL)isPeerPaired:(CBPeer *)peer;
- (void)pairPeer:(CBPeer *)peer useMITM:(BOOL)useMITM;
- (void)respondToPairingRequest:(CBPeer *)peer type:(NSInteger)type accept:(BOOL)accept
                           data:(NSDictionary *)data;
@end

@interface CBCentralManager (UkePenRadio)
@property(nonatomic, readonly) CBPairingAgent *sharedPairingAgent;
- (id)retrieveAddressForPeripheral:(CBPeripheral *)peripheral;
- (CBPeripheral *)retrievePeripheralWithAddress:(NSString *)address;
- (void)setDesiredConnectionLatency:(NSInteger)latency
                      forPeripheral:(CBPeripheral *)peripheral;
@end

@interface CBPeer (UkePencilIdentity)
- (void)tag:(NSString *)tag;
- (BOOL)hasTag:(NSString *)tag;
@end

@interface CBPeripheral (UkePencilAddress)
@property(nonatomic, readonly) NSString *BDAddress;
@end

static NSData *decodePeerAddress(id address) {
    if ([address isKindOfClass:NSData.class] && [address length] == 6) return address;
    if (![address isKindOfClass:NSString.class]) return nil;
    NSArray<NSString *> *fields = [address componentsSeparatedByCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@":-"]];
    if (fields.count != 6) return nil;
    uint8_t bytes[6];
    for (NSUInteger i = 0; i < fields.count; ++i) {
        unsigned value;
        NSScanner *scanner = [NSScanner scannerWithString:fields[i]];
        if (fields[i].length != 2 || ![scanner scanHexInt:&value] || !scanner.isAtEnd) return nil;
        bytes[i] = (uint8_t)value;
    }
    return [NSData dataWithBytes:bytes length:sizeof(bytes)];
}

static BOOL matchesDockAddress(NSData *address, const struct UkePenPowerState *state) {
    if (state->docked != 1 || !state->address_valid || address.length != sizeof(state->address)) return NO;
    const uint8_t *bytes = address.bytes;
    if (memcmp(bytes, state->address, sizeof(state->address)) == 0) return YES;
    // The CoreBluetooth NSData interface can expose the wire byte order.
    for (unsigned i = 0; i < sizeof(state->address); ++i)
        if (bytes[i] != state->address[sizeof(state->address) - 1 - i]) return NO;
    return YES;
}

static NSData *peerAddress(CBCentralManager *central, CBPeripheral *peer, const struct UkePenPowerState *state) {
    NSData *address = [peer respondsToSelector:@selector(BDAddress)] ? decodePeerAddress(peer.BDAddress) : nil;
    if (!matchesDockAddress(address, state) && [central respondsToSelector:@selector(retrieveAddressForPeripheral:)]) {
        NSData *retrieved = decodePeerAddress([central retrieveAddressForPeripheral:peer]);
        if (matchesDockAddress(retrieved, state) || !address) address = retrieved;
    }
    return address;
}

static BOOL resolvesDockPeer(CBCentralManager *central, CBPeripheral *peer,
                            const struct UkePenPowerState *state) {
    if (!peer.identifier || state->docked != 1 || !state->address_valid ||
        ![central respondsToSelector:@selector(retrievePeripheralWithAddress:)]) return NO;
    const uint8_t *b = state->address;
    NSString *address = [NSString stringWithFormat:@"%02X:%02X:%02X:%02X:%02X:%02X",
        (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3], (unsigned)b[4], (unsigned)b[5]];
    CBPeripheral *resolved = [central retrievePeripheralWithAddress:address];
    return [resolved.identifier isEqual:peer.identifier];
}

static BOOL validHostAddress(NSData *data) {
    if (data.length != UKE_P81C_ADDRESS_BYTES) return NO;
    const uint8_t *bytes = data.bytes;
    return (bytes[0] & 3) == 2;  // The Uke controller is programmed with a locally administered address.
}

static NSData *hostAddress(void) {
    io_registry_entry_t chosen = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/chosen");
    id value = nil;
    if (chosen) {
        value = CFBridgingRelease(IORegistryEntryCreateCFProperty(chosen, CFSTR("mac-address-bluetooth0"),
                                                                 kCFAllocatorDefault, 0));
        IOObjectRelease(chosen);
    }
    if ([value isKindOfClass:NSData.class] && validHostAddress(value)) {
        os_log(OS_LOG_DEFAULT, "UKE P81c radio host address from I/O Registry");
        return value;
    }
    value = [NSBundle.mainBundle objectForInfoDictionaryKey:@"UkeP81cHostAddress"];
    if ([value isKindOfClass:NSData.class] && validHostAddress(value)) {
        os_log(OS_LOG_DEFAULT, "UKE P81c radio host address from payload ADT identity");
        return value;
    }
    return nil;
}

@interface UkePenRadio () <CBCentralManagerDelegate, CBPeripheralDelegate> {
    UkePenRadioProtocol _protocol;
}
@property(nonatomic, strong) CBCentralManager *central;
@property(nonatomic, strong) CBPeripheral *pen;
@property(nonatomic, strong) CBCharacteristic *command;
@property(nonatomic, strong) CBCharacteristic *notifications;
@property(nonatomic, strong) CBCharacteristic *pnpId;
#if defined(UKE_PEN_STOCK_DOCK_BRIDGE)
@property(nonatomic, strong) UkePenDock *nativeDock;
@property(nonatomic, strong) CBPeripheral *verifiedDockPeer;
@property(nonatomic, copy) NSData *verifiedDockAddress;
#else
@property(nonatomic, strong) CBPairingAgent *pairingAgent;
#endif
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSTimer *replyReadTimer;
@property(nonatomic, strong) NSTimer *hapticTimer;
@property(nonatomic, strong) NSData *address;
@property(nonatomic) CBCharacteristicWriteType writeType;
@property(nonatomic) unsigned pendingOpcode;
@property(nonatomic) BOOL awaitingReply;
@property(nonatomic) BOOL awaitingWrite;
@property(nonatomic) BOOL completed;
@property(nonatomic) BOOL activeLink;
@property(nonatomic) BOOL requestedSlot;
@property(nonatomic) BOOL waitingForDisconnect;
@property(nonatomic) BOOL pairingAPIUnavailable;
@property(nonatomic) BOOL multiplePens;
@property(nonatomic) NSUInteger lastBondCount;
@property(nonatomic) IOPSPowerSourceID dockPowerSource;
@property(nonatomic) struct UkePenPowerState publishedPower;
@property(nonatomic, copy) NSUUID *publishedPowerPeer;
@property(nonatomic) BOOL powerPublishFailed;
@property(nonatomic) BOOL penIdentityVerified;
@property(nonatomic) BOOL tagRequested;
@property(nonatomic) BOOL tagConfirmed;
@property(nonatomic) BOOL dockMatched;
@property(nonatomic) int dockMatchStatus;
@property(nonatomic) NSTimeInterval nextDockAddressLookup;
#if defined(UKE_PEN_ACTIVE_INTERVAL)
@property(nonatomic) BOOL intervalTagRequested;
@property(nonatomic) BOOL intervalTagConfirmed;
#else
@property(nonatomic) unsigned lowLatencyRequests;
@property(nonatomic) BOOL slowPenConnection;
@property(nonatomic) NSUInteger penLinkGeneration;
#endif
#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
@property(nonatomic) BOOL scanningForDock;
@property(nonatomic) BOOL pairingPending;
@property(nonatomic) BOOL powerStateValid;
@property(nonatomic) struct UkePenPowerState powerState;
#endif
@property(nonatomic) NSTimeInterval nextControlAttempt;
@property(nonatomic) NSTimeInterval retryControlOnLink;
@property(nonatomic, strong) NSData *penAddress;
@end

@implementation UkePenRadio

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    if (&CBManagerIsPrivilegedDaemonKey == NULL || !CBManagerIsPrivilegedDaemonKey) {
        os_log_error(OS_LOG_DEFAULT, "UKE P81c radio daemon session option unavailable");
        return nil;
    }
    _address = hostAddress();
    if (!_address) {
        os_log_error(OS_LOG_DEFAULT, "UKE P81c radio host address unavailable");
        return nil;
    }
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:dispatch_get_main_queue()
        options:@{CBCentralManagerOptionShowPowerAlertKey: @NO, CBManagerIsPrivilegedDaemonKey: @YES}];
    if (!_central) return nil;
#if defined(UKE_PEN_STOCK_DOCK_BRIDGE)
    _nativeDock = [UkePenDock new];
#endif
    _lastBondCount = NSNotFound;
    __weak UkePenRadio *owner = self;
    [NSTimer scheduledTimerWithTimeInterval:5 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        @autoreleasepool {
            [owner refreshBond];
            [owner publishProIdentity];
#if defined(UKE_PEN_ACTIVE_INTERVAL)
            [owner publishIntervalIdentity];
#endif
#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
            [owner refreshDockPower];
#endif
        }
    }];
#if defined(UKE_PEN_STOCK_DOCK_BRIDGE)
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        @autoreleasepool { [owner refreshDockPower]; }
    }];
#endif
    os_log(OS_LOG_DEFAULT, "UKE P81c radio daemon waiting for a system bond");
    return self;
}

- (void)publishProIdentity {
    if (self.tagConfirmed || !self.penIdentityVerified || !self.pen || !self.activeLink ||
        ![self.pen respondsToSelector:@selector(tag:)] || ![self.pen respondsToSelector:@selector(hasTag:)]) return;
    if ([self.pen hasTag:PencilProTag]) {
        self.tagConfirmed = YES;
        os_log(OS_LOG_DEFAULT, "UKE P81c Pencil Pro tag confirmed");
        return;
    }
    if (!self.tagRequested) {
        [self.pen tag:PencilProTag];
        self.tagRequested = YES;
        os_log(OS_LOG_DEFAULT, "UKE P81c Pencil Pro tag requested");
    }
}

#if defined(UKE_PEN_ACTIVE_INTERVAL)
- (void)publishIntervalIdentity {
    if (!self.penIdentityVerified || !self.activeLink || self.intervalTagConfirmed ||
        ![self.pen respondsToSelector:@selector(hasTag:)] ||
        ![self.pen respondsToSelector:@selector(tag:)]) return;
    if ([self.pen hasTag:PenIntervalTag]) {
        self.intervalTagConfirmed = YES;
        os_log(OS_LOG_DEFAULT, "UKE P81c BLE active interval policy tag confirmed");
    } else if (!self.intervalTagRequested) {
        self.intervalTagRequested = YES;
        [self.pen tag:PenIntervalTag];
        os_log(OS_LOG_DEFAULT, "UKE P81c BLE active interval policy tag requested");
    }
}
#endif

- (void)refreshDockPower {
    struct UkePenPowerState state = {.docked = -1, .percent = -1, .charging = -1};
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("ShengBattery"));
    if (service) {
        id data = CFBridgingRelease(IORegistryEntryCreateCFProperty(service, CFSTR(UKE_PEN_POWER_PROPERTY),
                                                                  kCFAllocatorDefault, 0));
        IOObjectRelease(service);
        if ([data isKindOfClass:NSData.class] && [data length] == sizeof(state))
            [data getBytes:&state length:sizeof(state)];
    }
#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
    self.powerState = state;
    self.powerStateValid = state.address_valid != 0;
    if (state.docked == 1 && state.address_valid && !self.pen && !self.pairingPending)
        [self startDockDiscovery];
    else if (state.docked != 1 && self.scanningForDock) {
        [self.central stopScan];
        self.scanningForDock = NO;
        os_log(OS_LOG_DEFAULT, "UKE P81c dock discovery stopped after detach");
    }
#endif
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (state.docked != 1 || !state.address_valid) self.nextDockAddressLookup = 0;
    if (state.docked == 1 && state.address_valid && self.pen &&
        !matchesDockAddress(self.penAddress, &state) && now >= self.nextDockAddressLookup) {
        self.nextDockAddressLookup = now + 3;
        self.penAddress = peerAddress(self.central, self.pen, &state);
        if (!matchesDockAddress(self.penAddress, &state) && self.penIdentityVerified &&
            resolvesDockPeer(self.central, self.pen, &state)) {
            self.penAddress = [NSData dataWithBytes:state.address length:sizeof(state.address)];
            os_log(OS_LOG_DEFAULT, "UKE P81c dock address resolved to selected system peer");
        }
    }
    const BOOL matched = matchesDockAddress(self.penAddress, &state);
    int status = state.docked == 1 ? !state.address_valid ? 1 : !self.penAddress ? 2 : !matched ? 3 : 4 : 0;
    if (status != self.dockMatchStatus) {
        self.dockMatchStatus = status;
        os_log(OS_LOG_DEFAULT, "UKE P81c dock address match state=%d", status);
    }
    if (matched != self.dockMatched) {
        self.dockMatched = matched;
        os_log(OS_LOG_DEFAULT, "UKE P81c dock peer matched=%d", matched);
    }
    NSUUID *powerPeer = matched ? self.pen.identifier : nil;
#if defined(UKE_PEN_STOCK_DOCK_BRIDGE)
    CBPairingAgent *agent = self.central.state == CBManagerStatePoweredOn &&
        [self.central respondsToSelector:@selector(sharedPairingAgent)] ? self.central.sharedPairingAgent : nil;
    if (![agent respondsToSelector:@selector(isPeerPaired:)]) agent = nil;
    if (matched && self.penIdentityVerified && self.tagConfirmed && [agent isPeerPaired:self.pen]) {
        self.verifiedDockPeer = self.pen;
        self.verifiedDockAddress = [NSData dataWithBytes:state.address length:sizeof(state.address)];
    }
    CBPeripheral *dockPeer = self.verifiedDockPeer;
    BOOL verified = dockPeer && state.docked == 1 && [agent isPeerPaired:dockPeer];
    if (!agent || (state.docked == 1 && state.address_valid && !verified)) {
        self.verifiedDockPeer = nil;
        self.verifiedDockAddress = nil;
    }
    // A transient empty PMIC address while Hall3 still says docked must
    // not destroy an already verified attachment. A different valid
    // address, physical detach, or a reset invalidates it immediately.
    BOOL publishDock = verified && self.verifiedDockAddress.length == 6 &&
        (state.address_valid
            ? memcmp(self.verifiedDockAddress.bytes, state.address, sizeof(state.address)) == 0
            : self.nativeDock.active);
    [self.nativeDock updateAddress:publishDock ? self.verifiedDockAddress : nil
                             peer:publishDock ? dockPeer.identifier : nil];
    if (publishDock) powerPeer = dockPeer.identifier;
#endif
    if (!powerPeer || state.percent < 0 || state.percent > 100) {
        if (self.dockPowerSource) {
            IOPSReleasePowerSource(self.dockPowerSource);
            self.dockPowerSource = NULL;
            os_log(OS_LOG_DEFAULT, "UKE P81c dock power source removed");
        }
        return;
    }
    const struct UkePenPowerState published = self.publishedPower;
    if (self.dockPowerSource && [powerPeer isEqual:self.publishedPowerPeer] &&
        state.percent == published.percent && state.charging == published.charging) return;
    IOPSPowerSourceID source = self.dockPowerSource;
    IOReturn result = source ? kIOReturnSuccess : IOPSCreatePowerSource(&source);
    if (result == kIOReturnSuccess) {
        NSMutableDictionary *details = [@{
            @"Type": @"Accessory Source", @"Transport Type": @"Bluetooth",
            @"Name": PenName, @"Is Present": @YES,
            @"Accessory Identifier": powerPeer.UUIDString,
            @"Current Capacity": @(state.percent), @"Max Capacity": @100,
            @"Power Source State": state.charging == 1 ? @"AC Power" : @"Battery Power",
        } mutableCopy];
        if (state.charging >= 0) details[@"Is Charging"] = @(state.charging == 1);
        result = IOPSSetPowerSourceDetails(source, (__bridge CFDictionaryRef)details);
    }
    if (result != kIOReturnSuccess) {
        if (source) IOPSReleasePowerSource(source);
        self.dockPowerSource = NULL;
        if (!self.powerPublishFailed)
            os_log_error(OS_LOG_DEFAULT, "UKE P81c dock power publication failed rc=%x", result);
        self.powerPublishFailed = YES;
        return;
    }
    self.powerPublishFailed = NO;
    self.dockPowerSource = source;
    self.publishedPower = state;
    self.publishedPowerPeer = powerPeer;
    os_log(OS_LOG_DEFAULT, "UKE P81c dock power published percent=%d charging=%d", state.percent, state.charging);
}

#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
- (void)startDockDiscovery {
    if (self.scanningForDock || self.central.state != CBManagerStatePoweredOn ||
        self.pen || !self.powerStateValid) return;
    [self.central scanForPeripheralsWithServices:@[[CBUUID UUIDWithString:@"FEF5"]]
                                         options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
    self.scanningForDock = YES;
    os_log(OS_LOG_DEFAULT, "UKE P81c dock discovery started for unpaired P81c");
}
#endif

- (void)clearLink {
    [self.timer invalidate];
    self.timer = nil;
    [self.replyReadTimer invalidate];
    self.replyReadTimer = nil;
    [self.hapticTimer invalidate];
    self.hapticTimer = nil;
    self.retryControlOnLink = 0;
#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
    if (self.scanningForDock) {
        [self.central stopScan];
        self.scanningForDock = NO;
    }
    self.pairingAgent.delegate = nil;
    self.pairingAgent = nil;
    self.pairingPending = NO;
#endif
    self.pen = nil;
    self.command = nil;
    self.notifications = nil;
    self.pnpId = nil;
    self.penAddress = nil;
    self.nextDockAddressLookup = 0;
#if defined(UKE_PEN_ACTIVE_INTERVAL)
    self.intervalTagRequested = self.intervalTagConfirmed = NO;
#else
    self.lowLatencyRequests = 0;
    self.slowPenConnection = NO;
    ++self.penLinkGeneration;
#endif
    self.penIdentityVerified = self.tagRequested = self.tagConfirmed = NO;
    self.completed = self.awaitingReply = self.awaitingWrite = NO;
    self.activeLink = self.requestedSlot = self.waitingForDisconnect = NO;
    self.pendingOpcode = 0;
    _protocol = (UkePenRadioProtocol){0};
}

- (void)setDeadline:(NSTimeInterval)seconds phase:(NSString *)phase {
    [self.timer invalidate];
    __weak UkePenRadio *owner = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:seconds repeats:NO block:^(NSTimer *timer) {
        (void)timer;
        @autoreleasepool {
            [owner fail:phase error:nil];
        }
    }];
}

- (void)fail:(NSString *)phase error:(NSError *)error {
    os_log_error(OS_LOG_DEFAULT, "UKE P81c radio failed phase=%{public}@ opcode=%02x error=%{public}@/%ld",
                 phase, self.pendingOpcode, error.domain ?: @"", (long)error.code);
    [self.replyReadTimer invalidate];
    self.replyReadTimer = nil;
    if (self.pendingOpcode == 0x5f && uke_pen_radio_skip_double_tap(&_protocol)) {
        os_log(OS_LOG_DEFAULT, "UKE P81c double tap configuration unconfirmed");
        self.awaitingReply = self.awaitingWrite = NO;
        [self sendNext];
        return;
    }
    CBPeripheral *pen = self.pen;
    if ([phase isEqualToString:@"FE12 reply"] && !self.requestedSlot && self.command &&
        pen.state == CBPeripheralStateConnected) {
        // A suspended FE12 session can still have a writable FE11. Keep it
        // available for system haptics and retry setup without cycling HID.
        self.awaitingReply = self.awaitingWrite = NO;
        self.retryControlOnLink = NSProcessInfo.processInfo.systemUptime + 15;
        os_log(OS_LOG_DEFAULT, "UKE P81c keeping FE11 after FE12 timeout");
        return;
    }
    if (self.requestedSlot && pen.state == CBPeripheralStateConnected) {
        // The pen retains allocated slots until the underlying BLE link ends.
        // A CoreBluetooth client cancel is insufficient when system HID owns it too.
        [self.timer invalidate];
        self.timer = nil;
        self.waitingForDisconnect = YES;
        self.awaitingReply = self.awaitingWrite = NO;
        os_log(OS_LOG_DEFAULT, "UKE P81c radio waiting for fresh BLE link after slot request");
        return;
    }
    pen.delegate = nil;
    [self clearLink];
    self.nextControlAttempt = NSProcessInfo.processInfo.systemUptime + 15;
    if (pen && pen.state != CBPeripheralStateDisconnected)
        [self.central cancelPeripheralConnection:pen];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central != self.central) return;
    os_log(OS_LOG_DEFAULT, "UKE P81c radio central state=%ld", (long)central.state);
    if (central.state == CBManagerStatePoweredOn) {
        [self refreshBond];
        [self refreshDockPower];
        return;
    }
    self.pen.delegate = nil;
    [self clearLink];
    self.lastBondCount = NSNotFound;
    self.nextControlAttempt = 0;
}

#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI {
    (void)advertisementData;
    (void)RSSI;
    if (central != self.central || !self.scanningForDock ||
        ![peripheral.name isEqualToString:PenName] || self.pen || !self.powerStateValid) return;
    [central stopScan];
    self.scanningForDock = NO;
    self.pen = peripheral;
    self.pen.delegate = self;
    self.pairingAgent = [central respondsToSelector:@selector(sharedPairingAgent)]
        ? central.sharedPairingAgent : nil;
    if (![self.pairingAgent respondsToSelector:@selector(pairPeer:useMITM:)] ||
        ![self.pairingAgent respondsToSelector:@selector(respondToPairingRequest:type:accept:data:)]) {
        os_log_error(OS_LOG_DEFAULT, "UKE P81c system pairing agent unavailable for dock discovery");
        [self clearLink];
        return;
    }
    self.pairingAgent.delegate = (id)self;
    os_log(OS_LOG_DEFAULT, "UKE P81c dock discovery found unpaired pen, connecting");
    [self setDeadline:15 phase:@"dock connect"];
    [central connectPeripheral:peripheral options:nil];
}
#endif

- (void)refreshBond {
    if (self.central.state != CBManagerStatePoweredOn) return;
    CBPairingAgent *agent = [self.central respondsToSelector:@selector(sharedPairingAgent)]
        ? self.central.sharedPairingAgent : nil;
    if (![agent respondsToSelector:@selector(retrievePairedPeers)] ||
        ![agent respondsToSelector:@selector(isPeerPaired:)]) {
        if (!self.pairingAPIUnavailable)
            os_log_error(OS_LOG_DEFAULT, "UKE P81c radio system bond lookup unavailable");
        self.pairingAPIUnavailable = YES;
        return;
    }
    self.pairingAPIUnavailable = NO;
    if (self.pen) {
        if ([agent isPeerPaired:self.pen]) return;
        CBPeripheral *oldPen = self.pen;
        oldPen.delegate = nil;
        [self clearLink];
        [self.central cancelPeripheralConnection:oldPen];
        os_log(OS_LOG_DEFAULT, "UKE P81c radio system bond removed");
        return;
    }
    if (NSProcessInfo.processInfo.systemUptime < self.nextControlAttempt) return;

    NSMutableArray<NSUUID *> *identifiers = [NSMutableArray array];
    for (CBPeer *peer in [agent retrievePairedPeers]) {
        if ([peer isKindOfClass:CBPeer.class] && peer.identifier)
            [identifiers addObject:peer.identifier];
    }
    if (identifiers.count != self.lastBondCount) {
        self.lastBondCount = identifiers.count;
        os_log(OS_LOG_DEFAULT, "UKE P81c radio visible system bonds=%lu", (unsigned long)identifiers.count);
    }
    if (!identifiers.count) return;
    CBPeripheral *selected = nil;
    for (CBPeripheral *candidate in [self.central retrievePeripheralsWithIdentifiers:identifiers]) {
        if (![candidate.name isEqualToString:PenName] || ![agent isPeerPaired:candidate]) continue;
        if (selected && ![selected.identifier isEqual:candidate.identifier]) {
            if (!self.multiplePens)
                os_log_error(OS_LOG_DEFAULT, "UKE P81c radio multiple system bonds match the pen");
            self.multiplePens = YES;
            return;
        }
        selected = candidate;
    }
    self.multiplePens = NO;
    if (!selected) return;
    self.pen = selected;
    selected.delegate = self;
    os_log(OS_LOG_DEFAULT, "UKE P81c radio selected system-bonded pen");
    // Hold this client's connection request even if system HID opened the link first.
    const BOOL alreadyConnected = selected.state == CBPeripheralStateConnected;
    [self.central connectPeripheral:selected options:nil];
    if (alreadyConnected && selected.state == CBPeripheralStateConnected)
        [self centralManager:self.central didConnectPeripheral:selected];
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral {
    if (central != self.central || peripheral != self.pen || self.activeLink) return;
    self.activeLink = YES;
#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
    if (self.pairingAgent && ![self.pairingAgent isPeerPaired:peripheral]) {
        self.pairingPending = YES;
        [self setDeadline:30 phase:@"system pairing"];
        os_log(OS_LOG_DEFAULT, "UKE P81c dock system Just Works pairing requested");
        [self.pairingAgent pairPeer:peripheral useMITM:NO];
        return;
    }
#endif
    os_log(OS_LOG_DEFAULT, "UKE P81c radio connected, discovering FE10");
    [self setDeadline:6 phase:@"FE10 discovery"];
    [peripheral discoverServices:@[[CBUUID UUIDWithString:ServiceUUID], [CBUUID UUIDWithString:@"180A"]]];
}

- (void)centralManager:(CBCentralManager *)central didUpdateConnectionParameters:(CBPeripheral *)peripheral
               interval:(NSNumber *)interval latency:(NSNumber *)latency supervisionTimeout:(NSNumber *)timeout {
    if (central != self.central || peripheral != self.pen) return;
    os_log(OS_LOG_DEFAULT, "UKE P81c BLE connection parameters interval=%{public}@ latency=%{public}@ timeout=%{public}@",
           interval, latency, timeout);
#if !defined(UKE_PEN_ACTIVE_INTERVAL)
    self.slowPenConnection = interval.doubleValue > 30 || latency.intValue > 0;
    if (!self.slowPenConnection || self.lowLatencyRequests != 1) return;
    const NSUInteger generation = self.penLinkGeneration;
    __weak UkePenRadio *owner = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
        UkePenRadio *radio = owner;
        if (radio.penLinkGeneration == generation && radio.pen == peripheral &&
            radio.slowPenConnection && radio.lowLatencyRequests == 1)
            [radio requestPenLowLatency];
    });
#endif
}

#if !defined(UKE_PEN_ACTIVE_INTERVAL)
- (void)requestPenLowLatency {
    if (!self.penIdentityVerified || !self.activeLink || !self.pen ||
        self.lowLatencyRequests >= 2 ||
        ![self.central respondsToSelector:@selector(setDesiredConnectionLatency:forPeripheral:)]) return;
    ++self.lowLatencyRequests;
    [self.central setDesiredConnectionLatency:UkePenConnectionLatencyLowFixed forPeripheral:self.pen];
    os_log(OS_LOG_DEFAULT, "UKE P81c BLE fixed low latency requested attempt=%u", self.lowLatencyRequests);
}
#endif

#if !defined(UKE_PEN_STOCK_DOCK_BRIDGE)
- (void)pairingAgent:(CBPairingAgent *)agent peerDidRequestPairing:(CBPeer *)peer
                type:(NSInteger)type passkey:(NSNumber *)passkey {
    if (agent != self.pairingAgent || peer != self.pen || !self.pairingPending) return;
    const BOOL accept = type == 0 && (!passkey || passkey.unsignedLongLongValue == 0);
    os_log(OS_LOG_DEFAULT, "UKE P81c dock pairing request type=%ld accept=%d", (long)type, accept);
    [agent respondToPairingRequest:peer type:type accept:accept data:nil];
}

- (void)pairingAgent:(CBPairingAgent *)agent peerDidCompletePairing:(CBPeer *)peer {
    if (agent != self.pairingAgent || peer != self.pen || !self.pairingPending) return;
    self.pairingPending = NO;
    agent.delegate = nil;
    os_log(OS_LOG_DEFAULT, "UKE P81c dock pairing completed");
    [self setDeadline:6 phase:@"FE10 discovery"];
    [self.pen discoverServices:@[[CBUUID UUIDWithString:ServiceUUID], [CBUUID UUIDWithString:@"180A"]]];
}

- (void)pairingAgent:(CBPairingAgent *)agent peerDidFailToCompletePairing:(CBPeer *)peer
               error:(NSError *)error {
    if (agent != self.pairingAgent || peer != self.pen || !self.pairingPending) return;
    os_log_error(OS_LOG_DEFAULT, "UKE P81c dock pairing failed domain=%{public}@/%ld",
                 error.domain, (long)error.code);
    [self fail:@"system pairing" error:error];
}
#endif

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error {
    if (central == self.central && peripheral == self.pen) {
        [self fail:@"connect" error:error];
    }
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error {
    if (central != self.central || peripheral != self.pen) return;
    os_log(OS_LOG_DEFAULT, "UKE P81c radio BLE link ended code=%ld", (long)error.code);
    peripheral.delegate = nil;
    [self clearLink];
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error {
    if (peripheral != self.pen) return;
    CBUUID *expected = [CBUUID UUIDWithString:ServiceUUID];
    if (!error) {
        for (CBService *service in peripheral.services) {
            if ([service.UUID isEqual:[CBUUID UUIDWithString:@"180A"]])
                [peripheral discoverCharacteristics:@[[CBUUID UUIDWithString:@"2A50"]] forService:service];
        }
        for (CBService *service in peripheral.services) {
            if (![service.UUID isEqual:expected]) continue;
            [self setDeadline:6 phase:@"FE11/FE12 discovery"];
            [peripheral discoverCharacteristics:@[[CBUUID UUIDWithString:WriteUUID],
                                                 [CBUUID UUIDWithString:NotifyUUID]] forService:service];
            return;
        }
    }
    [self fail:@"FE10 discovery" error:error];
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service
             error:(NSError *)error {
    if (peripheral != self.pen) return;
    if ([service.UUID isEqual:[CBUUID UUIDWithString:@"180A"]]) {
        if (!error) for (CBCharacteristic *value in service.characteristics) {
            if ([value.UUID isEqual:[CBUUID UUIDWithString:@"2A50"]] &&
                (value.properties & CBCharacteristicPropertyRead)) {
                self.pnpId = value;
                [peripheral readValueForCharacteristic:value];
                break;
            }
        }
        return;
    }
    if (![service.UUID isEqual:[CBUUID UUIDWithString:ServiceUUID]]) return;
    for (CBCharacteristic *value in service.characteristics) {
        if ([value.UUID isEqual:[CBUUID UUIDWithString:WriteUUID]]) self.command = value;
        if ([value.UUID isEqual:[CBUUID UUIDWithString:NotifyUUID]]) self.notifications = value;
    }
    CBCharacteristicProperties flags = self.command.properties;
    if (error || !(flags & (CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse)) ||
        !(self.notifications.properties & (CBCharacteristicPropertyNotify | CBCharacteristicPropertyIndicate))) {
        [self fail:@"FE11/FE12 discovery" error:error];
        return;
    }
    const NSUInteger withAck = flags & CBCharacteristicPropertyWrite
        ? [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithResponse] : 0;
    const NSUInteger withoutAck = flags & CBCharacteristicPropertyWriteWithoutResponse
        ? [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse] : 0;
    self.writeType = withoutAck >= UKE_P81C_CONFIG_BYTES
        ? CBCharacteristicWriteWithoutResponse : CBCharacteristicWriteWithResponse;
    os_log(OS_LOG_DEFAULT, "UKE P81c radio FE11/FE12 found max-write=%lu/%lu",
           (unsigned long)withAck, (unsigned long)withoutAck);
    if (MAX(withAck, withoutAck) < UKE_P81C_CONFIG_BYTES) {
        [self fail:@"FE11 MTU" error:nil];
        return;
    }
    [self setDeadline:6 phase:@"FE12 subscribe"];
    [peripheral setNotifyValue:YES forCharacteristic:self.notifications];
    __weak UkePenRadio *owner = self;
    self.hapticTimer = [NSTimer scheduledTimerWithTimeInterval:0.02 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [owner serviceWritableLink];
    }];
}

- (void)serviceWritableLink {
    if (!self.command || self.pen.state != CBPeripheralStateConnected) return;
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (self.retryControlOnLink && now >= self.retryControlOnLink) {
        self.retryControlOnLink = 0;
        [self sendNext];
    }
    if (!(self.command.properties & CBCharacteristicPropertyWriteWithoutResponse) ||
        !self.pen.canSendWriteWithoutResponse) return;
    uint32_t pending = 0;
    size_t length = sizeof(pending);
    if (sysctlbyname("kern.uke_pen_haptic", &pending, &length, NULL, 0) ||
        length != sizeof(pending) || !pending) return;
    // P81c firmware 0.0.32, FE11 handler 0x41758. Type 3 plays one tick.
    static const uint8_t tick[] = {0x5e, 0x02, 0x03, 0x00};
    [self.pen writeValue:[NSData dataWithBytes:tick length:sizeof(tick)] forCharacteristic:self.command
                    type:CBCharacteristicWriteWithoutResponse];
    os_log(OS_LOG_DEFAULT, "UKE P81c system haptic sent over FE11");
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)value
             error:(NSError *)error {
    if (peripheral != self.pen || value != self.notifications) return;
    if (error || !value.isNotifying) {
        [self fail:@"FE12 subscribe" error:error];
        return;
    }
    [self sendNext];
}

- (void)sendNext {
    if (!self.command || !self.notifications.isNotifying || self.awaitingReply ||
        self.awaitingWrite || self.completed || self.waitingForDisconnect) return;
    uint8_t packet[UKE_P81C_CONFIG_BYTES];
    uint32_t now = (uint32_t)NSDate.date.timeIntervalSince1970;
    const size_t size = uke_pen_radio_request(&_protocol, self.address.bytes, now, packet);
    if (!size) {
        [self.timer invalidate];
        self.timer = nil;
        self.completed = YES;
        os_log(OS_LOG_DEFAULT, "UKE P81c radio ready slots=%u/%u", _protocol.slots[0], _protocol.slots[1]);
        return;
    }
    self.pendingOpcode = packet[0];
    [self setDeadline:4 phase:@"FE12 reply"];
    if (self.writeType == CBCharacteristicWriteWithoutResponse && !self.pen.canSendWriteWithoutResponse) return;
    if (self.pendingOpcode == 0x04) self.requestedSlot = YES;
    self.awaitingReply = YES;
    self.awaitingWrite = self.writeType == CBCharacteristicWriteWithResponse;
    os_log(OS_LOG_DEFAULT, "UKE P81c radio tx step=%u opcode=%02x bytes=%zu",
           _protocol.step, self.pendingOpcode, size);
    [self.pen writeValue:[NSData dataWithBytes:packet length:size] forCharacteristic:self.command type:self.writeType];
    if (self.notifications.properties & CBCharacteristicPropertyRead) {
        // bluetoothd may withhold unsolicited FE12 notifications from the
        // daemon. Try one solicited read and still require the exact ACK.
        __weak UkePenRadio *owner = self;
        self.replyReadTimer = [NSTimer scheduledTimerWithTimeInterval:0.15 repeats:NO block:^(NSTimer *timer) {
            (void)timer;
            UkePenRadio *radio = owner;
            if (radio.awaitingReply) {
                os_log(OS_LOG_DEFAULT, "UKE P81c radio FE12 read fallback opcode=%02x", radio.pendingOpcode);
                [radio.pen readValueForCharacteristic:radio.notifications];
            }
        }];
    }
}

- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral *)peripheral {
    if (peripheral == self.pen) [self sendNext];
}

- (void)peripheral:(CBPeripheral *)peripheral didWriteValueForCharacteristic:(CBCharacteristic *)value
             error:(NSError *)error {
    if (peripheral != self.pen || value != self.command || !self.awaitingWrite) return;
    if (error) {
        [self fail:@"FE11 write" error:error];
        return;
    }
    self.awaitingWrite = NO;
    if (!self.awaitingReply) [self sendNext];
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)value
             error:(NSError *)error {
    if (peripheral != self.pen) return;
    if (value == self.pnpId) {
        const uint8_t *pnp = value.value.bytes;
        self.penIdentityVerified = !error && value.value.length == 7 &&
            pnp[0] == 1 && pnp[1] == 0x22 && pnp[2] == 0 && pnp[3] == 0x81 && pnp[4] == 0x50;
        os_log(OS_LOG_DEFAULT, "UKE P81c PnP verification matched=%d length=%lu",
               self.penIdentityVerified, (unsigned long)value.value.length);
#if defined(UKE_PEN_ACTIVE_INTERVAL)
        [self publishIntervalIdentity];
#else
        if (!self.lowLatencyRequests) [self requestPenLowLatency];
#endif
        [self publishProIdentity];
        return;
    }
    if (value != self.notifications) return;
    static const uint8_t tickAck[] = {0xde, 0x00};
    if (!error && value.value.length == sizeof(tickAck) &&
        memcmp(value.value.bytes, tickAck, sizeof(tickAck)) == 0) {
        os_log(OS_LOG_DEFAULT, "UKE P81c system haptic acknowledged");
        return;
    }
    if (!self.awaitingReply) return;
    if (error) {
        os_log_error(OS_LOG_DEFAULT, "UKE P81c FE12 value error=%{public}@/%ld",
                     error.domain, (long)error.code);
        return;
    }
    NSData *data = value.value;
    const enum UkePenRadioReply reply = uke_pen_radio_accept(&_protocol, data.bytes, data.length);
    if (reply == UkePenRadioUnrelated) return;
    if (reply == UkePenRadioNoSlot) {
        [self fail:@"pen slot" error:nil];
        return;
    }
    if (reply != UkePenRadioAccepted) {
        os_log_error(OS_LOG_DEFAULT, "UKE P81c ignoring malformed FE12 reply opcode=%02x", self.pendingOpcode);
        return;
    }
    [self.replyReadTimer invalidate];
    self.replyReadTimer = nil;
    self.awaitingReply = NO;
    os_log(OS_LOG_DEFAULT, "UKE P81c radio ack step=%u opcode=%02x bytes=%lu",
           _protocol.step - 1, self.pendingOpcode, (unsigned long)data.length);
    [self sendNext];
}

@end
