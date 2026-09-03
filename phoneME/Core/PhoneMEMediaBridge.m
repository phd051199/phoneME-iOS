#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_TV
#import <UIKit/UIKit.h>
#endif
#if TARGET_OS_IOS
#import <MediaPlayer/MediaPlayer.h>
#endif

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

@class PMMediaEntry;

static void PMReevaluateNowPlaying(PMMediaEntry *preferredEntry);
static void PMClearNowPlaying(void);

static uint8_t gPMMediaQueueSpecificKey;

static dispatch_queue_t PMMediaQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("dev.phoneme.media", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(queue,
                                    &gPMMediaQueueSpecificKey,
                                    &gPMMediaQueueSpecificKey,
                                    NULL);
    });
    return queue;
}

static void PMPerformMediaQueueSync(dispatch_block_t block) {
    if (dispatch_get_specific(&gPMMediaQueueSpecificKey) != NULL) {
        block();
    } else {
        dispatch_sync(PMMediaQueue(), block);
    }
}

static NSMutableDictionary<NSNumber *, id> *PMMediaRegistry(void) {
    static NSMutableDictionary<NSNumber *, id> *registry;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        registry = [[NSMutableDictionary alloc] init];
    });
    return registry;
}

static NSMutableSet *PMTonePlayers(void) {
    static NSMutableSet *players;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        players = [[NSMutableSet alloc] init];
    });
    return players;
}

static int32_t gNextMediaHandle = 1;
static uint64_t gPMPlaybackSequence = 0;
static void *PMStreamStatusContext = &PMStreamStatusContext;
static void *PMStreamDurationContext = &PMStreamDurationContext;

#if TARGET_OS_IOS
static NSString *gPMMediaApplicationTitle;
static NSString *gPMMediaApplicationArtist;
static UIImage *gPMMediaApplicationArtwork;
static int32_t gPMNowPlayingHandle = 0;
#endif

#if TARGET_OS_IOS || TARGET_OS_TV
static void PMRegisterMediaLifecycleObservers(void);
void phoneme_ios_media_reset(void);
#endif

static void PMConfigureAudioSession(void) {
#if TARGET_OS_IOS || TARGET_OS_TV
    PMRegisterMediaLifecycleObservers();
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSError *categoryError = nil;
    [session setCategory:AVAudioSessionCategoryPlayback
                    mode:AVAudioSessionModeDefault
                 options:AVAudioSessionCategoryOptionMixWithOthers
                   error:&categoryError];
    if (categoryError != nil) {
        NSLog(@"phoneME media: unable to configure audio session: %@",
              categoryError.localizedDescription);
    }
#endif
}

static BOOL PMActivateAudioSession(void) {
#if TARGET_OS_IOS || TARGET_OS_TV
    PMConfigureAudioSession();
    NSError *activationError = nil;
    [AVAudioSession.sharedInstance setActive:YES error:&activationError];
    if (activationError != nil) {
        NSLog(@"phoneME media: unable to activate audio session: %@",
              activationError.localizedDescription);
        return NO;
    }
#endif
    return YES;
}

static NSString *PMStringFromUTF8(const char *value) {
    if (value == NULL) {
        return nil;
    }
    return [NSString stringWithUTF8String:value];
}

static NSString *PMNormalizedContentType(NSString *type) {
    NSString *value = [[type componentsSeparatedByString:@";"] firstObject].lowercaseString;
    if ([value isEqualToString:@"audio/mp3"]) return @"audio/mpeg";
    if ([value isEqualToString:@"audio/wav"]) return @"audio/x-wav";
    if ([value isEqualToString:@"audio/x-midi"] ||
        [value isEqualToString:@"audio/sp-midi"]) return @"audio/midi";
    if ([value isEqualToString:@"audio/x-m4a"]) return @"audio/mp4";
    if ([value isEqualToString:@"audio/x-aac"] ||
        [value isEqualToString:@"audio/aacp"] ||
        [value isEqualToString:@"audio/mp4a-latm"]) return @"audio/aac";
    return value;
}

static BOOL PMIsMIDIType(NSString *type) {
    return [PMNormalizedContentType(type) isEqualToString:@"audio/midi"];
}

// AVMIDIPlayer requires a caller-provided DLS/SF2 bank on iOS. Supplying nil
// works on macOS, but silently leaves many J2ME titles without music on iOS.
// Keep MIDI support self-contained by rendering Standard MIDI data to a small
// mono PCM wave there. This also lets the existing AVAudioPlayer path honor
// MMAPI VolumeControl and loopCount consistently.
typedef struct {
    uint64_t tick;
    uint32_t microseconds;
} PMMIDITempo;

typedef struct {
    uint64_t startTick;
    uint64_t endTick;
    uint8_t note;
    uint8_t velocity;
    uint8_t channel;
    uint8_t program;
} PMMIDINote;

typedef struct {
    BOOL active;
    uint64_t tick;
    uint8_t velocity;
    uint8_t program;
} PMMIDIActiveNote;

static uint16_t PMReadBE16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t PMReadBE32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void PMMIDIWriteLE16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
}

static void PMMIDIWriteLE32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xffU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static BOOL PMReadMIDIVLQ(const uint8_t *bytes,
                          NSUInteger end,
                          NSUInteger *cursor,
                          uint32_t *value) {
    uint32_t result = 0;
    for (NSUInteger index = 0; index < 4; ++index) {
        if (*cursor >= end) return NO;
        const uint8_t byte = bytes[(*cursor)++];
        result = (result << 7U) | (uint32_t)(byte & 0x7fU);
        if ((byte & 0x80U) == 0) {
            *value = result;
            return YES;
        }
    }
    *value = result;
    return YES;
}

static BOOL PMAppendMIDITempo(PMMIDITempo **tempos,
                              NSUInteger *count,
                              NSUInteger *capacity,
                              PMMIDITempo tempo) {
    if (*count >= *capacity) {
        const NSUInteger nextCapacity = *capacity == 0 ? 16 : *capacity * 2;
        PMMIDITempo *next = realloc(*tempos,
                                    nextCapacity * sizeof(PMMIDITempo));
        if (next == NULL) return NO;
        *tempos = next;
        *capacity = nextCapacity;
    }
    (*tempos)[(*count)++] = tempo;
    return YES;
}

static BOOL PMAppendMIDINote(PMMIDINote **notes,
                             NSUInteger *count,
                             NSUInteger *capacity,
                             PMMIDINote note) {
    if (*count >= *capacity) {
        const NSUInteger nextCapacity = *capacity == 0 ? 256 : *capacity * 2;
        PMMIDINote *next = realloc(*notes,
                                   nextCapacity * sizeof(PMMIDINote));
        if (next == NULL) return NO;
        *notes = next;
        *capacity = nextCapacity;
    }
    (*notes)[(*count)++] = note;
    return YES;
}

static int PMCompareMIDITempos(const void *lhs, const void *rhs) {
    const PMMIDITempo *a = lhs;
    const PMMIDITempo *b = rhs;
    if (a->tick < b->tick) return -1;
    if (a->tick > b->tick) return 1;
    return 0;
}

static double PMMIDISecondsForTick(uint64_t tick,
                                   const PMMIDITempo *tempos,
                                   NSUInteger tempoCount,
                                   uint16_t division) {
    uint64_t lastTick = 0;
    uint32_t currentTempo = 500000;
    double seconds = 0.0;
    for (NSUInteger index = 0; index < tempoCount; ++index) {
        const PMMIDITempo tempo = tempos[index];
        if (tempo.tick > tick) break;
        if (tempo.tick >= lastTick) {
            seconds += ((double)(tempo.tick - lastTick) * currentTempo) /
                       ((double)division * 1000000.0);
            lastTick = tempo.tick;
            currentTempo = tempo.microseconds;
        }
    }
    seconds += ((double)(tick - lastTick) * currentTempo) /
               ((double)division * 1000000.0);
    return seconds;
}

static float PMMIDIWaveSample(uint8_t program,
                              uint8_t channel,
                              uint8_t note,
                              double phase,
                              double elapsed,
                              double duration,
                              NSUInteger frame) {
    if (channel == 9) {
        const double decay = exp(-elapsed * (note < 48 ? 13.0 : 20.0));
        if (note == 35 || note == 36) {
            return (float)(sin(phase * (1.0 + 4.0 * exp(-elapsed * 18.0))) *
                           decay);
        }
        uint32_t noise = (uint32_t)(frame * 1664525U +
                                    (uint32_t)note * 1013904223U);
        noise ^= noise >> 13U;
        const float random = ((float)(noise & 0xffffU) / 32767.5f) - 1.0f;
        return random * (float)decay;
    }

    const uint8_t family = program / 8U;
    const double fundamental = sin(phase);
    switch (family) {
        case 0: // piano
            return (float)((fundamental + 0.42 * sin(phase * 2.0) +
                            0.18 * sin(phase * 3.0)) *
                           (0.72 * exp(-elapsed * 1.7) + 0.28));
        case 1: // chromatic percussion
            return (float)((fundamental + 0.6 * sin(phase * 3.01)) *
                           exp(-elapsed * 3.0));
        case 2: // organ
            return (float)(fundamental + 0.35 * sin(phase * 2.0) +
                           0.18 * sin(phase * 4.0));
        case 3: // guitar
            return (float)((fundamental + 0.32 * sin(phase * 2.0) +
                            0.12 * sin(phase * 3.0)) *
                           (0.55 * exp(-elapsed * 2.2) + 0.45));
        case 4: // bass
            return (float)(0.75 * sin(phase) + 0.25 * sin(phase * 2.0));
        case 5: // strings
        case 6: // ensemble
            return (float)(0.72 * sin(phase) + 0.2 * sin(phase * 2.0) +
                           0.08 * sin(phase * 3.0));
        case 7: // brass
            return (float)(0.7 * fundamental +
                           0.3 * (fundamental >= 0.0 ? 1.0 : -1.0));
        case 8: // reed
        case 9: // pipe
            return (float)(0.88 * fundamental + 0.12 * sin(phase * 2.0));
        case 10: // synth lead
        case 11: // synth pad
            return (float)(0.65 * fundamental +
                           0.2 * sin(phase * 2.0) +
                           0.15 * sin(phase * 0.5));
        default:
            (void)duration;
            return (float)fundamental;
    }
}

static NSData *PMRenderMIDIToWave(NSData *midiData) {
    static const NSUInteger kSampleRate = 22050;
    static const double kMaximumDuration = 10.0 * 60.0;
    const uint8_t *bytes = midiData.bytes;
    const NSUInteger length = midiData.length;
    if (bytes == NULL || length < 14 || memcmp(bytes, "MThd", 4) != 0) {
        return nil;
    }

    const uint32_t headerLength = PMReadBE32(bytes + 4);
    if (headerLength < 6 || 8ULL + headerLength > length) return nil;
    const uint16_t trackCount = PMReadBE16(bytes + 10);
    const uint16_t division = PMReadBE16(bytes + 12);
    if (trackCount == 0 || division == 0 || (division & 0x8000U) != 0) {
        return nil;
    }

    PMMIDITempo *tempos = NULL;
    PMMIDINote *notes = NULL;
    NSUInteger tempoCount = 0, tempoCapacity = 0;
    NSUInteger noteCount = 0, noteCapacity = 0;

    NSUInteger offset = 8U + headerLength;
    BOOL valid = YES;
    for (uint16_t track = 0; track < trackCount && offset + 8U <= length; ++track) {
        if (memcmp(bytes + offset, "MTrk", 4) != 0) {
            valid = NO;
            break;
        }
        const uint32_t trackLength = PMReadBE32(bytes + offset + 4U);
        NSUInteger cursor = offset + 8U;
        const NSUInteger end = MIN(length, cursor + (NSUInteger)trackLength);
        uint64_t tick = 0;
        uint8_t runningStatus = 0;
        uint8_t programs[16] = {0};
        PMMIDIActiveNote active[16][128] = {{{0}}};

        while (cursor < end) {
            uint32_t delta = 0;
            if (!PMReadMIDIVLQ(bytes, end, &cursor, &delta)) {
                valid = NO;
                break;
            }
            tick += delta;
            if (cursor >= end) break;
            uint8_t status = bytes[cursor];
            if ((status & 0x80U) != 0) {
                ++cursor;
                runningStatus = status;
            } else if (runningStatus != 0) {
                status = runningStatus;
            } else {
                valid = NO;
                break;
            }

            if (status == 0xffU) {
                runningStatus = 0;
                if (cursor >= end) { valid = NO; break; }
                const uint8_t metaType = bytes[cursor++];
                uint32_t metaLength = 0;
                if (!PMReadMIDIVLQ(bytes, end, &cursor, &metaLength) ||
                    metaLength > end - cursor) {
                    valid = NO;
                    break;
                }
                if (metaType == 0x51U && metaLength == 3U) {
                    const uint32_t microseconds =
                        ((uint32_t)bytes[cursor] << 16U) |
                        ((uint32_t)bytes[cursor + 1U] << 8U) |
                        bytes[cursor + 2U];
                    if (microseconds > 0 &&
                        !PMAppendMIDITempo(&tempos, &tempoCount, &tempoCapacity,
                                           (PMMIDITempo){tick, microseconds})) {
                        valid = NO;
                        break;
                    }
                }
                cursor += metaLength;
                continue;
            }
            if (status == 0xf0U || status == 0xf7U) {
                runningStatus = 0;
                uint32_t sysexLength = 0;
                if (!PMReadMIDIVLQ(bytes, end, &cursor, &sysexLength) ||
                    sysexLength > end - cursor) {
                    valid = NO;
                    break;
                }
                cursor += sysexLength;
                continue;
            }

            const uint8_t kind = status & 0xf0U;
            const uint8_t channel = status & 0x0fU;
            const NSUInteger dataLength = (kind == 0xc0U || kind == 0xd0U) ? 1U : 2U;
            if (cursor + dataLength > end) { valid = NO; break; }
            const uint8_t a = bytes[cursor++];
            const uint8_t b = dataLength == 2U ? bytes[cursor++] : 0U;
            if (kind == 0xc0U) {
                programs[channel] = a & 0x7fU;
                continue;
            }
            if (kind != 0x80U && kind != 0x90U) continue;
            const uint8_t note = a & 0x7fU;
            PMMIDIActiveNote *slot = &active[channel][note];
            if (kind == 0x90U && b > 0) {
                if (slot->active) {
                    PMAppendMIDINote(&notes, &noteCount, &noteCapacity,
                                     (PMMIDINote){slot->tick, MAX(slot->tick + 1, tick),
                                                  note, slot->velocity, channel,
                                                  slot->program});
                }
                *slot = (PMMIDIActiveNote){YES, tick, b, programs[channel]};
            } else if (slot->active) {
                if (!PMAppendMIDINote(&notes, &noteCount, &noteCapacity,
                                      (PMMIDINote){slot->tick, MAX(slot->tick + 1, tick),
                                                   note, slot->velocity, channel,
                                                   slot->program})) {
                    valid = NO;
                    break;
                }
                slot->active = NO;
            }
        }
        if (!valid) break;
        offset = end;
    }

    if (!valid || noteCount == 0) {
        free(tempos);
        free(notes);
        return nil;
    }
    qsort(tempos, tempoCount, sizeof(PMMIDITempo), PMCompareMIDITempos);

    double duration = 0.0;
    for (NSUInteger index = 0; index < noteCount; ++index) {
        const double end = PMMIDISecondsForTick(notes[index].endTick,
                                                tempos, tempoCount, division);
        duration = MAX(duration, end);
    }
    if (!(duration > 0.0) || duration > kMaximumDuration) {
        free(tempos);
        free(notes);
        return nil;
    }

    const NSUInteger frameCount = (NSUInteger)ceil((duration + 0.1) * kSampleRate);
    float *samples = calloc(frameCount, sizeof(float));
    if (samples == NULL) {
        free(tempos);
        free(notes);
        return nil;
    }

    for (NSUInteger index = 0; index < noteCount; ++index) {
        const PMMIDINote event = notes[index];
        const double startSeconds = PMMIDISecondsForTick(event.startTick,
                                                         tempos, tempoCount,
                                                         division);
        const double endSeconds = PMMIDISecondsForTick(event.endTick,
                                                       tempos, tempoCount,
                                                       division);
        const NSUInteger start = MIN(frameCount,
                                     (NSUInteger)floor(startSeconds * kSampleRate));
        const NSUInteger end = MIN(frameCount,
                                   (NSUInteger)ceil(endSeconds * kSampleRate));
        if (end <= start) continue;
        const double frequency = 440.0 * pow(2.0, ((double)event.note - 69.0) / 12.0);
        const double noteDuration = (double)(end - start) / kSampleRate;
        const NSUInteger attackFrames = MAX((NSUInteger)1, (NSUInteger)(0.008 * kSampleRate));
        const NSUInteger releaseFrames = MAX((NSUInteger)1, (NSUInteger)(0.04 * kSampleRate));
        const float amplitude = 0.075f * ((float)event.velocity / 127.0f);
        for (NSUInteger frame = start; frame < end; ++frame) {
            const NSUInteger local = frame - start;
            const NSUInteger remaining = end - frame;
            float envelope = MIN(1.0f,
                                 MIN((float)local / attackFrames,
                                     (float)remaining / releaseFrames));
            if (event.channel == 9) envelope = 1.0f;
            const double elapsed = (double)local / kSampleRate;
            const double phase = 2.0 * M_PI * frequency * elapsed;
            samples[frame] += amplitude * envelope *
                PMMIDIWaveSample(event.program, event.channel, event.note,
                                 phase, elapsed, noteDuration, frame);
        }
    }

    const NSUInteger pcmBytes = frameCount * sizeof(int16_t);
    if (pcmBytes > UINT32_MAX - 36U) {
        free(samples);
        free(tempos);
        free(notes);
        return nil;
    }
    NSMutableData *wave = [NSMutableData dataWithLength:44U + pcmBytes];
    uint8_t *output = wave.mutableBytes;
    memcpy(output, "RIFF", 4);
    PMMIDIWriteLE32(output + 4, (uint32_t)(36U + pcmBytes));
    memcpy(output + 8, "WAVEfmt ", 8);
    PMMIDIWriteLE32(output + 16, 16U);
    PMMIDIWriteLE16(output + 20, 1U);
    PMMIDIWriteLE16(output + 22, 1U);
    PMMIDIWriteLE32(output + 24, (uint32_t)kSampleRate);
    PMMIDIWriteLE32(output + 28, (uint32_t)(kSampleRate * sizeof(int16_t)));
    PMMIDIWriteLE16(output + 32, (uint16_t)sizeof(int16_t));
    PMMIDIWriteLE16(output + 34, 16U);
    memcpy(output + 36, "data", 4);
    PMMIDIWriteLE32(output + 40, (uint32_t)pcmBytes);
    int16_t *pcm = (int16_t *)(output + 44);
    for (NSUInteger frame = 0; frame < frameCount; ++frame) {
        const float clamped = fmaxf(-1.0f, fminf(1.0f, samples[frame]));
        pcm[frame] = (int16_t)lrintf(clamped * 32767.0f);
    }

    free(samples);
    free(tempos);
    free(notes);
    return wave;
}

static NSURL *PMURLFromLocator(NSString *locator) {
    NSString *trimmed = [locator stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (trimmed.length == 0) {
        return nil;
    }

    NSURL *url = [NSURL URLWithString:trimmed];
    if (url.scheme.length > 0) {
        return url;
    }

    NSString *escaped = [trimmed stringByAddingPercentEncodingWithAllowedCharacters:
        NSCharacterSet.URLFragmentAllowedCharacterSet];
    url = [NSURL URLWithString:escaped];
    if (url.scheme.length > 0) {
        return url;
    }
    return [NSURL fileURLWithPath:trimmed];
}

static NSString *PMMediaTitleFromURL(NSURL *url) {
    NSString *name = url.lastPathComponent.stringByDeletingPathExtension;
    name = name.stringByRemovingPercentEncoding ?: name;
    name = [name stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (name.length > 0) {
        return name;
    }
    NSString *host = [url.host stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return host.length > 0 ? host : nil;
}

@interface PMMediaEntry : NSObject <AVAudioPlayerDelegate>
@property(nonatomic, strong) AVAudioPlayer *audioPlayer;
@property(nonatomic, strong) AVMIDIPlayer *midiPlayer;
@property(nonatomic, strong) AVPlayer *streamPlayer;
@property(nonatomic, strong) id streamEndObserver;
@property(nonatomic, strong) id streamFailedObserver;
@property(nonatomic) BOOL streamFailed;
@property(nonatomic) BOOL observingStreamStatus;
@property(nonatomic) BOOL hasPendingSeek;
@property(nonatomic) int64_t pendingSeekMicroseconds;
@property(nonatomic) BOOL resumeAfterPendingSeek;
@property(nonatomic) BOOL resumeAfterSystemSuspend;
@property(nonatomic) BOOL seekInProgress;
@property(nonatomic) NSUInteger seekGeneration;
@property(nonatomic) NSUInteger pendingSeekRetryCount;
@property(nonatomic) int32_t handle;
@property(nonatomic) NSInteger loopCount;
@property(nonatomic) NSInteger midiLoopsRemaining;
@property(nonatomic) BOOL ended;
@property(nonatomic) BOOL muted;
@property(nonatomic) float volume;
@property(nonatomic) NSUInteger playbackGeneration;
@property(nonatomic) BOOL transientTone;
@property(nonatomic) BOOL hasStartedPlayback;
@property(nonatomic) BOOL logicallyPlaying;
@property(nonatomic) BOOL mediaServicesInvalidated;
@property(nonatomic) int64_t lastKnownMediaTimeMicroseconds;
@property(nonatomic) int64_t lastKnownDurationMicroseconds;
@property(nonatomic) uint64_t startedSequence;
@property(nonatomic, copy) NSString *mediaTitle;
@end

@implementation PMMediaEntry

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _loopCount = 1;
        _volume = 1.0f;
    }
    return self;
}

- (void)dealloc {
    if (_observingStreamStatus && _streamPlayer.currentItem != nil) {
        [_streamPlayer.currentItem removeObserver:self
                                       forKeyPath:@"status"
                                          context:PMStreamStatusContext];
        [_streamPlayer.currentItem removeObserver:self
                                       forKeyPath:@"duration"
                                          context:PMStreamDurationContext];
    }
    if (_streamEndObserver != nil) {
        [NSNotificationCenter.defaultCenter removeObserver:_streamEndObserver];
    }
    if (_streamFailedObserver != nil) {
        [NSNotificationCenter.defaultCenter removeObserver:_streamFailedObserver];
    }
}

- (void)applyVolume {
    float effectiveVolume = self.muted ? 0.0f : self.volume;
    self.audioPlayer.volume = effectiveVolume;
    self.streamPlayer.volume = effectiveVolume;
    self.streamPlayer.muted = self.muted;
}

- (void)configureLoopCount:(NSInteger)count {
    self.loopCount = count;
    self.audioPlayer.numberOfLoops = count == -1 ? -1 : MAX(0, count - 1);
}

- (BOOL)start {
    if (self.mediaServicesInvalidated) {
        // Let MediaService observe the failed start and recreate this handle
        // from the source it already owns. Do not touch AVFoundation objects
        // invalidated by a media-server reset.
        return NO;
    }
    if (!PMActivateAudioSession()) {
        return NO;
    }
    self.ended = NO;
    self.streamFailed = NO;
    self.playbackGeneration += 1;
    NSUInteger generation = self.playbackGeneration;

    if (self.audioPlayer != nil) {
        if (self.audioPlayer.currentTime >= self.audioPlayer.duration &&
            self.audioPlayer.duration > 0) {
            self.audioPlayer.currentTime = 0;
        }
        [self applyVolume];
        BOOL started = [self.audioPlayer play];
        if (started) {
            self.logicallyPlaying = YES;
            self.hasStartedPlayback = YES;
            self.startedSequence = ++gPMPlaybackSequence;
        }
        return started;
    }

    if (self.midiPlayer != nil) {
        if (self.midiPlayer.currentPosition >= self.midiPlayer.duration &&
            self.midiPlayer.duration > 0) {
            self.midiPlayer.currentPosition = 0;
        }
        self.midiLoopsRemaining = self.loopCount;
        [self applyVolume];
        self.logicallyPlaying = YES;
        self.hasStartedPlayback = YES;
        self.startedSequence = ++gPMPlaybackSequence;
        [self playMIDIGeneration:generation];
        return YES;
    }

    if (self.streamPlayer != nil) {
        AVPlayerItem *item = self.streamPlayer.currentItem;
        if (item.status == AVPlayerItemStatusFailed || self.streamPlayer.error != nil) {
            self.streamFailed = YES;
            return NO;
        }
        if (item != nil && CMTIME_IS_NUMERIC(item.duration) &&
            CMTimeCompare(self.streamPlayer.currentTime, item.duration) >= 0) {
            self.pendingSeekMicroseconds = 0;
            self.hasPendingSeek = YES;
        }
        [self applyVolume];
        if (self.hasPendingSeek) {
            self.resumeAfterPendingSeek = YES;
            self.logicallyPlaying = YES;
            self.hasStartedPlayback = YES;
            self.startedSequence = ++gPMPlaybackSequence;
            [self applyPendingSeekIfPossible];
            return YES;
        }
        [self.streamPlayer play];
        if (item.status == AVPlayerItemStatusFailed || self.streamPlayer.error != nil) {
            self.streamFailed = YES;
            return NO;
        }
        self.logicallyPlaying = YES;
        self.hasStartedPlayback = YES;
        self.startedSequence = ++gPMPlaybackSequence;
        return YES;
    }

    return NO;
}

- (void)playMIDIGeneration:(NSUInteger)generation {
    if (self.midiPlayer == nil || generation != self.playbackGeneration) {
        return;
    }

    __weak PMMediaEntry *weakSelf = self;
    [self.midiPlayer play:^{
        dispatch_async(PMMediaQueue(), ^{
            PMMediaEntry *strongSelf = weakSelf;
            if (strongSelf == nil || generation != strongSelf.playbackGeneration ||
                strongSelf.midiPlayer == nil) {
                return;
            }

            if (strongSelf.loopCount == -1 || strongSelf.midiLoopsRemaining > 1) {
                if (strongSelf.loopCount != -1) {
                    strongSelf.midiLoopsRemaining -= 1;
                }
                strongSelf.midiPlayer.currentPosition = 0;
                [strongSelf playMIDIGeneration:generation];
            } else {
                strongSelf.ended = YES;
                strongSelf.logicallyPlaying = NO;
                PMReevaluateNowPlaying(nil);
            }
        });
    }];
}

- (BOOL)stop {
    self.playbackGeneration += 1;
    self.logicallyPlaying = NO;
    if (self.mediaServicesInvalidated) return YES;
    if (self.audioPlayer != nil) {
        [self.audioPlayer pause];
        return YES;
    }
    if (self.midiPlayer != nil) {
        [self.midiPlayer stop];
        return YES;
    }
    if (self.streamPlayer != nil) {
        self.resumeAfterPendingSeek = NO;
        [self.streamPlayer pause];
        return YES;
    }
    return NO;
}

- (BOOL)isPlaying {
    if (self.mediaServicesInvalidated) return NO;
    if (self.audioPlayer != nil) return self.audioPlayer.isPlaying;
    if (self.midiPlayer != nil) return self.midiPlayer.isPlaying;
    if (self.streamPlayer != nil) {
        if (@available(iOS 10.0, macOS 10.12, tvOS 10.0, *)) {
            return self.streamPlayer.timeControlStatus == AVPlayerTimeControlStatusPlaying;
        }
        return self.streamPlayer.rate != 0.0f;
    }
    return NO;
}

- (BOOL)hasError {
    if (self.mediaServicesInvalidated) return YES;
    AVPlayerItem *item = self.streamPlayer.currentItem;
    if (item.status == AVPlayerItemStatusFailed || self.streamPlayer.error != nil) {
        self.streamFailed = YES;
    }
    return self.streamFailed;
}

- (int64_t)mediaTimeMicroseconds {
    if (self.mediaServicesInvalidated) {
        return self.lastKnownMediaTimeMicroseconds;
    }
    NSTimeInterval seconds = 0;
    if (self.audioPlayer != nil) {
        seconds = self.audioPlayer.currentTime;
    } else if (self.midiPlayer != nil) {
        seconds = self.midiPlayer.currentPosition;
    } else if (self.streamPlayer != nil) {
        if (self.hasPendingSeek) {
            return self.pendingSeekMicroseconds;
        }
        CMTime time = self.streamPlayer.currentTime;
        if (CMTIME_IS_NUMERIC(time)) {
            seconds = CMTimeGetSeconds(time);
        }
    }
    if (!isfinite(seconds) || seconds < 0) seconds = 0;
    self.lastKnownMediaTimeMicroseconds =
        (int64_t)llround(seconds * 1000000.0);
    return self.lastKnownMediaTimeMicroseconds;
}

- (int64_t)durationMicroseconds {
    if (self.mediaServicesInvalidated) {
        return self.lastKnownDurationMicroseconds > 0
            ? self.lastKnownDurationMicroseconds
            : -1;
    }
    NSTimeInterval seconds = 0;
    if (self.audioPlayer != nil) {
        seconds = self.audioPlayer.duration;
    } else if (self.midiPlayer != nil) {
        seconds = self.midiPlayer.duration;
    } else if (self.streamPlayer.currentItem != nil) {
        CMTime duration = self.streamPlayer.currentItem.duration;
        if (CMTIME_IS_NUMERIC(duration)) {
            seconds = CMTimeGetSeconds(duration);
        }
    }
    if (!isfinite(seconds) || seconds <= 0) return -1;
    self.lastKnownDurationMicroseconds =
        (int64_t)llround(seconds * 1000000.0);
    return self.lastKnownDurationMicroseconds;
}

- (int64_t)setMediaTimeMicroseconds:(int64_t)microseconds {
    if (self.mediaServicesInvalidated) {
        int64_t target = MAX((int64_t)0, microseconds);
        if (self.lastKnownDurationMicroseconds > 0) {
            target = MIN(target, self.lastKnownDurationMicroseconds);
        }
        self.lastKnownMediaTimeMicroseconds = target;
        return target;
    }
    NSTimeInterval seconds = MAX(0, (double)microseconds / 1000000.0);
    if (self.audioPlayer != nil) {
        BOOL resume = self.audioPlayer.isPlaying;
        NSTimeInterval duration = self.audioPlayer.duration;
        NSTimeInterval target = duration > 0 ? MIN(seconds, duration) : seconds;
        [self.audioPlayer pause];
        self.audioPlayer.currentTime = target;
        self.ended = NO;
        if (resume) {
            [self.audioPlayer play];
        }
        return [self mediaTimeMicroseconds];
    }
    if (self.midiPlayer != nil) {
        BOOL resume = self.midiPlayer.isPlaying;
        NSTimeInterval duration = self.midiPlayer.duration;
        NSTimeInterval target = duration > 0 ? MIN(seconds, duration) : seconds;
        [self.midiPlayer stop];
        self.midiPlayer.currentPosition = target;
        self.ended = NO;
        if (resume) {
            self.playbackGeneration += 1;
            [self playMIDIGeneration:self.playbackGeneration];
        }
        return [self mediaTimeMicroseconds];
    }
    if (self.streamPlayer != nil) {
        AVPlayerItem *item = self.streamPlayer.currentItem;
        if (item != nil && CMTIME_IS_NUMERIC(item.duration)) {
            NSTimeInterval duration = CMTimeGetSeconds(item.duration);
            if (isfinite(duration) && duration > 0) {
                seconds = MIN(seconds, duration);
            }
        }
        BOOL resume = self.resumeAfterPendingSeek ||
                      self.streamPlayer.rate != 0.0f;
        if (@available(iOS 10.0, macOS 10.12, tvOS 10.0, *)) {
            resume = resume || self.streamPlayer.timeControlStatus ==
                    AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate;
        }
        self.pendingSeekMicroseconds =
                (int64_t)llround(seconds * 1000000.0);
        self.lastKnownMediaTimeMicroseconds = self.pendingSeekMicroseconds;
        self.hasPendingSeek = YES;
        self.resumeAfterPendingSeek = resume;
        self.ended = NO;
        self.pendingSeekRetryCount = 0;
        self.seekGeneration += 1;
        [item cancelPendingSeeks];
        self.seekInProgress = NO;
        [self applyPendingSeekIfPossible];
        return self.pendingSeekMicroseconds;
    }
    return 0;
}

- (void)invalidateAfterMediaServicesReset {
    // Audio-server reset invalidates every AVFoundation object. Detach KVO /
    // notification observers before dropping those objects, then keep only a
    // lightweight handle marker. C++ MediaService already retains the source
    // bytes/locator, so duplicating them here would unnecessarily double media
    // payload memory just for an exceptional recovery path.
    self.playbackGeneration += 1;
    self.seekGeneration += 1;
    self.logicallyPlaying = NO;
    self.resumeAfterSystemSuspend = NO;
    self.resumeAfterPendingSeek = NO;
    self.hasPendingSeek = NO;
    self.seekInProgress = NO;
    self.pendingSeekRetryCount = 0;

    if (self.observingStreamStatus && self.streamPlayer.currentItem != nil) {
        [self.streamPlayer.currentItem removeObserver:self
                                           forKeyPath:@"status"
                                              context:PMStreamStatusContext];
        [self.streamPlayer.currentItem removeObserver:self
                                           forKeyPath:@"duration"
                                              context:PMStreamDurationContext];
    }
    self.observingStreamStatus = NO;
    if (self.streamEndObserver != nil) {
        [NSNotificationCenter.defaultCenter removeObserver:self.streamEndObserver];
        self.streamEndObserver = nil;
    }
    if (self.streamFailedObserver != nil) {
        [NSNotificationCenter.defaultCenter removeObserver:self.streamFailedObserver];
        self.streamFailedObserver = nil;
    }
    self.audioPlayer.delegate = nil;
    self.audioPlayer = nil;
    self.midiPlayer = nil;
    self.streamPlayer = nil;
    self.streamFailed = YES;
    self.mediaServicesInvalidated = YES;
}

- (void)applyPendingSeekIfPossible {
    AVPlayerItem *item = self.streamPlayer.currentItem;
    if (!self.hasPendingSeek || self.seekInProgress || item == nil ||
        item.status != AVPlayerItemStatusReadyToPlay) {
        return;
    }

    NSTimeInterval seconds = MAX(
        0, (double)self.pendingSeekMicroseconds / 1000000.0);
    if (CMTIME_IS_NUMERIC(item.duration)) {
        NSTimeInterval duration = CMTimeGetSeconds(item.duration);
        if (isfinite(duration) && duration > 0) {
            seconds = MIN(seconds, duration);
        }
    }

    CMTime target = CMTimeMakeWithSeconds(seconds, 1000000);
    CMTime tolerance = CMTimeMakeWithSeconds(0.1, 1000);

    NSTimeInterval resolvedSeconds = CMTimeGetSeconds(target);
    if (isfinite(resolvedSeconds) && resolvedSeconds >= 0) {
        seconds = resolvedSeconds;
    }
    self.pendingSeekMicroseconds =
        (int64_t)llround(seconds * 1000000.0);
    NSUInteger generation = self.seekGeneration;
    self.seekInProgress = YES;
    [self.streamPlayer pause];

    __weak PMMediaEntry *weakSelf = self;
    [self.streamPlayer seekToTime:target
                 toleranceBefore:tolerance
                  toleranceAfter:tolerance
               completionHandler:^(BOOL finished) {
        dispatch_async(PMMediaQueue(), ^{
            PMMediaEntry *strongSelf = weakSelf;
            if (strongSelf == nil || generation != strongSelf.seekGeneration) {
                return;
            }
            strongSelf.seekInProgress = NO;
            strongSelf.ended = NO;
            if (finished) {
                BOOL shouldResume = strongSelf.resumeAfterPendingSeek;
                strongSelf.hasPendingSeek = NO;
                strongSelf.resumeAfterPendingSeek = NO;
                strongSelf.pendingSeekRetryCount = 0;
                if (shouldResume && !strongSelf.streamFailed) {
                    [strongSelf.streamPlayer play];
                }
                PMReevaluateNowPlaying(nil);
                return;
            }

            strongSelf.pendingSeekRetryCount += 1;
            if (strongSelf.pendingSeekRetryCount > 5) {
                BOOL shouldResume = strongSelf.resumeAfterPendingSeek;
                strongSelf.hasPendingSeek = NO;
                strongSelf.resumeAfterPendingSeek = NO;
                strongSelf.pendingSeekRetryCount = 0;
                if (shouldResume && !strongSelf.streamFailed) {
                    [strongSelf.streamPlayer play];
                }
                PMReevaluateNowPlaying(nil);
                return;
            }
            NSUInteger retryGeneration = strongSelf.seekGeneration;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         (int64_t)(0.15 * NSEC_PER_SEC)),
                           PMMediaQueue(), ^{
                PMMediaEntry *retrySelf = weakSelf;
                if (retrySelf == nil ||
                    retryGeneration != retrySelf.seekGeneration ||
                    !retrySelf.hasPendingSeek) {
                    return;
                }
                [retrySelf applyPendingSeekIfPossible];
            });
        });
    }];

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(8.0 * NSEC_PER_SEC)),
                   PMMediaQueue(), ^{
        PMMediaEntry *strongSelf = weakSelf;
        if (strongSelf == nil || generation != strongSelf.seekGeneration ||
            !strongSelf.seekInProgress || !strongSelf.hasPendingSeek) {
            return;
        }
        BOOL shouldResume = strongSelf.resumeAfterPendingSeek;
        strongSelf.seekGeneration += 1;
        [strongSelf.streamPlayer.currentItem cancelPendingSeeks];
        strongSelf.seekInProgress = NO;
        strongSelf.hasPendingSeek = NO;
        strongSelf.resumeAfterPendingSeek = NO;
        strongSelf.pendingSeekRetryCount = 0;
        if (shouldResume && !strongSelf.streamFailed) {
            [strongSelf.streamPlayer play];
        }
        PMReevaluateNowPlaying(nil);
    });
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    (void)keyPath;
    (void)change;
    BOOL isStreamObservation = context == PMStreamStatusContext ||
        context == PMStreamDurationContext;
    if (!isStreamObservation) {
        [super observeValueForKeyPath:keyPath
                             ofObject:object
                               change:change
                              context:context];
        return;
    }

    __weak PMMediaEntry *weakSelf = self;
    dispatch_async(PMMediaQueue(), ^{
        PMMediaEntry *strongSelf = weakSelf;
        AVPlayerItem *item = (AVPlayerItem *)object;
        if (strongSelf == nil || item != strongSelf.streamPlayer.currentItem) {
            return;
        }
        if (item.status == AVPlayerItemStatusFailed) {
            strongSelf.streamFailed = YES;
            strongSelf.hasPendingSeek = NO;
            strongSelf.seekInProgress = NO;
            NSLog(@"phoneME media: player item failed: %@",
                  item.error.localizedDescription ?: @"unknown error");
            PMReevaluateNowPlaying(nil);
        } else if (item.status == AVPlayerItemStatusReadyToPlay) {
            [strongSelf applyPendingSeekIfPossible];
            PMReevaluateNowPlaying(nil);
        }
    });
}

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)player
                       successfully:(BOOL)flag {
    dispatch_async(PMMediaQueue(), ^{
        if (self.transientTone) {
            self.logicallyPlaying = NO;
            [PMTonePlayers() removeObject:self];
        } else if (player == self.audioPlayer) {
            self.ended = YES;
            self.logicallyPlaying = NO;
            PMReevaluateNowPlaying(nil);
        }
    });
}

@end

static PMMediaEntry *PMEntryForHandle(int32_t handle) {
    return PMMediaRegistry()[@(handle)];
}

static int32_t PMRegisterEntry(PMMediaEntry *entry) {
    if (entry == nil) return 0;
    int32_t handle = gNextMediaHandle++;
    if (handle <= 0) {
        gNextMediaHandle = 1;
        handle = gNextMediaHandle++;
    }
    while (PMMediaRegistry()[@(handle)] != nil) {
        handle = gNextMediaHandle++;
        if (handle <= 0) {
            gNextMediaHandle = 1;
            handle = gNextMediaHandle++;
        }
    }
    entry.handle = handle;
    PMMediaRegistry()[@(handle)] = entry;
    return handle;
}

#if TARGET_OS_IOS
static MPRemoteCommandHandlerStatus PMRemotePlay(void) {
    __block MPRemoteCommandHandlerStatus status =
        MPRemoteCommandHandlerStatusNoSuchContent;
    PMPerformMediaQueueSync(^{
        PMMediaEntry *entry = PMEntryForHandle(gPMNowPlayingHandle);
        if (entry == nil) return;
        if ([entry start]) {
            PMReevaluateNowPlaying(entry);
            status = MPRemoteCommandHandlerStatusSuccess;
        } else {
            status = MPRemoteCommandHandlerStatusCommandFailed;
        }
    });
    return status;
}

static MPRemoteCommandHandlerStatus PMRemotePause(void) {
    __block MPRemoteCommandHandlerStatus status =
        MPRemoteCommandHandlerStatusNoSuchContent;
    PMPerformMediaQueueSync(^{
        PMMediaEntry *entry = PMEntryForHandle(gPMNowPlayingHandle);
        if (entry == nil) return;
        if ([entry stop]) {
            PMReevaluateNowPlaying(nil);
            status = MPRemoteCommandHandlerStatusSuccess;
        } else {
            status = MPRemoteCommandHandlerStatusCommandFailed;
        }
    });
    return status;
}

static MPRemoteCommandHandlerStatus PMRemoteToggle(void) {
    __block BOOL playing = NO;
    PMPerformMediaQueueSync(^{
        playing = [PMEntryForHandle(gPMNowPlayingHandle) isPlaying];
    });
    return playing ? PMRemotePause() : PMRemotePlay();
}

static MPRemoteCommandHandlerStatus PMRemoteSetPosition(NSTimeInterval seconds) {
    if (!isfinite(seconds) || seconds < 0) {
        return MPRemoteCommandHandlerStatusCommandFailed;
    }
    __block MPRemoteCommandHandlerStatus status =
        MPRemoteCommandHandlerStatusNoSuchContent;
    PMPerformMediaQueueSync(^{
        PMMediaEntry *entry = PMEntryForHandle(gPMNowPlayingHandle);
        if (entry == nil) return;
        [entry setMediaTimeMicroseconds:
            (int64_t)llround(seconds * 1000000.0)];
        PMReevaluateNowPlaying(nil);
        status = MPRemoteCommandHandlerStatusSuccess;
    });
    return status;
}

static MPRemoteCommandHandlerStatus PMRemoteSkip(NSTimeInterval interval) {
    __block NSTimeInterval target = -1;
    PMPerformMediaQueueSync(^{
        PMMediaEntry *entry = PMEntryForHandle(gPMNowPlayingHandle);
        if (entry == nil) return;
        target = MAX(0, (double)[entry mediaTimeMicroseconds] / 1000000.0 +
                        interval);
    });
    return target >= 0
        ? PMRemoteSetPosition(target)
        : MPRemoteCommandHandlerStatusNoSuchContent;
}

static void PMEnsureRemoteCommandsInstalled(void) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        dispatch_async(dispatch_get_main_queue(), ^{
            MPRemoteCommandCenter *center =
                MPRemoteCommandCenter.sharedCommandCenter;
            [center.playCommand addTargetWithHandler:
                ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
                    (void)event;
                    return PMRemotePlay();
                }];
            [center.pauseCommand addTargetWithHandler:
                ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
                    (void)event;
                    return PMRemotePause();
                }];
            [center.togglePlayPauseCommand addTargetWithHandler:
                ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
                    (void)event;
                    return PMRemoteToggle();
                }];
            [center.stopCommand addTargetWithHandler:
                ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
                    (void)event;
                    return PMRemotePause();
                }];
            [center.changePlaybackPositionCommand addTargetWithHandler:
                ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
                    if (![event isKindOfClass:
                            MPChangePlaybackPositionCommandEvent.class]) {
                        return MPRemoteCommandHandlerStatusCommandFailed;
                    }
                    MPChangePlaybackPositionCommandEvent *positionEvent =
                        (MPChangePlaybackPositionCommandEvent *)event;
                    return PMRemoteSetPosition(positionEvent.positionTime);
                }];
            [center.skipForwardCommand addTargetWithHandler:
                ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
                    NSTimeInterval interval = 10;
                    if ([event isKindOfClass:MPSkipIntervalCommandEvent.class]) {
                        interval = ((MPSkipIntervalCommandEvent *)event).interval;
                    }
                    return PMRemoteSkip(interval);
                }];
            [center.skipBackwardCommand addTargetWithHandler:
                ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
                    NSTimeInterval interval = 10;
                    if ([event isKindOfClass:MPSkipIntervalCommandEvent.class]) {
                        interval = ((MPSkipIntervalCommandEvent *)event).interval;
                    }
                    return PMRemoteSkip(-interval);
                }];
            center.skipForwardCommand.preferredIntervals = @[@10];
            center.skipBackwardCommand.preferredIntervals = @[@10];
            center.nextTrackCommand.enabled = NO;
            center.previousTrackCommand.enabled = NO;
            center.seekForwardCommand.enabled = NO;
            center.seekBackwardCommand.enabled = NO;
        });
    });
}

static BOOL PMEntryIsNowPlayingEligible(PMMediaEntry *entry) {
    if (entry == nil || entry.transientTone || !entry.hasStartedPlayback ||
        entry.ended || entry.streamFailed) {
        return NO;
    }
    int64_t duration = [entry durationMicroseconds];
    return entry.streamPlayer != nil || entry.midiPlayer != nil ||
           entry.loopCount != 1 || duration >= 2000000;
}

static NSInteger PMNowPlayingScore(PMMediaEntry *entry,
                                   PMMediaEntry *preferredEntry,
                                   PMMediaEntry *currentEntry) {
    NSInteger score = [entry isPlaying] ? 300 : 0;
    if (entry.streamPlayer != nil) score += 400;
    if (entry.midiPlayer != nil) score += 180;
    if (entry.loopCount == -1) {
        score += 500;
    } else if (entry.loopCount > 1) {
        score += 180;
    }

    int64_t duration = [entry durationMicroseconds];
    if (duration < 0) {
        score += 150;
    } else if (duration >= 600000000) {
        score += 600;
    } else if (duration >= 60000000) {
        score += 450;
    } else if (duration >= 10000000) {
        score += 300;
    } else if (duration >= 2000000) {
        score += 100;
    }

    if (entry == currentEntry) score += 80;
    if (entry == preferredEntry) score += 120;
    return score;
}

static void PMPublishNowPlaying(PMMediaEntry *entry) {
    PMEnsureRemoteCommandsInstalled();

    NSString *applicationTitle = gPMMediaApplicationTitle.length > 0
        ? gPMMediaApplicationTitle
        : (NSBundle.mainBundle.infoDictionary[@"CFBundleDisplayName"]
            ?: NSBundle.mainBundle.infoDictionary[@"CFBundleName"]
            ?: @"phoneME");
    NSString *applicationArtist = gPMMediaApplicationArtist.length > 0
        ? gPMMediaApplicationArtist
        : @"phoneME";
    NSString *mediaTitle = entry.mediaTitle.length > 0
        ? entry.mediaTitle
        : nil;
    NSString *title = mediaTitle ?: applicationTitle;
    NSString *artist = mediaTitle != nil ? applicationTitle : applicationArtist;
    UIImage *artworkImage = gPMMediaApplicationArtwork;
    int64_t durationMicroseconds = [entry durationMicroseconds];
    NSTimeInterval duration = durationMicroseconds > 0
        ? (double)durationMicroseconds / 1000000.0
        : 0;
    NSTimeInterval elapsed =
        MAX(0, (double)[entry mediaTimeMicroseconds] / 1000000.0);
    BOOL playing = [entry isPlaying];
    BOOL liveStream = entry.streamPlayer != nil && duration <= 0;
    int32_t handle = entry.handle;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSMutableDictionary<NSString *, id> *info =
            [[NSMutableDictionary alloc] init];
        info[MPMediaItemPropertyTitle] = title;
        info[MPMediaItemPropertyArtist] = artist;
        info[MPMediaItemPropertyAlbumTitle] = @"phoneME";
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(elapsed);
        info[MPNowPlayingInfoPropertyPlaybackRate] = playing ? @1.0 : @0.0;
        info[MPNowPlayingInfoPropertyDefaultPlaybackRate] = @1.0;
        info[MPNowPlayingInfoPropertyMediaType] =
            @(MPNowPlayingInfoMediaTypeAudio);
        info[MPNowPlayingInfoPropertyIsLiveStream] = @(liveStream);
        info[MPNowPlayingInfoPropertyExternalContentIdentifier] =
            [NSString stringWithFormat:@"phoneme-media-%d", handle];
        if (duration > 0) {
            info[MPMediaItemPropertyPlaybackDuration] = @(duration);
        }
        if (artworkImage != nil) {
            info[MPMediaItemPropertyArtwork] = [[MPMediaItemArtwork alloc]
                initWithBoundsSize:artworkImage.size
                requestHandler:^UIImage * _Nonnull(CGSize size) {
                    (void)size;
                    return artworkImage;
                }];
        }

        MPNowPlayingInfoCenter *center = MPNowPlayingInfoCenter.defaultCenter;
        center.nowPlayingInfo = info;
        center.playbackState = playing
            ? MPNowPlayingPlaybackStatePlaying
            : MPNowPlayingPlaybackStatePaused;

        MPRemoteCommandCenter *commands =
            MPRemoteCommandCenter.sharedCommandCenter;
        commands.playCommand.enabled = !playing;
        commands.pauseCommand.enabled = playing;
        commands.togglePlayPauseCommand.enabled = YES;
        commands.stopCommand.enabled = playing;
        BOOL seekable = duration > 0;
        commands.changePlaybackPositionCommand.enabled = seekable;
        commands.skipForwardCommand.enabled = seekable;
        commands.skipBackwardCommand.enabled = seekable;
    });
}
#endif

static void PMReevaluateNowPlaying(PMMediaEntry *preferredEntry) {
#if TARGET_OS_IOS
    PMMediaEntry *currentEntry = PMEntryForHandle(gPMNowPlayingHandle);
    PMMediaEntry *bestEntry = nil;
    NSInteger bestScore = NSIntegerMin;
    uint64_t bestSequence = 0;

    for (PMMediaEntry *entry in PMMediaRegistry().allValues) {
        if (!PMEntryIsNowPlayingEligible(entry)) continue;
        if (![entry isPlaying] && entry != currentEntry) continue;
        NSInteger score = PMNowPlayingScore(entry,
                                            preferredEntry,
                                            currentEntry);
        if (bestEntry == nil || score > bestScore ||
            (score == bestScore && entry.startedSequence > bestSequence)) {
            bestEntry = entry;
            bestScore = score;
            bestSequence = entry.startedSequence;
        }
    }

    if (bestEntry == nil) {
        PMClearNowPlaying();
        return;
    }
    gPMNowPlayingHandle = bestEntry.handle;
    PMPublishNowPlaying(bestEntry);
#else
    (void)preferredEntry;
#endif
}

static void PMClearNowPlaying(void) {
#if TARGET_OS_IOS
    gPMNowPlayingHandle = 0;
    PMEnsureRemoteCommandsInstalled();
    dispatch_async(dispatch_get_main_queue(), ^{
        MPNowPlayingInfoCenter *center = MPNowPlayingInfoCenter.defaultCenter;
        center.nowPlayingInfo = nil;
        center.playbackState = MPNowPlayingPlaybackStateStopped;

        MPRemoteCommandCenter *commands =
            MPRemoteCommandCenter.sharedCommandCenter;
        commands.playCommand.enabled = NO;
        commands.pauseCommand.enabled = NO;
        commands.togglePlayPauseCommand.enabled = NO;
        commands.stopCommand.enabled = NO;
        commands.changePlaybackPositionCommand.enabled = NO;
        commands.skipForwardCommand.enabled = NO;
        commands.skipBackwardCommand.enabled = NO;
    });
#endif
}

static PMMediaEntry *PMEntryWithData(NSData *data, NSString *contentType) {
    if (data == nil || data.length == 0) return nil;
    PMConfigureAudioSession();

    PMMediaEntry *entry = [[PMMediaEntry alloc] init];
    NSError *error = nil;
    if (PMIsMIDIType(contentType)) {
#if TARGET_OS_IOS || TARGET_OS_TV
        NSData *wave = PMRenderMIDIToWave(data);
        if (wave == nil) {
            NSLog(@"phoneME media: unable to render MIDI data");
            return nil;
        }
        entry.audioPlayer = [[AVAudioPlayer alloc] initWithData:wave error:&error];
        if (entry.audioPlayer == nil) {
            NSLog(@"phoneME media: unable to create rendered MIDI player: %@",
                  error.localizedDescription);
            return nil;
        }
        entry.audioPlayer.delegate = entry;
        [entry.audioPlayer prepareToPlay];
#else
        entry.midiPlayer = [[AVMIDIPlayer alloc] initWithData:data
                                                soundBankURL:nil
                                                      error:&error];
        if (entry.midiPlayer == nil) {
            NSLog(@"phoneME media: unable to create MIDI player: %@",
                  error.localizedDescription);
            return nil;
        }
        [entry.midiPlayer prepareToPlay];
#endif
    } else {
        entry.audioPlayer = [[AVAudioPlayer alloc] initWithData:data error:&error];
        if (entry.audioPlayer == nil) return nil;
        entry.audioPlayer.delegate = entry;
        [entry.audioPlayer prepareToPlay];
    }
    return entry;
}

static PMMediaEntry *PMEntryWithURL(NSURL *url, NSString *contentType) {
    if (url == nil) return nil;
    PMConfigureAudioSession();

    PMMediaEntry *entry = [[PMMediaEntry alloc] init];
    entry.mediaTitle = PMMediaTitleFromURL(url);
    NSError *error = nil;
    if (url.isFileURL) {
        if (PMIsMIDIType(contentType) ||
            [@[@"mid", @"midi"] containsObject:url.pathExtension.lowercaseString]) {
#if TARGET_OS_IOS || TARGET_OS_TV
            NSData *midiData = [NSData dataWithContentsOfURL:url options:0 error:&error];
            if (midiData == nil) {
                NSLog(@"phoneME media: unable to read MIDI file: %@",
                      error.localizedDescription);
                return nil;
            }
            PMMediaEntry *midiEntry = PMEntryWithData(midiData, @"audio/midi");
            midiEntry.mediaTitle = entry.mediaTitle;
            return midiEntry;
#else
            entry.midiPlayer = [[AVMIDIPlayer alloc] initWithContentsOfURL:url
                                                             soundBankURL:nil
                                                                   error:&error];
            if (entry.midiPlayer == nil) {
                NSLog(@"phoneME media: unable to create MIDI player: %@",
                      error.localizedDescription);
                return nil;
            }
            [entry.midiPlayer prepareToPlay];
#endif
        } else {
            entry.audioPlayer = [[AVAudioPlayer alloc] initWithContentsOfURL:url
                                                                       error:&error];
            if (entry.audioPlayer == nil) return nil;
            entry.audioPlayer.delegate = entry;
            [entry.audioPlayer prepareToPlay];
        }
        return entry;
    }

    NSString *scheme = url.scheme.lowercaseString;
    if (![scheme isEqualToString:@"http"] && ![scheme isEqualToString:@"https"]) {
        return nil;
    }

    NSDictionary *assetOptions = @{
        AVURLAssetAllowsCellularAccessKey: @YES
    };
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:assetOptions];
    AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
    item.canUseNetworkResourcesForLiveStreamingWhilePaused = YES;
    entry.streamPlayer = [AVPlayer playerWithPlayerItem:item];
    entry.streamPlayer.automaticallyWaitsToMinimizeStalling = YES;
    [item addObserver:entry
           forKeyPath:@"status"
              options:NSKeyValueObservingOptionInitial | NSKeyValueObservingOptionNew
              context:PMStreamStatusContext];
    [item addObserver:entry
           forKeyPath:@"duration"
              options:NSKeyValueObservingOptionInitial | NSKeyValueObservingOptionNew
              context:PMStreamDurationContext];
    entry.observingStreamStatus = YES;
    __weak PMMediaEntry *weakEntry = entry;
    entry.streamFailedObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification
                    object:item
                     queue:nil
                usingBlock:^(NSNotification *notification) {
        dispatch_async(PMMediaQueue(), ^{
            PMMediaEntry *strongEntry = weakEntry;
            if (strongEntry == nil) return;
            strongEntry.streamFailed = YES;
            strongEntry.hasPendingSeek = NO;
            NSError *error = notification.userInfo[
                AVPlayerItemFailedToPlayToEndTimeErrorKey];
            NSLog(@"phoneME media: remote stream failed: %@",
                  error.localizedDescription ?: url.absoluteString);
            PMReevaluateNowPlaying(nil);
        });
    }];
    entry.streamEndObserver = [NSNotificationCenter.defaultCenter
        addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                    object:item
                     queue:nil
                usingBlock:^(NSNotification *notification) {
        (void)notification;
        dispatch_async(PMMediaQueue(), ^{
            PMMediaEntry *strongEntry = weakEntry;
            if (strongEntry == nil) return;
            if (strongEntry.loopCount == -1 || strongEntry.loopCount > 1) {
                if (strongEntry.loopCount > 1) {
                    strongEntry.loopCount -= 1;
                }
                [strongEntry.streamPlayer seekToTime:kCMTimeZero
                                  completionHandler:^(BOOL finished) {
                    if (finished) [strongEntry.streamPlayer play];
                }];
            } else {
                strongEntry.ended = YES;
                strongEntry.logicallyPlaying = NO;
                PMReevaluateNowPlaying(nil);
            }
        });
    }];
    return entry;
}

static void PMWriteLE16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value & 0xff);
    destination[1] = (uint8_t)((value >> 8) & 0xff);
}

static void PMWriteLE32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value & 0xff);
    destination[1] = (uint8_t)((value >> 8) & 0xff);
    destination[2] = (uint8_t)((value >> 16) & 0xff);
    destination[3] = (uint8_t)((value >> 24) & 0xff);
}

static NSData *PMToneWAVData(int32_t note, int32_t durationMilliseconds,
                             int32_t volume) {
    const uint32_t sampleRate = 22050;
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const double frequency = 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);
    const uint32_t sampleCount = MAX(1U,
        (uint32_t)(((uint64_t)sampleRate * (uint64_t)durationMilliseconds) / 1000ULL));
    const uint32_t dataSize = sampleCount * sizeof(int16_t);
    NSMutableData *data = [NSMutableData dataWithLength:44 + dataSize];
    uint8_t *bytes = data.mutableBytes;

    memcpy(bytes, "RIFF", 4);
    PMWriteLE32(bytes + 4, 36 + dataSize);
    memcpy(bytes + 8, "WAVEfmt ", 8);
    PMWriteLE32(bytes + 16, 16);
    PMWriteLE16(bytes + 20, 1);
    PMWriteLE16(bytes + 22, channels);
    PMWriteLE32(bytes + 24, sampleRate);
    PMWriteLE32(bytes + 28, sampleRate * channels * (bitsPerSample / 8));
    PMWriteLE16(bytes + 32, channels * (bitsPerSample / 8));
    PMWriteLE16(bytes + 34, bitsPerSample);
    memcpy(bytes + 36, "data", 4);
    PMWriteLE32(bytes + 40, dataSize);

    int16_t *samples = (int16_t *)(bytes + 44);
    double amplitude = 0.28 * ((double)MAX(0, MIN(100, volume)) / 100.0);
    const uint32_t fadeSamples = MIN(sampleCount / 2, sampleRate / 200);
    for (uint32_t index = 0; index < sampleCount; index++) {
        double envelope = 1.0;
        if (fadeSamples > 0 && index < fadeSamples) {
            envelope = (double)index / (double)fadeSamples;
        } else if (fadeSamples > 0 && index >= sampleCount - fadeSamples) {
            envelope = (double)(sampleCount - index - 1) / (double)fadeSamples;
        }
        double phase = (2.0 * M_PI * frequency * (double)index) / (double)sampleRate;
        samples[index] = (int16_t)lrint(sin(phase) * amplitude * envelope * INT16_MAX);
    }
    return data;
}

void phoneme_ios_media_set_application_metadata(const char *title,
                                                const char *artist,
                                                const char *artwork_path) {
#if TARGET_OS_IOS
    NSString *applicationTitle = [PMStringFromUTF8(title) copy];
    NSString *applicationArtist = [PMStringFromUTF8(artist) copy];
    NSString *artworkPath = PMStringFromUTF8(artwork_path);
    UIImage *artwork = artworkPath.length > 0
        ? [UIImage imageWithContentsOfFile:artworkPath]
        : nil;
    PMPerformMediaQueueSync(^{
        gPMMediaApplicationTitle = applicationTitle;
        gPMMediaApplicationArtist = applicationArtist;
        gPMMediaApplicationArtwork = artwork;
        PMReevaluateNowPlaying(nil);
    });
#else
    (void)title;
    (void)artist;
    (void)artwork_path;
#endif
}

int32_t phoneme_ios_media_create_data(const uint8_t *data, int32_t length,
                                      const char *content_type) {
    if (data == NULL || length <= 0) return 0;
    __block int32_t handle = 0;
    NSData *mediaData = [NSData dataWithBytes:data length:(NSUInteger)length];
    NSString *type = PMStringFromUTF8(content_type);
    dispatch_sync(PMMediaQueue(), ^{
        handle = PMRegisterEntry(PMEntryWithData(mediaData, type));
    });
    return handle;
}

int32_t phoneme_ios_media_create_locator(const char *locator,
                                         const char *content_type) {
    NSString *locatorString = PMStringFromUTF8(locator);
    if (locatorString.length == 0) return 0;
    NSString *type = PMStringFromUTF8(content_type);
    __block int32_t handle = 0;
    dispatch_sync(PMMediaQueue(), ^{
        handle = PMRegisterEntry(PMEntryWithURL(PMURLFromLocator(locatorString), type));
    });
    return handle;
}

int32_t phoneme_ios_media_start(int32_t handle) {
    __block BOOL result = NO;
    dispatch_sync(PMMediaQueue(), ^{
        PMMediaEntry *entry = PMEntryForHandle(handle);
        result = [entry start];
        if (result) {
            PMReevaluateNowPlaying(entry);
        }
    });
    return result ? 1 : 0;
}

int32_t phoneme_ios_media_stop(int32_t handle) {
    __block BOOL result = NO;
    dispatch_sync(PMMediaQueue(), ^{
        result = [PMEntryForHandle(handle) stop];
        PMReevaluateNowPlaying(nil);
    });
    return result ? 1 : 0;
}

void phoneme_ios_media_close(int32_t handle) {
    dispatch_sync(PMMediaQueue(), ^{
        PMMediaEntry *entry = PMEntryForHandle(handle);
        [entry stop];
        [PMMediaRegistry() removeObjectForKey:@(handle)];
        PMReevaluateNowPlaying(nil);
    });
}

void phoneme_ios_media_set_loop_count(int32_t handle, int32_t count) {
    dispatch_sync(PMMediaQueue(), ^{
        [PMEntryForHandle(handle) configureLoopCount:count];
        PMReevaluateNowPlaying(nil);
    });
}

void phoneme_ios_media_set_volume(int32_t handle, int32_t level) {
    dispatch_sync(PMMediaQueue(), ^{
        PMMediaEntry *entry = PMEntryForHandle(handle);
        entry.volume = MAX(0.0f, MIN(1.0f, (float)level / 100.0f));
        [entry applyVolume];
    });
}

void phoneme_ios_media_set_mute(int32_t handle, int32_t muted) {
    dispatch_sync(PMMediaQueue(), ^{
        PMMediaEntry *entry = PMEntryForHandle(handle);
        entry.muted = muted != 0;
        [entry applyVolume];
    });
}

int64_t phoneme_ios_media_set_time(int32_t handle, int64_t microseconds) {
    __block int64_t result = 0;
    dispatch_sync(PMMediaQueue(), ^{
        result = [PMEntryForHandle(handle) setMediaTimeMicroseconds:microseconds];
        PMReevaluateNowPlaying(nil);
    });
    return result;
}

int64_t phoneme_ios_media_get_time(int32_t handle) {
    __block int64_t result = 0;
    dispatch_sync(PMMediaQueue(), ^{
        result = [PMEntryForHandle(handle) mediaTimeMicroseconds];
    });
    return result;
}

int64_t phoneme_ios_media_get_duration(int32_t handle) {
    __block int64_t result = -1;
    dispatch_sync(PMMediaQueue(), ^{
        result = [PMEntryForHandle(handle) durationMicroseconds];
    });
    return result;
}

int32_t phoneme_ios_media_is_playing(int32_t handle) {
    __block BOOL result = NO;
    dispatch_sync(PMMediaQueue(), ^{
        result = [PMEntryForHandle(handle) isPlaying];
    });
    return result ? 1 : 0;
}

int32_t phoneme_ios_media_has_ended(int32_t handle) {
    __block BOOL result = NO;
    dispatch_sync(PMMediaQueue(), ^{
        result = PMEntryForHandle(handle).ended;
    });
    return result ? 1 : 0;
}

int32_t phoneme_ios_media_has_error(int32_t handle) {
    __block BOOL result = NO;
    dispatch_sync(PMMediaQueue(), ^{
        result = [PMEntryForHandle(handle) hasError];
    });
    return result ? 1 : 0;
}

int32_t phoneme_ios_media_has_active_playback(void) {
    __block BOOL result = NO;
    PMPerformMediaQueueSync(^{
        for (PMMediaEntry *entry in PMMediaRegistry().allValues) {
            if ([entry isPlaying]) {
                result = YES;
                return;
            }
            if (entry.streamPlayer != nil) {
                BOOL waitingForData = entry.resumeAfterPendingSeek;
                if (@available(iOS 10.0, macOS 10.12, tvOS 10.0, *)) {
                    waitingForData = waitingForData ||
                        entry.streamPlayer.timeControlStatus ==
                            AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate;
                }
                if (waitingForData && !entry.streamFailed && !entry.ended) {
                    result = YES;
                    return;
                }
            }
        }
        for (PMMediaEntry *entry in PMTonePlayers()) {
            if ([entry isPlaying]) {
                result = YES;
                return;
            }
        }
    });
    return result ? 1 : 0;
}

int32_t phoneme_ios_media_play_tone(int32_t note, int32_t duration_ms,
                                    int32_t volume) {
    if (note < 0 || note > 127 || duration_ms <= 0) return 0;
    PMConfigureAudioSession();
    NSData *toneData = PMToneWAVData(note, duration_ms, volume);
    __block BOOL result = NO;
    dispatch_sync(PMMediaQueue(), ^{
        PMMediaEntry *entry = PMEntryWithData(toneData, @"audio/x-wav");
        if (entry == nil) return;
        entry.transientTone = YES;
        [PMTonePlayers() addObject:entry];
        result = [entry start];
        if (!result) [PMTonePlayers() removeObject:entry];
    });
    return result ? 1 : 0;
}

#if TARGET_OS_IOS || TARGET_OS_TV
static uint64_t gPMVibrationGeneration = 0;
static uint64_t gPMLightGeneration = 0;
static BOOL gPMKeepScreenAwake = NO;
#endif

static void PMStopAllMediaForSuspension(void) {
    dispatch_sync(PMMediaQueue(), ^{
        for (PMMediaEntry *entry in PMMediaRegistry().allValues) {
            entry.resumeAfterSystemSuspend = [entry isPlaying] ||
                entry.resumeAfterPendingSeek;
            if (entry.resumeAfterSystemSuspend) {
                [entry stop];
            }
        }
        for (PMMediaEntry *entry in PMTonePlayers()) {
            [entry stop];
        }
        [PMTonePlayers() removeAllObjects];
        PMReevaluateNowPlaying(nil);
    });
}

void phoneme_ios_media_suspend(void) {
    PMStopAllMediaForSuspension();

#if TARGET_OS_IOS || TARGET_OS_TV
    NSError *deactivationError = nil;
    [AVAudioSession.sharedInstance
        setActive:NO
        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
        error:&deactivationError];
    if (deactivationError != nil) {
        NSLog(@"phoneME media: unable to suspend audio session: %@",
              deactivationError.localizedDescription);
    }
#endif
}

void phoneme_ios_media_resume(void) {
    dispatch_sync(PMMediaQueue(), ^{
        for (PMMediaEntry *entry in PMMediaRegistry().allValues) {
            if (!entry.resumeAfterSystemSuspend) {
                continue;
            }
            entry.resumeAfterSystemSuspend = NO;
            (void)[entry start];
        }
        PMReevaluateNowPlaying(nil);
    });
}

#if TARGET_OS_IOS || TARGET_OS_TV
// Reuses the app-suspend machinery for system audio interruptions (phone
// call, Siri, alarm): stop players, then restart them when the OS ends the
// interruption with the resume hint. Without this, the session stayed
// deactivated after an interruption and in-game audio died until the next
// playback start.
static void PMRegisterMediaLifecycleObservers(void) {
    static dispatch_once_t onceToken;
    static id interruptionObserver = nil;
    static id mediaResetObserver = nil;
    dispatch_once(&onceToken, ^{
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        interruptionObserver = [center
            addObserverForName:AVAudioSessionInterruptionNotification
                         object:AVAudioSession.sharedInstance
                          queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) {
                NSUInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey]
                    unsignedIntegerValue];
                if (type == AVAudioSessionInterruptionTypeBegan) {
                    // The system already deactivated the session; skip
                    // setActive:NO — NotifyOthersOnDeactivation would wrongly
                    // unpause other apps' audio mid-interruption.
                    PMStopAllMediaForSuspension();
                } else if (type == AVAudioSessionInterruptionTypeEnded) {
                    NSUInteger options =
                        [note.userInfo[AVAudioSessionInterruptionOptionKey]
                            unsignedIntegerValue];
                    if ((options & AVAudioSessionInterruptionOptionShouldResume)
                            != 0) {
                        phoneme_ios_media_resume();
                    }
                }
            }];
        // Media server death invalidates every AVFoundation player object.
        // Keep lightweight handle markers so C++ MMAPI can observe the reset
        // and rebuild from its retained source on the next explicit start.
        mediaResetObserver = [center
            addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                         object:AVAudioSession.sharedInstance
                          queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *_) {
                phoneme_ios_media_reset();
            }];
        // Static strong references intentionally retain block-based observers
        // for the process lifetime.
        (void)interruptionObserver;
        (void)mediaResetObserver;
    });
}
#endif

void phoneme_ios_media_reset(void) {
    dispatch_sync(PMMediaQueue(), ^{
        for (PMMediaEntry *entry in PMMediaRegistry().allValues) {
            [entry invalidateAfterMediaServicesReset];
        }
        for (PMMediaEntry *entry in PMTonePlayers()) {
            [entry stop];
        }
        [PMTonePlayers() removeAllObjects];
#if TARGET_OS_IOS
        gPMMediaApplicationTitle = nil;
        gPMMediaApplicationArtist = nil;
        gPMMediaApplicationArtwork = nil;
#endif
        PMClearNowPlaying();
    });

#if TARGET_OS_IOS || TARGET_OS_TV
    NSError *deactivationError = nil;
    [AVAudioSession.sharedInstance
        setActive:NO
        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
        error:&deactivationError];
    if (deactivationError != nil) {
        NSLog(@"phoneME media: unable to deactivate audio session: %@",
              deactivationError.localizedDescription);
    }

    void (^resetDeviceState)(void) = ^{
        ++gPMVibrationGeneration;
        ++gPMLightGeneration;
        gPMKeepScreenAwake = NO;
        UIApplication.sharedApplication.idleTimerDisabled = NO;
    };
    if (NSThread.isMainThread) {
        resetDeviceState();
    } else {
        dispatch_sync(dispatch_get_main_queue(), resetDeviceState);
    }
#endif
}

int phoneme_ios_device_start_vibrate(int frequency, int64_t duration_ms) {
#if TARGET_OS_IOS
    if (frequency <= 0 || duration_ms <= 0) return 1;
    dispatch_async(dispatch_get_main_queue(), ^{
        uint64_t generation = ++gPMVibrationGeneration;
        NSTimeInterval interval = 0.85 -
                (0.70 * (double)MIN(100, frequency) / 100.0);
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:
                (double)duration_ms / 1000.0];
        __block void (^pulse)(void);
        pulse = ^{
            if (generation != gPMVibrationGeneration ||
                    [deadline timeIntervalSinceNow] <= 0) {
                pulse = nil;
                return;
            }
            AudioServicesPlaySystemSound(kSystemSoundID_Vibrate);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                    (int64_t)(interval * NSEC_PER_SEC)),
                    dispatch_get_main_queue(), pulse);
        };
        pulse();
    });
    return 1;
#else
    (void)frequency;
    (void)duration_ms;
    return 0;
#endif
}

int phoneme_ios_device_stop_vibrate(void) {
#if TARGET_OS_IOS
    dispatch_async(dispatch_get_main_queue(), ^{
        ++gPMVibrationGeneration;
    });
    return 1;
#else
    return 0;
#endif
}

int phoneme_ios_device_set_vibrate(int enabled) {
    return enabled
        ? phoneme_ios_device_start_vibrate(100, 60000)
        : phoneme_ios_device_stop_vibrate();
}

int phoneme_ios_device_set_backlight(int mode) {
#if TARGET_OS_IOS || TARGET_OS_TV
    dispatch_async(dispatch_get_main_queue(), ^{
        switch (mode) {
            case 0:
                gPMKeepScreenAwake = NO;
                break;
            case 1:
                gPMKeepScreenAwake = YES;
                break;
            case 2:
                gPMKeepScreenAwake = !gPMKeepScreenAwake;
                break;
            case 3:
                break;
            default:
                return;
        }
        UIApplication.sharedApplication.idleTimerDisabled =
                gPMKeepScreenAwake;
    });
    return mode >= 0 && mode <= 3 ? 1 : 0;
#else
    (void)mode;
    return 0;
#endif
}

int phoneme_ios_device_flash_lights(int64_t duration_ms) {
#if TARGET_OS_IOS || TARGET_OS_TV
    if (duration_ms <= 0) return 1;
    dispatch_async(dispatch_get_main_queue(), ^{
        uint64_t generation = ++gPMLightGeneration;
        BOOL previous = UIApplication.sharedApplication.idleTimerDisabled;
        UIApplication.sharedApplication.idleTimerDisabled = YES;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                duration_ms * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (generation == gPMLightGeneration) {
                UIApplication.sharedApplication.idleTimerDisabled = previous;
            }
        });
    });
    return 1;
#else
    (void)duration_ms;
    return 0;
#endif
}
