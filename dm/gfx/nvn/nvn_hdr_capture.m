#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <stdio.h>
#include <string.h>
#include "nvn_hdr_capture.h"

static int capture_error(char *buffer, size_t capacity, NSString *message) {
    if (buffer && capacity) snprintf(buffer, capacity, "%s", message.UTF8String ?: "HDR capture failed");
    return 0;
}

int dm_hdr_capture_write(const void *bytes, size_t width, size_t height,
                         size_t row_bytes, unsigned metal_format,
                         int flip_vertical, const char *png_path,
                         char *error, size_t error_capacity) {
    @autoreleasepool {
        if (error && error_capacity) error[0] = 0;
        if (!bytes || !png_path || !*png_path || !width || !height ||
            width > SIZE_MAX / 6 || row_bytes < width * 4 || height > SIZE_MAX / row_bytes ||
            height > SIZE_MAX / (width * 6))
            return capture_error(error, error_capacity, @"Invalid capture dimensions, stride or buffer");
        if (metal_format != 90 && metal_format != 94)
            return capture_error(error, error_capacity, @"Only verified RGB10A2/BGR10A2 PQ captures are supported");
        NSString *path = [NSString stringWithUTF8String:png_path];
        if (!path) return capture_error(error, error_capacity, @"Capture path is not UTF-8");
        NSString *raw_path = [path stringByAppendingString:@".raw"];
        NSString *json_path = [path stringByAppendingString:@".json"];
        NSFileManager *files = NSFileManager.defaultManager;
        for (NSString *destination in @[path, raw_path, json_path])
            if ([files fileExistsAtPath:destination])
                return capture_error(error, error_capacity, @"Capture output already exists; refusing overwrite");

        size_t png_row = width * 6;
        NSMutableData *rgb = [NSMutableData dataWithLength:png_row * height];
        if (!rgb) return capture_error(error, error_capacity, @"Cannot allocate 16-bit capture buffer");
        uint8_t *output = rgb.mutableBytes;
        const uint8_t *input = bytes;
        for (size_t y = 0; y < height; y++) {
            const uint8_t *source = input + (flip_vertical ? height - 1 - y : y) * row_bytes;
            for (size_t x = 0; x < width; x++) {
                /* Metal readback is little-endian packed UNORM. Explicit byte
                 * assembly makes the file contract independent of host endian. */
                const uint8_t *p = source + x * 4;
                uint32_t packed = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
                unsigned channels[3] = {packed & 1023, (packed >> 10) & 1023, (packed >> 20) & 1023};
                if (metal_format == 94) { unsigned swap = channels[0]; channels[0] = channels[2]; channels[2] = swap; }
                for (unsigned c = 0; c < 3; c++) {
                    /* Reversible normalized 10 -> 16 bit representation, not a
                     * gamma conversion. Round(code16 * 1023 / 65535) is exact. */
                    uint16_t value = (uint16_t)((channels[c] * 65535u + 511u) / 1023u);
                    size_t at = y * png_row + x * 6 + c * 2;
                    output[at] = value >> 8; output[at + 1] = value & 255;
                }
            }
        }
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)rgb);
        CGImageRef image = space && provider ? CGImageCreate(width, height, 16, 48, png_row, space,
            (CGBitmapInfo)(kCGImageAlphaNone | kCGBitmapByteOrder16Big), provider, NULL, false, kCGRenderingIntentDefault) : NULL;
        NSMutableData *png = [NSMutableData data];
        CGImageDestinationRef destination = image ? CGImageDestinationCreateWithData((__bridge CFMutableDataRef)png, CFSTR("public.png"), 1, NULL) : NULL;
        bool encoded = false;
        if (destination) {
            CGImageDestinationAddImage(destination, image, NULL);
            encoded = CGImageDestinationFinalize(destination);
        }
        if (destination) CFRelease(destination);
        if (image) CGImageRelease(image);
        if (provider) CGDataProviderRelease(provider);
        if (space) CGColorSpaceRelease(space);
        if (!encoded) return capture_error(error, error_capacity, @"ImageIO could not encode the tagged 16-bit PQ PNG");

        NSDictionary *metadata = @{
            @"schema": @1, @"complete": @YES, @"width": @(width), @"height": @(height),
            @"metal_pixel_format": @(metal_format), @"source_row_bytes": @(row_bytes),
            @"source_bytes": @(row_bytes * height), @"source_byte_order": @"little-endian",
            @"source_channel_layout": metal_format == 90 ? @"R10 G10 B10 A2, low to high bits" : @"B10 G10 R10 A2, low to high bits",
            @"primaries": @"ITU-R BT.2020", @"transfer": @"SMPTE ST 2084 PQ, absolute 0-10000 cd/m2",
            @"color_contract": @"Caller-verified native HDR10 terminal image; pixel format alone is not proof",
            @"png_color_space": @"ITU-R BT.2100 PQ", @"png_bits_per_component": @16,
            @"png_alpha": @"omitted; exact source alpha retained in packed raw",
            @"png_code_conversion": @"round(code10 * 65535 / 1023); reversible to the original 10-bit code",
            @"png_vertical_flip_from_raw": @(!!flip_vertical), @"raw_orientation": @"unchanged source buffer",
            @"tone_mapping": @"none", @"exposure_adjustment": @"none",
            @"raw_file": raw_path.lastPathComponent,
            @"viewing_note": @"PQ HDR image; appearance depends on HDR-aware color management. Not an SDR preview or panel-luminance measurement."
        };
        NSError *failure = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:&failure];
        NSData *raw = [NSData dataWithBytesNoCopy:(void *)bytes length:row_bytes * height freeWhenDone:NO];
        NSMutableArray<NSString *> *written = [NSMutableArray array];
        BOOL ok = json != nil;
        if (ok && [raw writeToFile:raw_path options:NSDataWritingAtomic error:&failure]) [written addObject:raw_path]; else ok = NO;
        if (ok && [png writeToFile:path options:NSDataWritingAtomic error:&failure]) [written addObject:path]; else ok = NO;
        /* Metadata is last: its presence is the completed capture marker. */
        if (ok && [json writeToFile:json_path options:NSDataWritingAtomic error:&failure]) [written addObject:json_path]; else ok = NO;
        if (!ok) {
            for (NSString *created in written) [files removeItemAtPath:created error:nil];
            return capture_error(error, error_capacity, failure.localizedDescription ?: @"Cannot write complete HDR capture");
        }
        return 1;
    }
}
