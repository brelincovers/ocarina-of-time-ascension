#ifdef __APPLE__
#include "apple_spatial_mixer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Fixed bound makes rendering predictable without heap allocations in callback.
#define DM_SPATIAL_BLOCK 128
struct dm_apple_spatial {
    AudioUnit unit;
    dm_spatial_pull_s16 pull;
    void *context;
    int head_tracking_property_set;
    int built_in_speakers;
    AudioTimeStamp stamp;
};

static AudioStreamBasicDescription f32_format(double sample_rate, unsigned ch) {
    AudioStreamBasicDescription f={0};
    f.mSampleRate=sample_rate;
    f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket=sizeof(float);f.mBytesPerFrame=sizeof(float);
    f.mFramesPerPacket=1;f.mChannelsPerFrame=ch;f.mBitsPerChannel=32;
    return f;
}

static OSStatus pull51(void *ref, AudioUnitRenderActionFlags *flags,
                       const AudioTimeStamp *stamp, UInt32 bus, UInt32 frames,
                       AudioBufferList *bl) {
    (void)flags;(void)stamp;(void)bus;
    dm_apple_spatial *m=ref;
    if (!m || !bl || bl->mNumberBuffers != 6 || frames > DM_SPATIAL_BLOCK)
        return kAudio_ParamError;
    for (unsigned channel = 0; channel < 6; channel++) {
        const AudioBuffer *buffer = &bl->mBuffers[channel];
        if (!buffer->mData || buffer->mNumberChannels != 1 ||
            buffer->mDataByteSize < frames * sizeof(float))
            return kAudio_ParamError;
    }
    int16_t source[DM_SPATIAL_BLOCK * 6];
    m->pull(m->context, source, frames);
    for (unsigned channel = 0; channel < 6; channel++) {
        float *dst = bl->mBuffers[channel].mData;
        for (UInt32 frame = 0; frame < frames; frame++) {
            const int16_t *bed = source + frame * 6;
            float sample = bed[channel] / 32768.0f;
            // The built-in-speaker mixer drops a discrete LFE input. Route it
            // equally to both fronts at the existing stereo downmix gain.
            if (m->built_in_speakers) {
                if (channel < 2) sample += (8224.0f / 32768.0f) * (bed[3] / 32768.0f);
                else if (channel == 3) sample = 0;
            }
            dst[frame] = sample;
        }
    }
    return noErr;
}

dm_apple_spatial *dm_apple_spatial_create(double rate, dm_spatial_pull_s16 pull,
                                          void *context, int want_head_tracking, int allow_personalized, int built_in_speakers) {
    if (!pull || !(rate>=8000 && rate<=192000) || !isfinite(rate)) return NULL;
    dm_apple_spatial *m=calloc(1,sizeof *m);
    if (!m) return NULL;
    m->pull=pull;m->context=context;
    m->built_in_speakers = built_in_speakers;
    m->stamp.mFlags=kAudioTimeStampSampleTimeValid;
    AudioComponentDescription d={0};
    d.componentType=kAudioUnitType_Mixer;
    d.componentSubType=kAudioUnitSubType_SpatialMixer;
    d.componentManufacturer=kAudioUnitManufacturer_Apple;
    AudioComponent component=AudioComponentFindNext(NULL,&d);
    if (!component || AudioComponentInstanceNew(component,&m->unit)) goto fail;
    AudioStreamBasicDescription out=f32_format(rate,2),in=f32_format(rate,6);
    AudioChannelLayout layout={0};
    layout.mChannelLayoutTag=kAudioChannelLayoutTag_Stereo;
    UInt32 inputs = 1;
    OSStatus s = AudioUnitSetProperty(m->unit, kAudioUnitProperty_ElementCount,
        kAudioUnitScope_Input, 0, &inputs, sizeof inputs);
    if (!s) s=AudioUnitSetProperty(m->unit,kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Output,0,&out,sizeof out);
    if (!s) s=AudioUnitSetProperty(m->unit,kAudioUnitProperty_AudioChannelLayout,
        kAudioUnitScope_Output,0,&layout,sizeof layout);
    if (!s) s=AudioUnitSetProperty(m->unit,kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,0,&in,sizeof in);
    layout.mChannelLayoutTag=kAudioChannelLayoutTag_MPEG_5_1_A;
    if (!s) s=AudioUnitSetProperty(m->unit,kAudioUnitProperty_AudioChannelLayout,
        kAudioUnitScope_Input,0,&layout,sizeof layout);
    UInt32 algorithm=kSpatializationAlgorithm_UseOutputType;
    if (!s) s=AudioUnitSetProperty(m->unit,kAudioUnitProperty_SpatializationAlgorithm,
        kAudioUnitScope_Input,0,&algorithm,sizeof algorithm);
    UInt32 bed=kSpatialMixerSourceMode_AmbienceBed;
    if (!s) s=AudioUnitSetProperty(m->unit,kAudioUnitProperty_SpatialMixerSourceMode,
        kAudioUnitScope_Input,0,&bed,sizeof bed);
    UInt32 output = built_in_speakers ? kSpatialMixerOutputType_BuiltInSpeakers : kSpatialMixerOutputType_Headphones;
    if (!s) s = AudioUnitSetProperty(m->unit, kAudioUnitProperty_SpatialMixerOutputType,
        kAudioUnitScope_Global, 0, &output, sizeof output);
    AURenderCallbackStruct cb={pull51,m};
    if (!s) s=AudioUnitSetProperty(m->unit,kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input,0,&cb,sizeof cb);
    if (s) goto fail;
    if (want_head_tracking) {
        UInt32 head=1;
        m->head_tracking_property_set=!AudioUnitSetProperty(m->unit,
           kAudioUnitProperty_SpatialMixerEnableHeadTracking,kAudioUnitScope_Global,0,
           &head,sizeof head);
        // Lack of entitlement/device support never prevents fixed-head spatial.
    }
    UInt32 profile = allow_personalized ? kSpatialMixerPersonalizedHRTFMode_Auto : kSpatialMixerPersonalizedHRTFMode_Off;
    AudioUnitSetProperty(m->unit, kAudioUnitProperty_SpatialMixerPersonalizedHRTFMode,
        kAudioUnitScope_Global, 0, &profile, sizeof profile);
    if (AudioUnitInitialize(m->unit)) goto fail;
    return m;
fail:
    if (m->unit) AudioComponentInstanceDispose(m->unit);
    free(m);
    return NULL;
}

int dm_apple_spatial_uses_built_in_speakers(const dm_apple_spatial *mixer) {
    return mixer && mixer->built_in_speakers;
}

void dm_apple_spatial_destroy(dm_apple_spatial *m) {
    if (!m)return;
    if (m->unit) {AudioUnitUninitialize(m->unit);AudioComponentInstanceDispose(m->unit);}
    free(m);
}
static int16_t clamp_pcm16(float x) {
    x=fmaxf(-1.0f,fminf(0.9999695f,isfinite(x)?x:0.0f));
    return (int16_t)lrintf(x*32768.0f);
}
int dm_apple_spatial_render(dm_apple_spatial *m, int16_t *out, size_t frames) {
    if (!m || !out || !m->unit) return 0;
    while (frames) {
        UInt32 n=frames<DM_SPATIAL_BLOCK?(UInt32)frames:DM_SPATIAL_BLOCK;
        float left[DM_SPATIAL_BLOCK], right[DM_SPATIAL_BLOCK];
        struct {
            UInt32 count;
            AudioBuffer buffers[2];
        } stereo = {2, {{1, n * sizeof(float), left}, {1, n * sizeof(float), right}}};
        AudioUnitRenderActionFlags flags = 0;
        OSStatus s = AudioUnitRender(m->unit, &flags, &m->stamp, 0, n, (AudioBufferList *)&stereo);
        if (s) {
            memset(out, 0, frames * 2 * sizeof *out);
            return 0;
        }
        for (UInt32 i = 0; i < n; i++) {
            out[i * 2] = clamp_pcm16(left[i]);
            out[i * 2 + 1] = clamp_pcm16(right[i]);
        }
        m->stamp.mSampleTime+=n;
        out+=n*2;frames-=n;
    }
    return 1;
}
int dm_apple_spatial_head_tracking_requested(const dm_apple_spatial *m) {
    if (!m || !m->head_tracking_property_set) return 0;
    UInt32 enabled = 0, size = sizeof enabled;
    OSStatus status = AudioUnitGetProperty(m->unit, kAudioUnitProperty_SpatialMixerEnableHeadTracking,
        kAudioUnitScope_Global, 0, &enabled, &size);
    return status == noErr && size == sizeof enabled && enabled != 0;
}

int dm_apple_spatial_personalized(const dm_apple_spatial *m) {
    if (!m || !m->unit) return 0;
    UInt32 active = 0, size = sizeof active;
    OSStatus status = AudioUnitGetProperty(m->unit, kAudioUnitProperty_SpatialMixerAnyInputIsUsingPersonalizedHRTF,
        kAudioUnitScope_Global, 0, &active, &size);
    return status == noErr && size == sizeof active && active != 0;
}
#endif
