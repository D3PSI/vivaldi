// -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//
// Copyright (c) 2018 Vivaldi Technologies AS. All rights reserved.
// Copyright (C) 2015 Opera Software ASA.  All rights reserved.
//
// This file is an original work developed by Opera Software ASA

#ifndef PLATFORM_MEDIA_RENDERER_DECODERS_IPC_AUDIO_DECODER_H_
#define PLATFORM_MEDIA_RENDERER_DECODERS_IPC_AUDIO_DECODER_H_

#include "platform_media/common/feature_toggles.h"

#include "base/sequence_checker.h"
#include "base/synchronization/waitable_event.h"
#include "media/base/media_export.h"
#include "platform_media/renderer/pipeline/ipc_media_pipeline_host.h"

namespace base {
class SequencedTaskRunner;
}

namespace media {

class AudioBus;
class FFmpegURLProtocol;

// Audio decoder based on the IPCMediaPipeline. It decodes in-memory audio file
// data. It is used for Web Audio API, so its usage has to be synchronous.
// The IPCMediaPipeline flow is asynchronous, so IPCAudioDecoder has to use some
// synchronization tricks in order to appear synchronous.
class MEDIA_EXPORT IPCAudioDecoder {
 public:
  class MEDIA_EXPORT ScopedDisableForTesting {
   public:
    ScopedDisableForTesting();
    ~ScopedDisableForTesting();
  };

  static bool IsAvailable();

  explicit IPCAudioDecoder(FFmpegURLProtocol* protocol);
  ~IPCAudioDecoder();
  IPCAudioDecoder(const IPCAudioDecoder&) = delete;
  IPCAudioDecoder& operator=(const IPCAudioDecoder&) = delete;

  bool Initialize();

  int Read(std::vector<std::unique_ptr<AudioBus>>* decoded_audio_packets);

  int channels() const { return channels_; }
  int sample_rate() const { return sample_rate_; }
  int number_of_frames() const { return number_of_frames_; }
  base::TimeDelta duration() const { return duration_; }

 private:
  class InMemoryDataSource;

  void OnInitialized(bool success);
  void ReadInternal();
  void DataReady(DemuxerStream::Status status,
                 scoped_refptr<DecoderBuffer> buffer);

  static void FinishHostOnMediaThread(
      std::unique_ptr<InMemoryDataSource> data_source,
      std::unique_ptr<IPCMediaPipelineHost> ipc_media_pipeline_host);

  scoped_refptr<base::SequencedTaskRunner> media_task_runner_;
  std::unique_ptr<InMemoryDataSource> data_source_;

  int channels_;
  int sample_rate_;
  int number_of_frames_;
  int bytes_per_frame_;
  SampleFormat sample_format_;
  base::TimeDelta duration_;

  std::vector<std::unique_ptr<AudioBus>>* decoded_audio_packets_;
  int frames_read_;

  std::unique_ptr<IPCMediaPipelineHost> ipc_media_pipeline_host_;
  base::WaitableEvent async_task_done_;

  SEQUENCE_CHECKER(decoder_sequence_checker_);
};

}  // namespace media

#endif  // PLATFORM_MEDIA_RENDERER_DECODERS_IPC_AUDIO_DECODER_H_
