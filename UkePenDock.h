// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

// Called on the radio service's main queue. An address and peer identifier
// are supplied only for an address-matched, verified system bond.
@interface UkePenDock : NSObject
@property(nonatomic, readonly, getter=isActive) BOOL active;
- (void)updateAddress:(NSData *)address peer:(NSUUID *)peer;
@end
