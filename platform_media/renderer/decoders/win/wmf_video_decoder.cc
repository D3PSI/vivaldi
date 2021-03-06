// -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//
// Copyright (c) 2018 Vivaldi Technologies AS. All rights reserved.
// Copyright (C) 2015 Opera Software ASA.  All rights reserved.
//
// This file is an original work developed by Opera Software ASA

#include "platform_media/renderer/decoders/win/wmf_video_decoder.h"

namespace media {

WMFVideoDecoder::WMFVideoDecoder(
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : impl_(std::move(task_runner)) {}

WMFVideoDecoder::~WMFVideoDecoder() {}

void WMFVideoDecoder::Initialize(
    const VideoDecoderConfig& config,
    bool low_delay,
    CdmContext* cdm_context,
    InitCB init_cb,
    const OutputCB& output_cb,
    const WaitingCB& waiting_for_decryption_key_cb) {
  impl_.Initialize(config, std::move(init_cb), output_cb);
}

void WMFVideoDecoder::Decode(scoped_refptr<DecoderBuffer> buffer,
                             DecodeCB decode_cb) {
  impl_.Decode(buffer, std::move(decode_cb));
}

void WMFVideoDecoder::Reset(base::OnceClosure closure) {
  VLOG(1) << " PROPMEDIA(RENDERER) : " << __FUNCTION__;
  impl_.Reset(std::move(closure));
}

VideoDecoderType WMFVideoDecoder::GetDecoderType() const {
  return VideoDecoderType::kVivWMFDecoder;
}

bool WMFVideoDecoder::NeedsBitstreamConversion() const {
  return true;
}

}  // namespace media
