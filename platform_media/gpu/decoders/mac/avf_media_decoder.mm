// -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//
// Copyright (c) 2018 Vivaldi Technologies AS. All rights reserved.
// Copyright (C) 2014 Opera Software ASA.  All rights reserved.
//
// This file is an original work developed by Opera Software ASA.

#include "platform_media/gpu/decoders/mac/avf_media_decoder.h"

#include "base/mac/foundation_util.h"
#include "base/mac/scoped_cftyperef.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/task_runner_util.h"
#include "base/threading/thread.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "common/platform_media_pipeline_types.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/data_buffer.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_frame.h"
#include "net/base/mime_util.h"

#include "platform_media/common/mac/framework_type_conversions.h"
#include "platform_media/gpu/decoders/mac/avf_audio_tap.h"
#include "platform_media/gpu/decoders/mac/data_request_handler.h"
#include "platform_media/gpu/pipeline/mac/media_utils_mac.h"

namespace {
using PlayerObserverOnceCallback = base::OnceClosure;
using PlayerObserverRepeatingCallback =
    base::RepeatingCallback<void(base::scoped_nsobject<id>)>;
}  // namespace

@interface PlayerObserver : NSObject {
 @private
  NSString* keyPath_;
  base::OnceClosure once_callback_;
  PlayerObserverRepeatingCallback repeating_callback_;
}

@property(retain, readonly) NSString* keyPath;

- (id)initForKeyPath:(NSString*)keyPath
         withOnceCallback:(base::OnceClosure)once_callback
    withRepeatingCallback:(PlayerObserverRepeatingCallback)repeating_callback;
@end

@implementation PlayerObserver

@synthesize keyPath = keyPath_;

- (id)initForKeyPath:(NSString*)keyPath
         withOnceCallback:(base::OnceClosure)once_callback
    withRepeatingCallback:(PlayerObserverRepeatingCallback)repeating_callback {
  if ((self = [super init])) {
    keyPath_ = [keyPath retain];
    once_callback_ = std::move(once_callback);
    repeating_callback_ = std::move(repeating_callback);
  }
  return self;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  if ([keyPath isEqualToString:[self keyPath]]) {
    if (once_callback_) {
      std::move(once_callback_).Run();
    }
    if (repeating_callback_) {
      repeating_callback_.Run(base::scoped_nsobject<id>(
          [[change objectForKey:NSKeyValueChangeNewKey] retain]));
    }
  }
}

@end

@interface PlayerNotificationObserver : NSObject {
 @private
  base::OnceClosure callback_;
}
- (id)initWithCallback:(base::OnceClosure)callback;
@end

@implementation PlayerNotificationObserver

- (id)initWithCallback:(base::OnceClosure)callback {
  if ((self = [super init])) {
    callback_ = std::move(callback);
  }
  return self;
}

- (void)observe:(NSNotification*)notification {
  if (callback_) {
    std::move(callback_).Run();
  }
}

@end

namespace media {

namespace {

// The initial value of the amount of data that we require AVPlayer to have in
// order to consider it unlikely to stall right after starting to play.
constexpr base::TimeDelta kInitialRequiredLoadedTimeRange =
    base::Milliseconds(300);

// Each time AVPlayer runs out of data we increase the required loaded time
// range size up to this value.
constexpr base::TimeDelta kMaxRequiredLoadedTimeRange =
    base::Seconds(4);

class BackgroundThread {
 public:
  BackgroundThread() : thread_("OpMediaDecoder") { CHECK(thread_.Start()); }

  static BackgroundThread* getInstance() {
    static BackgroundThread* instance = new BackgroundThread();
    return instance;
  }

  scoped_refptr<base::SequencedTaskRunner> task_runner() const {
    return thread_.task_runner();
  }

 private:
  base::Thread thread_;
};

class ScopedBufferLock {
 public:
  explicit ScopedBufferLock(CVPixelBufferRef buffer) : buffer_(buffer) {
    CVPixelBufferLockBaseAddress(buffer_, kCVPixelBufferLock_ReadOnly);
  }
  ~ScopedBufferLock() {
    CVPixelBufferUnlockBaseAddress(buffer_, kCVPixelBufferLock_ReadOnly);
  }

 private:
  const CVPixelBufferRef buffer_;
};

scoped_refptr<DataBuffer> GetVideoFrame(AVPlayerItemVideoOutput* video_output,
                                        const CMTime& timestamp,
                                        const gfx::Size& coded_size) {
  TRACE_EVENT0("IPC_MEDIA", __FUNCTION__);

  base::ScopedCFTypeRef<CVPixelBufferRef> pixel_buffer;
  if ([video_output hasNewPixelBufferForItemTime:timestamp]) {
    pixel_buffer.reset([video_output copyPixelBufferForItemTime:timestamp
                                             itemTimeForDisplay:nil]);
  }
  if (pixel_buffer == NULL) {
    VLOG(3) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " No pixel buffer available for time "
            << CMTimeToTimeDelta(timestamp).InMicroseconds();
    return nullptr;
  }

  DCHECK_EQ(CVPixelBufferGetPlaneCount(pixel_buffer), 3u);

  // TODO(wdzierzanowski): Don't copy pixel buffers to main memory, share GL
  // textures with the render process instead. Will be investigated in work
  // package DNA-21454.

  ScopedBufferLock auto_lock(pixel_buffer);

  int strides[3] = {0};
  size_t plane_sizes[3] = {0};
  const int video_frame_planes[] = {VideoFrame::kYPlane, VideoFrame::kUPlane,
                                    VideoFrame::kVPlane};

  // The planes in the pixel buffer are YUV, but PassThroughVideoDecoder
  // assumes YVU, so we switch the order of the last two planes.
  const int planes[] = {0, 2, 1};
  for (int i = 0; i < 3; ++i) {
    const int plane = planes[i];

    // TODO(wdzierzanowski): Use real stride values for video config. Will be
    // fixed in work package DNA-21454
    strides[plane] = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, plane);
    VLOG(7) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " strides[" << plane
            << "] = " << strides[plane];

    plane_sizes[plane] =
        strides[plane] *
        VideoFrame::PlaneSize(VideoPixelFormat::PIXEL_FORMAT_YV12,
                              video_frame_planes[plane], coded_size)
            .height();
  }

  // Copy all planes into contiguous memory.
  const int data_size = plane_sizes[0] + plane_sizes[1] + plane_sizes[2];
  scoped_refptr<DataBuffer> video_data_buffer = new DataBuffer(data_size);
  size_t data_offset = 0;
  for (int i = 0; i < 3; ++i) {
    const int plane = planes[i];

    memcpy(video_data_buffer->writable_data() + data_offset,
           CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, plane),
           plane_sizes[plane]);

    data_offset += plane_sizes[plane];
  }

  video_data_buffer->set_data_size(data_size);
  video_data_buffer->set_timestamp(CMTimeToTimeDelta(timestamp));

  return video_data_buffer;
}

void SetAudioMix(
    const scoped_refptr<AVFMediaDecoder::SharedCancellationFlag> canceled,
    AVPlayerItem* item,
    base::scoped_nsobject<AVAudioMix> audio_mix) {
  DCHECK(BackgroundThread::getInstance()
             ->task_runner()
             ->RunsTasksInCurrentSequence());
  TRACE_EVENT0("IPC_MEDIA", __FUNCTION__);

  if (canceled->data.IsSet())
    return;

  [item setAudioMix:audio_mix];
}

bool IsPlayerLikelyToStallWithRanges(
    AVPlayerItem* item,
    base::scoped_nsobject<NSArray> loaded_ranges,
    base::TimeDelta min_range_size) {
  // The ranges provided might be discontinuous, but this decoder is interested
  // only in first continuous range, and how much time is buffered in this
  // range. Other ranges are not necessary for playback continuation.
  if ([loaded_ranges count] > 0) {
    CMTimeRange time_range = [[loaded_ranges objectAtIndex:0] CMTimeRangeValue];

    const base::TimeDelta end_of_loaded_range =
        CMTimeToTimeDelta(time_range.start) +
        CMTimeToTimeDelta(time_range.duration);
    if (end_of_loaded_range >= CMTimeToTimeDelta([item duration]))
      return false;

    const base::TimeDelta current_time = CMTimeToTimeDelta([item currentTime]);
    return end_of_loaded_range - current_time < min_range_size;
  }

  VLOG(5) << " PROPMEDIA(GPU) : " << __FUNCTION__
          << " AVPlayerItem does not have any loadedTimeRanges value";
  return true;
}

bool IsPlayerLikelyToStall(
    const scoped_refptr<AVFMediaDecoder::SharedCancellationFlag> canceled,
    AVPlayerItem* item,
    base::TimeDelta min_range_size) {
  DCHECK(BackgroundThread::getInstance()
             ->task_runner()
             ->RunsTasksInCurrentSequence());
  TRACE_EVENT0("IPC_MEDIA", "will stall?");

  if (canceled->data.IsSet())
    return false;

  return IsPlayerLikelyToStallWithRanges(
      item, base::scoped_nsobject<NSArray>([[item loadedTimeRanges] retain]),
      min_range_size);
}

}  // namespace

AVFMediaDecoder::AVFMediaDecoder(AVFMediaDecoderClient* client)
    : client_(client),
      min_loaded_range_size_(kInitialRequiredLoadedTimeRange),
      background_tasks_canceled_(new SharedCancellationFlag) {
  DCHECK(client_ != NULL);
}

AVFMediaDecoder::~AVFMediaDecoder() {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__;
  DCHECK(thread_checker_.CalledOnValidThread());

  background_tasks_canceled_->data.Set();

  // Without it, memory allocated by AVFoundation when we call
  // -[AVAssetResourceLoadingDataRequest respondWithData:] in DataSourceLoader
  // is never released.  Yes, it's weird, but I don't know how else we can
  // avoid the memory leak.
  // Also, the AVSimplePlayer demo application does it, too.
  [player_ pause];

  [player_ removeTimeObserver:time_observer_handle_];

  [[player_ currentItem] removeOutput:video_output_];

  // This finalizes the audio processing tap.
  [[player_ currentItem] setAudioMix:nil];

  if (player_item_loaded_times_observer_.get() != nil) {
    [[player_ currentItem]
        removeObserver:player_item_loaded_times_observer_
            forKeyPath:[player_item_loaded_times_observer_ keyPath]];
  }

  [[NSNotificationCenter defaultCenter] removeObserver:played_to_end_observer_];

  if (rate_observer_.get() != nil) {
    [player_ removeObserver:rate_observer_ forKeyPath:[rate_observer_ keyPath]];
  }
  if (status_observer_.get() != nil) {
    [player_ removeObserver:status_observer_
                 forKeyPath:[status_observer_ keyPath]];
  }

  data_request_handler_->Stop();
}

void AVFMediaDecoder::Initialize(ipc_data_source::Info source_info,
                                 InitializeCallback initialize_cb) {
  DCHECK(thread_checker_.CalledOnValidThread());

  data_request_handler_ = base::MakeRefCounted<media::DataRequestHandler>();
  data_request_handler_->Init(std::move(source_info), nil);

  base::scoped_nsobject<NSArray> asset_keys_to_load_and_test(
      [[NSArray arrayWithObjects:@"playable", @"hasProtectedContent", @"tracks",
                                 @"duration", nil] retain]);
  __block base::OnceClosure asset_keys_loaded_cb =
      BindToCurrentLoop(base::BindOnce(
          &AVFMediaDecoder::AssetKeysLoaded, weak_ptr_factory_.GetWeakPtr(),
          std::move(initialize_cb), asset_keys_to_load_and_test));
  [data_request_handler_->GetAsset()
      loadValuesAsynchronouslyForKeys:asset_keys_to_load_and_test
                    completionHandler:^{
                      std::move(asset_keys_loaded_cb).Run();
                    }];
}

void AVFMediaDecoder::Seek(const base::TimeDelta& time, SeekCallback seek_cb) {
  DCHECK(thread_checker_.CalledOnValidThread());

  TRACE_EVENT_ASYNC_BEGIN0("IPC_MEDIA", "AVFMediaDecoder::Seek", this);
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Seeking to "
          << time.InMicroseconds() << " per pipeline request";

  if (seeking_) {
    DCHECK(!seek_on_seek_done_task_);
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Auto-seeking now, postponing pipeline seek request";
    seek_on_seek_done_task_ =
        base::BindOnce(&AVFMediaDecoder::Seek, weak_ptr_factory_.GetWeakPtr(),
                       time, std::move(seek_cb));
    return;
  }

  ScheduleSeekTask(base::BindOnce(&AVFMediaDecoder::SeekTask,
                                  weak_ptr_factory_.GetWeakPtr(), time,
                                  std::move(seek_cb)));
}

void AVFMediaDecoder::NotifyStreamCapacityDepleted() {
  DCHECK(thread_checker_.CalledOnValidThread());
  VLOG(5) << " PROPMEDIA(GPU) : " << __FUNCTION__;

  // We were notified by _a_ stream.  Other streams may still have some
  // capacity available.
  if (client_->HasAvailableCapacity())
    return;

  if (playback_state_ != PLAYING)
    return;

  if (seeking_) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Ignoring stream capacity depletion while seeking";
    return;
  }

  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " PAUSING AVPlayer";
  playback_state_ = STOPPING;
  [player_ pause];
}

void AVFMediaDecoder::NotifyStreamCapacityAvailable() {
  DCHECK(thread_checker_.CalledOnValidThread());
  VLOG(5) << " PROPMEDIA(GPU) : " << __FUNCTION__;

  PlayWhenReady("stream capacity available");
}

base::TimeDelta AVFMediaDecoder::start_time() const {
  DCHECK(thread_checker_.CalledOnValidThread());

  return GetStartTimeFromTrack(has_audio_track() ? AudioTrack() : VideoTrack());
}

void AVFMediaDecoder::AssetKeysLoaded(InitializeCallback initialize_cb,
                                      base::scoped_nsobject<NSArray> keys) {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__;
  DCHECK(thread_checker_.CalledOnValidThread());

  // First test whether the values of each of the keys we need have been
  // successfully loaded.

  AVAsset* asset = data_request_handler_->GetAsset();
  for (NSString* key in keys.get()) {
    NSError* error = nil;
    if ([asset statusOfValueForKey:key
                             error:&error] != AVKeyValueStatusLoaded) {
      VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
              << " Can't access asset key: " << [key UTF8String];
      std::move(initialize_cb).Run(false);
      return;
    }
  }

  if (![asset isPlayable]) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Asset is not playable";
    std::move(initialize_cb).Run(false);
    return;
  }

  if ([asset hasProtectedContent]) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Asset has protected content";
    std::move(initialize_cb).Run(false);
    return;
  }

  // We can play this asset.

  AVPlayerItem* player_item = [AVPlayerItem playerItemWithAsset:asset];
  player_.reset([[AVPlayer alloc] initWithPlayerItem:player_item]);

  base::OnceClosure status_known_cb = BindToCurrentLoop(
      base::BindOnce(&AVFMediaDecoder::PlayerStatusKnown,
                     weak_ptr_factory_.GetWeakPtr(), std::move(initialize_cb)));

  if ([player_ status] == AVPlayerStatusReadyToPlay) {
    std::move(status_known_cb).Run();
  } else {
    DCHECK([player_ status] == AVPlayerStatusUnknown);

    status_observer_.reset([[PlayerObserver alloc]
               initForKeyPath:@"status"
             withOnceCallback:std::move(status_known_cb)
        withRepeatingCallback:PlayerObserverRepeatingCallback()]);

    [player_ addObserver:status_observer_
              forKeyPath:[status_observer_ keyPath]
                 options:0
                 context:nil];
  }
}

void AVFMediaDecoder::PlayerStatusKnown(InitializeCallback initialize_cb) {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
          << " Player status: " << [player_ status];
  DCHECK(thread_checker_.CalledOnValidThread());

  if ([player_ status] != AVPlayerStatusReadyToPlay) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Player status changed to not playable";
    std::move(initialize_cb).Run(false);
    return;
  }

  if (!has_video_track() && !has_audio_track()) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " No tracks to play";
    std::move(initialize_cb).Run(false);
    return;
  }

  if (!CalculateBitrate()) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Bitrate unavailable";
    std::move(initialize_cb).Run(false);
    return;
  }

  // AVPlayer is ready to play.

  duration_ = CMTimeToTimeDelta([[[player_ currentItem] asset] duration]);

  PlayerObserverRepeatingCallback rate_cb =
      BindToCurrentLoop(base::BindRepeating(&AVFMediaDecoder::PlayerRateChanged,
                                            weak_ptr_factory_.GetWeakPtr()));
  rate_observer_.reset([[PlayerObserver alloc]
             initForKeyPath:@"rate"
           withOnceCallback:base::OnceClosure()
      withRepeatingCallback:std::move(rate_cb)]);

  [player_ addObserver:rate_observer_
            forKeyPath:[rate_observer_ keyPath]
               options:NSKeyValueObservingOptionNew
               context:nil];

  base::OnceClosure finish_cb = BindToCurrentLoop(
      base::BindOnce(&AVFMediaDecoder::PlayerPlayedToEnd,
                     weak_ptr_factory_.GetWeakPtr(), "notification"));
  played_to_end_observer_.reset([[PlayerNotificationObserver alloc]
      initWithCallback:std::move(finish_cb)]);

  [[NSNotificationCenter defaultCenter]
      addObserver:played_to_end_observer_
         selector:@selector(observe:)
             name:AVPlayerItemDidPlayToEndTimeNotification
           object:[player_ currentItem]];
  [[NSNotificationCenter defaultCenter]
      addObserver:played_to_end_observer_
         selector:@selector(observe:)
             name:AVPlayerItemFailedToPlayToEndTimeNotification
           object:[player_ currentItem]];

  PlayerObserverRepeatingCallback time_ranges_changed_cb = BindToCurrentLoop(
      base::BindRepeating(&AVFMediaDecoder::PlayerItemTimeRangesChanged,
                          weak_ptr_factory_.GetWeakPtr()));
  player_item_loaded_times_observer_.reset([[PlayerObserver alloc]
             initForKeyPath:@"loadedTimeRanges"
           withOnceCallback:base::OnceClosure()
      withRepeatingCallback:std::move(time_ranges_changed_cb)]);

  [[player_ currentItem]
      addObserver:player_item_loaded_times_observer_
       forKeyPath:[player_item_loaded_times_observer_ keyPath]
          options:NSKeyValueObservingOptionNew
          context:nil];

  InitializeAudioOutput(std::move(initialize_cb));
}

bool AVFMediaDecoder::CalculateBitrate() {
  DCHECK(thread_checker_.CalledOnValidThread());

  const float bitrate =
      [AudioTrack() estimatedDataRate] + [VideoTrack() estimatedDataRate];
  if (std::isnan(bitrate) || bitrate < 0.0 ||
      !base::IsValueInRangeForNumericType<decltype(bitrate_)>(bitrate))
    return false;

  bitrate_ = bitrate;
  return true;
}

void AVFMediaDecoder::PlayerItemTimeRangesChanged(
    base::scoped_nsobject<id> new_ranges) {
  DCHECK(thread_checker_.CalledOnValidThread());

  base::scoped_nsobject<NSArray> new_ranges_value(
      [base::mac::ObjCCastStrict<NSArray>(new_ranges.get()) retain]);

  const bool likely_to_stall = IsPlayerLikelyToStallWithRanges(
      [player_ currentItem], new_ranges_value, min_loaded_range_size_);

  if (pending_seek_task_) {
    SeekIfNotLikelyToStall(likely_to_stall);
  } else {
    PlayIfNotLikelyToStall("has enough data", likely_to_stall);
  }
}

void AVFMediaDecoder::PlayerRateChanged(base::scoped_nsobject<id> new_rate) {
  const int new_rate_value =
      [base::mac::ObjCCastStrict<NSNumber>(new_rate.get()) intValue];
  VLOG(3) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " : " << new_rate_value;
  DCHECK(thread_checker_.CalledOnValidThread());

  if (new_rate_value > 0) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " AVPlayer started PLAYING";
    DCHECK_EQ(playback_state_, STARTING);
    playback_state_ = PLAYING;
    return;
  }

  if (playback_state_ != STOPPING) {
    // AVPlayer stopped spontaneously.  This happens when it can't load data
    // fast enough.  Let's try to give it more chance of playing smoothly by
    // increasing the required amount of data loaded before resuming playback.
    min_loaded_range_size_ =
        std::min(min_loaded_range_size_ * 2, kMaxRequiredLoadedTimeRange);
  }

  playback_state_ = STOPPED;

  const base::TimeDelta current_time = CMTimeToTimeDelta([player_ currentTime]);
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " AVPlayer was PAUSED @"
          << current_time.InMicroseconds();

  if (last_audio_timestamp_ >= duration_) {
    PlayerPlayedToEnd("pause");
  } else if (!seeking_ && last_audio_timestamp_ != media::kNoTimestamp) {
    TRACE_EVENT_ASYNC_BEGIN0("IPC_MEDIA", "AVFMediaDecoder::Auto-seek", this);

    DCHECK(has_audio_track());

    // AVFMediaDecoder receives audio ahead of [player_ currentTime].  In order
    // to preserve audio signal continuity when |player_| resumes playing, we
    // have to seek forwards to the last audio timestamp we got, see
    // |AutoSeekTask()|.
    ScheduleSeekTask(base::BindOnce(&AVFMediaDecoder::AutoSeekTask,
                                    weak_ptr_factory_.GetWeakPtr()));
  }

  if (play_on_pause_done_) {
    play_on_pause_done_ = false;
    PlayWhenReady("Player rate changed");
  }
}

void AVFMediaDecoder::PlayWhenReady(base::StringPiece reason) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (!client_->HasAvailableCapacity())
    return;

  if (playback_state_ == PLAYING || playback_state_ == STARTING) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Giving up on playing AVPlayer when '" << reason
            << ", because already playing/starting to play";
    return;
  }

  if (stream_has_ended_) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Giving up on playing AVPlayer when '" << reason
            << "', because the stream has ended";
    return;
  }

  base::PostTaskAndReplyWithResult(
      background_runner().get(), FROM_HERE,
      base::BindOnce(&IsPlayerLikelyToStall, background_tasks_canceled_,
                     [player_ currentItem], min_loaded_range_size_),
      base::BindOnce(&AVFMediaDecoder::PlayIfNotLikelyToStall,
                     weak_ptr_factory_.GetWeakPtr(), reason));
}

void AVFMediaDecoder::PlayIfNotLikelyToStall(base::StringPiece reason,
                                             bool likely_to_stall) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (!client_->HasAvailableCapacity())
    return;

  if (playback_state_ == PLAYING || playback_state_ == STARTING) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Giving up on playing AVPlayer when '" << reason
            << "', because already playing/starting to play";
    return;
  }

  if (stream_has_ended_) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Giving up on playing AVPlayer when '" << reason
            << "', because the stream has ended";
    return;
  }

  if (likely_to_stall) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Giving up on playing AVPlayer when '" << reason
            << "', because the player is likely to stall";
    return;
  }

  // In the following two cases the client might require a new buffer before it
  // emits another such notification, and we can't provide a new buffer until
  // we play our AVPlayer again.  Thus, instead of just ignoring this
  // notification, postpone it.
  if (seeking_) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Temporarily ignoring '"
            << reason << "' notification while seeking";
    play_on_seek_done_ = true;
    return;
  }
  if (playback_state_ == STOPPING) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Temporarily ignoring '"
            << reason << "' notification while pausing";
    play_on_pause_done_ = true;
    return;
  }

  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
          << " PLAYING AVPlayer because " << reason;
  DCHECK_EQ(playback_state_, STOPPED);
  playback_state_ = STARTING;
  [player_ play];
}

void AVFMediaDecoder::SeekIfNotLikelyToStall(bool likely_to_stall) {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << '(' << likely_to_stall
          << ')';
  DCHECK(thread_checker_.CalledOnValidThread());

  if (likely_to_stall)
    return;

  // If |PlayerItemTimeRangesChanged()| is called while seek task is scheduled
  // but not finished yet, then pending_seek_task_ might be consumed already
  // and might be null here.
  if (pending_seek_task_) {
    std::move(pending_seek_task_).Run();
  }
}

void AVFMediaDecoder::ScheduleSeekTask(base::OnceClosure seek_task) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(!seeking_);
  DCHECK(!pending_seek_task_);

  // We must not request a seek if AVPlayer's internal buffers are drained.
  // Sometimes, AVPlayer never finishes the seek if the seek is started in such
  // a state.

  seeking_ = true;
  pending_seek_task_ = std::move(seek_task);

  base::PostTaskAndReplyWithResult(
      background_runner().get(), FROM_HERE,
      base::BindOnce(&IsPlayerLikelyToStall, background_tasks_canceled_,
                     [player_ currentItem], min_loaded_range_size_),
      base::BindOnce(&AVFMediaDecoder::SeekIfNotLikelyToStall,
                     weak_ptr_factory_.GetWeakPtr()));
}

void AVFMediaDecoder::SeekTask(const base::TimeDelta& time,
                               SeekCallback seek_cb) {
  DCHECK(thread_checker_.CalledOnValidThread());

  const scoped_refptr<base::TaskRunner> runner = background_runner();

  __block base::OnceClosure set_audio_mix_task =
      has_audio_track()
          // The audio mix will have to be reset to preserve A/V
          // synchronization.
          ? base::BindOnce(&SetAudioMix, background_tasks_canceled_,
                           [player_ currentItem], GetAudioMix(AudioTrack()))
          : base::OnceClosure();

  __block base::OnceCallback<void(bool)> seek_done_cb = BindToCurrentLoop(
      base::BindOnce(&AVFMediaDecoder::SeekDone, weak_ptr_factory_.GetWeakPtr(),
                     std::move(seek_cb)));

  [player_ seekToTime:TimeDeltaToCMTime(time)
        toleranceBefore:CoreMediaGlueCMTimeToCMTime(kCMTimeZero)
         toleranceAfter:CoreMediaGlueCMTimeToCMTime(kCMTimeZero)
      completionHandler:^(BOOL finished) {
        VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
                << (finished ? "  Seek DONE"
                             : "  Seek was interrupted/rejected");
        if (finished && set_audio_mix_task) {
          runner->PostTaskAndReply(
              FROM_HERE, std::move(set_audio_mix_task),
              base::BindOnce(std::move(seek_done_cb), true));
        } else {
          std::move(seek_done_cb).Run(finished);
        }
      }];
}

void AVFMediaDecoder::AutoSeekTask() {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK_EQ(playback_state_, STOPPED);

  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Auto-seeking to "
          << last_audio_timestamp_.InMicroseconds();

  const scoped_refptr<base::TaskRunner> runner = background_runner();

  // The audio mix will have to be reset to prevent a "phase shift" in the
  // series of audio buffers.  The rendering end of the Chrome pipeline
  // treats such shifts as errors.
  __block base::OnceClosure set_audio_mix_task =
      base::BindOnce(&SetAudioMix, background_tasks_canceled_,
                     [player_ currentItem], GetAudioMix(AudioTrack()));

  __block base::OnceClosure auto_seek_done_cb =
      BindToCurrentLoop(base::BindOnce(&AVFMediaDecoder::AutoSeekDone,
                                       weak_ptr_factory_.GetWeakPtr()));

  [player_ seekToTime:TimeDeltaToCMTime(last_audio_timestamp_)
        toleranceBefore:CoreMediaGlueCMTimeToCMTime(kCMTimeZero)
         toleranceAfter:CoreMediaGlueCMTimeToCMTime(kCMTimeZero)
      completionHandler:^(BOOL finished) {
        VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
                << (finished ? " Auto-seek DONE"
                             : " Auto-seek was interrupted/rejected");
        // Need to set a new audio mix whether the auto-seek was successful
        // or not.  We will most likely continue decoding.
        runner->PostTaskAndReply(FROM_HERE, std::move(set_audio_mix_task),
                                 std::move(auto_seek_done_cb));
      }];
}

void AVFMediaDecoder::InitializeAudioOutput(InitializeCallback initialize_cb) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(player_ != NULL);

  AVAssetTrack* audio_track = AudioTrack();

  // If AVPlayer detects video track but no audio track (or audio format is not
  // supported) we proceed with video initialization, to show at least
  // audio-less video.
  if (audio_track == NULL) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Playing video only";
    DCHECK(has_video_track());
    AudioFormatKnown(std::move(initialize_cb), audio_stream_format_);
    return;
  }

  // Otherwise, we create an audio processing tap and wait for it to provide
  // the audio stream format (AudioFormatKnown()).
  DCHECK(audio_track != NULL);

  // First, though, make sure AVPlayer doesn't emit any sound on its own
  // (DNA-28672).  The only sound we want is the one played by Chrome from the
  // samples obtained through the audio processing tap.
  [player_ setVolume:0.0];

  base::scoped_nsobject<AVAudioMix> audio_mix =
      GetAudioMix(audio_track, std::move(initialize_cb));
  if (audio_mix.get() == nil) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Could not create AVAudioMix with audio processing tap";
    return;
  }

  [[player_ currentItem] setAudioMix:audio_mix];
}

base::scoped_nsobject<AVAudioMix> AVFMediaDecoder::GetAudioMix(
    AVAssetTrack* audio_track,
    InitializeCallback initialize_cb) {
  AVFAudioTap::FormatKnownCB format_known_cb;
  if (initialize_cb) {
    format_known_cb = base::BindOnce(&AVFMediaDecoder::AudioFormatKnown,
                                     weak_ptr_factory_.GetWeakPtr(),
                                     std::move(initialize_cb));
  }
  return AVFAudioTap::GetAudioMix(
      audio_track, base::ThreadTaskRunnerHandle::Get(),
      std::move(format_known_cb),
      base::BindRepeating(&AVFMediaDecoder::AudioSamplesReady,
                          weak_ptr_factory_.GetWeakPtr()));
}

void AVFMediaDecoder::AudioFormatKnown(
    InitializeCallback initialize_cb,
    const AudioStreamBasicDescription& format) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(!is_audio_format_known());
  DCHECK(player_ != NULL);

  audio_stream_format_ = format;

  // The audio output is now fully initialized, proceed to initialize the video
  // output.

  if (has_video_track()) {
    if (!InitializeVideoOutput()) {
      if (initialize_cb) {
        std::move(initialize_cb).Run(false);
      }
      return;
    }
  }

  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " PLAYING AVPlayer";
  playback_state_ = STARTING;
  [player_ play];

  if (initialize_cb) {
    std::move(initialize_cb).Run(true);
  }
}

void AVFMediaDecoder::AudioSamplesReady(scoped_refptr<DataBuffer> buffer) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (seeking_ || stream_has_ended_) {
    LOG(WARNING) << " PROPMEDIA(GPU) : " << __FUNCTION__
                 << " Ignoring audio samples: "
                 << (seeking_ ? "seeking" : "stream has ended");
    return;
  }

  if (last_audio_timestamp_ != media::kNoTimestamp &&
      buffer->timestamp() <= last_audio_timestamp_) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Audio buffer @"
            << buffer->timestamp().InMicroseconds()
            << " older than last buffer @"
            << last_audio_timestamp_.InMicroseconds() << ", dropping";
    return;
  }

  last_audio_timestamp_ = buffer->timestamp();

  client_->MediaSamplesReady(PlatformStreamType::kAudio, std::move(buffer));
}

bool AVFMediaDecoder::InitializeVideoOutput() {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(has_video_track());

  if (VideoFrameRate() <= 0.0)
    return false;

  DCHECK_EQ([[VideoTrack() formatDescriptions] count], 1u);
  video_stream_format_ = reinterpret_cast<CMFormatDescriptionRef>(
      [[VideoTrack() formatDescriptions] objectAtIndex:0]);

  const CMVideoDimensions coded_size =
      CMVideoFormatDescriptionGetDimensions(video_stream_format());
  video_coded_size_ = gfx::Size(coded_size.width, coded_size.height);

  NSDictionary* output_settings = @{
    base::mac::CFToNSCast(kCVPixelBufferPixelFormatTypeKey) :
        @(kCVPixelFormatType_420YpCbCr8Planar)
  };
  video_output_.reset([[AVPlayerItemVideoOutput alloc]
      initWithPixelBufferAttributes:output_settings]);

  [[player_ currentItem] addOutput:video_output_];

  const base::RepeatingCallback<void(const CMTime&)> periodic_cb =
      BindToCurrentLoop(
          base::BindRepeating(&AVFMediaDecoder::ReadFromVideoOutput,
                              weak_ptr_factory_.GetWeakPtr()));

  CMTime interval = CMTimeMake(1, VideoFrameRate());
  id handle = [player_
      addPeriodicTimeObserverForInterval:CoreMediaGlueCMTimeToCMTime(interval)
                                   queue:nil
                              usingBlock:^(CMTime time) {
                                std::move(periodic_cb).Run(time);
                              }];
  DCHECK(time_observer_handle_.get() == nil);
  time_observer_handle_.reset([handle retain]);

  return true;
}

void AVFMediaDecoder::ReadFromVideoOutput(const CMTime& cm_timestamp) {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__;
  DCHECK(thread_checker_.CalledOnValidThread());

  if (seeking_ || playback_state_ != PLAYING || stream_has_ended_) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__
            << " Not reading video output: "
            << (seeking_ ? "seeking"
                         : playback_state_ != PLAYING ? "not playing"
                                                      : "stream has ended");
    return;
  }

  const base::TimeDelta timestamp = CMTimeToTimeDelta(cm_timestamp);

  if (last_video_timestamp_ != media::kNoTimestamp &&
      timestamp <= last_video_timestamp_) {
    VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << " Video buffer @"
            << timestamp.InMicroseconds() << " older than last buffer @"
            << last_video_timestamp_.InMicroseconds() << ", dropping";
    return;
  }

  const scoped_refptr<DataBuffer> buffer =
      GetVideoFrame(video_output_, cm_timestamp, video_coded_size_);
  if (buffer.get() == NULL) {
    if (timestamp >= duration_)
      PlayerPlayedToEnd("video output");
    return;
  }

  last_video_timestamp_ = buffer->timestamp();

  client_->MediaSamplesReady(PlatformStreamType::kVideo, buffer);
}

void AVFMediaDecoder::AutoSeekDone() {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__;
  DCHECK(thread_checker_.CalledOnValidThread());

  seeking_ = false;

  RunTasksPendingSeekDone();

  TRACE_EVENT_ASYNC_END0("IPC_MEDIA", "AVFMediaDecoder::Auto-seek", this);
}

void AVFMediaDecoder::SeekDone(SeekCallback seek_cb, bool finished) {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__;
  DCHECK(thread_checker_.CalledOnValidThread());

  seeking_ = false;
  stream_has_ended_ = false;

  last_audio_timestamp_ = media::kNoTimestamp;
  last_video_timestamp_ = media::kNoTimestamp;

  std::move(seek_cb).Run(finished);

  RunTasksPendingSeekDone();

  TRACE_EVENT_ASYNC_END0("IPC_MEDIA", "AVFMediaDecoder::Seek", this);
}

void AVFMediaDecoder::RunTasksPendingSeekDone() {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (seek_on_seek_done_task_) {
    std::move(seek_on_seek_done_task_).Run();
  }

  if (play_on_seek_done_) {
    play_on_seek_done_ = false;
    PlayWhenReady("Pending seek done");
  }
}

void AVFMediaDecoder::PlayerPlayedToEnd(base::StringPiece source) {
  VLOG(1) << " PROPMEDIA(GPU) : " << __FUNCTION__ << '(' << source << ')';
  DCHECK(thread_checker_.CalledOnValidThread());

  if (stream_has_ended_)
    return;

  stream_has_ended_ = true;

  // Typically, we receive the |PlayerRateChanged()| callback for a paused
  // AVPlayer right before we receive this callback.  Let's cancel the seek we
  // may have started there.
  [[player_ currentItem] cancelPendingSeeks];

  client_->StreamHasEnded();
}

AVAssetTrack* AVFMediaDecoder::AssetTrackForType(
    NSString* track_type_name) const {
  NSArray* tracks =
      [[[player_ currentItem] asset] tracksWithMediaType:track_type_name];
  AVAssetTrack* track = [tracks count] > 0u ? [tracks objectAtIndex:0] : nil;

  if (track && track_type_name == AVMediaTypeVideo) {
    // NOTE(tomas@vivaldi.com): VB-45871
    // Return nil to avoid renderer crash
    if (track.formatDescriptions.count > 1) {
      return nil;
    }
    CMFormatDescriptionRef desc = reinterpret_cast<CMFormatDescriptionRef>(
        [[track formatDescriptions] objectAtIndex:0]);
    if ("jpeg" == FourCCToString(CMFormatDescriptionGetMediaSubType(desc))) {
      return nil;
    }
  }

  return track;
}

AVAssetTrack* AVFMediaDecoder::VideoTrack() const {
  return AssetTrackForType(AVMediaTypeVideo);
}

AVAssetTrack* AVFMediaDecoder::AudioTrack() const {
  return AssetTrackForType(AVMediaTypeAudio);
}

double AVFMediaDecoder::VideoFrameRate() const {
  return [VideoTrack() nominalFrameRate];
}

scoped_refptr<base::TaskRunner> AVFMediaDecoder::background_runner() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  return BackgroundThread::getInstance()->task_runner();
}

}  // namespace media
