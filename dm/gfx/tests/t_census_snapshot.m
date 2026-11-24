// CPU-only diagnostic lifetime/identity checks: no Metal device or game needed.
#include "../nvn/nvn_census.h"
#include <stdio.h>

static unsigned checks, failures, destroyed;
#define CHECK(x, text) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL: %s\n", text); } } while (0)

@interface CensusProbe : NSObject
@property unsigned token;
@end
@implementation CensusProbe
- (void)dealloc { destroyed++; }
- (BOOL)isEqual:(id)other { return [other isKindOfClass:CensusProbe.class]; }
- (NSUInteger)hash { return 0; }
@end

int main(void) { @autoreleasepool {
    NSHashTable *snapshot = nvn_census_objects();
    __weak CensusProbe *weak;
    @autoreleasepool {
        CensusProbe *p = [CensusProbe new]; p.token = 731; weak = p;
        [snapshot addObject:p];
    }
    CHECK(weak != nil && destroyed == 0, "snapshot survives release of the original descriptor owner");
    // A regression must fail this test without messaging the dangling pointer
    // that the broken collection still contains.
    if (!weak) {
        printf("census snapshot: %u checks, %u failures (ownership guard)\n", checks, failures);
        return 1;
    }
    for (CensusProbe *p in snapshot) CHECK(p.token == 731, "inspection after owner unlock reads a live object");
    @autoreleasepool {
        CensusProbe *other = [CensusProbe new]; other.token = 732;
        [snapshot addObject:other];
        CHECK(snapshot.count == 2, "pointer identity keeps distinct equal resources separate");
        [snapshot addObject:weak];
        CHECK(snapshot.count == 2, "multiple names of one resource do not duplicate its allocation");
    }
    [snapshot removeAllObjects];
    CHECK(weak == nil && destroyed == 2, "diagnostic snapshot releases its resources when cleared");

    NSHashTable *named = nvn_census_objects(), *pins = nvn_census_objects();
    NSHashTable *retired = nvn_census_objects(), *waiting = nvn_census_objects();
    CensusProbe *p = [CensusProbe new];
    CHECK(nvn_census_classify(p, named, pins, retired, waiting) == NVN_CENSUS_UNNAMED, "truly unnamed allocation");
    [waiting addObject:p];
    CHECK(nvn_census_classify(p, named, pins, retired, waiting) == NVN_CENSUS_WAITING, "deferred owner identified");
    [retired addObject:p];
    CHECK(nvn_census_classify(p, named, pins, retired, waiting) == NVN_CENSUS_RETIRED, "retired descriptor takes priority over deferred copy");
    [pins addObject:p];
    CHECK(nvn_census_classify(p, named, pins, retired, waiting) == NVN_CENSUS_PINNED, "valid pin-only owner is not classified as unnamed or retired");
    [named addObject:p];
    CHECK(nvn_census_classify(p, named, pins, retired, waiting) == NVN_CENSUS_NAMED, "ordinary named allocation counted once even when also pinned");
    printf("census snapshot: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
} }
