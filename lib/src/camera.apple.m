#include "camera.h"
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <TargetConditionals.h>

#if TARGET_OS_VISION
i32 camera_permission(void) { return 2; }
void camera_request_permission(void) {}
ns_bool camera_open(i32 position, i32 fps) { (void)position; (void)fps; return false; }
void camera_close(void) {}
i32 camera_status(void) { return -1; }
ns_bool camera_read_rgba(u8 *data, i32 capacity, camera_frame *info) { (void)data; (void)capacity; (void)info; return false; }
ns_bool camera_record_start(const char *path) { (void)path; return false; }
void camera_record_stop(void) {}
i32 camera_record_status(void) { return -1; }
f64 camera_record_duration(void) { return 0; }
const char *camera_last_error(void) { return "Camera capture is not supported on visionOS"; }
#else
// Queue owns AVFoundation objects. The monitor protects only the published
// snapshot/status: no blocking AVFoundation operation runs while holding it.
@interface NSCameraCapture : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, strong) AVAssetWriter *writer;
@property(nonatomic, strong) AVAssetWriterInput *writerInput;
@property(nonatomic, strong) NSMutableData *rgba;
@property(nonatomic, copy) NSString *error;
@property(nonatomic) camera_frame frame;
@property(nonatomic) NSInteger state;
@property(nonatomic) NSInteger recordState;
@property(nonatomic) CMTime firstTime;
@property(nonatomic) double duration;
@property(nonatomic) BOOL recording;
@property(nonatomic) BOOL finishing;
- (void)finishRecording;
- (void)interrupted:(NSNotification *)note;
@end
static NSCameraCapture *camera;
static dispatch_queue_t camera_queue;
static void camera_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        camera = [[NSCameraCapture alloc] init];
        camera_queue = dispatch_queue_create("ns.camera.capture", DISPATCH_QUEUE_SERIAL);
    });
}
static void camera_error(NSString *message) {
    @synchronized(camera) { camera.error = message ?: @"Camera operation failed"; }
}
@implementation NSCameraCapture
- (void)interrupted:(NSNotification *)note {
    dispatch_async(camera_queue, ^{
        if (note.object != self.session) return;
        BOOL fatal = [note.name isEqualToString:AVCaptureSessionRuntimeErrorNotification];
        [self finishRecording];
        @synchronized(self) {
            self.state = fatal ? -1 : 3;
            self.error = fatal ? @"Camera runtime error; close and reopen the camera" : @"Camera interrupted; close and reopen when foreground";
        }
    });
}
- (void)finishRecording {
    if (!self.recording || self.finishing) return;
    self.recording = NO;
    AVAssetWriter *writer = self.writer;
    if (writer.status != AVAssetWriterStatusWriting) {
        [writer cancelWriting];
        self.writer = nil;
        self.writerInput = nil;
        @synchronized(self) { self.recordState = -1; self.error = @"No video frames were recorded"; }
        return;
    }
    self.finishing = YES;
    [self.writerInput markAsFinished];
    @synchronized(self) { self.recordState = 2; }
    [writer finishWritingWithCompletionHandler:^{
        dispatch_async(camera_queue, ^{
            self.writer = nil;
            self.writerInput = nil;
            self.finishing = NO;
            @synchronized(self) {
                self.recordState = writer.status == AVAssetWriterStatusCompleted ? 3 : -1;
                if (self.recordState == -1) self.error = writer.error.localizedDescription ?: @"Could not save video";
            }
        });
    }];
}
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection *)connection {
    (void)output; (void)connection;
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    if (!pixel) return;
    CMTime time = CMSampleBufferGetPresentationTimeStamp(sample);
    if (!CMTIME_IS_NUMERIC(time)) return;
    size_t width = CVPixelBufferGetWidth(pixel), height = CVPixelBufferGetHeight(pixel);
    if (width == 0 || height == 0 || width > 1920 || height > 1920) return;
    if (self.recording) {
        if (self.writer.status == AVAssetWriterStatusUnknown) {
            NSDictionary *settings = @{AVVideoCodecKey: AVVideoCodecTypeH264,
                AVVideoWidthKey: @(width), AVVideoHeightKey: @(height)};
            self.writerInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
            self.writerInput.expectsMediaDataInRealTime = YES;
            if (![self.writer canAddInput:self.writerInput]) {
                [self.writer cancelWriting];
            } else {
                [self.writer addInput:self.writerInput];
                if ([self.writer startWriting]) {
                    self.firstTime = time;
                    [self.writer startSessionAtSourceTime:time];
                }
            }
        }
        if (self.writer.status != AVAssetWriterStatusWriting) {
            camera_error(self.writer.error.localizedDescription ?: @"Could not start video encoder");
            self.recording = NO;
            @synchronized(self) { self.recordState = -1; }
        } else if (self.writerInput.readyForMoreMediaData) {
            if (![self.writerInput appendSampleBuffer:sample]) {
                camera_error(self.writer.error.localizedDescription ?: @"Video encoder failed");
                self.recording = NO;
                @synchronized(self) { self.recordState = -1; }
            } else {
                @synchronized(self) { self.duration = CMTimeGetSeconds(CMTimeSubtract(time, self.firstTime)); }
            }
        }
    }
    if (CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const uint8_t *base = CVPixelBufferGetBaseAddress(pixel);
    size_t stride = CVPixelBufferGetBytesPerRow(pixel);
    @synchronized(self) {
        if (!self.rgba) self.rgba = [[NSMutableData alloc] init];
        self.rgba.length = width * height * 4;
        uint8_t *dest = self.rgba.mutableBytes;
        for (size_t y = 0; y < height; y++) {
            const uint8_t *row = base + stride * y;
            for (size_t x = 0; x < width; x++) {
                size_t d = (y * width + x) * 4, s = x * 4;
                dest[d] = row[s + 2]; dest[d + 1] = row[s + 1];
                dest[d + 2] = row[s]; dest[d + 3] = 255;
            }
        }
        self.frame = (camera_frame){(i32)width, (i32)height, self.frame.sequence + 1, CMTimeGetSeconds(time)};
    }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
}
@end

i32 camera_permission(void) {
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    return status == AVAuthorizationStatusAuthorized ? 1 : (status == AVAuthorizationStatusNotDetermined ? 0 : 2);
}
void camera_request_permission(void) {
    camera_init();
    if (![[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSCameraUsageDescription"]) {
        camera_error(@"Camera access needs an app bundle with NSCameraUsageDescription; use ns project");
        return;
    }
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
        if (!granted) camera_error(@"Camera permission denied; enable it in Settings");
    }];
}
ns_bool camera_open(i32 position, i32 fps) {
    camera_init();
    if ((position != 0 && position != 1) || (fps != 15 && fps != 30)) {
        camera_error(@"Expected back/front camera and 15 or 30 fps"); return false;
    }
    if (camera_permission() != 1) { camera_error(@"Camera permission is required"); return false; }
    @synchronized(camera) {
        if (camera.state != 0 && camera.state != -1) return false;
        camera.state = 1; camera.error = @"";
    }
    dispatch_async(camera_queue, ^{
        [[NSNotificationCenter defaultCenter] removeObserver:camera];
        [camera.session stopRunning];
        camera.session = nil;
        AVCaptureDevicePosition pos = position == 0 ? AVCaptureDevicePositionBack : AVCaptureDevicePositionFront;
        AVCaptureDevice *device = [AVCaptureDevice defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera mediaType:AVMediaTypeVideo position:pos];
#if TARGET_OS_OSX
        if (!device) device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
#endif
        NSError *error = nil;
        AVCaptureDeviceInput *input = device ? [AVCaptureDeviceInput deviceInputWithDevice:device error:&error] : nil;
        AVCaptureSession *session = [[AVCaptureSession alloc] init];
        AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
        output.alwaysDiscardsLateVideoFrames = YES;
        output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
        [output setSampleBufferDelegate:camera queue:camera_queue];
        if (!input || ![session canAddInput:input] || ![session canAddOutput:output]) {
            camera_error(error.localizedDescription ?: @"Camera unavailable on this device");
            @synchronized(camera) { camera.state = -1; } return;
        }
        [session beginConfiguration];
        if ([session canSetSessionPreset:AVCaptureSessionPreset1280x720]) session.sessionPreset = AVCaptureSessionPreset1280x720;
        [session addInput:input]; [session addOutput:output];
        [session commitConfiguration];
        BOOL supported = NO;
        for (AVFrameRateRange *range in device.activeFormat.videoSupportedFrameRateRanges) {
            if (fps >= range.minFrameRate && fps <= range.maxFrameRate) supported = YES;
        }
        if (!supported || ![device lockForConfiguration:&error]) {
            camera_error(error.localizedDescription ?: @"Requested frame rate unavailable");
            @synchronized(camera) { camera.state = -1; } return;
        }
        device.activeVideoMinFrameDuration = CMTimeMake(1, fps);
        device.activeVideoMaxFrameDuration = CMTimeMake(1, fps);
        [device unlockForConfiguration];
        AVCaptureConnection *connection = [output connectionWithMediaType:AVMediaTypeVideo];
#if TARGET_OS_IOS && !TARGET_OS_VISION
        if (@available(iOS 17.0, *)) {
            if ([connection isVideoRotationAngleSupported:90]) connection.videoRotationAngle = 90;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if (connection.isVideoOrientationSupported) connection.videoOrientation = AVCaptureVideoOrientationPortrait;
#pragma clang diagnostic pop
        }
#endif
        if (connection.isVideoMirroringSupported) {
            connection.automaticallyAdjustsVideoMirroring = NO; connection.videoMirrored = NO;
        }
        camera.session = session;
        [[NSNotificationCenter defaultCenter] addObserver:camera selector:@selector(interrupted:) name:AVCaptureSessionWasInterruptedNotification object:session];
        [[NSNotificationCenter defaultCenter] addObserver:camera selector:@selector(interrupted:) name:AVCaptureSessionRuntimeErrorNotification object:session];
        [session startRunning];
        @synchronized(camera) { camera.state = session.isRunning ? 2 : -1; }
        if (!session.isRunning) camera_error(@"Camera failed to start");
    });
    return true;
}
void camera_close(void) {
    camera_init();
    dispatch_async(camera_queue, ^{
        [camera finishRecording];
        [[NSNotificationCenter defaultCenter] removeObserver:camera];
        [camera.session stopRunning]; camera.session = nil;
        @synchronized(camera) { camera.state = 0; camera.rgba = nil; }
    });
}
i32 camera_status(void) { camera_init(); @synchronized(camera) { return (i32)camera.state; } }
ns_bool camera_read_rgba(u8 *data, i32 capacity, camera_frame *info) {
    camera_init();
    @synchronized(camera) {
        if (!data || !info || !camera.rgba || capacity < 0 || (NSUInteger)capacity < camera.rgba.length || camera.frame.sequence <= info->sequence) return false;
        memcpy(data, camera.rgba.bytes, camera.rgba.length); *info = camera.frame; return true;
    }
}
ns_bool camera_record_start(const char *path) {
    camera_init();
    NSString *name = path ? [NSString stringWithUTF8String:path] : nil;
    if (!name.length || !name.isAbsolutePath || ![name.pathExtension.lowercaseString isEqualToString:@"mp4"]) {
        camera_error(@"Recording requires an absolute .mp4 path"); return false;
    }
    @synchronized(camera) {
        if (camera.state != 2 || camera.recordState == 1 || camera.recordState == 2) return false;
        camera.recordState = 1; camera.duration = 0; camera.error = @"";
    }
    dispatch_async(camera_queue, ^{
        if (!camera.session.isRunning || camera.finishing) {
            @synchronized(camera) { camera.recordState = -1; camera.error = @"Camera stopped before recording started"; }
            return;
        }
        NSError *error = nil;
        camera.writer = [[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:name] fileType:AVFileTypeMPEG4 error:&error];
        camera.recording = camera.writer != nil;
        if (!camera.recording) {
            camera_error(error.localizedDescription);
            @synchronized(camera) { camera.recordState = -1; }
        }
    });
    return true;
}
void camera_record_stop(void) { camera_init(); dispatch_async(camera_queue, ^{ [camera finishRecording]; }); }
i32 camera_record_status(void) { camera_init(); @synchronized(camera) { return (i32)camera.recordState; } }
f64 camera_record_duration(void) { camera_init(); @synchronized(camera) { return camera.duration; } }
const char *camera_last_error(void) {
    static _Thread_local char message[1024];
    camera_init(); @synchronized(camera) { snprintf(message, sizeof(message), "%s", camera.error.UTF8String ?: ""); }
    return message;
}

#endif
