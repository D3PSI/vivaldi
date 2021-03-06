// Copyright (c) 2018 Vivaldi Technologies AS. All rights reserved
// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/initialize_extensions_client.h"

#include "base/no_destructor.h"
#include "extensions/common/extensions_client.h"
#include "extensions/vivaldi_extensions_client.h"

void EnsureExtensionsClientInitialized() {
  static bool initialized = false;

  static base::NoDestructor<extensions::VivaldiExtensionsClient>
      extensions_client;

  if (!initialized) {
    initialized = true;
    extensions::ExtensionsClient::Set(extensions_client.get());
  }

  // ExtensionsClient::Set() will early-out if the client was already set, so
  // this allows us to check that this was the only site setting it.
  DCHECK_EQ(extensions_client.get(), extensions::ExtensionsClient::Get())
      << "ExtensionsClient should only be initialized through "
      << "EnsureExtensionsClientInitialized() when using "
      << "ChromeExtensionsClient.";
}
