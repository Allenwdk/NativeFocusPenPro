// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "UkePenRadio.h"

int main(void) {
    @autoreleasepool {
        __attribute__((objc_precise_lifetime)) UkePenRadio *radio = [UkePenRadio new];
        if (!radio) return 70;
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
