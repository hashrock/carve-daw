#include "SoundLibrary.h"

#import <Foundation/Foundation.h>

namespace carve::app
{

void excludeFromBackup (const juce::File& file)
{
    @autoreleasepool
    {
        NSString* path = [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()];

        if (path == nil)
            return;

        NSURL* url = [NSURL fileURLWithPath: (NSString* _Nonnull) path];
        [url setResourceValue: @YES forKey: NSURLIsExcludedFromBackupKey error: nil];
    }
}

} // namespace carve::app
