// -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//
// Copyright (c) 2018 Vivaldi Technologies AS. All rights reserved.
// Copyright (C) 2015 Opera Software ASA.  All rights reserved.
//
// This file is an original work developed by Opera Software ASA.

#include "platform_media/renderer/decoders/win/wmf_decoder_impl.h"

#include <mferror.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/memory/free_deleter.h"
#include "base/numerics/safe_conversions.h"
#include "base/win/windows_version.h"
#include "media/base/audio_buffer.h"
#include "media/base/channel_layout.h"
#include "media/base/data_buffer.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_status.h"
#include "platform_media/common/platform_logging_util.h"
#include "platform_media/common/win/platform_media_init.h"

#define LOG_HR_FAIL(hr, message)                                            \
  do {                                                                      \
    LOG(WARNING) << " PROPMEDIA(RENDERER) : " << __FUNCTION__ << " Failed " \
                 << message << ", hr=0x" << std::hex << hr;                 \
  } while (false)

#define RETURN_ON_HR_FAIL(hr, message, return_value) \
  do {                                               \
    HRESULT _hr_value = (hr);                        \
    if (FAILED(_hr_value)) {                         \
      LOG_HR_FAIL(_hr_value, message);               \
      return (return_value);                         \
    }                                                \
  } while (false)

namespace media {

namespace {

// This function is used as |no_longer_needed_cb| of
// VideoFrame::WrapExternalYuvData to make sure we keep reference to
// DataBuffer object as long as we need it.
void BufferHolder(const scoped_refptr<DataBuffer>& buffer) {
  /* Intentionally empty */
}

SampleFormat ConvertToSampleFormat(uint32_t sample_size) {
  // We set output stream to use MFAudioFormat_PCM. MSDN does not state openly
  // that this is an integer format but there is an example which shows that
  // floating point PCM audio is set using MFAudioFormat_Float subtype for AAC
  // decoder, so we have to assume that for an integer format we should use
  // MFAudioFormat_PCM.
  switch (sample_size) {
    case 1:
      return SampleFormat::kSampleFormatU8;
    case 2:
      return SampleFormat::kSampleFormatS16;
    case 4:
      return SampleFormat::kSampleFormatS32;
    default:
      return kUnknownSampleFormat;
  }
}

int CalculateBufferAlignment(DWORD alignment) {
  return (alignment > 1) ? alignment - 1 : 0;
}

GUID AudioCodecToAudioSubtypeGUID(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kAAC:
      return MFAudioFormat_AAC;
    default:
      NOTREACHED();
  }
  return GUID();
}

}  // namespace

template <DemuxerStream::Type StreamType>
WMFDecoderImpl<StreamType>::WMFDecoderImpl(
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)), output_sample_size_(0) {}

template <DemuxerStream::Type StreamType>
void WMFDecoderImpl<StreamType>::Initialize(const DecoderConfig& config,
                                            InitCB init_cb,
                                            const OutputCB& output_cb) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  if (!IsValidConfig(config)) {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Media Config not accepted for codec : "
            << GetCodecName(config.codec());
    std::move(init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
    return;
  } else {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Supported decoder config for codec : " << Loggable(config);
  }

  config_ = config;

  decoder_ = CreateWMFDecoder(config_);
  if (!decoder_ || !ConfigureDecoder()) {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Creating/Configuring failed for codec : "
            << GetCodecName(config.codec());
    std::move(init_cb).Run(DecoderStatus::Codes::kFailedToCreateDecoder);
    return;
  }

  debug_buffer_logger_.Initialize(GetCodecName(config_.codec()));

  output_cb_ = output_cb;
  ResetTimestampState();

  std::move(init_cb).Run(DecoderStatus::Codes::kOk);
}

template <DemuxerStream::Type StreamType>
void WMFDecoderImpl<StreamType>::Decode(scoped_refptr<DecoderBuffer> buffer,
                                        DecodeCB decode_cb) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  debug_buffer_logger_.Log(*buffer);

  if (buffer->end_of_stream()) {
    VLOG(5) << " PROPMEDIA(RENDERER) : " << __FUNCTION__ << " (EOS)";
    const bool drained_ok = Drain();
    LOG_IF(WARNING, !drained_ok)
        << " PROPMEDIA(RENDERER) : " << __FUNCTION__
        << " Drain did not succeed - returning kMalformedBitstream";
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(decode_cb),
                       drained_ok ? DecoderStatus::Codes::kOk
                                  : DecoderStatus::Codes::kMalformedBitstream));
    return;
  }
  VLOG(5) << " PROPMEDIA(RENDERER) : " << __FUNCTION__ << " ("
          << buffer->timestamp() << ")";

  const HRESULT hr = ProcessInput(buffer);
  DCHECK_NE(MF_E_NOTACCEPTING, hr)
      << "The transform is neither producing output "
         "nor accepting input? This must not happen, see ProcessOutputLoop()";
  typename media::DecoderStatus::Codes status =
      SUCCEEDED(hr) && ProcessOutputLoop()
          ? DecoderStatus::Codes::kOk
          : DecoderStatus::Codes::kPlatformDecodeFailure;

  LOG_IF(WARNING, (status == DecoderStatus::Codes::kPlatformDecodeFailure))
      << " PROPMEDIA(RENDERER) : " << __FUNCTION__
      << " processing buffer failed, returning kPlatformDecodeFailure";

  task_runner_->PostTask(FROM_HERE,
                         base::BindOnce(std::move(decode_cb), status));
}

template <DemuxerStream::Type StreamType>
void WMFDecoderImpl<StreamType>::Reset(base::OnceClosure closure) {
  VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__;
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  // Transform needs to be reset, skip this and seeking may fail.
  decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);

  ResetTimestampState();

  task_runner_->PostTask(FROM_HERE, std::move(closure));
}

template <>
bool WMFDecoderImpl<DemuxerStream::AUDIO>::IsValidConfig(
    const DecoderConfig& config) {
  if (config.codec() != AudioCodec::kAAC) {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Unsupported Audio codec : " << GetCodecName(config.codec());
    return false;
  }

  if (config.is_encrypted()) {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Unsupported Encrypted Audio codec : "
            << GetCodecName(config.codec());
    return false;
  }

  bool isAvailable = !!platform_media_init::GetWMFLibraryForAAC();

  LOG_IF(WARNING, !isAvailable)
      << " PROPMEDIA(RENDERER) : " << __FUNCTION__
      << " Audio Platform Decoder (" << GetCodecName(config.codec())
      << ") : Unavailable";

  return isAvailable;
}

template <>
bool WMFDecoderImpl<DemuxerStream::VIDEO>::IsValidConfig(
    const DecoderConfig& config) {
  if (!platform_media_init::GetWMFLibraryForH264()) {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Video Platform Decoder : Unavailable";
    return false;
  }

  LOG_IF(WARNING, config.codec() != VideoCodec::kH264)
      << " PROPMEDIA(RENDERER) : " << __FUNCTION__
      << " Unsupported Video codec : " << GetCodecName(config.codec());

  if (config.codec() == VideoCodec::kH264) {
    LOG_IF(WARNING, !(config.profile() >= VideoCodecProfile::H264PROFILE_MIN))
        << " PROPMEDIA(RENDERER) : " << __FUNCTION__
        << " Unsupported Video profile (too low) : " << config.profile();

    LOG_IF(WARNING, !(config.profile() <= VideoCodecProfile::H264PROFILE_MAX))
        << " PROPMEDIA(RENDERER) : " << __FUNCTION__
        << " Unsupported Video profile (too high) : " << config.profile();
  }

  if (config.is_encrypted()) {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Unsupported Encrypted VIDEO codec : "
            << GetCodecName(config.codec());
    return false;
  }

  return config.codec() == VideoCodec::kH264 &&
         config.profile() >= VideoCodecProfile::H264PROFILE_MIN &&
         config.profile() <= VideoCodecProfile::H264PROFILE_MAX;
}

// static
template <>
HMODULE WMFDecoderImpl<DemuxerStream::AUDIO>::GetModuleLibrary() {
  return platform_media_init::GetWMFLibraryForAAC();
}

// static
template <>
HMODULE WMFDecoderImpl<DemuxerStream::VIDEO>::GetModuleLibrary() {
  return platform_media_init::GetWMFLibraryForH264();
}

// static
template <>
GUID WMFDecoderImpl<DemuxerStream::AUDIO>::GetMediaObjectGUID(
    const DecoderConfig& config) {
  switch (config.codec()) {
    case AudioCodec::kAAC:
      return __uuidof(CMSAACDecMFT);
    default:
      NOTREACHED();
  }
  return GUID();
}

// static
template <>
GUID WMFDecoderImpl<DemuxerStream::VIDEO>::GetMediaObjectGUID(
    const DecoderConfig& /* config */) {
  return __uuidof(CMSH264DecoderMFT);
}

// static
template <DemuxerStream::Type StreamType>
Microsoft::WRL::ComPtr<IMFTransform>
WMFDecoderImpl<StreamType>::CreateWMFDecoder(const DecoderConfig& config) {
  // CoCreateInstance() is not avaliable in the sandbox, reimplement it.
  HMODULE library = GetModuleLibrary();
  if (!library)
    return nullptr;

  auto* const get_class_object = reinterpret_cast<decltype(DllGetClassObject)*>(
      ::GetProcAddress(library, "DllGetClassObject"));
  if (!get_class_object) {
    LOG(WARNING) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
                 << " Error while retrieving class object getter function.";
    return nullptr;
  }

  Microsoft::WRL::ComPtr<IClassFactory> factory;
  HRESULT hr =
      get_class_object(GetMediaObjectGUID(config), __uuidof(IClassFactory),
                       reinterpret_cast<void**>(factory.GetAddressOf()));
  RETURN_ON_HR_FAIL(hr, "DllGetClassObject()", nullptr);

  Microsoft::WRL::ComPtr<IMFTransform> decoder;
  hr =
      factory->CreateInstance(nullptr, __uuidof(IMFTransform),
                              reinterpret_cast<void**>(decoder.GetAddressOf()));
  RETURN_ON_HR_FAIL(hr, "IClassFactory::CreateInstance(wmf_decoder)", nullptr);

  return decoder;
}

template <DemuxerStream::Type StreamType>
bool WMFDecoderImpl<StreamType>::ConfigureDecoder() {
  if (!SetInputMediaType())
    return false;

  if (!SetOutputMediaType())
    return false;

  // It requires both input and output to be set.
  HRESULT hr = decoder_->GetInputStreamInfo(0, &input_stream_info_);
  RETURN_ON_HR_FAIL(hr, "IMFTransform::GetInputStreamInfo()", false);

  return true;
}

template <>
bool WMFDecoderImpl<DemuxerStream::AUDIO>::SetInputMediaType() {
  Microsoft::WRL::ComPtr<IMFMediaType> media_type;
  HRESULT hr = MFCreateMediaType(media_type.GetAddressOf());
  RETURN_ON_HR_FAIL(hr, "MFCreateMediaType()", false);

  hr = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE)", false);

  hr = media_type->SetGUID(MF_MT_SUBTYPE,
                           AudioCodecToAudioSubtypeGUID(config_.codec()));
  RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetGUID(MF_MT_SUBTYPE)", false);

  hr = media_type->SetUINT32(
      MF_MT_AUDIO_NUM_CHANNELS,
      ChannelLayoutToChannelCount(config_.channel_layout()));
  RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetUINT32(MF_MT_AUDIO_NUM_CHANNELS)",
                    false);
  VLOG(5) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
          << " samples_per_second : " << config_.samples_per_second();

  hr = media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                             config_.samples_per_second());
  RETURN_ON_HR_FAIL(
      hr, "IMFMediaType::SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND)", false);

  if (config_.codec() == AudioCodec::kAAC) {
    // For details of media_type attributes see
    // http://msdn.microsoft.com/en-us/library/windows/desktop/dd742784%28v=vs.85%29.aspx

    // For ChunkDemuxer the payload contains adts_sequence() headers, for
    // FFmpeg it is raw which is the default.
    if (!config_.platform_media_ffmpeg_demuxer_) {
      hr = media_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0x1);
      RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetUINT32(MF_MT_AAC_PAYLOAD_TYPE)",
                        false);
    }

    // AAC decoder requires setting up HEAACWAVEFORMAT as a MF_MT_USER_DATA,
    // without this decoder fails to work (e.g. ProcessOutput returns
    // repeatedly with mysterious MF_E_TRANSFORM_STREAM_CHANGE status).
    size_t format_size = offsetof(HEAACWAVEFORMAT, pbAudioSpecificConfig);
    if (config_.platform_media_ffmpeg_demuxer_) {
      format_size += config_.extra_data().size();
    }
    std::unique_ptr<HEAACWAVEFORMAT, base::FreeDeleter> wave_format(
        static_cast<HEAACWAVEFORMAT*>(malloc(format_size)));
    memset(wave_format.get(), 0, format_size);
    if (!config_.platform_media_ffmpeg_demuxer_) {
      // Set input type to adts.
      wave_format->wfInfo.wPayloadType = 1;
    } else {
      // Keep wPayloadType at 0 to indicate raw data and set
      // AudioSpecificConfig() from the extra data.
      memcpy(wave_format->pbAudioSpecificConfig, config_.extra_data().data(),
             config_.extra_data().size());
    }
    // The blob must be set to the portion of HEAACWAVEFORMAT starting from
    // wfInfo.wPayloadType.
    hr = media_type->SetBlob(
        MF_MT_USER_DATA,
        reinterpret_cast<const uint8_t*>(&wave_format->wfInfo.wPayloadType),
        format_size - offsetof(HEAACWAVEFORMAT, wfInfo.wPayloadType));
    RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetBlob(MF_MT_USER_DATA)", false);
  }

  hr = decoder_->SetInputType(0, media_type.Get(), 0);  // No flags.
  if (FAILED(hr)) {
    std::string error_code;
    switch (hr) {
      case S_OK:
        error_code = "S_OK";
        break;
      case MF_E_INVALIDMEDIATYPE:
        error_code = "MF_E_INVALIDMEDIATYPE";
        break;
      case MF_E_INVALIDSTREAMNUMBER:
        error_code = "MF_E_INVALIDSTREAMNUMBER";
        break;
      case MF_E_INVALIDTYPE:
        error_code = "MF_E_INVALIDTYPE";
        break;
      case MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING:
        error_code = "MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING";
        break;
      case MF_E_TRANSFORM_TYPE_NOT_SET:
        error_code = "MF_E_TRANSFORM_TYPE_NOT_SET";
        break;
      case MF_E_UNSUPPORTED_D3D_TYPE:
        error_code = "MF_E_UNSUPPORTED_D3D_TYPE";
        break;
    }

    LOG_HR_FAIL(hr, "IMFTransform::SetInputType(), error=" << error_code);
    return false;
  }

  return true;
}

template <>
bool WMFDecoderImpl<DemuxerStream::VIDEO>::SetInputMediaType() {
  Microsoft::WRL::ComPtr<IMFMediaType> media_type;
  HRESULT hr = MFCreateMediaType(media_type.GetAddressOf());
  RETURN_ON_HR_FAIL(hr, "MFCreateMediaType()", false);

  hr = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE)", false);

  hr = media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetGUID(MF_MT_SUBTYPE)", false);

  hr = media_type->SetUINT32(MF_MT_INTERLACE_MODE,
                             MFVideoInterlace_MixedInterlaceOrProgressive);
  RETURN_ON_HR_FAIL(hr, "IMFMediaType::SetUINT32(MF_MT_INTERLACE_MODE)", false);

  hr = MFSetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE,
                          config_.coded_size().width(),
                          config_.coded_size().height());
  RETURN_ON_HR_FAIL(hr, "MFSetAttributeSize()", false);

  hr = decoder_->SetInputType(0, media_type.Get(), 0);  // No flags.
  RETURN_ON_HR_FAIL(hr, "IMFTransform::SetInputType()", false);

  return true;
}

template <DemuxerStream::Type StreamType>
bool WMFDecoderImpl<StreamType>::SetOutputMediaType() {
  VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__;

  Microsoft::WRL::ComPtr<IMFMediaType> out_media_type;

  HRESULT hr = S_OK;
  for (uint32_t i = 0; SUCCEEDED(decoder_->GetOutputAvailableType(
           0, i, out_media_type.GetAddressOf()));
       ++i) {
    GUID out_subtype = {0};
    hr = out_media_type->GetGUID(MF_MT_SUBTYPE, &out_subtype);
    RETURN_ON_HR_FAIL(hr, "IMFMediaType::GetGUID(MF_MT_SUBTYPE)", false);

    hr = SetOutputMediaTypeInternal(out_subtype, out_media_type.Get());
    if (hr == S_OK) {
      break;
    } else if (hr != S_FALSE) {
      LOG_HR_FAIL(hr, "SetOutputMediaTypeInternal()");
      return false;
    }

    out_media_type.Reset();
  }

  MFT_OUTPUT_STREAM_INFO output_stream_info = {0};
  hr = decoder_->GetOutputStreamInfo(0, &output_stream_info);
  RETURN_ON_HR_FAIL(hr, "IMFTransform::GetOutputStreamInfo()", false);

  output_sample_.Reset();

  const bool decoder_creates_samples =
      (output_stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) !=
      0;
  if (!decoder_creates_samples) {
    output_sample_ =
        CreateSample(CalculateOutputBufferSize(output_stream_info),
                     CalculateBufferAlignment(output_stream_info.cbAlignment));
    if (!output_sample_)
      return false;
  }

  return true;
}

template <>
HRESULT WMFDecoderImpl<DemuxerStream::AUDIO>::SetOutputMediaTypeInternal(
    GUID subtype,
    IMFMediaType* media_type) {
  if (subtype == MFAudioFormat_PCM) {
    HRESULT hr = decoder_->SetOutputType(0, media_type, 0);  // No flags.
    RETURN_ON_HR_FAIL(hr, "IMFTransform::SetOutputType()", hr);

    hr = media_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                               &output_samples_per_second_);
    RETURN_ON_HR_FAIL(
        hr, "IMFMediaType::GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND)", hr);

    uint32_t output_channel_count;
    hr = media_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &output_channel_count);
    RETURN_ON_HR_FAIL(hr, "IMFMediaType::GetUINT32(MF_MT_AUDIO_NUM_CHANNELS)",
                      hr);

    if (base::checked_cast<int>(output_channel_count) == config_.channels())
      output_channel_layout_ = config_.channel_layout();
    else
      output_channel_layout_ = GuessChannelLayout(output_channel_count);

    hr = media_type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
                               &output_sample_size_);
    RETURN_ON_HR_FAIL(
        hr, "IMFMediaType::GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE)", hr);

    // We will need size in Bytes.
    output_sample_size_ /= 8;
    return S_OK;
  }
  return S_FALSE;
}

template <>
HRESULT WMFDecoderImpl<DemuxerStream::VIDEO>::SetOutputMediaTypeInternal(
    GUID subtype,
    IMFMediaType* media_type) {
  if (subtype == MFVideoFormat_YV12) {
    HRESULT hr = decoder_->SetOutputType(0, media_type, 0);  // No flags.
    RETURN_ON_HR_FAIL(hr, "IMFTransform::SetOutputType()", hr);
    return S_OK;
  }
  return S_FALSE;
}

template <>
size_t WMFDecoderImpl<DemuxerStream::AUDIO>::CalculateOutputBufferSize(
    const MFT_OUTPUT_STREAM_INFO& stream_info) const {
  size_t buffer_size = stream_info.cbSize;
  return buffer_size;
}

template <>
size_t WMFDecoderImpl<DemuxerStream::VIDEO>::CalculateOutputBufferSize(
    const MFT_OUTPUT_STREAM_INFO& stream_info) const {
  return stream_info.cbSize;
}

template <DemuxerStream::Type StreamType>
HRESULT WMFDecoderImpl<StreamType>::ProcessInput(
    const scoped_refptr<DecoderBuffer>& input) {
  VLOG(5) << " PROPMEDIA(RENDERER) : " << __FUNCTION__;
  DCHECK(input.get());

  const Microsoft::WRL::ComPtr<IMFSample> sample =
      PrepareInputSample(input.get());
  if (!sample) {
    VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
            << " Failed to create input sample.";
    return MF_E_UNEXPECTED;
  }

  const HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);

  if (SUCCEEDED(hr))
    RecordInput(input);

  return hr;
}

template <>
void WMFDecoderImpl<DemuxerStream::AUDIO>::RecordInput(
    const scoped_refptr<DecoderBuffer>& input) {
  // We use AudioDiscardHelper to calculate output audio timestamps and
  // discard output buffers per the instructions in DecoderBuffer.
  // AudioDiscardHelper needs both the output buffers and the corresponsing
  // timing for the input buffers to do its work, so we need to queue the
  // input time info to cover the case when Decode() doesn't produce output
  // immediately.
  queued_input_timing_.push_back(input->time_info());
}

template <>
void WMFDecoderImpl<DemuxerStream::VIDEO>::RecordInput(
    const scoped_refptr<DecoderBuffer>& input) {
  // Do nothing.  We obtain timestamps from IMFTransform::GetSampleTime() for
  // video.
}

template <DemuxerStream::Type StreamType>
HRESULT WMFDecoderImpl<StreamType>::ProcessOutput() {
  VLOG(5) << " PROPMEDIA(RENDERER) : " << __FUNCTION__;

  // Make the whole buffer available for use by |decoder_| again after it was
  // filled with data by the previous call to ProcessOutput().
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = output_sample_->ConvertToContiguousBuffer(buffer.GetAddressOf());
  RETURN_ON_HR_FAIL(hr, "IMFSample::ConvertToContiguousBuffer()", hr);

  hr = buffer->SetCurrentLength(0);
  RETURN_ON_HR_FAIL(hr, "IMFMediaBuffer::SetCurrentLength()", hr);

  MFT_OUTPUT_DATA_BUFFER output_data_buffer = {0};
  output_data_buffer.pSample = output_sample_.Get();

  DWORD process_output_status = 0;
  hr = decoder_->ProcessOutput(0, 1, &output_data_buffer,
                               &process_output_status);
  IMFCollection* events = output_data_buffer.pEvents;
  if (events != nullptr) {
    // Even though we're not interested in events we have to clean them up.
    events->Release();
  }

  switch (hr) {
    case S_OK: {
      scoped_refptr<OutputType> output_buffer =
          CreateOutputBuffer(output_data_buffer);
      if (!output_buffer)
        return MF_E_UNEXPECTED;

      if (!ProcessBuffer(output_buffer))
        break;

      if (!output_cb_.is_null()) {
        task_runner_->PostTask(FROM_HERE,
                               base::BindRepeating(output_cb_, output_buffer));
      } else {
        return E_ABORT;
      }

      break;
    }
    case MF_E_TRANSFORM_NEED_MORE_INPUT:
      VLOG(5) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
              << " NEED_MORE_INPUT";
      // Need to wait for more input data to produce output.
      break;
    case MF_E_TRANSFORM_STREAM_CHANGE:
      VLOG(5) << " PROPMEDIA(RENDERER) : " << __FUNCTION__ << " STREAM_CHANGE";
      // For some reason we need to set up output media type again.
      if (!SetOutputMediaType())
        return MF_E_UNEXPECTED;
      // This kind of change will probably prevent us from getting more
      // output.
      break;
    default:
      LOG_HR_FAIL(hr, "IMFTransform::ProcessOutput()");
      break;
  }

  return hr;
}

template <>
bool WMFDecoderImpl<DemuxerStream::AUDIO>::ProcessBuffer(
    const scoped_refptr<AudioBuffer>& output) {
  if (queued_input_timing_.empty())
    return false;

  DecoderBuffer::TimeInfo dequeued_timing = queued_input_timing_.front();
  queued_input_timing_.pop_front();

  return discard_helper_->ProcessBuffers(dequeued_timing, output.get());
}

template <>
bool WMFDecoderImpl<DemuxerStream::VIDEO>::ProcessBuffer(
    const scoped_refptr<VideoFrame>& /* output */) {
  // Nothing to do.
  return true;
}

template <DemuxerStream::Type StreamType>
bool WMFDecoderImpl<StreamType>::ProcessOutputLoop() {
  for (;;) {
    const HRESULT hr = ProcessOutput();
    if (FAILED(hr)) {
      // If ProcessOutput fails with MF_E_TRANSFORM_NEED_MORE_INPUT or
      // MF_E_TRANSFORM_STREAM_CHANGE, it means it failed to get any output,
      // but still this is not a decoding error - the decoder just needs more
      // input data or reconfiguration on stream format change, so those
      // errors do not mean that ProcessOutputLoop failed.
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        return true;

      if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        continue;

      return false;
    }
  }
}

template <DemuxerStream::Type StreamType>
bool WMFDecoderImpl<StreamType>::Drain() {
  decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  return ProcessOutputLoop();
}

template <DemuxerStream::Type StreamType>
Microsoft::WRL::ComPtr<IMFSample>
WMFDecoderImpl<StreamType>::PrepareInputSample(
    const scoped_refptr<DecoderBuffer>& input) const {
  Microsoft::WRL::ComPtr<IMFSample> sample =
      CreateSample(input->data_size(),
                   CalculateBufferAlignment(input_stream_info_.cbAlignment));
  if (!sample)
    return nullptr;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = sample->GetBufferByIndex(0, buffer.GetAddressOf());
  RETURN_ON_HR_FAIL(hr, "IMFSample::GetBufferByIndex()", nullptr);

  uint8_t* buff_ptr = nullptr;
  hr = buffer->Lock(&buff_ptr, nullptr, nullptr);
  RETURN_ON_HR_FAIL(hr, "IMFMediaBuffer::Lock()", nullptr);

  memcpy(buff_ptr, input->data(), input->data_size());

  hr = buffer->Unlock();
  RETURN_ON_HR_FAIL(hr, "IMFMediaBuffer::Unlock()", nullptr);

  hr = buffer->SetCurrentLength(input->data_size());
  RETURN_ON_HR_FAIL(hr, "IMFMediaBuffer::SetCurrentLength()", nullptr);

  // IMFSample's timestamp is expressed in hundreds of nanoseconds.
  hr = sample->SetSampleTime(input->timestamp().InMicroseconds() * 10);
  RETURN_ON_HR_FAIL(hr, "IMFSample::SetSampleTime()", nullptr);

  return sample;
}

template <DemuxerStream::Type StreamType>
scoped_refptr<typename WMFDecoderImpl<StreamType>::OutputType>
WMFDecoderImpl<StreamType>::CreateOutputBuffer(
    const MFT_OUTPUT_DATA_BUFFER& output_data_buffer) {
  LONGLONG sample_time = 0;
  HRESULT hr = output_data_buffer.pSample->GetSampleTime(&sample_time);
    RETURN_ON_HR_FAIL(hr, "IMFSample::GetSampleTime()", nullptr);

  // The sample time in IMFSample is expressed in hundreds of nanoseconds.
  const base::TimeDelta timestamp = base::Microseconds(sample_time / 10);

  Microsoft::WRL::ComPtr<IMFMediaBuffer> media_buffer;
  hr = output_data_buffer.pSample->ConvertToContiguousBuffer(
      media_buffer.GetAddressOf());
  RETURN_ON_HR_FAIL(hr, "IMFSample::ConvertToContiguousBuffer()", nullptr);

  uint8_t* data = nullptr;
  DWORD data_size = 0;
  hr = media_buffer->Lock(&data, nullptr, &data_size);
  RETURN_ON_HR_FAIL(hr, "IMFMediaBuffer::Lock()", nullptr);

  scoped_refptr<typename WMFDecoderImpl<StreamType>::OutputType> output =
      CreateOutputBufferInternal(data, data_size, timestamp);

  hr = media_buffer->Unlock();
  RETURN_ON_HR_FAIL(hr, "IMFMediaBuffer::Unlock()", nullptr);

  return output;
}

template <>
scoped_refptr<AudioBuffer>
WMFDecoderImpl<DemuxerStream::AUDIO>::CreateOutputBufferInternal(
    const uint8_t* data,
    DWORD data_size,
    base::TimeDelta /* timestamp */) {
  DCHECK_GT(output_sample_size_, 0u) << "Division by zero";

  const int frame_count = data_size / output_sample_size_ /
                          ChannelLayoutToChannelCount(output_channel_layout_);

  // The timestamp will be calculated by |discard_helper_| later on.

  VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
          << " samples_per_second : " << config_.samples_per_second();

  return AudioBuffer::CopyFrom(
      ConvertToSampleFormat(output_sample_size_), output_channel_layout_,
      ChannelLayoutToChannelCount(output_channel_layout_),
      output_samples_per_second_, frame_count, &data, kNoTimestamp);
}

template <>
scoped_refptr<VideoFrame>
WMFDecoderImpl<DemuxerStream::VIDEO>::CreateOutputBufferInternal(
    const uint8_t* data,
    DWORD data_size,
    base::TimeDelta timestamp) {
  const scoped_refptr<DataBuffer> data_buffer =
      DataBuffer::CopyFrom(data, data_size);

  LONG stride = 0;
  HRESULT hr = MFGetStrideForBitmapInfoHeader(
      MFVideoFormat_YV12.Data1, config_.coded_size().width(), &stride);
  RETURN_ON_HR_FAIL(hr, "MFGetStrideForBitmapInfoHeader()", nullptr);

  // Stride has to be divisible by 16.
  LONG adjusted_stride = ((stride + 15) & ~15);

  if (stride != adjusted_stride) {
    // patricia@vivaldi.com : I don't know why we do this and it smells fishy
    LOG(WARNING) << __FUNCTION__ << " Before Stride : " << stride;
    stride = adjusted_stride;
    LOG(WARNING) << __FUNCTION__ << " After Stride : " << stride;
  }

  // Number of rows has to be divisible by 16.
  LONG rows = config_.coded_size().height();
  LONG adjusted_rows = ((rows + 15) & ~15);

  if (rows != adjusted_rows) {
    // patricia@vivaldi.com : I don't know why we do this and it smells fishy
    LOG(WARNING) << __FUNCTION__ << " Before rows : " << rows;
    rows = adjusted_rows;
    LOG(WARNING) << __FUNCTION__ << " After rows : " << rows;
  }

  scoped_refptr<VideoFrame> frame = VideoFrame::WrapExternalYuvData(
      VideoPixelFormat::PIXEL_FORMAT_YV12, config_.coded_size(),
      config_.visible_rect(), config_.natural_size(), stride, stride / 2,
      stride / 2, const_cast<uint8_t*>(data_buffer->data()),
      const_cast<uint8_t*>(data_buffer->data() +
                           (rows * stride + rows * stride / 4)),
      const_cast<uint8_t*>(data_buffer->data() + (rows * stride)), timestamp);
  frame->AddDestructionObserver(
      base::BindRepeating(&BufferHolder, data_buffer));
  return frame;
}

template <DemuxerStream::Type StreamType>
Microsoft::WRL::ComPtr<IMFSample> WMFDecoderImpl<StreamType>::CreateSample(
    DWORD buffer_size,
    int buffer_alignment) const {
  Microsoft::WRL::ComPtr<IMFSample> sample;
  HRESULT hr = MFCreateSample(sample.GetAddressOf());
  RETURN_ON_HR_FAIL(hr, "MFCreateSample()", nullptr);

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  hr = MFCreateAlignedMemoryBuffer(buffer_size, buffer_alignment,
                                   buffer.GetAddressOf());
  RETURN_ON_HR_FAIL(hr, "MFCreateAlignedMemoryBuffer()", nullptr);

  hr = sample->AddBuffer(buffer.Get());
  RETURN_ON_HR_FAIL(hr, "IMFSample::AddBuffer()", nullptr);

  return sample;
}

template <>
void WMFDecoderImpl<DemuxerStream::AUDIO>::ResetTimestampState() {
  VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__
          << " samples_per_second : " << config_.samples_per_second();

  discard_helper_.reset(new AudioDiscardHelper(config_.samples_per_second(),
                                               config_.codec_delay(), false));
  discard_helper_->Reset(config_.codec_delay());

  queued_input_timing_.clear();
}

template <>
void WMFDecoderImpl<DemuxerStream::VIDEO>::ResetTimestampState() {
  // Nothing to do.
}

template class WMFDecoderImpl<DemuxerStream::AUDIO>;
template class WMFDecoderImpl<DemuxerStream::VIDEO>;

}  // namespace media
