// Cold diagnostic snapshots only. Retain objects while inspecting them outside
// their owner locks; pointer identity avoids merging distinct equal resources.
// Release these collections inside the caller's bounded autorelease pool.
#pragma once
#import <Foundation/Foundation.h>

static inline NSHashTable *nvn_census_objects(void) {
    // ObjectPointerPersonality is essential: Foundation does not use object
    // retain/release for OpaquePersonality even with StrongMemory. Keep pointer
    // identity while selecting the actual Objective-C ownership callbacks.
    return [NSHashTable hashTableWithOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality];
}

typedef enum {
    NVN_CENSUS_NAMED, NVN_CENSUS_PINNED, NVN_CENSUS_RETIRED,
    NVN_CENSUS_WAITING, NVN_CENSUS_UNNAMED
} nvn_census_owner;

static inline nvn_census_owner nvn_census_classify(id object, NSHashTable *named,
        NSHashTable *pins, NSHashTable *retired, NSHashTable *waiting) {
    if ([named containsObject:object]) return NVN_CENSUS_NAMED;
    if ([pins containsObject:object]) return NVN_CENSUS_PINNED;
    if ([retired containsObject:object]) return NVN_CENSUS_RETIRED;
    if ([waiting containsObject:object]) return NVN_CENSUS_WAITING;
    return NVN_CENSUS_UNNAMED;
}
