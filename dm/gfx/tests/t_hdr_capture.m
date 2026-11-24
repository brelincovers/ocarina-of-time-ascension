// CPU/ImageIO only: no Metal device, queue, host or game launch.
// clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework CoreGraphics \
//   -framework ImageIO dm/gfx/nvn/nvn_hdr_capture.m dm/gfx/tests/t_hdr_capture.m -o /tmp/t_hdr_capture
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../nvn/nvn_hdr_capture.h"

static unsigned checks;
#define CHECK(expression) do { checks++; if (!(expression)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); exit(1); } } while (0)

static uint32_t pack(unsigned r, unsigned g, unsigned b, unsigned format) {
    return format == 90 ? r | g << 10 | b << 20 | 3u << 30 : b | g << 10 | r << 20 | 3u << 30;
}

static unsigned pq_code(double nits) {
    double p = pow(nits / 10000., 2610. / 16384.);
    return (unsigned)llround(1023 * pow((3424. / 4096. + (2413. / 128.) * p) / (1 + (2392. / 128.) * p), 2523. / 32.));
}

static void check_image(NSString *path, const uint32_t *source, size_t width, size_t height,
                        size_t source_row, unsigned format, int flip) {
    CGImageSourceRef loader = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    CHECK(loader);
    CGImageRef image = CGImageSourceCreateImageAtIndex(loader, 0, NULL);
    CHECK(image);
    CHECK(CGImageGetWidth(image) == width && CGImageGetHeight(image) == height);
    CHECK(CGImageGetBitsPerComponent(image) == 16);
    CHECK(CGColorSpaceIsHDR(CGImageGetColorSpace(image)));
    CHECK(CGColorSpaceIsPQBased(CGImageGetColorSpace(image)));
    CGColorSpaceRef expected = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
    CHECK(CFEqual(CGImageGetColorSpace(image), expected));
    CGColorSpaceRelease(expected);
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
    CHECK(data);
    const uint8_t *pixels = CFDataGetBytePtr(data);
    size_t pixel_size = CGImageGetBitsPerPixel(image) / 8, row = CGImageGetBytesPerRow(image);
    CHECK(pixel_size == 6 || pixel_size == 8);
    unsigned order = CGImageGetBitmapInfo(image) & kCGBitmapByteOrderMask;
    CHECK(order == kCGBitmapByteOrder16Big || order == kCGBitmapByteOrder16Little || order == kCGBitmapByteOrderDefault);
    for (size_t y = 0; y < height; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)source + (flip ? height - 1 - y : y) * source_row);
        for (size_t x = 0; x < width; x++) {
            unsigned expected_rgb[3] = {src[x] & 1023, (src[x] >> 10) & 1023, (src[x] >> 20) & 1023};
            if (format == 94) { unsigned swap = expected_rgb[0]; expected_rgb[0] = expected_rgb[2]; expected_rgb[2] = swap; }
            for (unsigned c = 0; c < 3; c++) {
                const uint8_t *p = pixels + y * row + x * pixel_size + c * 2;
                unsigned code = order == kCGBitmapByteOrder16Little ? p[0] | p[1] << 8 : p[0] << 8 | p[1];
                CHECK((code * 1023u + 32767u) / 65535u == expected_rgb[c]);
            }
        }
    }
    CFRelease(data); CGImageRelease(image); CFRelease(loader);
}

int main(void) {
    @autoreleasepool {
        NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:[@"ascension-hdr-capture-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        NSError *failure = nil;
        CHECK([NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:NO attributes:nil error:&failure]);
        char error[512];
        unsigned white = pq_code(203), highlight = pq_code(1000);
        CHECK(white > 0 && white < highlight && highlight < 1023);
        for (unsigned format = 90; format <= 94; format += 4) {
            for (int flip = 0; flip <= 1; flip++) {
                uint32_t src[2][4] = {{pack(0, white, highlight, format), pack(1023, 1, 2, format), pack(594, 769, 1000, format), 0x12345678},
                                      {pack(white, highlight, 0, format), pack(3, 4, 5, format), pack(1010, 767, 593, format), 0xAABBCCDD}};
                NSString *path = [root stringByAppendingPathComponent:[NSString stringWithFormat:@"capture-%u-%d.png", format, flip]];
                CHECK(dm_hdr_capture_write(src, 3, 2, sizeof src[0], format, flip, path.fileSystemRepresentation, error, sizeof error));
                CHECK(!error[0]);
                NSData *raw = [NSData dataWithContentsOfFile:[path stringByAppendingString:@".raw"]];
                CHECK(raw.length == sizeof src && !memcmp(raw.bytes, src, sizeof src));
                NSDictionary *metadata = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[path stringByAppendingString:@".json"]] options:0 error:&failure];
                CHECK([metadata[@"complete"] boolValue] && [metadata[@"png_vertical_flip_from_raw"] boolValue] == !!flip);
                CHECK([metadata[@"tone_mapping"] isEqualToString:@"none"]);
                check_image(path, &src[0][0], 3, 2, sizeof src[0], format, flip);
                CHECK(!dm_hdr_capture_write(src, 3, 2, sizeof src[0], format, flip, path.fileSystemRepresentation, error, sizeof error));
                CHECK(strstr(error, "exists"));
            }
        }
        // Exhaustively prove every original 10-bit component survives ImageIO.
        uint32_t codes[1024];
        for (unsigned i = 0; i < 1024; i++) codes[i] = pack(i, 1023 - i, (i * 37) % 1024, 90);
        NSString *all = [root stringByAppendingPathComponent:@"all-codes.png"];
        CHECK(dm_hdr_capture_write(codes, 1024, 1, sizeof codes, 90, 0, all.fileSystemRepresentation, error, sizeof error));
        check_image(all, codes, 1024, 1, sizeof codes, 90, 0);

        NSString *bad = [root stringByAppendingPathComponent:@"missing/file.png"];
        CHECK(!dm_hdr_capture_write(codes, 1, 1, 4, 90, 0, bad.fileSystemRepresentation, error, sizeof error));
        CHECK(error[0]);
        CHECK(![NSFileManager.defaultManager fileExistsAtPath:[bad stringByAppendingString:@".json"]]);
        CHECK(!dm_hdr_capture_write(codes, 1, 1, 4, 70, 0, bad.fileSystemRepresentation, error, sizeof error));
        CHECK(!dm_hdr_capture_write(codes, 2, 1, 4, 90, 0, bad.fileSystemRepresentation, error, sizeof error));
        CHECK(!dm_hdr_capture_write(codes, SIZE_MAX, 1, 4, 90, 0, bad.fileSystemRepresentation, error, sizeof error));
        CHECK([NSFileManager.defaultManager removeItemAtPath:root error:&failure]);
        printf("HDR CPU capture: %u checks passed; PQ 203 nits=%u/1023, 1000 nits=%u/1023; no GPU used.\n", checks, white, highlight);
    }
    return 0;
}
