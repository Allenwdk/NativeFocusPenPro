// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "UkePenDock.h"
#import <os/log.h>

// CoreAccessories 24A446 transport-server NSXPC interfaces. The endpoint
// tuple is the one created by the stock inductive BLE pairing transport.
enum {
    InductiveConnection = 4,
    InductivePairingTransport = 12,
    BLEPairingProtocol = 8,
};

// 24A446 accessoryd exchanges two little-endian u16 fields followed by
// the payload. Address updates and authentication data use distinct IDs.
enum {
    DockStartUpdates = 0,
    DockStopUpdates = 1,
    DockHostAddress = 2,
    DockAccessoryAddress = 4,
    DockAddressType = 1,
    DockUpdateType = 2,
    DockAddressBytes = 6,
    DockHeaderBytes = 4,
};

// Foundation exports this initializer on 24A446, where it is used by system
// daemons. The public iOS SDK marks it unavailable to third-party apps.
@protocol UkeStockXPCInitializer
- (instancetype)initWithMachServiceName:(NSString *)name options:(NSXPCConnectionOptions)options;
@end

@protocol ACCTransportXPCServerProtocol
- (void)createConnectionWithType:(int)type andIdentifier:(NSString *)identifier
                       withReply:(void (^)(BOOL, NSString *))reply;
- (void)setProperties:(NSDictionary *)properties forConnectionWithUUID:(NSString *)uuid
            withReply:(void (^)(BOOL))reply;
- (void)createEndpointWithTransportType:(int)type andProtocol:(int)protocol
                         andIdentifier:(NSString *)identifier forConnectionWithUUID:(NSString *)uuid
                             withReply:(void (^)(BOOL, NSString *))reply;
- (void)setAccessoryInfo:(NSDictionary *)info forEndpointWithUUID:(NSString *)uuid
               withReply:(void (^)(BOOL))reply;
- (void)publishConnectionWithUUID:(NSString *)uuid withReply:(void (^)(BOOL))reply;
- (void)processIncomingData:(NSData *)data forEndpointWithUUID:(NSString *)uuid
                  withReply:(void (^)(BOOL))reply;
- (void)destroyConnectionWithUUID:(NSString *)uuid withReply:(void (^)(BOOL))reply;
@end

@protocol ACCTransportXPCClientProtocol
- (void)sendOutgoingData:(NSData *)data forEndpointWithUUID:(NSString *)endpoint
          connectionUUID:(NSString *)connection withReply:(void (^)(BOOL))reply;
- (void)receivedSecureTunnelData:(NSData *)data forEndpoint:(NSString *)endpoint;
- (void)authStateDidChange:(int)state forConnectionWithUUID:(NSString *)connection
         previousAuthState:(int)previous authType:(int)type
 connectionIsAuthenticated:(BOOL)authenticated connectionWasAuthenticated:(BOOL)wasAuthenticated;
- (void)propertiesDidChange:(NSDictionary *)properties forConnectionWithUUID:(NSString *)connection
         previousProperties:(NSDictionary *)previous;
- (void)propertiesDidChange:(NSDictionary *)properties forEndpointWithUUID:(NSString *)endpoint
         previousProperties:(NSDictionary *)previous connectionUUID:(NSString *)connection;
@end

@interface UkePenDock () <ACCTransportXPCClientProtocol>
@property(nonatomic, strong) NSXPCConnection *client;
@property(nonatomic, strong) id<ACCTransportXPCServerProtocol> server;
@property(nonatomic, copy) NSData *address;
@property(nonatomic, copy) NSUUID *peer;
@property(nonatomic, copy) NSString *connectionUUID;
@property(nonatomic, copy) NSString *endpointUUID;
@property(nonatomic) BOOL published;
@property(nonatomic) BOOL addressUpdates;
@property(nonatomic) NSTimeInterval retryAt;
@property(nonatomic) unsigned generation;
@end

@implementation UkePenDock

- (BOOL)isActive {
    return self.client != nil;
}

- (void)close {
    NSXPCConnection *client = self.client;
    NSString *connection = self.connectionUUID;
    self.client = nil;
    self.server = nil;
    self.connectionUUID = nil;
    self.endpointUUID = nil;
    self.published = NO;
    self.addressUpdates = NO;
    ++self.generation;
    if (!client) return;
    // The server also frees all connections owned by an invalidated client.
    if (!connection) {
        [client invalidate];
        return;
    }
    id<ACCTransportXPCServerProtocol> server = [client remoteObjectProxyWithErrorHandler:^(NSError *error) {
        (void)error;
        [client invalidate];
    }];
    [server destroyConnectionWithUUID:connection withReply:^(BOOL ok) {
        os_log(OS_LOG_DEFAULT, "UKE PEN dock transport detached ok=%d", ok);
        [client invalidate];
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        [client invalidate];
    });
}

- (void)failed:(const char *)stage generation:(unsigned)generation {
    if (self.generation != generation) return;
    os_log_error(OS_LOG_DEFAULT, "UKE PEN dock transport failed stage=%{public}s", stage);
    self.retryAt = NSProcessInfo.processInfo.systemUptime + 5;
    [self close];
}

- (void)updateAddress:(NSData *)address peer:(NSUUID *)peer {
    if (address.length != DockAddressBytes || !peer) {
        if (self.client) [self close];
        self.address = nil;
        self.peer = nil;
        self.retryAt = 0;
        return;
    }
    if (![address isEqualToData:self.address] || ![peer isEqual:self.peer]) {
        [self close];
        self.address = address;
        self.peer = peer;
        self.retryAt = 0;
    }
    if (self.client || NSProcessInfo.processInfo.systemUptime < self.retryAt) return;

    id<NSObject, UkeStockXPCInitializer> uninitialized = (id)[NSXPCConnection alloc];
    if (![uninitialized respondsToSelector:@selector(initWithMachServiceName:options:)]) return;
    NSXPCConnection *client = (id)[uninitialized initWithMachServiceName:@"com.apple.accessories.transport-server"
                                                               options:NSXPCConnectionPrivileged];
    if (!client) return;
    const unsigned generation = ++self.generation;
    self.client = client;
    client.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(ACCTransportXPCServerProtocol)];
    client.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(ACCTransportXPCClientProtocol)];
    client.exportedObject = self;
    __weak UkePenDock *owner = self;
    client.interruptionHandler = ^{
        dispatch_async(dispatch_get_main_queue(), ^{ [owner failed:"interrupted" generation:generation]; });
    };
    client.invalidationHandler = ^{
        dispatch_async(dispatch_get_main_queue(), ^{ [owner failed:"invalidated" generation:generation]; });
    };
    [client resume];
    self.server = [client remoteObjectProxyWithErrorHandler:^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (owner.generation != generation) return;
            os_log_error(OS_LOG_DEFAULT, "UKE PEN dock transport XPC error %{public}@/%ld", error.domain, (long)error.code);
            [owner failed:"request" generation:generation];
        });
    }];
    const uint8_t *bytes = address.bytes;
    NSString *uid = [NSString stringWithFormat:@"%02x:%02x:%02x:%02x:%02x:%02x",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]];
    const uint8_t supported = 1;
    NSDictionary *info = @{@"Name": @"Apple Pencil Pro", @"Model": @"A2538",
                           @"Manufacturer": @"Xiaomi",
                           @"HardwareVersion": @"P81c",
                           @"VendorID": @0x0022, @"ProductID": @0x5081,
                           @"AccessoryDeviceUID": uid,
                           @"SupportedTypes": [NSData dataWithBytes:&supported length:sizeof(supported)]};
    NSDictionary *properties = @{@"OOBPairingEarlyInfoBDADDR": address,
                                  @"OOBPairingEarlyInfoSessionState": @1};
    [self.server createConnectionWithType:InductiveConnection andIdentifier:nil withReply:^(BOOL ok, NSString *connection) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (owner.generation != generation) return;
            if (!ok || !connection.length) {
                [owner failed:"create connection" generation:generation];
                return;
            }
            owner.connectionUUID = connection;
            [owner.server setProperties:properties forConnectionWithUUID:connection withReply:^(BOOL set) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (owner.generation != generation) return;
                    if (!set) {
                        [owner failed:"set early info" generation:generation];
                        return;
                    }
                    [owner publishEndpointForConnection:connection info:info generation:generation];
                });
            }];
        });
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (owner.generation == generation && !owner.published) [owner failed:"publish timeout" generation:generation];
    });
}

- (void)publishEndpointForConnection:(NSString *)connection info:(NSDictionary *)info generation:(unsigned)generation {
    __weak UkePenDock *owner = self;
    [self.server createEndpointWithTransportType:InductivePairingTransport andProtocol:BLEPairingProtocol
                              andIdentifier:nil forConnectionWithUUID:connection withReply:^(BOOL ok, NSString *endpoint) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (owner.generation != generation) return;
            if (!ok || !endpoint.length) {
                [owner failed:"create endpoint" generation:generation];
                return;
            }
            owner.endpointUUID = endpoint;
            [owner.server setAccessoryInfo:info forEndpointWithUUID:endpoint withReply:^(BOOL set) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (owner.generation != generation) return;
                    if (!set) {
                        [owner failed:"set accessory info" generation:generation];
                        return;
                    }
                    [owner.server publishConnectionWithUUID:connection withReply:^(BOOL published) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if (owner.generation != generation) return;
                            if (!published) {
                                [owner failed:"publish" generation:generation];
                                return;
                            }
                            owner.published = YES;
                            os_log(OS_LOG_DEFAULT, "UKE PEN dock transport published inductive endpoint");
                        });
                    }];
                });
            }];
        });
    }];
}

- (void)sendOutgoingData:(NSData *)data forEndpointWithUUID:(NSString *)endpoint
          connectionUUID:(NSString *)connection withReply:(void (^)(BOOL))reply {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (![self.connectionUUID isEqualToString:connection] ||
            ![self.endpointUUID isEqualToString:endpoint] || self.address.length != DockAddressBytes ||
            !self.peer || data.length < DockHeaderBytes) {
            if (reply) reply(NO);
            return;
        }
        const uint8_t *bytes = data.bytes;
        const unsigned message = bytes[0] | ((unsigned)bytes[1] << 8);
        const unsigned type = bytes[2] | ((unsigned)bytes[3] << 8);
        if (data.length == DockHeaderBytes && type == DockUpdateType &&
            (message == DockStartUpdates || message == DockStopUpdates)) {
            self.addressUpdates = message == DockStartUpdates;
            if (reply) reply(YES);
            return;
        }
        if (message != DockHostAddress || type != DockAddressType || !self.addressUpdates ||
            data.length != DockHeaderBytes + DockAddressBytes) {
            os_log(OS_LOG_DEFAULT, "UKE PEN dock outbound unhandled id=%u type=%u length=%lu",
                   message, type, (unsigned long)data.length);
            if (reply) reply(NO);
            return;
        }

        // updateAddress: is reached only for a PMIC address resolved to an
        // existing system bond. Let PencilPairingAngel resolve that same
        // peripheral through its normal pairing-info callback.
        uint8_t response[DockHeaderBytes + DockAddressBytes] = {
            DockAccessoryAddress, 0, DockAddressType, 0,
        };
        [self.address getBytes:response + DockHeaderBytes length:DockAddressBytes];
        if (reply) reply(YES);
        [self.server processIncomingData:[NSData dataWithBytes:response length:sizeof(response)]
                    forEndpointWithUUID:endpoint withReply:^(BOOL accepted) {
            os_log(OS_LOG_DEFAULT, "UKE PEN dock bonded address delivered ok=%d", accepted);
        }];
    });
}

- (void)receivedSecureTunnelData:(NSData *)data forEndpoint:(NSString *)endpoint {
    (void)data; (void)endpoint;
}

- (void)authStateDidChange:(int)state forConnectionWithUUID:(NSString *)connection
         previousAuthState:(int)previous authType:(int)type
 connectionIsAuthenticated:(BOOL)authenticated connectionWasAuthenticated:(BOOL)wasAuthenticated {
    (void)previous; (void)wasAuthenticated;
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([self.connectionUUID isEqualToString:connection])
            os_log(OS_LOG_DEFAULT, "UKE PEN dock auth state=%d type=%d authenticated=%d", state, type, authenticated);
    });
}

- (void)propertiesDidChange:(NSDictionary *)properties forConnectionWithUUID:(NSString *)connection
         previousProperties:(NSDictionary *)previous {
    (void)properties; (void)connection; (void)previous;
}

- (void)propertiesDidChange:(NSDictionary *)properties forEndpointWithUUID:(NSString *)endpoint
         previousProperties:(NSDictionary *)previous connectionUUID:(NSString *)connection {
    (void)properties; (void)endpoint; (void)previous; (void)connection;
}

@end
