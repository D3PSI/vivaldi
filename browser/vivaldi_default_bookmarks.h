// Copyright 2019 Vivaldi Technologies. All rights reserved.
//

#ifndef BROWSER_VIVALDI_DEFAULT_BOOKMARKS_H_
#define BROWSER_VIVALDI_DEFAULT_BOOKMARKS_H_

#include "base/callback.h"

class Profile;

namespace vivaldi_default_bookmarks {

extern bool g_bookmark_update_actve;

using UpdateCallback = base::OnceCallback<
    void(bool ok, bool no_version, const std::string& locale)>;

void UpdatePartners(Profile* profile,
                    UpdateCallback callback = UpdateCallback());

}  // namespace vivaldi_default_bookmarks

#endif  // BROWSER_VIVALDI_DEFAULT_BOOKMARKS_H_
